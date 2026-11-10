// The bitwise codec driver: runs any Predictor through the arithmetic coder,
// one bit at a time, most significant bit of each byte first.
//
// encode_bitwise and decode_bitwise are the only place the two directions are
// paired. Model code is written once, as a Predictor, and both directions
// call it identically; that is what keeps the decoder in step with the
// encoder.
//
// Payload layout: the arithmetic coder's output, then its four flush bytes.
// There is no length field; the byte count in the container header says how
// many bytes to decode, and the payload must be consumed exactly.
#pragma once

#include "coder/arithmetic_coder.hpp"
#include "cpress/cpress.hpp"
#include "model/predictor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace cpress::codec {

// Each coded bit leaves at most 4095/4096 of the coder's interval, so a
// payload byte can stand for at most about 2,840 output bytes. 4096 is a safe
// ceiling; see decode_bitwise.
//
// The bound holds from any point in a stream, not only its start: the output
// still to come is at most kMaxExpansion times the payload still to be read
// plus the coder::kCodeBytes the decoder already holds. No valid archive can
// break it, so checking it never rejects one.
inline constexpr std::uint64_t kMaxExpansion = 4096;

// How much output decode_bitwise reserves ahead, as a multiple of the payload
// size. kMaxExpansion bounds what an archive may claim; this bounds what is
// allocated on the strength of the claim alone. Output beyond it is paid for
// by vector growth, as it is actually decoded.
inline constexpr std::uint64_t kReserveExpansion = 8;

namespace detail {

// a * b, or the largest representable value if the product would overflow.
[[nodiscard]] constexpr std::uint64_t saturating_multiply(std::uint64_t a,
                                                          std::uint64_t b) noexcept {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    return (b != 0 && a > kMax / b) ? kMax : a * b;
}

// Throws unless `output_bytes` of output could come from `payload_bytes` of
// payload.
inline void require_within_expansion(std::uint64_t output_bytes, std::uint64_t payload_bytes) {
    if (output_bytes > saturating_multiply(payload_bytes, kMaxExpansion)) {
        throw Error("corrupt archive: header claims more data than the payload can hold");
    }
}

}  // namespace detail

// Appends the payload for `input` to `output`.
template <model::Predictor P>
void encode_bitwise(ByteView input, Bytes& output) {
    // The coder appends to a vector, which can allocate. Reserve a little
    // more than the input so growth is rare: a compressible input needs less,
    // and random input grows by about one percent.
    output.reserve(output.size() + input.size() + (input.size() / 64) + 16);

    P predictor;
    coder::Encoder encoder(output);
    for (const std::uint8_t byte : input) {
        for (int shift = 7; shift >= 0; --shift) {
            const int bit = (byte >> shift) & 1;
            encoder.encode(bit, predictor.predict());
            predictor.update(bit);
        }
    }
    encoder.finish();
}

// Appends `original_size` decoded bytes to `output`. Everything here comes
// from an untrusted archive, so it throws cpress::Error unless:
//   1. original_size is at most kMaxExpansion times the payload size, which
//      bounds both the work and the memory by the size of the input. This
//      holds on entry and for what is left after every byte;
//   2. the decoder never reads past the payload (checked once per byte);
//   3. the decoder consumes the payload exactly, so an appended byte is
//      noticed.
// Memory is allocated in proportion to the payload and to the bytes actually
// decoded, never to original_size alone.
// Errors are thrown here, never from inside the coder or the model.
//
// What these rules cannot do is tell a damaged payload from a compressed
// one before it ends. Random bytes decode to a sample of what the model
// expects, and an adaptive model then learns from its own sample and grows
// confident: to the order-0 model they read as data compressed about twenty
// times over. A header that claims that much or more, short of the ceiling,
// costs a decode of everything the payload yields before rule 2 rejects it.
template <model::Predictor P>
void decode_bitwise(ByteView payload, std::uint64_t original_size, Bytes& output) {
    const std::uint64_t payload_size = payload.size();
    detail::require_within_expansion(original_size, payload_size);

    // original_size is still only a claim: a corrupt header can pass the
    // check above and ask for kMaxExpansion times the payload. Reserve no
    // more than kReserveExpansion times it, and let the vector grow if the
    // data really is there.
    const std::uint64_t reserve_limit =
        detail::saturating_multiply(payload_size, kReserveExpansion);
    output.reserve(output.size() +
                   static_cast<std::size_t>(std::min(original_size, reserve_limit)));

    P predictor;
    coder::Decoder decoder(payload);
    for (std::uint64_t decoded = 0; decoded < original_size; ++decoded) {
        // Rule 1 for what is left: the bytes still to decode must fit in the
        // payload still to read, plus the kCodeBytes the decoder holds. A
        // claim at the ceiling fails here as soon as the payload is spent
        // any faster than the ceiling allows, which is within a few bytes.
        const std::uint64_t payload_left = payload_size - decoder.consumed() + coder::kCodeBytes;
        detail::require_within_expansion(original_size - decoded, payload_left);

        int byte = 0;
        for (int bit_index = 0; bit_index < 8; ++bit_index) {
            const int bit = decoder.decode(predictor.predict());
            predictor.update(bit);
            byte = (byte * 2) + bit;
        }
        if (decoder.overrun()) {
            throw Error("corrupt archive: payload ended before the data did");
        }
        output.push_back(static_cast<std::uint8_t>(byte));
    }

    // Also catches an empty or short payload when there is nothing to decode.
    if (decoder.overrun()) {
        throw Error("corrupt archive: payload is shorter than the coder's header");
    }
    if (decoder.consumed() != payload.size()) {
        throw Error("corrupt archive: payload has bytes left over");
    }
}

}  // namespace cpress::codec
