// Mix9: the stage 3 predictor. It predicts each bit nine times, mixes the
// nine predictions, and corrects the mix.
//
//   orders 1 to 6   the bit history of the last 1 to 6 bytes as a context
//                   (HistoryTable), read through a state map
//   word            the history of the current word so far
//   bigram          the same, with the word before it
//   match           the byte that followed the last occurrence of the last
//                   seven bytes (MatchModel)
//   mixer           how far to trust each, given the previous byte and how
//                   long the match has run
//   two APMs        the mixer's output refined by the partial byte, and by
//                   the partial byte with the previous byte
//
// Why histories rather than probabilities. A stage 2 counter says what the
// bits in its context averaged to. It cannot say how much evidence the
// average rests on, and it turns evidence into a probability by the same
// rule in every table. A state says how many of each bit were seen,
// recently, and a state map learns, in each table separately, what that
// much evidence is worth there: three 1s and no 0s mean more after six
// bytes of context than after one. The slots are checked too, so a context
// that lands on another's slot notices (history_table.hpp).
//
// Why words. A byte context of fixed length straddles words badly: after
// "the " order 4 sees the end of one word and a space. The word contexts
// see the word being spelled, and the word before it, whatever their
// lengths.
//
// Memory: set by the level, kMinLevel to kMaxLevel; see memory_bytes().
// Each history table has 2^level slots of 16 bytes, except orders 1 and 2,
// which have too few contexts to use more than 2^14 and 2^22. The match
// model's buffer is as large as one history table. The two APMs, the mixer and the maps add
// a fixed 4.2 MiB, most of it the second APM.
//
// Update rule: every map, table, the match model, the mixer's selected
// weights and the APMs' nearer points learn each bit; see each component.
//
// Technique: this is the lpaq class of compressor, after Matt Mahoney's
// lpaq1 (2007): bit-history states in checked nibble slots, read through
// state maps, a match model and a word context, mixed and refined by
// APMs. The component files say what each takes from it.
#pragma once

#include "model/apm.hpp"
#include "model/context_table.hpp"
#include "model/history_table.hpp"
#include "model/logistic.hpp"
#include "model/match_model.hpp"
#include "model/mixer.hpp"
#include "model/predictor.hpp"
#include "model/probability_map.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace cpress::model {

// Word hashing. A letter is A to Z, a to z, or any byte of 128 or more, so
// the bytes of a UTF-8 sequence are letters too. On each byte:
//
//   letter     word = (word + lowercase(byte) + 1) * kWordMultiplier
//   otherwise  if word != 0: previous = word; word = 0
//
// The multipliers are odd 64-bit constants; any would do, and these are
// the ones the design was measured with. All arithmetic wraps.
namespace words {

inline constexpr std::uint64_t kWordMultiplier = 0x100000001B3;  // the 64-bit FNV prime
inline constexpr std::uint64_t kWordKey1 = 0x2545F4914F6CDD1D;   // the word table's key
inline constexpr std::uint64_t kWordKey2 = 0xD6E8FEB86659FD93;   // the bigram table's key

[[nodiscard]] constexpr bool is_letter(std::uint32_t byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || byte >= 0x80;
}

[[nodiscard]] constexpr std::uint32_t lowercase(std::uint32_t byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') ? byte + ('a' - 'A') : byte;
}

// Folds the high bits of a product into its low bits, so that the top
// kMarkerBits, which the marker shift drops, still count.
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t hash) noexcept {
    return hash ^ (hash >> 29);
}

// A word key, before the marker: the word so far (0 between words).
[[nodiscard]] constexpr std::uint64_t word_key(std::uint64_t word) noexcept {
    return mix((word * kWordKey1) + 1);
}

// A bigram key, before the marker: the word so far and the word before it.
[[nodiscard]] constexpr std::uint64_t bigram_key(std::uint64_t word,
                                                 std::uint64_t previous) noexcept {
    return mix(((word + (previous * kHashMultiplier)) * kWordKey2) + 2);
}

}  // namespace words

namespace detail {

// Order n has 17 * 256^n keys: a first-nibble slot and sixteen
// second-nibble slots per context. Twice that, rounded up to a power of
// two, is all the slots it can use: log2 of that is returned.
[[nodiscard]] constexpr int order_slot_bits(int order) noexcept {
    const std::uint64_t keys = std::uint64_t{17} << (8 * order);
    return std::bit_width((2 * keys) - 1);
}

static_assert(order_slot_bits(1) == 14);
static_assert(order_slot_bits(2) == 22);

}  // namespace detail

