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
// and the maps add a fixed 4.2 MiB, most of it the second APM; with
// classes, 20 MiB more (below). With shortcuts, FlagModel adds two tables
// of one byte per entry, each with an eighth as many entries as a history
// table has slots (flag_table_bits()).
// The tables, the match model and the APMs are on large pages where the
// platform grants them (table_allocator.hpp).
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
// prefetch_from_match() fetches for that guess. As bits arrive, the decoder
// also narrows what is possible, and prefetch_decoded() fetches for the
// last two candidates. A wrong guess fetches lines nobody reads: a little
// memory traffic, never a changed prediction, since a hint writes nothing
// and reads nothing the model uses.
//
// Update rule: every map, table, the match model, the mixer's selected
// weights and the APMs' nearer points learn each bit, except what the
// shortcuts below skip; see each component.
//
// Five models, one class. ContextMix is a template on the number of byte
// contexts, the largest level, whether it takes shortcuts, whether it uses
// byte classes, an order it skips, and whether it has a second layer, and
// everything above is written in terms of them. Mix9 is ContextMix<6, 23>, codec 3's model, frozen
// at v0.3. Mix10 is ContextMix<7, 25>, codec 5's: an order-7 table as well, so ten predictions, and
// a largest level with four times the memory, for the word streams of files of a gigabyte
// (docs/stages/stage-7.md, sections 3 and 4), frozen at v1.1. Mix10Flag is Mix10 with shortcuts,
// codec 6's, frozen at v1.2. Mix11 is Mix10Flag with classes and without
// order 6, codec 7's: eleven predictions, frozen at v1.3. Mix12 is Mix11
// with order 6 again and a second layer, codec 8's: twelve predictions.
//
// Shortcuts: work skipped where the model is already sure, measured in
// docs/stages/stage-8.md.
//   the flag    while a long match runs, the byte can be coded as one
//               decision, "it is the match's guess" (FlagModel). When it
//               is, nothing else predicts or learns the byte. When it is
//               not, the eighth bit is certain once seven agree with the
//               guess, and is not coded (exclusion).
//   the APMs    are neither read nor trained on a bit the mixer is already
//               near certain of.
//   the mixer   does not learn from a bit it predicted within a few parts
//               in 4096.
//
// Classes: codec 7's additions, measured in docs/stages/stage-9.md,
// section 2. Each byte has one of sixteen classes (byte_classes.hpp).
//   class tables  two more history tables, keyed by the classes of the
//                 last 12 bytes and of the last 6.
//   weight sets   chosen per bit, not per byte: by the classes of the last
//                 two bytes, the match's length and the partial byte,
//                 262,144 sets of 64 bytes, 16 MiB. Each has its own
//                 learning rate, which starts high and decays (mixer.hpp).
//   a bias        a constant input, so each set learns an offset of its own.
//   APM 1         keyed by the classes of the last two bytes as well as the
//                 partial byte: 65,536 contexts, 4 MiB.
// Order 6 is skipped: on enwik9 its table was worth 0.28% of the size and
// cost 12% of the decoding time.
//
// A second layer: codec 8's additions, measured in docs/stages/
// stage-10.md, section 1. One mixer sees every input with weights chosen
// one way; mixers with weights chosen other ways see the same inputs
// differently, and a final mixer learns which to trust where.
//   two mixers    of the same inputs as the first: one with weight sets
//                 chosen by a 12-bit hash of the last two bytes, 64 MiB,
//                 one by the word's length class, the last byte's class
//                 and the match's length, 6 MiB; both also by the partial
//                 byte. They learn when the first mixer does.
//   final mixer   of the three mixers' outputs, a weight set per partial
//                 byte. It learns every bit the model codes.
//   APM 3         keyed by a 12-bit hash of the last three bytes and the
//                 partial byte, 66 MiB, averaged in with the other two.
// These are the same size at every level.
//
// Technique: this is the lpaq class of compressor, after Matt Mahoney's
// lpaq1 (2007): bit-history states in checked nibble slots, read through
// state maps, a match model and a word context, mixed and refined by
// APMs. The component files say what each takes from it.
#pragma once

