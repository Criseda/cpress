// Counter: one adaptive probability in 16 bits, the cell every context table
// is made of. It predicts a bit from the bits seen before in the same place.
//
// Layout:
//   bits 15..3   p13: P(1) out of kP13One (8192)
//   bits  2..0   n:   how many updates it has had, saturating at kCountLimit
//
// Update rule: the probability moves towards the bit just seen by
// 1 / (n + 1.5) of the distance. A new counter has no history worth keeping,
// so its first update covers two thirds of the way; by the time n reaches
// kCountLimit it moves by less than an eighth and holds an estimate steady
// against noise. A fixed rate has to choose between the two.
//
// A counter that has never been updated predicts exactly one half, which
// stretches to 0: a context with no history adds nothing to the mix.
//
// Memory: 2 bytes.
//
// Technique: a probability with a count, adjusted by error / (n + 1.5), is
// the StateMap of Matt Mahoney's lpaq1 (its rate table is 16384 / (2n + 3))
// and the CM component of ZPAQ (specification, section 3.2). Both keep 22
// bits of probability and count to 1023; this one is cut down to 16 bits so
// that sixteen fit in half a cache line.
#pragma once

#include "model/predictor.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

namespace cpress::model {

inline constexpr int kP13Bits = 13;
inline constexpr std::uint32_t kP13One = 1U << kP13Bits;  // probability 1.0, never stored
inline constexpr int kCountBits = 3;
inline constexpr std::uint32_t kCountLimit = 7;
static_assert(kCountLimit < (1U << kCountBits), "the count must fit in its bits");

namespace detail {

inline constexpr int kRateBits = 16;

// kCounterRate16[n] is 1 / (n + 1.5) in 16 bits: 2^16 / (n + 1.5), which is
// 2^17 / (2n + 3) in integers.
[[nodiscard]] constexpr std::array<std::uint32_t, kCountLimit + 1> make_counter_rates() noexcept {
    std::array<std::uint32_t, kCountLimit + 1> rates{};
    for (std::uint32_t n = 0; n <= kCountLimit; ++n) {
        rates[n] = (2U << kRateBits) / ((2 * n) + 3);
    }
    return rates;
}

inline constexpr std::array<std::uint32_t, kCountLimit + 1> kCounterRate16 = make_counter_rates();

}  // namespace detail

class Counter {
public:
    // P(next bit = 1) as a p12. Always in [kProbMin, kProbMax], with no
    // clamp: see kP13Min and kP13Max below.
    [[nodiscard]] constexpr std::uint32_t p12() const noexcept {
        return p13() >> (kP13Bits - kProbBits);
    }

    // Learns `bit`: moves p13 towards it and counts the update.
    constexpr void update(int bit) noexcept {
        assert(bit == 0 || bit == 1);
        // Arithmetic in std::uint32_t; the store back is a visible cast. The
        // products are below kP13One * 2^kRateBits = 2^29. The two branches
        // keep every intermediate value unsigned, as Order0 does.
        const std::uint32_t n = count();
        const std::uint32_t rate16 = detail::kCounterRate16[n];
        std::uint32_t p13 = this->p13();
        if (bit != 0) {
            p13 += ((kP13One - p13) * rate16) >> detail::kRateBits;
        } else {
            p13 -= (p13 * rate16) >> detail::kRateBits;
        }
        assert(p13 > 0 && p13 < kP13One);
        const std::uint32_t next_n = std::min(n + 1, kCountLimit);
        packed_ = static_cast<std::uint16_t>((p13 << kCountBits) | next_n);
    }

    // The two fields. For tests, and for the proof of the bounds below.
    [[nodiscard]] constexpr std::uint32_t p13() const noexcept {
        return std::uint32_t{packed_} >> kCountBits;
    }
    [[nodiscard]] constexpr std::uint32_t count() const noexcept {
        return packed_ & kCountMask;
    }

private:
    static constexpr std::uint32_t kCountMask = (1U << kCountBits) - 1;

    // One half, never updated.
    std::uint16_t packed_ = static_cast<std::uint16_t>((kP13One / 2) << kCountBits);
};

static_assert(sizeof(Counter) == sizeof(std::uint16_t));

namespace detail {

// Where p13 comes to rest in a counter fed `bit` and nothing else.
[[nodiscard]] constexpr std::uint32_t resting_p13(int bit) noexcept {
    Counter counter;
    std::uint32_t before = 0;
    do {
        before = counter.p13();
        counter.update(bit);
    } while (counter.p13() != before || counter.count() != kCountLimit);
    return counter.p13();
}

}  // namespace detail

// Bounds on p13, for any sequence of bits: [8, 8184] for a kCountLimit of 7.
//
// Proof. Each branch of update() is non-decreasing in p13 (raising p13 by
// one changes the step by at most one, because every rate is below 1.0), and
// the rate depends only on how many updates a counter has had. So after k
// updates no sequence leaves p13 above where k ones leave it, or below where
// k zeros do; and those two sequences only ever move one way, so they are
// bounded by where they come to rest.
inline constexpr std::uint32_t kP13Max = detail::resting_p13(1);
inline constexpr std::uint32_t kP13Min = detail::resting_p13(0);

// So a prediction, p13 with its low bit dropped, stays inside the coder's
// range without a clamp, and a count limit for which it would not fails to
// compile.
static_assert((kP13Min >> (kP13Bits - kProbBits)) >= kProbMin, "a counter can predict 0");
static_assert((kP13Max >> (kP13Bits - kProbBits)) <= kProbMax, "a counter can predict 1");

}  // namespace cpress::model
