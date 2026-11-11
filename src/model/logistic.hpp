// stretch and squash: the change of scale that lets predictions be added.
//
//   stretch(p) = ln(p / (1 - p))        probability -> evidence
//   squash(x)  = 1 / (1 + e^-x)         evidence -> probability, its inverse
//
// Why a mixer works on stretched values. A probability says how likely a 1
// is; its stretch says how much evidence there is for a 1. One half
// stretches to 0, no evidence either way. 0.999 stretches to about +7 and
// 0.001 to about -7. Evidence from separate sources adds, and probabilities
// do not: averaging 0.999 with five models that know nothing (0.5 each)
// gives 0.58 and throws the one good prediction away, while adding their
// stretches gives 7 + 0 + ... + 0, still 0.999. A weighted sum in this
// domain lets one confident model outvote any number of unsure ones, and the
// weights say how far each model's confidence has earned belief.
//
// Fixed point: a stretched value `st` is ln(p / (1 - p)) * kStretchOne, in
// [-kStretchMax, kStretchMax]. Both functions are table lookups, built at
// compile time by integer arithmetic alone, so every compiler produces the
// same tables: no floating point, here or anywhere in the model.
//
// Memory: (2 * kStretchMax + 1) * 2 bytes for squash and kProbOne * 2 bytes
// for stretch, 16 KiB together, read on every bit.
//
// Technique: mixing in the logistic domain was introduced by Matt Mahoney's
// PAQ7. His lpaq1 also builds stretch by inverting squash; its squash
// interpolates a table of 33 values, where every entry here is computed
// from the definition.
#pragma once

#include "model/predictor.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace cpress::model {

// One unit of ln(p / (1 - p)) is kStretchOne steps of `st`.
inline constexpr std::int32_t kStretchOne = 256;

// The largest stretched value: just under 8.0, where squash reaches kProbMax.
inline constexpr std::int32_t kStretchMax = 2047;

namespace detail {

inline constexpr int kQ32Bits = 32;
inline constexpr std::uint64_t kQ32One = std::uint64_t{1} << kQ32Bits;

// e^(-1 / kStretchOne) in Q32, summed from its Taylor series
// 1 - x + x^2/2! - x^3/3! + ... with x = 1 / kStretchOne. Each term is the
// one before divided by kStretchOne * k; the terms shrink, so the running
// sum never goes negative, and the loop ends when a term rounds to zero.
[[nodiscard]] constexpr std::uint64_t decay_per_step_q32() noexcept {
    std::uint64_t sum = 0;
    std::uint64_t term = kQ32One;
    for (std::uint64_t k = 0; term != 0; ++k) {
        sum = (k % 2 == 0) ? sum + term : sum - term;
        term /= static_cast<std::uint64_t>(kStretchOne) * (k + 1);
    }
    return sum;
}

inline constexpr std::uint64_t kDecayQ32 = decay_per_step_q32();
static_assert(kDecayQ32 == 4'278'222'806, "e^(-1/256) * 2^32, to the last digit");

inline constexpr std::size_t kSquashEntries = (2 * static_cast<std::size_t>(kStretchMax)) + 1;

// squash for every st in [-kStretchMax, kStretchMax], at index st + kStretchMax.
//
// For x >= 0, squash(x) = kProbOne / (1 + e^(-x / kStretchOne)), rounded to
// nearest and capped at kProbMax. e^(-x / kStretchOne) is `decay`, kept in
// Q32 and multiplied by kDecayQ32 once per step: both factors are at most
// 2^32, so the 64-bit product cannot overflow. The negative half follows
// from squash(-x) = 1 - squash(x), which makes the table exactly symmetric.
[[nodiscard]] constexpr std::array<std::uint16_t, kSquashEntries> make_squash_table() noexcept {
    constexpr std::uint64_t kProbOneQ32 = std::uint64_t{kProbOne} << kQ32Bits;
    const auto zero = static_cast<std::size_t>(kStretchMax);  // the index of st = 0

    std::array<std::uint16_t, kSquashEntries> table{};
    std::uint64_t decay = kQ32One;
    for (std::size_t x = 0; x <= zero; ++x) {
        const std::uint64_t denominator = kQ32One + decay;
        const std::uint64_t p12 =
            std::min<std::uint64_t>((kProbOneQ32 + (denominator / 2)) / denominator, kProbMax);
        table[zero + x] = static_cast<std::uint16_t>(p12);
        table[zero - x] = static_cast<std::uint16_t>(kProbOne - p12);
        decay = (decay * kDecayQ32) >> kQ32Bits;
    }
    return table;
}

inline constexpr std::array<std::uint16_t, kSquashEntries> kSquashTable = make_squash_table();

}  // namespace detail

// The p12 for a stretched value: kProbOne / (1 + e^(-st / kStretchOne)),
// rounded to nearest. Accepts any st and clamps it to +-kStretchMax first,
// so the result is always in [kProbMin, kProbMax].
[[nodiscard]] constexpr std::uint32_t squash(std::int32_t st) noexcept {
    const std::int32_t index = std::clamp(st, -kStretchMax, kStretchMax) + kStretchMax;
    return detail::kSquashTable[static_cast<std::size_t>(index)];
}

static_assert(squash(kStretchMax) == kProbMax && squash(-kStretchMax) == kProbMin,
              "squash must span exactly the range the coder accepts");

namespace detail {

// stretch for every p12, at index p12. Entry 0 is unused.
//
// For p12 >= one half it is the least x with squash(x) >= p12; squash never
// decreases, so one sweep of x serves every p12 in turn, and it stops because
// squash(kStretchMax) is kProbMax. Below one half, stretch(p) is
// -stretch(1 - p): the table is odd-symmetric by construction, so a 0 and a
// 1 are treated alike.
[[nodiscard]] constexpr std::array<std::int16_t, kProbOne> make_stretch_table() noexcept {
    std::array<std::int16_t, kProbOne> table{};
    std::int32_t x = 0;
    for (std::uint32_t p12 = kProbOne / 2; p12 <= kProbMax; ++p12) {
        while (squash(x) < p12) ++x;
        table[p12] = static_cast<std::int16_t>(x);
        table[kProbOne - p12] = static_cast<std::int16_t>(-x);
    }
    return table;
}

inline constexpr std::array<std::int16_t, kProbOne> kStretchTable = make_stretch_table();

}  // namespace detail

// The stretched value of a p12 in [kProbMin, kProbMax]: the inverse of
// squash, within its rounding. Always in [-kStretchMax, kStretchMax].
[[nodiscard]] constexpr std::int32_t stretch(std::uint32_t p12) noexcept {
    assert(p12 >= kProbMin && p12 <= kProbMax);
    return detail::kStretchTable[p12];
}

}  // namespace cpress::model