#include "model/apm.hpp"
#include "model/byte_classes.hpp"
#include "model/context_table.hpp"
#include "model/flag_model.hpp"
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
#include <optional>
#include <type_traits>
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

// What a model without shortcuts holds in the flag model's place: nothing.
struct NoFlagModel {
    explicit constexpr NoFlagModel(int /*table_bits*/) noexcept {}
};

// What a model without a second layer holds in its place: nothing.
struct NoSecondLayer {};

}  // namespace detail

template <int Orders, int MaxLevel, bool Shortcuts = false, bool Classes = false,
          int SkippedOrder = 0, bool Layered = false>
class ContextMix {
public:
    // The byte contexts: 1 to kOrders bytes, but kSkippedOrder, if not 0.
    static constexpr int kOrders = Orders;
    static constexpr int kSkippedOrder = SkippedOrder;
    static_assert(kOrders >= 2 && kOrders <= kMaxOrder);
    static_assert(kSkippedOrder == 0 || (kSkippedOrder > 1 && kSkippedOrder < kOrders));
    static constexpr std::size_t kOrderTables =
        static_cast<std::size_t>(kOrders) - (kSkippedOrder != 0 ? 1 : 0);

    // The order of each order table: 1 to kOrders, without kSkippedOrder.
    static constexpr std::array<int, kOrderTables> kOrderOf = [] {
        std::array<int, kOrderTables> orders{};
        int order = 1;
        for (int& o : orders) {
            if (order == kSkippedOrder) ++order;
            o = order++;
        }
        return orders;
    }();

    // Codec 6's shortcuts; see the top of this file. The values were chosen
    // in the prototype (docs/stages/stage-8.md, section 2).
    static constexpr bool kShortcuts = Shortcuts;
    // The flag is consulted while a match has agreed this many bytes...
    static constexpr std::uint32_t kFlagMinLength = 12;
    // ...and coded if FlagModel gives the guess this p12 or more: 95%.
    static constexpr std::uint32_t kFlagGate = 3900;
    // The APMs are skipped when the mixer's stretched output is beyond
    // +-this: 5.9 units of ln odds, so the mixer is over 99.7% sure.
    static constexpr std::int32_t kApmSkipStretch = 1500;
    // The mixer does not learn from a bit it predicted within this, out of
    // kProbOne.
    static constexpr std::uint32_t kMixerSkipError = 20;

    // Codec 7's classes; see the top of this file.
    static constexpr bool kClasses = Classes;
    // The bias input: one unit of ln odds, always.
    static constexpr std::int32_t kBiasStretch = kStretchOne;

    // Codec 8's second layer; see the top of this file. It mixes what codec
    // 7's model mixes, so it needs the classes and the shortcuts.
    static constexpr bool kLayered = Layered;
    static_assert(!kLayered || (kClasses && kShortcuts));

    // The history tables: the orders, then the word and bigram contexts,
    // then with classes the long and short class contexts. The mixer's
    // inputs are their predictions, the match model's, and with classes
    // the bias.
    static constexpr std::size_t kTables = kOrderTables + 2 + (kClasses ? 2 : 0);
    static constexpr std::size_t kInputs = kTables + 1 + (kClasses ? 1 : 0);

    // The level is log2 of the slots in each history table but order 1's.
    static constexpr int kMinLevel = 10;
    static constexpr int kMaxLevel = MaxLevel;
    static_assert(usable_slot_bits(3) > kMaxLevel, "only orders 1 and 2 are capped");

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

    // log2 of the slots in history table `table` (the orders, then the rest)
    // at `level`: the level, or less for an order with too few contexts to
    // use that many.
    //
    // The order 2 cap, 2^22, binds from level 23, and changes no
    // prediction there: at 2^22 slots, as at more, every order 2 key has a
    // bucket with room for it and a check no other key there shares, so no
    // slot is ever shared or evicted (mix9_test.cpp checks every key).
    [[nodiscard]] static constexpr int table_slot_bits(std::size_t table, int level) noexcept {
        if (table >= kOrderTables) return level;
        return std::min(level, usable_slot_bits(kOrderOf[table]));
    }

