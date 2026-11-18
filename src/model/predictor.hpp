// The Predictor concept and the probability convention shared by the coder
// and every model.
#pragma once

#include <concepts>
#include <cstdint>
#include <optional>

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

// A Predictor that can be told what is likely to come, so that it can start
// fetching the memory that coding it will touch.
//
//   prefetch_after(byte, next)  at a byte boundary: `byte` is the byte about
//                               to be coded and `next` the one after it,
//                               both known.
//   prefetch_from_match()       at a byte boundary: the bytes are not known;
//                               guess them.
//   prefetch_decoded(bits)      after a decoded bit: `bits` of the byte, 1
//                               to 8, are known, and the rest are not.
//
// The contract: all three are hints. They change nothing that predict() or
// update() will do, so a predictor given every hint and one given none
// predict identically. All are noexcept and allocation-free, like the
// per-bit calls.
template <typename P>
concept Prefetching = Predictor<P> && requires(P& p, std::uint32_t byte, int bits) {
    { p.prefetch_after(byte, byte) } noexcept;
    { p.prefetch_from_match() } noexcept;
    { p.prefetch_decoded(bits) } noexcept;
};

// A Predictor that can code a byte it is sure of as one decision, a flag:
// "the byte is my guess". A byte with a flag that hits has no bits.
//
//   offer_flag()      at each byte boundary, before the byte: whether the
//                     byte starts with a flag.
//   guess()           the byte the flag claims, 0 to 255; and
//   predict_flag()    P(the byte is the guess) as a p12. Both only after
//                     offer_flag() returned true, and before update_flag().
//   update_flag(hit)  the flag's outcome, 1 if the byte is the guess. On a
//                     hit the byte is done; on a miss its bits follow.
//   forced_bit()      before each bit: the bit, if the predictor knows it
//                     already, because any other would make the byte the
//                     guess a flag ruled out. A forced bit is predicted and
//                     learned like any other, but not coded.
//
// The contract: offer_flag() is called at every byte boundary, so the
// predictor may leave work to it, and both directions make the same calls
// in the same order. All are noexcept and allocation-free.
template <typename P>
concept Flagging = Predictor<P> && requires(P& p, const P& c, int hit) {
    { p.offer_flag() } noexcept -> std::same_as<bool>;
    { c.guess() } noexcept -> std::same_as<std::uint32_t>;
    { c.predict_flag() } noexcept -> std::same_as<std::uint32_t>;
    { p.update_flag(hit) } noexcept -> std::same_as<void>;
    { c.forced_bit() } noexcept -> std::same_as<std::optional<int>>;
};

}  // namespace cpress::model
