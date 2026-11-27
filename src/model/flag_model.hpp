// FlagModel: predicts the predicted-byte flag of codec 6, "is the next byte
// the byte the match model guesses?", so that a byte the model is sure of
// can be coded as one decision instead of eight.
//
// A guess right 82% of the time is not worth a decision of its own: the
// full model codes those bytes for about a quarter of a bit each, and the
// flag would cost more (docs/stages/stage-8.md, section 2). What makes the
// flag cheap is knowing where the guess has been right before, so it is
// predicted from four inputs, mixed:
//
//   outcomes   a probability for each match length class and the last four
//              outcomes of the flag
//   order 3    the bit history of "was the guess right?" for the last three
//              bytes and this guess
//   order 5    the same for the last five bytes and this guess
//   bias       a constant
//
// The two histories are bit-history states (state_table.hpp), read through
// a state map each. They are kept in direct-mapped tables of one byte per
// entry with no check: a context that lands on another's entry shares its
// history, which costs a little compression and never correctness. The
// mixer's weight set is the match's length class.
//
// Memory: two tables of 2^table_bits bytes, through TableAllocator, and
// under 8 KiB of maps and weights.
//
// Update rule: every input learns each outcome, the two entries move on by
// it, and the mixer learns from its own prediction, as in Mix9. Codec 7's
// flag model, BasicFlagModel<true>, has a mixer whose rate decays, as its
// main mixer's does (mixer.hpp): the prototype's switch for the rate
// changed every mixer, and codec 7 makes the prototype's bytes.
//
// Technique: coding the predicted byte as a flag before the byte is LZP
// (Charles Bloom, "LZP: A New Data Compression Algorithm", Data Compression
// Conference, 1996); mcm 0.83 codes such a flag ahead of a context-mixing
// model. Predicting the flag from tables of bit histories keyed by the
// context and the guess, mixed, is this project's own.
#pragma once

#include "model/context_table.hpp"
#include "model/history_table.hpp"
#include "model/logistic.hpp"
#include "model/match_model.hpp"
#include "model/mixer.hpp"
#include "model/prefetch.hpp"
#include "model/probability_map.hpp"
#include "model/state_table.hpp"
#include "model/table_allocator.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace cpress::model {

template <bool DecayingRate = false>
class BasicFlagModel {
public:
    // The limits on log2 of each table's entries.
    static constexpr int kMinTableBits = 1;
    static constexpr int kMaxTableBits = 32;

    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int table_bits) noexcept {
        return (2 * (std::uint64_t{1} << table_bits)) +
               ProbabilityMap::memory_bytes(kOutcomeContexts) +
               (2 * ProbabilityMap::memory_bytes(kStateCount)) +
               FlagMixer::memory_bytes(kWeightSets);
    }

    // Allocates two tables of 2^table_bits entries, every history empty.
    // Throws std::bad_alloc if there is no memory.
    explicit BasicFlagModel(int table_bits)
        : near_(std::size_t{1} << table_bits),
          far_(std::size_t{1} << table_bits),
          table_shift_(64 - table_bits) {
        assert(table_bits >= kMinTableBits && table_bits <= kMaxTableBits);
    }

    // P(the next byte is `guess`) as a p12, given `history`, the bytes
    // before it, newest in the low byte, and `length`, how many bytes the
    // match that guesses has agreed. Remembers what update() needs.
    [[nodiscard]] std::uint32_t predict(std::uint64_t history, std::uint32_t guess,
                                        std::uint32_t length) noexcept {
        assert(guess <= 0xFF);
        const std::uint32_t length_class = detail::length_class(length);
        near_entry_ = entry(history, guess, kNearOrder, kNearKey);
        far_entry_ = entry(history, guess, kFarOrder, kFarKey);
        typename FlagMixer::Stretched st{};
        st[0] = stretch(by_outcomes_.p12((length_class * kOutcomeHistories) + recent_outcomes()));
        st[1] = stretch(near_map_.p12(near_[near_entry_]));
        st[2] = stretch(far_map_.p12(far_[far_entry_]));
        st[3] = kBias;
        mixer_.select(length_class);
        return mixer_.mix(st);
    }

    // Learns whether the byte was the guess: 1 if it was.
    void update(int hit) noexcept {
        assert(hit == 0 || hit == 1);
        near_map_.update(hit);
        far_map_.update(hit);
        mixer_.update(hit);
        near_[near_entry_] = next_state(near_[near_entry_], hit);
        far_[far_entry_] = next_state(far_[far_entry_], hit);
        by_outcomes_.update(hit);
        outcomes_ = (outcomes_ * 2) + static_cast<std::uint32_t>(hit);
    }

    // Starts loading the two entries a predict(history, guess, ...) will
    // read. A hint: it changes nothing in the model.
    void prefetch(std::uint64_t history, std::uint32_t guess) const noexcept {
        model::prefetch(&near_[entry(history, guess, kNearOrder, kNearKey)]);
        model::prefetch(&far_[entry(history, guess, kFarOrder, kFarKey)]);
    }