    // log2 of each flag table's entries at `level`: an eighth as many as a
    // history table has slots, 2^22 at level 25.
    [[nodiscard]] static constexpr int flag_table_bits(int level) noexcept {
        return level - 3;
    }
    static_assert(flag_table_bits(kMinLevel) >= FlagModel::kMinTableBits);

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
        bytes += Mixer<kInputs, kClasses>::memory_bytes(kWeightSets);
        if constexpr (kShortcuts) bytes += Flag::memory_bytes(flag_table_bits(level));
        if constexpr (kLayered) bytes += SecondLayer::memory_bytes();
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
        std::uint64_t classes = 0;      // with classes, their history (byte_classes.hpp)
        std::uint32_t word_length = 0;  // with a second layer, letters since a non-letter
    };

    // The context of the byte after `byte`, if `context` is the context of
    // `byte`. The word keys are derived from the words here and nowhere else.
    [[nodiscard]] static constexpr ByteContext next_context(const ByteContext& context,
                                                            std::uint32_t byte) noexcept {
        assert(byte <= 0xFF);
        const words::Words next = words::next_words(context.words, byte);
        ByteContext after{.history = (context.history << 8) | byte,
                          .words = next,
                          .word_key = words::word_key(next.word),
                          .bigram_key = words::bigram_key(next.word, next.previous)};
        if constexpr (kClasses) after.classes = classes::next_history(context.classes, byte);
        if constexpr (kLayered) {
            after.word_length = words::is_letter(byte) ? context.word_length + 1 : 0;
        }
        return after;
    }

    // The key of every history table in `context`, for the nibble `marker`
    // names: the orders, word and bigram, and with classes the class tables.
    [[nodiscard]] static constexpr std::array<std::uint64_t, kTables> table_keys(
        const ByteContext& context, std::uint32_t marker) noexcept {
        std::array<std::uint64_t, kTables> keys{};
        for (std::size_t i = 0; i < kOrderTables; ++i) {
            keys[i] = context_key(context.history, kOrderOf[i], marker);
        }
        keys[kWordTable] = with_marker(context.word_key, marker);
        keys[kBigramTable] = with_marker(context.bigram_key, marker);
        if constexpr (kClasses) {
            keys[kLongClassTable] = with_marker(classes::long_key(context.classes), marker);
            keys[kShortClassTable] = with_marker(classes::short_key(context.classes), marker);
        }
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
          by_previous_byte_(kPreviousByteContexts),
          flag_(flag_table_bits(level)) {
        select_slots();
    }

    [[nodiscard]] std::uint32_t predict() noexcept {
        typename Mixer<kInputs>::Stretched st{};
        for (std::size_t i = 0; i < kTables; ++i) {
            st[i] = stretch(maps_[i].p12(tables_[i].state(node_)));
        }
        st[kTables] = match_.predict(partial_);
        if constexpr (kClasses) {
            st[kTables + 1] = kBiasStretch;
            mixer_.select((byte_weight_set_ * kByteDone) + partial_);
        }
        std::uint32_t mixed = mixer_.mix(st);
        if constexpr (kLayered) mixed = mix_second_layer(st, mixed);
        const std::int32_t mixed_st = stretch(mixed);
        if constexpr (kShortcuts) {
            mixed_ = mixed;
            apms_skipped_ = mixed_st > kApmSkipStretch || mixed_st < -kApmSkipStretch;
            if (apms_skipped_) return mixed;
        }

        // Each term is in [kProbMin, kProbMax], so their weighted mean is.
        const std::uint32_t previous_byte = static_cast<std::uint32_t>(context_.history) & 0xFF;
        const std::uint32_t refined = by_partial_byte_.refine(mixed_st, partial_byte_context());
        const std::uint32_t refined_by_previous =
            by_previous_byte_.refine(mixed_st, partial_ + (kByteDone * previous_byte));
        if constexpr (kLayered) {
            const std::uint32_t refined_by_order3 =
                layer_.by_order3.refine(mixed_st, (order3_hash_ * kByteDone) + partial_);
            return (mixed + refined + refined_by_previous + refined_by_order3) / 4;
        }
        return (mixed + refined + (2 * refined_by_previous)) / 4;
    }

    // Not inlined, on GCC and Clang. Once both coding loops also call
    // prefetch_after(), GCC 13 inlines this into each of them, and on enwik8
    // the larger loops ran about 10% slower each way than with a call here,
    // even with every table in the cache (stage 4's brief, section 3). MSVC
    // ignores the attribute; its loops were not slowed.
    [[gnu::noinline]] void update(int bit) noexcept {
        assert(bit == 0 || bit == 1);
        // The second APM's curves are 4 MiB, read at a place each bit moves,
        // and the bit has just decided the next: fetch it now, an update's
        // work before predict() reads it. A hint; it changes nothing.
        by_previous_byte_.prefetch(next_previous_byte_context(bit));
        if constexpr (kClasses) prefetch_next_weight_set(bit);
        learn_mix(bit);
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
            if constexpr (kShortcuts) settle_guess(partial_ - kByteDone);
            push_byte(partial_ - kByteDone);
            marker_ = kFirstNibble;
            partial_ = 1;
        }
        // With shortcuts, a byte's first slots wait for offer_flag(), so
        // that a byte the flag codes never selects them.
        if (!kShortcuts || marker_ != kFirstNibble) select_slots();
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
        if constexpr (kShortcuts) {
            // If the match runs on through `byte`, the flag of the byte
            // after it will weigh the match's next guess. Without this,
            // the flag's two reads cost what its skipped bytes save.
            if (match_.length() != 0 && byte == match_.predicted_byte(0)) {
                flag_.prefetch(ahead.context.history, match_.predicted_byte(1));
            }
        }
    }

    // prefetch_after() with the match model's guess for both bytes, if a
    // match is running; otherwise nothing. Call it at a byte boundary.
    void prefetch_from_match() const noexcept {
        if (match_.length() == 0) return;
        prefetch_after(match_.predicted_byte(0), match_.predicted_byte(1));
    }

    // A hint for a decoder, after each bit it decodes: `bits` of the byte
    // being decoded are known, 1 to 8, and the rest are not. Two lookups
    // the encoder fetched a byte ahead wait on bits the decoder does not
    // have yet: the second nibble's buckets, which depend on the first
    // nibble, and the next byte's first, which depend on this byte. Each is
    // fetched once only two candidates are left:
    //
    //   kSecondNibbleBits  the first nibble is one of two, so in every
    //                      table the second nibble's bucket is one of two:
    //                      both are fetched, a bit before select_slots()
    //                      needs one.
    //   kNextByteBits      the byte is one of two, and so are the next
    //                      byte's first buckets and its match model entry:
    //                      both are fetched, a bit before the byte ends.
    //                      Not while a match runs: it has agreed with every
    //                      bit so far, and prefetch_from_match() fetched
    //                      for its guess at the byte boundary.
    //
    // Fetching the second nibble's four candidates, a bit earlier, was
    // slower than fetching none. Changes nothing the model predicts.
    void prefetch_decoded(int bits) const noexcept {
        if (bits == kSecondNibbleBits) {
            for (const std::array<std::uint64_t, kTables>& keys : second_nibble_candidates()) {
                for (std::size_t i = 0; i < kTables; ++i) tables_[i].prefetch(keys[i]);
            }
        } else if (bits == kNextByteBits && match_.length() == 0) {
            for (const ByteContext& after : next_byte_candidates()) {
                match_.prefetch_after(after.history);
                const std::array<std::uint64_t, kTables> keys = table_keys(after, kFirstNibble);
                for (std::size_t i = 0; i < kTables; ++i) tables_[i].prefetch(keys[i]);
            }
        }
    }

    // How many bits of a byte prefetch_decoded() waits for, each time.
    static constexpr int kSecondNibbleBits = 3;
    static constexpr int kNextByteBits = 7;

    // In every table, the second nibble's key for each first nibble still
    // possible when kSecondNibbleBits of it are known: the one whose last
    // bit is 0, then the one whose last bit is 1.
    [[nodiscard]] std::array<std::array<std::uint64_t, kTables>, 2> second_nibble_candidates()
        const noexcept {
        assert(marker_ == kFirstNibble && (node_ >> kSecondNibbleBits) == 1);
        const std::uint32_t nibble = (node_ - (1U << kSecondNibbleBits)) * 2;
        return {table_keys(context_, kNibbleDone + nibble),
                table_keys(context_, kNibbleDone + nibble + 1)};
    }

    // The context after each byte still possible when kNextByteBits of it
    // are known: the byte whose last bit is 0, then the one whose last bit
    // is 1.
    [[nodiscard]] std::array<ByteContext, 2> next_byte_candidates() const noexcept {
        assert(marker_ != kFirstNibble && (partial_ >> kNextByteBits) == 1);
        const std::uint32_t byte = (partial_ - (1U << kNextByteBits)) * 2;
        return {next_context(context_, byte), next_context(context_, byte + 1)};
    }

    // The predicted-byte flag, with shortcuts only; the calls and their
    // order are the Flagging concept's (predictor.hpp).
    //
    // At a byte boundary: whether this byte starts with a flag. FlagModel
    // weighs the match's guess while the match has agreed kFlagMinLength
    // bytes or more, and the flag is coded if it gives the guess kFlagGate
    // or more. If it is not coded, the byte's first slots are selected now.
    [[nodiscard]] bool offer_flag() noexcept
        requires kShortcuts
    {
        assert(node_ == 1 && marker_ == kFirstNibble);  // at a byte boundary
        guess_.reset();
        flag_coded_ = false;
        if (match_.length() >= kFlagMinLength) {
            guess_ = match_.predicted_byte(0);
            flag_p12_ = flag_.predict(context_.history, *guess_, match_.length());
            flag_coded_ = flag_p12_ >= kFlagGate;
        }
        if (!flag_coded_) select_slots();
        return flag_coded_;
    }

    // The byte the flag claims, and P(the byte is it) as a p12. Only after
    // offer_flag() returned true.
    [[nodiscard]] std::uint32_t guess() const noexcept
        requires kShortcuts
    {
        assert(flag_coded_ && guess_.has_value());
        return *guess_;
    }
    [[nodiscard]] std::uint32_t predict_flag() const noexcept
        requires kShortcuts
    {
        assert(flag_coded_);
        return flag_p12_;
    }

    // The flag's outcome. A hit is the whole byte: it joins the context and
    // the match, and nothing else learns it. A miss rules the guess out,
    // and the byte's eight bits follow.
    void update_flag(int hit) noexcept
        requires kShortcuts
    {
        assert(flag_coded_ && (hit == 0 || hit == 1));
        flag_.update(hit);
        if (hit != 0) {
            const std::uint32_t byte = *guess_;
            guess_.reset();
            push_byte(byte);
            return;
        }
        select_slots();
        excluded_ = guess_;
    }

    // Exclusion: after a flag that missed, the byte is not the guess, so
    // once seven of its bits are the guess's, the eighth is certain. Before
    // each bit: that bit, or nothing.
    [[nodiscard]] std::optional<int> forced_bit() const noexcept
        requires kShortcuts
    {
        if (!excluded_.has_value() || partial_ != ((kByteDone | *excluded_) >> 1)) {
            return std::nullopt;
        }
        return static_cast<int>(~*excluded_ & 1);
    }

    // For tests.
    [[nodiscard]] const ByteContext& context() const noexcept {
        return context_;
    }
    [[nodiscard]] const HistoryTable& table(std::size_t i) const noexcept {
        return tables_[i];
    }

