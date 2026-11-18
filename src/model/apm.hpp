// Apm, an adaptive probability map: refines a probability given a small
// context. It predicts the bit again, from the mixer's prediction and where
// in the data the coder is.
//
// The mixer's output is right on average but biased in particular places:
// it may be too sure in the middle of one byte and not sure enough at the
// start of another. An APM learns the correction. For each context it holds
// a curve from the input probability to the output probability, as 33
// points spaced evenly in the stretched domain, 128 apart (kApmPoints,
// kApmPointSpacing):
//
//   point j        p16 at st = (j - 16) * 128, for j = 0 to 32
//   initial        squash(st) * 16, so a fresh curve maps a probability to
//                  itself, within the error of interpolating between points
//   refine         interpolate linearly between the two points around the
//                  stretched input, and remember the nearer one
//   update         the nearer point moves 1 / 2^kApmRate of the way to the bit
//
// Fixed point: a point is a p16, P(1) out of 65536, in [0, 65535].
//
// Memory: contexts * kPoints * 2 bytes, through TableAllocator: a model's
// second APM is 4 MiB, read at a place that changes with every byte.
//
// Technique: secondary symbol estimation (Mahoney, Data Compression
// Explained, section 4.3.3). Thirty-three points at these places, these
// initial values and a rate of 7 are the APM of Matt Mahoney's paq8l
// (2007), which updates both points around the input. His lpaq1 (2007)
// updates only the nearer one, as this does, but has 24 points.
#pragma once

#include "model/logistic.hpp"
#include "model/predictor.hpp"
#include "model/prefetch.hpp"
#include "model/table_allocator.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cpress::model {

// A point moves 1 / 2^kApmRate of the way to each bit it learns.
inline constexpr int kApmRate = 7;

namespace detail {

inline constexpr std::size_t kApmPoints = 33;
inline constexpr int kApmPointShift = 7;
inline constexpr std::int32_t kApmPointSpacing = 1 << kApmPointShift;  // in units of st

// The point at st = 0, and the offset that makes st + kApmOffset at least 0.
inline constexpr std::int32_t kApmCentre = static_cast<std::int32_t>(kApmPoints / 2);
inline constexpr std::int32_t kApmOffset = kApmCentre * kApmPointSpacing;

inline constexpr std::int32_t kP16Max = 0xFFFF;
inline constexpr int kP16ToP12 = 16 - kProbBits;

// Every stretched value lands between two points of the curve: st +
// kApmOffset is at least 0, and below the last point, so the point above it
// is at most kApmPoints - 1.
static_assert(kApmOffset - kStretchMax >= 0);
static_assert(kApmOffset + kStretchMax <
              static_cast<std::int32_t>(kApmPoints - 1) * kApmPointSpacing);

// squash(st) * 16 at each point's st. squash clamps the end points,
// st = +-2048, to +-kStretchMax, so the largest is 4095 * 16 = 65,520.
[[nodiscard]] constexpr std::array<std::uint16_t, kApmPoints> make_apm_curve() noexcept {
    std::array<std::uint16_t, kApmPoints> curve{};
    for (std::size_t j = 0; j < kApmPoints; ++j) {
        const std::int32_t st = (static_cast<std::int32_t>(j) - kApmCentre) * kApmPointSpacing;
        curve[j] = static_cast<std::uint16_t>(squash(st) << kP16ToP12);
    }
    return curve;
}

inline constexpr std::array<std::uint16_t, kApmPoints> kInitialApmCurve = make_apm_curve();

}  // namespace detail

class Apm {
public:
    static constexpr std::size_t kPoints = detail::kApmPoints;

    [[nodiscard]] static constexpr std::uint64_t memory_bytes(std::size_t contexts) noexcept {
        return contexts * kPoints * sizeof(std::uint16_t);
    }

