// TwoWayCoder: the arithmetic coder behind one call that runs in either
// direction, so that a model which decides what to code is written once.
//
// The bitwise codecs keep the two directions in step by sharing a
// Predictor: the driver owns the bytes and the model owns the bits. A
// structured model also decides which field comes next, as a difference
// from what, in how many bytes. If the encoder and the decoder each had a
// copy of that logic the copies would drift, so there is one copy, a
// template over the coder:
//
//   code(bit, p12)   when encoding, codes `bit` and leaves it as it was;
//                    when decoding, sets `bit` to the decoded bit.
//
// A function above the coder takes a value, codes it and returns it. When
// encoding, the value passed in is the one to code and comes back
// unchanged; when decoding, the value passed in is ignored and what comes
// back is the decoded one. The caller then carries on with the returned
// value, by the same lines in both directions.
//
// kDecoding is for the few places that must differ, all at the edge:
// reading a field from a message or writing it there, and searching for
// something's place or walking to a place already known.
#pragma once

#include "coder/arithmetic_coder.hpp"
#include "cpress/cpress.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace cpress::codec {

// A coder that codes one bit in either direction. p12 is P(bit = 1), in
// [kProbMin, kProbMax], as everywhere.
template <typename C>
concept TwoWayCoder = requires(C& coder, int& bit, std::uint32_t p12) {
    { coder.code(bit, p12) } noexcept -> std::same_as<void>;
    { C::kDecoding } -> std::convertible_to<bool>;
};

class EncodingCoder {
public:
    static constexpr bool kDecoding = false;

    // Appends to `output`; bytes already in it are left alone.
    explicit EncodingCoder(Bytes& output) noexcept : encoder_(output) {}

    void code(int& bit, std::uint32_t p12) noexcept {
        encoder_.encode(bit, p12);
    }

    // Writes the coder's flush bytes. Call exactly once, after the last bit.
    void finish() noexcept {
        encoder_.finish();
    }

private:
    coder::Encoder encoder_;
};

class DecodingCoder {
public:
    static constexpr bool kDecoding = true;

    // The payload must outlive the coder.
    explicit DecodingCoder(ByteView payload) noexcept : decoder_(payload) {}

    void code(int& bit, std::uint32_t p12) noexcept {
        bit = decoder_.decode(p12);
    }

    // As coder::Decoder's.
    [[nodiscard]] bool overrun() const noexcept {
        return decoder_.overrun();
    }
    [[nodiscard]] std::size_t consumed() const noexcept {
        return decoder_.consumed();
    }

private:
    coder::Decoder decoder_;
};

static_assert(TwoWayCoder<EncodingCoder>);
static_assert(TwoWayCoder<DecodingCoder>);

}  // namespace cpress::codec