private:
    // A partial byte is 1 to 255: the bits so far, after a leading 1.
    static constexpr std::uint32_t kByteDone = 0x100;
    static constexpr std::size_t kClassPairs =
        std::size_t{classes::kClassCount} * classes::kClassCount;

    // The weight sets: one per previous byte and match length class, chosen
    // per byte; with classes, one per class of the last two bytes, match
    // length class and partial byte, chosen per bit.
    static constexpr std::size_t kByteWeightSets =
        (kClasses ? kClassPairs : std::size_t{256}) * MatchModel::kMixerContexts;
    static constexpr std::size_t kWeightSets = kByteWeightSets * (kClasses ? kByteDone : 1);

    // The APMs' contexts: the partial byte, with classes also the classes of
    // the last two bytes; and the partial byte with the previous byte.
    static constexpr std::size_t kPartialByteContexts = kByteDone * (kClasses ? kClassPairs : 1);
    static constexpr std::size_t kPreviousByteContexts = std::size_t{256} * kByteDone;

    // The second layer: two more mixers of the same inputs, one with a
    // weight set per hash of the last two bytes, one per word length class,
    // class of the last byte and match length class, both also per partial
    // byte; a final mixer of the three mixers' outputs, per partial byte;
    // and a third APM, per hash of the last three bytes and partial byte.
    // Every mixer's rate decays, as codec 7's does.
    static constexpr int kHashBits = 12;
    static constexpr std::size_t kHashes = std::size_t{1} << kHashBits;
    static constexpr std::uint32_t kWordLengthClasses = 6;
    static constexpr std::size_t kWordSets =
        std::size_t{kWordLengthClasses} * classes::kClassCount * MatchModel::kMixerContexts;
    static constexpr std::size_t kLayerInputs = 3;

    // The top kHashBits of the low 32 bits of `bytes` times an odd constant
    // (2^32 over the golden ratio): the hash of the last two or three bytes.
    [[nodiscard]] static constexpr std::size_t hash_bytes(std::uint64_t bytes) noexcept {
        return (static_cast<std::uint32_t>(bytes) * 0x9E3779B1U) >> (32 - kHashBits);
    }

    // A word's length, 0 to kWordLengthClasses - 1: no letters, 1, 2, 3 or
    // 4, 5 or 6, 7 or more.
    [[nodiscard]] static constexpr std::uint32_t word_length_class(std::uint32_t length) noexcept {
        if (length < 3) return length;
        if (length < 5) return 3;
        return length < 7 ? 4 : 5;
    }

    struct SecondLayer {
        Mixer<kInputs, true> by_order2{kHashes * kByteDone};
        Mixer<kInputs, true> by_word{kWordSets * kByteDone};
        Mixer<kLayerInputs, true> final{kByteDone};
        Apm by_order3{kHashes * kByteDone};

        [[nodiscard]] static constexpr std::uint64_t memory_bytes() noexcept {
            return Mixer<kInputs, true>::memory_bytes(kHashes * kByteDone) +
                   Mixer<kInputs, true>::memory_bytes(kWordSets * kByteDone) +
                   Mixer<kLayerInputs, true>::memory_bytes(kByteDone) +
                   Apm::memory_bytes(kHashes * kByteDone);
        }
    };

    // The marker of a first-nibble slot; see context_key().
    static constexpr std::uint32_t kFirstNibble = 0;

    static constexpr std::size_t kWordTable = kOrderTables;
    static constexpr std::size_t kBigramTable = kOrderTables + 1;
    static constexpr std::size_t kLongClassTable = kOrderTables + 2;   // with classes
    static constexpr std::size_t kShortClassTable = kOrderTables + 3;  // with classes

    // The flag model, with shortcuts; with classes its mixer's rate decays.
    using Flag = BasicFlagModel<kClasses>;

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

    // The first APM's context: the partial byte, and with classes the
    // classes of the last two bytes.
    [[nodiscard]] std::size_t partial_byte_context() const noexcept {
        if constexpr (kClasses) {
            return partial_ + (std::size_t{kByteDone} * classes::last_two(context_.classes));
        } else {
            return partial_;
        }
    }

    // With classes: fetches the weight sets the bit after `bit` will select,
    // and with a second layer its third APM's curve, if that bit is in this
    // byte. The next byte's depend on the byte, so they are not known until
    // push_byte(). A hint; it changes nothing.
    void prefetch_next_weight_set(int bit) const noexcept {
        const std::uint32_t partial = (partial_ * 2) + static_cast<std::uint32_t>(bit);
        if (partial >= kByteDone) return;
        mixer_.prefetch((byte_weight_set_ * kByteDone) + partial);
        if constexpr (kLayered) {
            layer_.by_order2.prefetch((order2_hash_ * kByteDone) + partial);
            layer_.by_word.prefetch((word_set_ * kByteDone) + partial);
            layer_.by_order3.prefetch((order3_hash_ * kByteDone) + partial);
        }
    }

    // The second layer: its two mixers mix `st`, and the final mixer mixes
    // their outputs with `first`, the first mixer's.
    [[nodiscard]] std::uint32_t mix_second_layer(const Mixer<kInputs>::Stretched& st,
                                                 std::uint32_t first) noexcept
        requires kLayered
    {
        layer_.by_order2.select((order2_hash_ * kByteDone) + partial_);
        layer_.by_word.select((word_set_ * kByteDone) + partial_);
        layer_.final.select(partial_);
        const std::uint32_t by_order2 = layer_.by_order2.mix(st);
        const std::uint32_t by_word = layer_.by_word.mix(st);
        return layer_.final.mix({stretch(first), stretch(by_order2), stretch(by_word)});
    }

    // The second APM's context for the bit after `bit`: the partial byte
    // with `bit` in it and the previous byte, or, if `bit` ends a byte, an
    // empty partial byte and that byte. predict() computes the same context
    // from the state update() leaves.
    [[nodiscard]] std::size_t next_previous_byte_context(int bit) const noexcept {
        const std::uint32_t partial = (partial_ * 2) + static_cast<std::uint32_t>(bit);
        if (partial >= kByteDone) return 1 + (kByteDone * (partial - kByteDone));
        const std::uint32_t previous_byte = static_cast<std::uint32_t>(context_.history) & 0xFF;
        return partial + (kByteDone * previous_byte);
    }

    // The mixer and the APMs learn the bit, from what predict() left. With
    // shortcuts, the mixer skips a bit it predicted within kMixerSkipError,
    // and the APMs a bit predict() did not give them.
    void learn_mix(int bit) noexcept {
        if constexpr (kShortcuts) {
            const std::uint32_t error = bit != 0 ? kProbOne - mixed_ : mixed_;
            if (error >= kMixerSkipError) {
                mixer_.update(bit);
                if constexpr (kLayered) {
                    layer_.by_order2.update(bit);
                    layer_.by_word.update(bit);
                }
            }
            if constexpr (kLayered) layer_.final.update(bit);  // every bit
            if (apms_skipped_) return;
        } else {
            mixer_.update(bit);  // from its own output, not the refined one
        }
        by_partial_byte_.update(bit);
        by_previous_byte_.update(bit);
        if constexpr (kLayered) layer_.by_order3.update(bit);
    }

    // At the end of a byte coded bit by bit. A guess the flag weighed but
    // did not code still teaches FlagModel whether it was right; a coded
    // flag learned at update_flag(). Either way, exclusion ends.
    void settle_guess(std::uint32_t byte) noexcept {
        if (guess_.has_value() && !flag_coded_) flag_.update(byte == *guess_ ? 1 : 0);
        guess_.reset();
        excluded_.reset();
    }

    // Points every table at its slot for the nibble about to be coded.
    void select_slots() noexcept {
        const std::array<std::uint64_t, kTables> keys = table_keys(context_, marker_);
        for (std::size_t i = 0; i < kTables; ++i) tables_[i].select(keys[i]);
    }

    // A whole byte has been coded: it joins the context and the match model,
    // and picks the weight sets for the byte after it, which depend on the
    // match the byte left running.
    void push_byte(std::uint32_t byte) noexcept {
        context_ = next_context(context_, byte);
        match_.push_byte(context_.history);
        if constexpr (kClasses) {
            byte_weight_set_ = (MatchModel::kMixerContexts * classes::last_two(context_.classes)) +
                               match_.mixer_context();
            if constexpr (kLayered) {
                order2_hash_ = hash_bytes(context_.history & 0xFFFF);
                order3_hash_ = hash_bytes(context_.history & 0xFFFFFF);
                const std::uint32_t length = word_length_class(context_.word_length);
                word_set_ = (((length * classes::kClassCount) + classes::class_of(byte)) *
                             MatchModel::kMixerContexts) +
                            match_.mixer_context();
            }
        } else {
            mixer_.select((MatchModel::kMixerContexts * byte) + match_.mixer_context());
        }
    }

    std::array<HistoryTable, kTables> tables_;  // the orders, word, bigram, class tables
    std::array<ProbabilityMap, kTables> maps_;  // the state map of each table
    MatchModel match_;
    Mixer<kInputs, kClasses> mixer_;
    Apm by_partial_byte_;              // APM 1
    Apm by_previous_byte_;             // APM 2
    ByteContext context_;              // of the byte being coded
    std::size_t byte_weight_set_ = 0;  // with classes: this byte's weight sets, over kByteDone
    std::conditional_t<kLayered, SecondLayer, detail::NoSecondLayer> layer_;
    std::size_t order2_hash_ = 0;  // with a second layer: this byte's other two weight sets
    std::size_t word_set_ = 0;     // and its third APM's context, over kByteDone
    std::size_t order3_hash_ = 0;
    std::uint32_t marker_ = kFirstNibble;  // which nibble is being coded; see context_key()
    std::uint32_t node_ = 1;               // position in that nibble's tree, 1 to 15
    std::uint32_t partial_ = 1;            // the byte's bits so far, after a leading 1

    // The shortcuts' state. Without shortcuts the flag model is empty and
    // the rest is never read.
    std::conditional_t<kShortcuts, Flag, detail::NoFlagModel> flag_;
    std::optional<std::uint32_t> guess_;     // the guess FlagModel weighed for this byte
    std::optional<std::uint32_t> excluded_;  // the guess a coded flag ruled out
    std::uint32_t flag_p12_ = 0;             // FlagModel's P(the byte is the guess)
    bool flag_coded_ = false;                // whether this byte started with a flag
    std::uint32_t mixed_ = kProbOne / 2;     // the mixer's last output
    bool apms_skipped_ = false;              // whether the last predict() skipped the APMs
};

// Codec 3's model, frozen at v0.3; codec 5's, frozen at v1.1; codec 6's,
// frozen at v1.2; and codec 7's.
using Mix9 = ContextMix<6, 23>;
using Mix10 = ContextMix<7, 25>;
using Mix10Flag = ContextMix<7, 25, true>;
using Mix11 = ContextMix<7, 25, true, true, 6>;
using Mix12 = ContextMix<7, 25, true, true, 0, true>;

static_assert(Predictor<Mix9> && Predictor<Mix10> && Predictor<Mix10Flag> && Predictor<Mix11> &&
              Predictor<Mix12>);
// Without this, a hint whose signature drifted would leave the models short
// of the concept, the coding loops would quietly stop calling it, and every
// test of the output would still pass. The same holds for the flag.
static_assert(Prefetching<Mix9> && Prefetching<Mix10> && Prefetching<Mix10Flag> &&
              Prefetching<Mix11> && Prefetching<Mix12>);
static_assert(!Flagging<Mix9> && !Flagging<Mix10> && Flagging<Mix10Flag> && Flagging<Mix11> &&
              Flagging<Mix12>);

}  // namespace cpress::model