private:
    // The two contexts, in bytes.
    static constexpr int kNearOrder = 3;
    static constexpr int kFarOrder = 5;
    static_assert(kFarOrder < 8, "a context fits in a 64-bit history");

    // Odd 64-bit constants that make each table's key from its context and
    // the guess. Any would do; these are the ones measured.
    static constexpr std::uint64_t kNearKey = 0x2545F4914F6CDD1D;
    static constexpr std::uint64_t kFarKey = 0x9E3779B97F4A7C15;
    static constexpr std::uint64_t kGuessKey = 0xD6E8FEB86659FD93;

    // The outcomes input: a probability per length class and per value of
    // the last four outcomes.
    static constexpr std::uint32_t kOutcomeBits = 4;
    static constexpr std::uint32_t kOutcomeHistories = 1U << kOutcomeBits;
    static constexpr std::size_t kOutcomeContexts =
        std::size_t{MatchModel::kLengthClasses} * kOutcomeHistories;

    // The mixer: one weight set per length class. The bias input is a
    // constant 1 in stretched units, so its weight is learned as a bias.
    static constexpr std::size_t kInputs = 4;
    static constexpr std::size_t kWeightSets = MatchModel::kLengthClasses;
    static constexpr std::int32_t kBias = kStretchOne;
    using FlagMixer = Mixer<kInputs, DecayingRate>;

    // Every outcomes entry starts at one half.
    static constexpr std::array<std::uint32_t, kOutcomeContexts> kHalves = [] {
        std::array<std::uint32_t, kOutcomeContexts> p22{};
        p22.fill(kP22One / 2);
        return p22;
    }();

    // The entry of the table whose context is the last `order` bytes of
    // `history`, keyed by `key`, for `guess`. The 64-bit products wrap;
    // that is the hash, as in context_table.hpp.
    [[nodiscard]] std::size_t entry(std::uint64_t history, std::uint32_t guess, int order,
                                    std::uint64_t key) const noexcept {
        const std::uint64_t context = history & ((std::uint64_t{1} << (8 * order)) - 1);
        const std::uint64_t hash = ((context + 1) * key) ^ ((std::uint64_t{guess} + 1) * kGuessKey);
        return static_cast<std::size_t>((hash * kHashMultiplier) >> table_shift_);
    }

    [[nodiscard]] std::uint32_t recent_outcomes() const noexcept {
        return outcomes_ & (kOutcomeHistories - 1);
    }

    TableArray<std::uint8_t> near_;  // order 3 and guess -> bit-history state
    TableArray<std::uint8_t> far_;   // order 5 and guess -> bit-history state
    int table_shift_;                // 64 - table_bits
    ProbabilityMap near_map_ = make_state_map();
    ProbabilityMap far_map_ = make_state_map();
    ProbabilityMap by_outcomes_{kHalves};  // length class and last outcomes -> P(hit)
    FlagMixer mixer_{kWeightSets};
    std::uint32_t outcomes_ = 0;  // every outcome so far, the newest in bit 0
    std::size_t near_entry_ = 0;  // the entries of the last predict()
    std::size_t far_entry_ = 0;
};

// Codec 6's flag model.
using FlagModel = BasicFlagModel<>;

}  // namespace cpress::model
