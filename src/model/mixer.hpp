// Mixer: combines several predictions of the same bit into one, and learns
// online how far to trust each.
//
// Predictions arrive stretched (see logistic.hpp for why). The output is the
// squash of their weighted sum:
//
//   p12 = squash(sum of weight[i] * st[i])
//
// Update rule: after the bit is known, each weight moves in proportion to
// its own input and to the error of the mix:
//
//   weight[i] += st[i] * (bit - p) * learning rate
//
// An input that argued for the bit that came (st and error of the same sign)
// gains weight; one that argued against it loses weight; one that said
// nothing (st = 0) is left alone. This is one step of gradient descent on
// the cost of coding the bit, -ln of the probability given to it: the
// derivative of that cost with respect to weight[i] is -(bit - p) * st[i].
//
// Weight sets. A mixer holds several weight vectors and uses the one the
// caller selects, so which inputs deserve trust can depend on where in the
// data the coder is. Only the selected set is read or trained.
//
// A decaying learning rate, for codec 7. A set that has seen few bits
// should learn fast and one that has seen many should settle; with one set
// per byte that hardly matters, since each sees millions of updates, but
// codec 7 has 262,144, chosen per bit. Each set then keeps its own rate,
// which starts at kRateInitial sixteenths of the base rate and falls by a
// sixteenth with every update until it reaches the base rate, after 202
// updates. From there it learns exactly as a mixer without the decay.
//
// Fixed point: weights are 16.16 signed, in [-kWeightLimit, kWeightLimit].
//
// Memory: weight_sets * Inputs * 4 bytes; with a decaying rate,
// weight_sets * 64 bytes for up to 15 inputs, one set and its rate to a
// cache line.
//
// Technique: the mixer of Matt Mahoney's PAQ7 and later, here in the single
// layer form of lpaq1. The derivation of the update from coding cost is in
// his Data Compression Explained, section 4.3.2; training mixing weights on
// coding cost goes back to PAQ6 (Mahoney, Adaptive Weighing of Context
// Models for Lossless Data Compression, 2005).
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
#include <type_traits>
#include <vector>

namespace cpress::model {

inline constexpr int kWeightBits = 16;
inline constexpr std::int32_t kWeightOne = static_cast<std::int32_t>(1U << kWeightBits);

// Every weight starts here: six inputs at a quarter each already make a
// usable mix, before anything is learned.
inline constexpr std::int32_t kWeightInitial = kWeightOne / 4;

// Weights are clamped to +-8.0. Compression does not need it: on enwik8 no
// weight passes 1.6. It exists for the decoder. A crafted archive can feed
// the mixer any bit sequence, and without a bound a weight could be walked
// up until the arithmetic overflowed, which for signed integers is
// undefined behaviour.
inline constexpr std::int32_t kWeightLimit = 8 * kWeightOne;

// The learning rate: a weight moves by st * error / 2^kLearningShift, rounded
// to nearest.
inline constexpr int kLearningShift = 13;
inline constexpr std::int32_t kLearningRounding =
    static_cast<std::int32_t>(1U << (kLearningShift - 1));

// A decaying rate is in sixteenths of that base rate: it starts at
// kRateInitial, 13.6 times the base, and falls to kRateFinal, the base.
// Chosen in the prototype (docs/stages/stage-9.md, section 2).
inline constexpr int kRateBits = 4;
inline constexpr std::uint16_t kRateFinal = 1U << kRateBits;
inline constexpr std::uint16_t kRateInitial = 218;

namespace detail {

// A weight set, alone; or with its rate, one to a cache line, so that
// reading a set is one line and prefetching it fetches all of it.
template <std::size_t Inputs>
struct PlainRow {
    std::array<std::int32_t, Inputs> weights;
};
static_assert(sizeof(PlainRow<10>) == 40, "a plain mixer's layout is codecs 2 to 6's");

template <std::size_t Inputs>
struct alignas(kCacheLineBytes) DecayingRow {
    static constexpr std::size_t kUsedBytes =
        (Inputs * sizeof(std::int32_t)) + sizeof(std::uint16_t);
    static_assert(kUsedBytes <= kCacheLineBytes, "a set and its rate fit a cache line");

    std::array<std::int32_t, Inputs> weights;
    std::uint16_t rate;
    // The rest of the line, as a member so that no compiler pads silently.
    std::array<std::uint8_t, kCacheLineBytes - kUsedBytes> unused;
};

}  // namespace detail

template <std::size_t Inputs, bool DecayingRate = false>
class Mixer {
public:
    using Stretched = std::array<std::int32_t, Inputs>;

    // Allocates `weight_sets` weight vectors, every weight at kWeightInitial,
    // and selects set 0. Throws std::bad_alloc if there is no memory.
    explicit Mixer(std::size_t weight_sets) : rows_(weight_sets, initial_row()) {
        assert(weight_sets > 0);
    }

