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

#include <cstddef>
#include <cstdint>
#include <limits>

namespace cpress::codec {

// One coded bit shrinks the coder's interval by at most a factor of
// 4095/4096, so a payload byte can stand for at most about 2,840 output
// bytes. 4096 is a safe ceiling; see decode_bitwise.
inline constexpr std::uint64_t kMaxExpansion = 4096;

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
//      bounds both the work and the memory by the size of the input;
//   2. the decoder never reads past the payload (checked once per byte);
//   3. the decoder consumes the payload exactly, so an appended byte is
//      noticed.
// Errors are thrown here, never from inside the coder or the model.
template <model::Predictor P>
void decode_bitwise(ByteView payload, std::uint64_t original_size, Bytes& output) {
    const std::uint64_t payload_size = payload.size();
    const std::uint64_t limit =
        payload_size > std::numeric_limits<std::uint64_t>::max() / kMaxExpansion
            ? std::numeric_limits<std::uint64_t>::max()
            : payload_size * kMaxExpansion;
    if (original_size > limit) {
        throw Error("corrupt archive: header claims more data than the payload can hold");
    }

    // original_size is within the bound above, so this is at most
    // kMaxExpansion times the input and the reserve cannot be abused to
    // allocate without limit.
    output.reserve(output.size() + static_cast<std::size_t>(original_size));

    P predictor;
    coder::Decoder decoder(payload);
    for (std::uint64_t i = 0; i < original_size; ++i) {
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
