// The bitwise codec driver: runs any Predictor through the arithmetic coder,
// one bit at a time, most significant bit of each byte first.
//
// encode_bitwise and decode_bitwise are the only place the two directions are
// paired. Model code is written once, as a Predictor, and both directions
// call it identically; that is what keeps the decoder in step with the
// encoder.
//
// A Prefetching predictor also gets a hint before each byte, in the same
// place in both loops: the encoder, which has the input, gives it the byte
// and the one after; the decoder, which has not decoded them yet, asks it to
// guess. That is why compressing is the faster direction for such a model.
// The decoder also says, after each bit, how much of the byte it knows,
// which narrows what the model must fetch; the encoder knew it all along.
//
// A Flagging predictor may code a byte as one decision instead of eight:
// before the byte it may offer a flag, "the byte is my guess", which is
// coded like a bit. If the flag hits, the byte is done. If not, the bits
// follow, and any bit the predictor knows already (forced_bit()) is
// predicted and learned but not coded. Both loops do this in
// encode_byte() and decode_byte(), which mirror each other line by line.
//
// Each comes in two forms. One takes a predictor the caller has constructed,
// for models that need arguments; it must be fresh, never used before. The
// other constructs a predictor that needs none and calls the first.
//
// Payload layout: the arithmetic coder's output, then its four flush bytes.
// There is no length field; the byte count in the container header says how
// many bytes to decode, and the payload must be consumed exactly.
#pragma once

#include "coder/arithmetic_coder.hpp"
#include "cpress/cpress.hpp"
#include "model/predictor.hpp"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

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

// A Flagging predictor can code a byte as one decision, its flag, where
// another spends eight, so a payload byte can stand for eight times as many
// output bytes: about 22,700. Its ceiling is eight times as high.
inline constexpr std::uint64_t kMaxFlaggedExpansion = 8 * kMaxExpansion;

// The ceiling for a payload coded with P.
template <model::Predictor P>
inline constexpr std::uint64_t kMaxExpansionFor =
    model::Flagging<P> ? kMaxFlaggedExpansion : kMaxExpansion;

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
// payload, at most `max_expansion` bytes for each: kMaxExpansionFor<P> for the
// codecs here, and a ceiling of its own for a codec that codes otherwise.
inline void require_within_expansion(std::uint64_t output_bytes, std::uint64_t payload_bytes,
                                     std::uint64_t max_expansion) {
    if (output_bytes > saturating_multiply(payload_bytes, max_expansion)) {
        throw Error("corrupt archive: header claims more data than the payload can hold");
    }
}

// The bit about to be coded, if the predictor knows it already. Only a
// Flagging predictor can.
template <model::Predictor P>
[[nodiscard]] std::optional<int> forced_bit(const P& predictor) noexcept {
    if constexpr (model::Flagging<P>) {
        return predictor.forced_bit();
    } else {
        return std::nullopt;
    }
}

// Codes `byte`: its flag, if the predictor offers one, then its bits unless
// the flag hit.
template <model::Predictor P>
void encode_byte(P& predictor, coder::Encoder& encoder, std::uint8_t byte) {
    if constexpr (model::Flagging<P>) {
        if (predictor.offer_flag()) {
            const int hit = byte == predictor.guess() ? 1 : 0;
            encoder.encode(hit, predictor.predict_flag());
            predictor.update_flag(hit);
            if (hit != 0) return;
        }
    }
    for (int shift = 7; shift >= 0; --shift) {
        const int bit = (byte >> shift) & 1;
        const std::uint32_t p12 = predictor.predict();
        const std::optional<int> forced = forced_bit(predictor);
        assert(!forced.has_value() || *forced == bit);
        if (!forced.has_value()) encoder.encode(bit, p12);
        predictor.update(bit);
    }
}

