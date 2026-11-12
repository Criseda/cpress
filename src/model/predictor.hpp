// The Predictor concept and the probability convention shared by the coder
// and every model.
#pragma once

#include <concepts>
#include <cstdint>

namespace cpress::model {

// A prediction is P(next bit = 1) as an unsigned integer in kProbBits bits.
// Valid values are kProbMin to kProbMax inclusive; 0 and 4096 are never
// produced, so neither symbol ever has zero probability. Variables holding
// one are named with a `p12` suffix or prefix. Bytes are coded most
// significant bit first.
inline constexpr int kProbBits = 12;
inline constexpr std::uint32_t kProbOne = 1U << kProbBits;  // probability 1.0, never produced
inline constexpr std::uint32_t kProbMax = 4095;
inline constexpr std::uint32_t kProbMin = 1;

// A model that predicts a stream of bits one at a time.
//
//   predict()    P(next bit = 1) as a p12 in [kProbMin, kProbMax].
//   update(bit)  learns the bit that was actually coded.
//
// The contract, which the concept cannot express:
//   - predict() then update(bit) are called strictly alternately, once per
//     bit, so a model may keep state between them.
//   - predict() depends only on the bits passed to earlier update() calls.
//     This is what lets the decoder, which has not seen the bit yet, rebuild
//     the encoder's predictions exactly.
//   - A freshly constructed predictor is in its initial state. Two
//     constructed with equal arguments and fed the same bits produce the same
//     predictions on every platform, so models use integer arithmetic only.
//   - Both calls are noexcept and allocation-free: they run once per bit.
//     Construction is where a model allocates, and it may throw.
template <typename P>
concept Predictor = requires(P& p, int bit) {
    { p.predict() } noexcept -> std::same_as<std::uint32_t>;
    { p.update(bit) } noexcept -> std::same_as<void>;
};

// A Predictor that can be told, at each byte boundary, what is likely to
// come, so that it can start fetching the memory that coding it will touch.
//
//   prefetch_after(byte, next)  `byte` is the byte about to be coded and
//                               `next` the one after it, both known.
//   prefetch_from_match()       the bytes are not known; guess them.
//
// The contract: both are hints. They change nothing that predict() or
// update() will do, so a predictor given every hint and one given none
// predict identically. Both are noexcept and allocation-free, like the
// per-bit calls.
template <typename P>
concept Prefetching = Predictor<P> && requires(P& p, std::uint32_t byte) {
    { p.prefetch_after(byte, byte) } noexcept;
    { p.prefetch_from_match() } noexcept;
};

}  // namespace cpress::model