    // Allocates `contexts` curves, each the identity. Throws std::bad_alloc
    // if there is no memory.
    //
    // Every curve starts the same, so the first curve is written, and the
    // filled part of the table is then doubled until it is full: a handful
    // of block copies rather than one per context. A model holds 65,536
    // curves and every decode builds one. Copied a curve at a time, they
    // took 45 ms to build under ASan, most of the mutation test's time; this
    // takes 5. Whatever `contexts` is, nothing is written past the table:
    // the doubling copies only what the table already holds.
    explicit Apm(std::size_t contexts) : points_(contexts * kPoints) {
        assert(contexts > 0);
        const std::span<std::uint16_t> points(points_);
        std::ranges::copy(detail::kInitialApmCurve, points.begin());
        for (std::size_t filled = kPoints; filled < points.size(); filled *= 2) {
            const std::size_t count = std::min(filled, points.size() - filled);
            std::ranges::copy(points.first(count), points.subspan(filled).begin());
        }
    }

    // A probability refined in `context`, as a p12 in [kProbMin, kProbMax].
    // The probability comes stretched, `st` in [-kStretchMax, kStretchMax],
    // so that a caller refining one prediction in several maps stretches it
    // once. Remembers the nearer point for update().
    [[nodiscard]] std::uint32_t refine(std::int32_t st, std::size_t context) noexcept {
        assert(context < points_.size() / kPoints);
        assert(st >= -kStretchMax && st <= kStretchMax);
        constexpr auto kSpacing = static_cast<std::uint32_t>(detail::kApmPointSpacing);
        // Between two points of the curve; see the static asserts above.
        const auto x = static_cast<std::uint32_t>(st + detail::kApmOffset);
        const std::uint32_t weight = x & (kSpacing - 1);  // towards `above`
        const std::size_t below = (context * kPoints) + (x >> detail::kApmPointShift);
        const std::size_t above = below + 1;
        // At most 65535 * kSpacing < 2^23 before the shift.
        const std::uint32_t p16 =
            ((points_[below] * (kSpacing - weight)) + (points_[above] * weight)) >>
            detail::kApmPointShift;
        nearest_ = weight < kSpacing / 2 ? below : above;
        return std::clamp(p16 >> detail::kP16ToP12, kProbMin, kProbMax);
    }

    // Learns `bit` at the point the last refine() found nearest.
    //
    // The step is a signed difference shifted right, which rounds towards
    // minus infinity. Towards a 1 the difference d = 65535 - p is at least
    // 0 and the step d >> kApmRate at most d, so p stays at most 65535.
    // Towards a 0 the difference is -p and the step floor(-p / 2^kApmRate)
    // is at least -p, so p stays at least 0.
    void update(int bit) noexcept {
        assert(bit == 0 || bit == 1);
        const std::int32_t target = bit != 0 ? detail::kP16Max : 0;
        const std::int32_t p16 = points_[nearest_];
        const std::int32_t next = p16 + ((target - p16) >> kApmRate);
        assert(next >= 0 && next <= detail::kP16Max);
        points_[nearest_] = static_cast<std::uint16_t>(next);
    }

    // Starts loading the curve of `context` into the cache, for a refine()
    // to come. A hint: it changes nothing in the map. Which two points
    // refine() reads depends on a probability not yet known, so the whole
    // curve is asked for: 66 bytes from an even address, so its first point
    // and its last are on the only two cache lines it can touch.
    void prefetch(std::size_t context) const noexcept {
        assert(context < points_.size() / kPoints);
        const std::size_t first = context * kPoints;
        model::prefetch(&points_[first]);
        model::prefetch(&points_[first + kPoints - 1]);
    }

    // A point of a context's curve. For tests.
    [[nodiscard]] std::uint32_t p16(std::size_t context, std::size_t point) const noexcept {
        assert(point < kPoints && context < points_.size() / kPoints);
        return points_[(context * kPoints) + point];
    }

private:
    TableArray<std::uint16_t> points_;  // context * kPoints + point
    std::size_t nearest_ = 0;           // the point the last refine() found nearest
};

}  // namespace cpress::model
