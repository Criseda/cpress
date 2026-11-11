// ProbabilityMap: a table of adaptive probabilities indexed by a small
// context. It predicts a bit from the bits seen before in the same context.
//
// Stage 3 uses it twice. Beside each history table it is a state map: the
// context is a bit-history state, and the map learns what that state
// predicts in that table. In the match model the context is the match
// length and the bit the match expects.
//
// Entry layout:
//   bits 31..10   p22: P(1) out of kP22One (2^22)
//   bits  9..0    n:   updates seen, saturating at kMapCountLimit
//
// Update rule: p22 moves towards the bit just seen by 1 / (n + 1.5) of the
// distance, the Counter's rule (counter.hpp) with room to keep learning: 22
// bits of probability where the counter has 13, and a count to 1023 where
// it has 7. A state map entry is shared by every context of its table that
// is in that state, so it sees millions of updates and can afford a slow
// final rate.
//
// Memory: 4 bytes per context.
//
// Technique: a 22-bit probability and a 10-bit count in 32 bits, with
// rate 1 / (n + 1.5) and a limit of 1023, is the StateMap of Matt Mahoney's
// lpaq1 (2007), where it also maps bit-history states to probabilities.
#pragma once

#include "model/predictor.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cpress::model {

inline constexpr int kP22Bits = 22;
inline constexpr std::uint32_t kP22One = 1U << kP22Bits;  // probability 1.0, never stored
inline constexpr int kMapCountBits = 10;
inline constexpr std::uint32_t kMapCountLimit = 1023;
static_assert(kP22Bits + kMapCountBits == 32, "an entry is exactly 32 bits");
static_assert(kMapCountLimit < (1U << kMapCountBits), "the count must fit in its bits");

namespace detail {

inline constexpr int kMapRateBits = 16;

// kMapRate16[n] is 1 / (n + 1.5) in 16 bits: 2^17 / (2n + 3). From 43,690
// at n = 0 down to 63 at kMapCountLimit; every rate is below 2^16.
[[nodiscard]] constexpr std::array<std::uint32_t, kMapCountLimit + 1> make_map_rates() noexcept {
    std::array<std::uint32_t, kMapCountLimit + 1> rates{};
    for (std::uint32_t n = 0; n <= kMapCountLimit; ++n) {
        rates[n] = (2U << kMapRateBits) / ((2 * n) + 3);
    }
    return rates;
}

inline constexpr std::array<std::uint32_t, kMapCountLimit + 1> kMapRate16 = make_map_rates();
static_assert(kMapRate16[0] < (1U << kMapRateBits), "a step never covers the whole distance");

}  // namespace detail

class ProbabilityMap {
public:
    [[nodiscard]] static constexpr std::uint64_t memory_bytes(std::size_t contexts) noexcept {
        return contexts * sizeof(std::uint32_t);
    }

    // Allocates one entry per value of `initial_p22`, each starting at that
    // p22, in (0, kP22One), with count 0. Throws std::bad_alloc if there is
    // no memory.
    explicit ProbabilityMap(std::span<const std::uint32_t> initial_p22)
        : entries_(initial_p22.begin(), initial_p22.end()) {
        assert(!entries_.empty());
        for (std::uint32_t& entry : entries_) {
            assert(entry > 0 && entry < kP22One);
            entry <<= kMapCountBits;
        }
    }

    // P(next bit = 1) in `context`, as a p12 in [kProbMin, kProbMax].
    // Remembers the context for update().
    //
    // The clamp is needed. Each update keeps (2n + 1) / (2n + 3) of the
    // distance to the bit, so k zeros from a fresh entry divide p22 by
    // about 2k + 1, and p22 can come to rest below 2^10, which would shift
    // to a p12 of 0. From one half, kMapCountLimit zeros reach about 1,024.
    // Once the count is saturated the rate is 63 / 65536, and an entry
    // below 1,041 no longer moves down at all.
    [[nodiscard]] std::uint32_t p12(std::size_t context) noexcept {
        assert(context < entries_.size());
        context_ = context;
        const std::uint32_t p12 = p22(context) >> (kP22Bits - kProbBits);
        return std::clamp(p12, kProbMin, kProbMax);
    }

    // Learns `bit` at the context of the last p12().
    void update(int bit) noexcept {
        assert(bit == 0 || bit == 1);
        // 64 bits: the products are below kP22One * 2^kMapRateBits = 2^38.
        // A step is less than the distance it covers, because every rate is
        // below 2^16, so p22 stays in (0, kP22One).
        const std::uint32_t n = count(context_);
        const std::uint64_t rate16 = detail::kMapRate16[n];
        std::uint64_t p22 = this->p22(context_);
        if (bit != 0) {
            p22 += ((kP22One - p22) * rate16) >> detail::kMapRateBits;
        } else {
            p22 -= (p22 * rate16) >> detail::kMapRateBits;
        }
        assert(p22 > 0 && p22 < kP22One);
        const std::uint32_t next_n = std::min(n + 1, kMapCountLimit);
        entries_[context_] = static_cast<std::uint32_t>((p22 << kMapCountBits) | next_n);
    }

    // The two fields of an entry. For tests.
    [[nodiscard]] std::uint32_t p22(std::size_t context) const noexcept {
        assert(context < entries_.size());
        return entries_[context] >> kMapCountBits;
    }
    [[nodiscard]] std::uint32_t count(std::size_t context) const noexcept {
        assert(context < entries_.size());
        return entries_[context] & kCountMask;
    }

private:
    static constexpr std::uint32_t kCountMask = (1U << kMapCountBits) - 1;

    std::vector<std::uint32_t> entries_;
    std::size_t context_ = 0;  // the context of the last p12()
};

}  // namespace cpress::model