    // Bytes a mixer of `weight_sets` sets allocates.
    [[nodiscard]] static constexpr std::uint64_t memory_bytes(std::size_t weight_sets) noexcept {
        return std::uint64_t{weight_sets} * sizeof(Row);
    }

    // Chooses the weight set that mix() and update() use, until the next
    // select(). Call it between an update() and the next mix(), never
    // between a mix() and its update().
    void select(std::size_t weight_set) noexcept {
        assert(weight_set < rows_.size());
        selected_ = weight_set;
    }

    // A hint: fetches a weight set that a select() is about to choose.
    // Changes nothing.
    void prefetch(std::size_t weight_set) const noexcept {
        assert(weight_set < rows_.size());
        model::prefetch(&rows_[weight_set]);
    }

    // P(next bit = 1) as a p12 in [kProbMin, kProbMax], given each input's
    // stretched prediction in [-kStretchMax, kStretchMax].
    [[nodiscard]] std::uint32_t mix(const Stretched& st) noexcept {
        // 64 bits: each product is at most kWeightLimit * kStretchMax < 2^30
        // in magnitude, so three of them can already pass 2^31.
        std::int64_t dot = 0;
        const Weights& weights = rows_[selected_].weights;
        for (std::size_t i = 0; i < Inputs; ++i) {
            assert(st[i] >= -kStretchMax && st[i] <= kStretchMax);
            dot += std::int64_t{weights[i]} * st[i];
        }
        const std::int64_t mixed =
            std::clamp<std::int64_t>(dot >> kWeightBits, -kStretchMax, kStretchMax);
        st_ = st;
        p12_ = squash(static_cast<std::int32_t>(mixed));
        return p12_;
    }

    // Learns from the bit that the last mix() predicted.
    void update(int bit) noexcept {
        assert(bit == 0 || bit == 1);
        // In [-kProbMax, kProbMax]: p12_ is in [kProbMin, kProbMax].
        const std::int32_t error =
            static_cast<std::int32_t>(bit != 0 ? kProbOne : 0U) - static_cast<std::int32_t>(p12_);
        Row& row = rows_[selected_];
        if constexpr (DecayingRate) {
            learn_decaying(row, error);
        } else {
            for (std::size_t i = 0; i < Inputs; ++i) {
                // 32 bits are enough: |st * error| <= kStretchMax * kProbMax
                // < 2^23, so the rounded step is at most 2^10, and a weight
                // is at most kWeightLimit = 2^19 before the step is added.
                const std::int32_t step = ((st_[i] * error) + kLearningRounding) >> kLearningShift;
                row.weights[i] = std::clamp(row.weights[i] + step, -kWeightLimit, kWeightLimit);
            }
        }
    }

    // A set's current rate, in sixteenths of the base. For tests.
    [[nodiscard]] std::uint16_t rate(std::size_t weight_set) const noexcept
        requires DecayingRate
    {
        assert(weight_set < rows_.size());
        return rows_[weight_set].rate;
    }

    // One set's weights. For tests.
    [[nodiscard]] std::span<const std::int32_t, Inputs> weights(
        std::size_t weight_set) const noexcept {
        assert(weight_set < rows_.size());
        return rows_[weight_set].weights;
    }

private:
    using Weights = std::array<std::int32_t, Inputs>;

    using Row =
        std::conditional_t<DecayingRate, detail::DecayingRow<Inputs>, detail::PlainRow<Inputs>>;

    [[nodiscard]] static constexpr Row initial_row() noexcept {
        Row row{};
        row.weights.fill(kWeightInitial);
        if constexpr (DecayingRate) row.rate = kRateInitial;
        return row;
    }

    // update() at the set's own rate, which then decays.
    void learn_decaying(Row& row, std::int32_t error) noexcept {
        const std::int64_t rate = row.rate;
        if (row.rate > kRateFinal) --row.rate;
        constexpr int kShift = kLearningShift + kRateBits;
        constexpr std::int64_t kRounding = std::int64_t{1} << (kShift - 1);
        for (std::size_t i = 0; i < Inputs; ++i) {
            // |st * error * rate| < 2^23 * 2^8 = 2^31, in 64 bits with room;
            // at kRateFinal the step is exactly the plain one, since
            // (x * 16 + 2^16) >> 17 == (x + 2^12) >> 13.
            const std::int64_t product = std::int64_t{st_[i]} * error * rate;
            const auto step = static_cast<std::int32_t>((product + kRounding) >> kShift);
            row.weights[i] = std::clamp(row.weights[i] + step, -kWeightLimit, kWeightLimit);
        }
    }

    std::vector<Row> rows_;
    std::size_t selected_ = 0;
    Stretched st_{};                    // the inputs of the last mix()
    std::uint32_t p12_ = kProbOne / 2;  // the result of the last mix()
};

}  // namespace cpress::model