// The byte encode_byte() coded, read back.
template <model::Predictor P>
[[nodiscard]] std::uint8_t decode_byte(P& predictor, coder::Decoder& decoder) {
    if constexpr (model::Flagging<P>) {
        if (predictor.offer_flag()) {
            const std::uint32_t guess = predictor.guess();
            const int hit = decoder.decode(predictor.predict_flag());
            predictor.update_flag(hit);
            if (hit != 0) return static_cast<std::uint8_t>(guess);
        }
    }
    std::uint32_t byte = 0;
    for (int bits = 1; bits <= 8; ++bits) {
        const std::uint32_t p12 = predictor.predict();
        const std::optional<int> forced = forced_bit(predictor);
        const int bit = forced.has_value() ? *forced : decoder.decode(p12);
        predictor.update(bit);
        if constexpr (model::Prefetching<P>) predictor.prefetch_decoded(bits);
        byte = (byte * 2) + static_cast<std::uint32_t>(bit);
    }
    return static_cast<std::uint8_t>(byte);
}

}  // namespace detail

// Appends the payload for `input` to `output`, coded with `predictor`, which
// must be freshly constructed.
template <model::Predictor P>
void encode_bitwise(P& predictor, ByteView input, Bytes& output) {
    // The coder appends to a vector, which can allocate. Reserve a little
    // more than the input so growth is rare: a compressible input needs less,
    // and random input grows by about one percent.
    output.reserve(output.size() + input.size() + (input.size() / 64) + 16);

    coder::Encoder encoder(output);
    for (std::size_t i = 0; i < input.size(); ++i) {
        if constexpr (model::Prefetching<P>) {
            if (i + 1 < input.size()) predictor.prefetch_after(input[i], input[i + 1]);
        }
        detail::encode_byte(predictor, encoder, input[i]);
    }
    encoder.finish();
}

// The same, with a default-constructed predictor.
template <model::Predictor P>
    requires std::default_initializable<P>
void encode_bitwise(ByteView input, Bytes& output) {
    P predictor;
    encode_bitwise(predictor, input, output);
}

// Appends `original_size` decoded bytes to `output`, decoded with
// `predictor`, which must be freshly constructed, as the encoder's was and
// with the same arguments. Everything here comes from an untrusted archive,
// so it throws cpress::Error unless:
//   1. original_size is at most kMaxExpansionFor<P> times the payload size, which
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
void decode_bitwise(P& predictor, ByteView payload, std::uint64_t original_size, Bytes& output) {
    const std::uint64_t payload_size = payload.size();
    detail::require_within_expansion(original_size, payload_size, kMaxExpansionFor<P>);

    // original_size is still only a claim: a corrupt header can pass the
    // check above and ask for kMaxExpansionFor<P> times the payload. Reserve no
    // more than kReserveExpansion times it, and let the vector grow if the
    // data really is there.
    const std::uint64_t reserve_limit =
        detail::saturating_multiply(payload_size, kReserveExpansion);
    output.reserve(output.size() +
                   static_cast<std::size_t>(std::min(original_size, reserve_limit)));

    coder::Decoder decoder(payload);
    for (std::uint64_t decoded = 0; decoded < original_size; ++decoded) {
        // Rule 1 for what is left: the bytes still to decode must fit in the
        // payload still to read, plus the kCodeBytes the decoder holds. A
        // claim at the ceiling fails here as soon as the payload is spent
        // any faster than the ceiling allows, which is within a few bytes.
        const std::uint64_t payload_left = payload_size - decoder.consumed() + coder::kCodeBytes;
        detail::require_within_expansion(original_size - decoded, payload_left,
                                         kMaxExpansionFor<P>);

        if constexpr (model::Prefetching<P>) predictor.prefetch_from_match();
        const std::uint8_t byte = detail::decode_byte(predictor, decoder);
        if (decoder.overrun()) {
            throw Error("corrupt archive: payload ended before the data did");
        }
        output.push_back(byte);
    }

    // Also catches an empty or short payload when there is nothing to decode.
    if (decoder.overrun()) {
        throw Error("corrupt archive: payload is shorter than the coder's header");
    }
    if (decoder.consumed() != payload.size()) {
        throw Error("corrupt archive: payload has bytes left over");
    }
}

// The same, with a default-constructed predictor.
template <model::Predictor P>
    requires std::default_initializable<P>
void decode_bitwise(ByteView payload, std::uint64_t original_size, Bytes& output) {
    P predictor;
    decode_bitwise(predictor, payload, original_size, output);
}

}  // namespace cpress::codec