class Mix9 {
public:
    // The byte contexts: 1 to kOrders bytes.
    static constexpr int kOrders = 6;
    static_assert(kOrders <= kMaxOrder);

    // The history tables: the orders, then the word and bigram contexts.
    static constexpr std::size_t kTables = static_cast<std::size_t>(kOrders) + 2;
    static constexpr std::size_t kInputs = kTables + 1;  // and the match model

    // The level is log2 of the slots in each history table but order 1's.
    static constexpr int kMinLevel = 10;
    static constexpr int kMaxLevel = 23;
    static_assert(detail::order_slot_bits(3) > kMaxLevel, "only orders 1 and 2 are capped");

    // The smallest level with 2^level >= size, within the two limits: about
    // one slot per input byte in each table.
    [[nodiscard]] static constexpr int level_for_size(std::uint64_t size) noexcept {
        const int bits = std::bit_width(size == 0 ? 0 : size - 1);  // 2^bits >= size
        return std::clamp(bits, kMinLevel, kMaxLevel);
    }

    // The match model's buffer holds as many bytes as one history table
    // (2^level slots of 16), and its hash table has half as many entries as
    // a history table has slots.
    [[nodiscard]] static constexpr int match_buffer_bits(int level) noexcept {
        return level + 4;
    }
    [[nodiscard]] static constexpr int match_hash_bits(int level) noexcept {
        return level - 1;
    }

    // log2 of the slots in history table `table` (orders 1 to kOrders, then
    // word and bigram) at `level`: the level, or less for an order with too
    // few contexts to use that many.
    //
    // The order 2 cap, 2^22, binds only at level 23, and changes no
    // prediction there: at 2^22 slots, as at 2^23, every order 2 key has a
    // bucket with room for it and a check no other key there shares, so no
    // slot is ever shared or evicted (mix9_test.cpp checks every key).
    [[nodiscard]] static constexpr int table_slot_bits(std::size_t table, int level) noexcept {
        if (table >= kOrderTables) return level;
        return std::min(level, detail::order_slot_bits(static_cast<int>(table) + 1));
    }

    // Bytes a model at `level` allocates: every table, buffer, map and
    // weight.
    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int level) noexcept {
        std::uint64_t bytes = 0;
        for (std::size_t table = 0; table < kTables; ++table) {
            bytes += HistoryTable::memory_bytes(table_slot_bits(table, level));
            bytes += ProbabilityMap::memory_bytes(kStateCount);
        }
        bytes += MatchModel::memory_bytes(match_buffer_bits(level), match_hash_bits(level));
        bytes += Apm::memory_bytes(kPartialByteContexts);
        bytes += Apm::memory_bytes(kPreviousByteContexts);
        bytes += kWeightSets * kInputs * sizeof(std::int32_t);  // the mixer
        return bytes;
    }

    // Allocates everything for `level`, in [kMinLevel, kMaxLevel]. Throws
    // std::bad_alloc if there is no memory.
    explicit Mix9(int level)
        : tables_(make_tables(level, std::make_index_sequence<kTables>())),
          maps_(make_maps(std::make_index_sequence<kTables>())),
          match_(match_buffer_bits(level), match_hash_bits(level)),
          mixer_(kWeightSets),
          by_partial_byte_(kPartialByteContexts),
          by_previous_byte_(kPreviousByteContexts) {
        select_slots();
    }

    [[nodiscard]] std::uint32_t predict() noexcept {
        Mixer<kInputs>::Stretched st{};
        for (std::size_t i = 0; i < kTables; ++i) {
            st[i] = stretch(maps_[i].p12(tables_[i].state(node_)));
        }
        st[kTables] = match_.predict(partial_);
        const std::uint32_t mixed = mixer_.mix(st);

        // Each term is in [kProbMin, kProbMax], so their weighted mean is.
        const std::int32_t mixed_st = stretch(mixed);
        const std::uint32_t previous_byte = static_cast<std::uint32_t>(history_) & 0xFF;
        const std::uint32_t refined = by_partial_byte_.refine(mixed_st, partial_);
        const std::uint32_t refined_by_previous =
            by_previous_byte_.refine(mixed_st, partial_ + (kByteDone * previous_byte));
        return (mixed + refined + (2 * refined_by_previous)) / 4;
    }

    void update(int bit) noexcept {
        assert(bit == 0 || bit == 1);
        mixer_.update(bit);  // from its own output, not the refined one
        by_partial_byte_.update(bit);
        by_previous_byte_.update(bit);
        for (std::size_t i = 0; i < kTables; ++i) {
            maps_[i].update(bit);
            tables_[i].update(node_, bit);
        }
        match_.update(bit);

        node_ = (node_ * 2) + static_cast<std::uint32_t>(bit);
        partial_ = (partial_ * 2) + static_cast<std::uint32_t>(bit);
        if (node_ < kNibbleDone) return;

        // A nibble is complete, and node_ is 16 + its value.
        if (marker_ == kFirstNibble) {
            marker_ = node_;  // exactly the second nibble's marker
        } else {
            push_byte(partial_ - kByteDone);
            marker_ = kFirstNibble;
            partial_ = 1;
        }
        select_slots();
        node_ = 1;
    }

