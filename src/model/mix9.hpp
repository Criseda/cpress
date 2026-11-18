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
// model's buffer is as large as one history table. The two APMs, the mixer
// and the maps add a fixed 4.2 MiB, most of it the second APM. The tables,
// the match model and the APMs are on large pages where the platform grants
// them (table_allocator.hpp).
//
// Why look ahead. Each byte selects sixteen buckets, two per history table,
// and a match model entry, at places that depend on the byte before it, in
// tables far larger than the cache. Waiting for them was about 40% of the
// time at v0.3. A hint can fetch them early, if something knows the byte
// before the model does.
// prefetch_after(byte, next) fetches what the byte after `byte` will select,
// computed from a copy of the model's state by the functions select_slots()
// uses, so the two cannot disagree. The encoder has the whole input and
// gives it the true bytes, a whole byte before the buckets are needed. The
// decoder cannot know the next byte, but a running match predicts it, and
// prefetch_from_match() fetches for that guess. A wrong guess fetches lines
// nobody reads: a little memory traffic, never a changed prediction, since
// a hint writes nothing and reads nothing the model uses.
//
// Update rule: every map, table, the match model, the mixer's selected
// weights and the APMs' nearer points learn each bit; see each component.
//
// Two models, one class. ContextMix is a template on the number of byte
// contexts and the largest level, and everything above is written in terms
// of them. Mix9 is ContextMix<6, 23>, codec 3's model, frozen at v0.3.
// Mix10 is ContextMix<7, 25>, codec 5's: an order-7 table as well, so ten
// predictions, and a largest level with four times the memory, for the
// word streams of files of a gigabyte (docs/stages/stage-7.md, sections 3
// and 4).
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

// The words so far, as hashes.
struct Words {
    std::uint64_t word = 0;      // the current word; 0 between words
    std::uint64_t previous = 0;  // the last complete word
};

