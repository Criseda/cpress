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
// Fixed point: weights are 16.16 signed, in [-kWeightLimit, kWeightLimit].
//
// Memory: weight_sets * Inputs * 4 bytes.
//
// Technique: the mixer of Matt Mahoney's PAQ7 and later, here in the single
// layer form of lpaq1. The derivation of the update from coding cost is in
// his Data Compression Explained, section 4.3.2; training mixing weights on
// coding cost goes back to PAQ6 (Mahoney, Adaptive Weighing of Context
// Models for Lossless Data Compression, 2005).
#pragma once

#include "model/logistic.hpp"
#include "model/predictor.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
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

template <std::size_t Inputs>
class Mixer {
public:
    using Stretched = std::array<std::int32_t, Inputs>;

    // Allocates `weight_sets` weight vectors, every weight at kWeightInitial,
    // and selects set 0. Throws std::bad_alloc if there is no memory.
    explicit Mixer(std::size_t weight_sets) : weights_(weight_sets, initial_weights()) {
        assert(weight_sets > 0);
    }

    // Chooses the weight set that mix() and update() use, until the next
    // select(). Call it between an update() and the next mix(), never
    // between a mix() and its update().
    void select(std::size_t weight_set) noexcept {
        assert(weight_set < weights_.size());
        selected_ = weight_set;
    }

    // P(next bit = 1) as a p12 in [kProbMin, kProbMax], given each input's
    // stretched prediction in [-kStretchMax, kStretchMax].
    [[nodiscard]] std::uint32_t mix(const Stretched& st) noexcept {
        // 64 bits: each product is at most kWeightLimit * kStretchMax < 2^30
        // in magnitude, so three of them can already pass 2^31.
        std::int64_t dot = 0;
        const Weights& weights = weights_[selected_];
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
        Weights& weights = weights_[selected_];
        for (std::size_t i = 0; i < Inputs; ++i) {
            // 32 bits are enough: |st * error| <= kStretchMax * kProbMax
            // < 2^23, so the rounded step is at most 2^10, and a weight is at
            // most kWeightLimit = 2^19 before the step is added.
            const std::int32_t step = ((st_[i] * error) + kLearningRounding) >> kLearningShift;
            weights[i] = std::clamp(weights[i] + step, -kWeightLimit, kWeightLimit);
        }
    }

    // One set's weights. For tests.
    [[nodiscard]] std::span<const std::int32_t, Inputs> weights(
        std::size_t weight_set) const noexcept {
        assert(weight_set < weights_.size());
        return weights_[weight_set];
    }

private:
    using Weights = std::array<std::int32_t, Inputs>;

    [[nodiscard]] static constexpr Weights initial_weights() noexcept {
        Weights weights{};
        weights.fill(kWeightInitial);
        return weights;
    }

    std::vector<Weights> weights_;
    std::size_t selected_ = 0;
    Stretched st_{};                    // the inputs of the last mix()
    std::uint32_t p12_ = kProbOne / 2;  // the result of the last mix()
};

}  // namespace cpress::model