private:
    // One weight set per previous byte and match length class.
    static constexpr std::size_t kWeightSets = std::size_t{256} * MatchModel::kMixerContexts;

    // The APMs' contexts: the partial byte, 1 to 255; and that with the
    // previous byte.
    static constexpr std::uint32_t kByteDone = 0x100;
    static constexpr std::size_t kPartialByteContexts = kByteDone;
    static constexpr std::size_t kPreviousByteContexts = std::size_t{256} * kByteDone;

    // The marker of a first-nibble slot; see context_key().
    static constexpr std::uint32_t kFirstNibble = 0;

    static constexpr std::size_t kOrderTables = static_cast<std::size_t>(kOrders);
    static constexpr std::size_t kWordTable = kOrderTables;
    static constexpr std::size_t kBigramTable = kOrderTables + 1;

    template <std::size_t... Index>
    [[nodiscard]] static std::array<HistoryTable, kTables> make_tables(
        int level, std::index_sequence<Index...> /*tables*/) {
        assert(level >= kMinLevel && level <= kMaxLevel);
        return {HistoryTable(table_slot_bits(Index, level))...};
    }

    template <std::size_t... Index>
    [[nodiscard]] static std::array<ProbabilityMap, kTables> make_maps(
        std::index_sequence<Index...> /*tables*/) {
        return {(static_cast<void>(Index), make_state_map())...};
    }

    // Points every table at its slot for the nibble about to be coded.
    void select_slots() noexcept {
        for (std::size_t i = 0; i < kOrderTables; ++i) {
            tables_[i].select(context_key(history_, static_cast<int>(i) + 1, marker_));
        }
        tables_[kWordTable].select(with_marker(word_key_));
        tables_[kBigramTable].select(with_marker(bigram_key_));
    }

    // A word key with the marker of the nibble about to be coded, as
    // context_key() builds the key of a byte context.
    [[nodiscard]] std::uint64_t with_marker(std::uint64_t key) const noexcept {
        return (key << kMarkerBits) | marker_;
    }

    // A whole byte has been coded: it joins the history, the words and the
    // match model, and picks the weight set for the byte after it, which
    // depends on the match the byte left running.
    void push_byte(std::uint32_t byte) noexcept {
        assert(byte <= 0xFF);
        history_ = (history_ << 8) | byte;
        push_word_byte(byte);
        match_.push_byte(history_);
        mixer_.select((MatchModel::kMixerContexts * byte) + match_.mixer_context());
    }

    // The byte extends the current word or ends it (see namespace words),
    // and the two word keys follow.
    void push_word_byte(std::uint32_t byte) noexcept {
        if (words::is_letter(byte)) {
            word_ = (word_ + words::lowercase(byte) + 1) * words::kWordMultiplier;
        } else if (word_ != 0) {
            previous_word_ = word_;
            word_ = 0;
        }
        word_key_ = words::word_key(word_);
        bigram_key_ = words::bigram_key(word_, previous_word_);
    }

    std::array<HistoryTable, kTables> tables_;  // orders 1 to kOrders, word, bigram
    std::array<ProbabilityMap, kTables> maps_;  // the state map of each table
    MatchModel match_;
    Mixer<kInputs> mixer_;
    Apm by_partial_byte_;                          // APM 1
    Apm by_previous_byte_;                         // APM 2
    std::uint64_t history_ = 0;                    // the last 8 bytes, newest in the low byte
    std::uint64_t word_ = 0;                       // the hash of the current word; 0 between words
    std::uint64_t previous_word_ = 0;              // the hash of the last complete word
    std::uint64_t word_key_ = words::word_key(0);  // the word table's, before the marker
    std::uint64_t bigram_key_ = words::bigram_key(0, 0);  // the bigram table's, likewise
    std::uint32_t marker_ = kFirstNibble;  // which nibble is being coded; see context_key()
    std::uint32_t node_ = 1;               // position in that nibble's tree, 1 to 15
    std::uint32_t partial_ = 1;            // the byte's bits so far, after a leading 1
};

static_assert(Predictor<Mix9>);

}  // namespace cpress::model