// The words after `byte`, which extends the current word or ends it.
[[nodiscard]] constexpr Words next_words(Words words, std::uint32_t byte) noexcept {
    if (is_letter(byte)) {
        return {.word = (words.word + lowercase(byte) + 1) * kWordMultiplier,
                .previous = words.previous};
    }
    if (words.word != 0) return {.word = 0, .previous = words.word};
    return words;
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

template <int Orders, int MaxLevel>
class ContextMix {
public:
    // The byte contexts: 1 to kOrders bytes.
    static constexpr int kOrders = Orders;
    static_assert(kOrders >= 2 && kOrders <= kMaxOrder);

    // The history tables: the orders, then the word and bigram contexts.
    static constexpr std::size_t kTables = static_cast<std::size_t>(kOrders) + 2;
    static constexpr std::size_t kInputs = kTables + 1;  // and the match model

    // The level is log2 of the slots in each history table but order 1's.
    static constexpr int kMinLevel = 10;
    static constexpr int kMaxLevel = MaxLevel;
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
    // The order 2 cap, 2^22, binds from level 23, and changes no
    // prediction there: at 2^22 slots, as at more, every order 2 key has a
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

    // What a byte's table keys are made of, besides the marker: the bytes
    // before it, the words they spell, and the two word keys, which change
    // only from byte to byte and so are kept rather than derived per nibble.
    // A context is the initial one or the result of next_context().
    struct ByteContext {
        std::uint64_t history = 0;  // the last 8 bytes, newest in the low byte
        words::Words words;
        std::uint64_t word_key = words::word_key(0);         // the word table's, before the marker
        std::uint64_t bigram_key = words::bigram_key(0, 0);  // the bigram table's, likewise
    };

    // The context of the byte after `byte`, if `context` is the context of
    // `byte`. The word keys are derived from the words here and nowhere else.
    [[nodiscard]] static constexpr ByteContext next_context(const ByteContext& context,
                                                            std::uint32_t byte) noexcept {
        assert(byte <= 0xFF);
        const words::Words next = words::next_words(context.words, byte);
        return {.history = (context.history << 8) | byte,
                .words = next,
                .word_key = words::word_key(next.word),
                .bigram_key = words::bigram_key(next.word, next.previous)};
    }

    // The key of every history table in `context`, for the nibble `marker`
    // names: orders 1 to kOrders, then word and bigram.
    [[nodiscard]] static constexpr std::array<std::uint64_t, kTables> table_keys(
        const ByteContext& context, std::uint32_t marker) noexcept {
        std::array<std::uint64_t, kTables> keys{};
        for (std::size_t i = 0; i < kOrderTables; ++i) {
            keys[i] = context_key(context.history, static_cast<int>(i) + 1, marker);
        }
        keys[kWordTable] = with_marker(context.word_key, marker);
        keys[kBigramTable] = with_marker(context.bigram_key, marker);
        return keys;
    }

    // The marker of the second nibble of `byte`: the node its first nibble
    // ends on (context_key()).
    [[nodiscard]] static constexpr std::uint32_t second_nibble_marker(std::uint32_t byte) noexcept {
        assert(byte <= 0xFF);
        return kNibbleDone + (byte >> 4);
    }

    // Allocates everything for `level`, in [kMinLevel, kMaxLevel]. Throws
    // std::bad_alloc if there is no memory.
    explicit ContextMix(int level)
        : tables_(make_tables(level, std::make_index_sequence<kTables>())),
          maps_(make_maps(std::make_index_sequence<kTables>())),
          match_(match_buffer_bits(level), match_hash_bits(level)),
          mixer_(kWeightSets),
          by_partial_byte_(kPartialByteContexts),
          by_previous_byte_(kPreviousByteContexts) {
        select_slots();
    }

    [[nodiscard]] std::uint32_t predict() noexcept {
        typename Mixer<kInputs>::Stretched st{};
        for (std::size_t i = 0; i < kTables; ++i) {
            st[i] = stretch(maps_[i].p12(tables_[i].state(node_)));
        }
        st[kTables] = match_.predict(partial_);
        const std::uint32_t mixed = mixer_.mix(st);

        // Each term is in [kProbMin, kProbMax], so their weighted mean is.
        const std::int32_t mixed_st = stretch(mixed);
        const std::uint32_t previous_byte = static_cast<std::uint32_t>(context_.history) & 0xFF;
        const std::uint32_t refined = by_partial_byte_.refine(mixed_st, partial_);
        const std::uint32_t refined_by_previous =
            by_previous_byte_.refine(mixed_st, partial_ + (kByteDone * previous_byte));
        return (mixed + refined + (2 * refined_by_previous)) / 4;
    }

    // Not inlined, on GCC and Clang. Once both coding loops also call
    // prefetch_after(), GCC 13 inlines this into each of them, and on enwik8
    // the larger loops ran about 10% slower each way than with a call here,
    // even with every table in the cache (stage 4's brief, section 3). MSVC
    // ignores the attribute; its loops were not slowed.
    [[gnu::noinline]] void update(int bit) noexcept {
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

    // What coding the byte after `byte` will look up, if `byte` is the byte
    // about to be coded and `next` the one after it: every table's key at
    // both nibbles of `next`, and the context the match model will hash.
    struct Lookahead {
        std::array<std::uint64_t, kTables> first_nibble{};
        std::array<std::uint64_t, kTables> second_nibble{};
        ByteContext context;
    };

    // Computed from a copy of the model's state, by the functions the model
    // itself selects with.
    [[nodiscard]] Lookahead lookahead(std::uint32_t byte, std::uint32_t next) const noexcept {
        const ByteContext after = next_context(context_, byte);
        return {.first_nibble = table_keys(after, kFirstNibble),
                .second_nibble = table_keys(after, second_nibble_marker(next)),
                .context = after};
    }

    // A hint, and nothing else: fetches what coding the byte after `byte`
    // will touch, if `byte` is the byte about to be coded and `next` the one
    // after. Call it at a byte boundary. Changes nothing the model predicts.
    void prefetch_after(std::uint32_t byte, std::uint32_t next) const noexcept {
        assert(node_ == 1 && marker_ == kFirstNibble);  // at a byte boundary
        const Lookahead ahead = lookahead(byte, next);
        match_.prefetch_after(ahead.context.history);  // read as `byte` ends
        for (std::size_t i = 0; i < kTables; ++i) tables_[i].prefetch(ahead.first_nibble[i]);
        for (std::size_t i = 0; i < kTables; ++i) tables_[i].prefetch(ahead.second_nibble[i]);
    }

    // prefetch_after() with the match model's guess for both bytes, if a
    // match is running; otherwise nothing. Call it at a byte boundary.
    void prefetch_from_match() const noexcept {
        if (match_.length() == 0) return;
        prefetch_after(match_.predicted_byte(0), match_.predicted_byte(1));
    }

    // For tests.
    [[nodiscard]] const ByteContext& context() const noexcept {
        return context_;
    }
    [[nodiscard]] const HistoryTable& table(std::size_t i) const noexcept {
        return tables_[i];
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

    // A word key with a marker, as context_key() builds the key of a byte
    // context.
    [[nodiscard]] static constexpr std::uint64_t with_marker(std::uint64_t key,
                                                             std::uint32_t marker) noexcept {
        return (key << kMarkerBits) | marker;
    }

    // Points every table at its slot for the nibble about to be coded.
    void select_slots() noexcept {
        const std::array<std::uint64_t, kTables> keys = table_keys(context_, marker_);
        for (std::size_t i = 0; i < kTables; ++i) tables_[i].select(keys[i]);
    }

    // A whole byte has been coded: it joins the context and the match model,
    // and picks the weight set for the byte after it, which depends on the
    // match the byte left running.
    void push_byte(std::uint32_t byte) noexcept {
        context_ = next_context(context_, byte);
        match_.push_byte(context_.history);
        mixer_.select((MatchModel::kMixerContexts * byte) + match_.mixer_context());
    }

    std::array<HistoryTable, kTables> tables_;  // orders 1 to kOrders, word, bigram
    std::array<ProbabilityMap, kTables> maps_;  // the state map of each table
    MatchModel match_;
    Mixer<kInputs> mixer_;
    Apm by_partial_byte_;                  // APM 1
    Apm by_previous_byte_;                 // APM 2
    ByteContext context_;                  // of the byte being coded
    std::uint32_t marker_ = kFirstNibble;  // which nibble is being coded; see context_key()
    std::uint32_t node_ = 1;               // position in that nibble's tree, 1 to 15
    std::uint32_t partial_ = 1;            // the byte's bits so far, after a leading 1
};

// Codec 3's model, frozen at v0.3, and codec 5's.
using Mix9 = ContextMix<6, 23>;
using Mix10 = ContextMix<7, 25>;

static_assert(Predictor<Mix9> && Predictor<Mix10>);
// Without this, a hint whose signature drifted would leave the models short
// of the concept, the coding loops would quietly stop calling it, and every
// test of the output would still pass.
static_assert(Prefetching<Mix9> && Prefetching<Mix10>);

}  // namespace cpress::model
