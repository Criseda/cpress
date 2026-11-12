// Mix9 tests: valid predictions on any input, determinism, the level
// arithmetic, and the hints: they change no prediction, and the keys they
// fetch for are the keys the model then selects.
#include "check.hpp"

#include "model/context_table.hpp"
#include "model/history_table.hpp"
#include "model/mix9.hpp"
#include "model/predictor.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

namespace cpress::test {
namespace {

using model::Mix9;

static_assert(model::Predictor<Mix9>);

// level_for_size: the smallest level with 2^level >= size, within the limits.
static_assert(Mix9::level_for_size(0) == Mix9::kMinLevel);
static_assert(Mix9::level_for_size(1) == Mix9::kMinLevel);
static_assert(Mix9::level_for_size(1024) == 10);
static_assert(Mix9::level_for_size(1025) == 11);
static_assert(Mix9::level_for_size(std::uint64_t{1} << 23) == 23);
static_assert(Mix9::level_for_size(std::uint64_t{1} << 40) == Mix9::kMaxLevel);

// memory_bytes: the table in docs/stage-3.md, section 7.
static_assert(Mix9::memory_bytes(10) == 4'534'656);
static_assert(Mix9::memory_bytes(16) == 13'166'976);
static_assert(Mix9::memory_bytes(20) == 140'962'176);
static_assert(Mix9::memory_bytes(22) == 549'906'816);
static_assert(Mix9::memory_bytes(23) == 1'028'057'472);

// table_slot_bits: orders 1 and 2 capped, the rest at the level.
static_assert(Mix9::table_slot_bits(0, Mix9::kMaxLevel) == 14);
static_assert(Mix9::table_slot_bits(1, Mix9::kMaxLevel) == 22);
static_assert(Mix9::table_slot_bits(1, 20) == 20);
static_assert(Mix9::table_slot_bits(2, Mix9::kMaxLevel) == Mix9::kMaxLevel);
static_assert(Mix9::table_slot_bits(Mix9::kTables - 1, Mix9::kMaxLevel) == Mix9::kMaxLevel);

// Whether, in a table of 2^slot_bits slots, every order 2 key has a slot of
// its own: no bucket holds more keys than it has ways, and no two keys in a
// bucket share a check. Then no slot is shared or evicted, whatever the
// input.
bool order2_keys_fit(int slot_bits) {
    using model::HistoryTable;
    const HistoryTable table(slot_bits);
    std::vector<std::uint64_t> places;  // bucket and check, one per key
    const auto add = [&](std::uint64_t context, std::uint32_t marker) {
        const auto [bucket, check] = table.locate(model::context_key(context, 2, marker));
        places.push_back((std::uint64_t{bucket} << 8) | check);
    };
    for (std::uint64_t context = 0; context <= 0xFFFF; ++context) {
        add(context, 0);  // the first nibble's slot
        for (std::uint32_t marker = model::kNibbleDone; marker < 2 * model::kNibbleDone; ++marker) {
            add(context, marker);
        }
    }
    CHECK(places.size() == std::size_t{17} * 65'536);
    std::ranges::sort(places);

    std::size_t in_bucket = 0;
    for (std::size_t i = 0; i < places.size(); ++i) {
        const bool same_bucket = i > 0 && (places[i] >> 8) == (places[i - 1] >> 8);
        if (same_bucket && places[i] == places[i - 1]) return false;  // a shared check
        in_bucket = same_bucket ? in_bucket + 1 : 1;
        if (in_bucket > HistoryTable::kWays) return false;
    }
    return true;
}

// The order 2 cap changes no prediction: at its 2^22 slots, as at the 2^23
// it would have at level 23 without it, every key has a slot of its own.
// That rests on the hash, not on the sizing rule, so it is checked here.
void test_order2_cap_changes_nothing() {
    CHECK(order2_keys_fit(Mix9::table_slot_bits(1, Mix9::kMaxLevel)));
    CHECK(order2_keys_fit(Mix9::kMaxLevel));
}

// Random bits are the input a model is worst at, and at the smallest level
// contexts collide constantly. Predictions must stay valid all the same.
void test_predictions_stay_in_range() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(16);
    Mix9 model(Mix9::kMinLevel);
    bool in_range = true;
    for (int i = 0; i < 1'000'000; ++i) {
        const std::uint32_t p12 = model.predict();
        in_range = in_range && p12 >= model::kProbMin && p12 <= model::kProbMax;
        model.update(static_cast<int>(rng() & 1));
    }
    CHECK(in_range);
}

void test_deterministic() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(17);
    Mix9 a(Mix9::kMinLevel);
    Mix9 b(Mix9::kMinLevel);
    bool identical = true;
    for (int i = 0; i < 100'000; ++i) {
        identical = identical && a.predict() == b.predict();
        // Bytes with only two bits of entropy, so contexts, words and
        // matches repeat, and every part is exercised beyond its initial
        // state.
        const int bit = (rng() % 4) == 0 ? 1 : 0;
        a.update(bit);
        b.update(bit);
    }
    CHECK(identical);
}

// Stage 3's word step, as Mix9 wrote it before next_words() and
// table_keys() replaced it: the words, and the two word keys before the
// marker, kept up to date on every byte.
struct Stage3Words {
    std::uint64_t word = 0;
    std::uint64_t previous_word = 0;
    std::uint64_t word_key = model::words::word_key(0);
    std::uint64_t bigram_key = model::words::bigram_key(0, 0);

    void push(std::uint32_t byte) {
        if (model::words::is_letter(byte)) {
            word = (word + model::words::lowercase(byte) + 1) * model::words::kWordMultiplier;
        } else if (word != 0) {
            previous_word = word;
            word = 0;
        }
        word_key = model::words::word_key(word);
        bigram_key = model::words::bigram_key(word, previous_word);
    }
};

// The word and bigram tables, the last two.
constexpr std::size_t kWordTable = Mix9::kTables - 2;
constexpr std::size_t kBigramTable = Mix9::kTables - 1;

// next_words() gives the words stage 3's step gave, and next_context() and
// table_keys() the word keys, for every byte value from several starting
// states: no word, within a word, just after one, and within a word of
// UTF-8.
void test_next_words_matches_stage_3() {
    constexpr std::array<std::string_view, 4> kPrefixes{"", "the qu", "the cat ", "Z\xC3\xBCr"};
    constexpr std::uint32_t kMarker = model::kNibbleDone + 3;
    bool matches = true;
    for (const std::string_view prefix : kPrefixes) {
        Stage3Words start3;
        Mix9::ByteContext start;
        for (const char c : prefix) {
            const auto byte = static_cast<std::uint8_t>(c);
            start3.push(byte);
            start = Mix9::next_context(start, byte);
        }
        for (std::uint32_t byte = 0; byte <= 0xFF; ++byte) {
            Stage3Words expected = start3;
            expected.push(byte);
            const model::words::Words words = model::words::next_words(start.words, byte);
            const auto keys = Mix9::table_keys(Mix9::next_context(start, byte), kMarker);
            matches = matches && words.word == expected.word &&
                      words.previous == expected.previous_word &&
                      keys[kWordTable] == ((expected.word_key << model::kMarkerBits) | kMarker) &&
                      keys[kBigramTable] == ((expected.bigram_key << model::kMarkerBits) | kMarker);
        }
    }
    CHECK(matches);
}

// The inputs the hint tests run over: text, where matches run and the
// decoder's guess is often right, and random bytes, where it is not.
std::array<cpress::Bytes, 2> hint_inputs() {
    return {sample_text(8192), random_bytes(8192, 41)};
}

// Hints change nothing. A model given both hints before every byte, the
// encoder's with the true bytes, predicts exactly as one given none.
void test_hints_change_no_prediction() {
    for (const cpress::Bytes& data : hint_inputs()) {
        Mix9 hinted(Mix9::kMinLevel);
        Mix9 plain(Mix9::kMinLevel);
        const auto give_hints = [&](std::size_t i) {
            if (i + 1 < data.size()) hinted.prefetch_after(data[i], data[i + 1]);
            hinted.prefetch_from_match();
        };
        CHECK(predictions(hinted, data, give_hints) == predictions(plain, data));
    }
}

// Whether every table of `model` has selected the slot of its key in
// `keys`: in the bucket locate() gives the key, with the key's check.
bool selects(const Mix9& model, const std::array<std::uint64_t, Mix9::kTables>& keys) {
    for (std::size_t i = 0; i < Mix9::kTables; ++i) {
        const model::HistoryTable& table = model.table(i);
        const model::HistoryTable::Location location = table.locate(keys.at(i));
        if (table.selected_slot() / model::HistoryTable::kWays != location.bucket ||
            table.selected_check() != location.check) {
            return false;
        }
    }
    return true;
}

// The keys are the keys. Before each byte, lookahead() from the true bytes,
// which is what prefetch_after() fetches, names the slot every table then
// selects at both nibbles of the byte after, and the context the model is
// in when that byte starts.
void test_prefetched_keys_are_the_keys_used() {
    for (const cpress::Bytes& data : hint_inputs()) {
        Mix9 model(Mix9::kMinLevel);
        std::optional<Mix9::Lookahead> ahead;  // for the byte about to be coded
        bool first_nibbles = true;
        bool second_nibbles = true;
        bool contexts = true;
        for (std::size_t i = 0; i < data.size(); ++i) {
            std::optional<Mix9::Lookahead> after;
            if (i + 1 < data.size()) after = model.lookahead(data[i], data[i + 1]);
            if (ahead) {
                contexts = contexts && ahead->context.history == model.context().history &&
                           ahead->context.words.word == model.context().words.word &&
                           ahead->context.words.previous == model.context().words.previous;
            }
            for (int shift = 7; shift >= 0; --shift) {
                if (ahead && shift == 7)
                    first_nibbles = first_nibbles && selects(model, ahead->first_nibble);
                if (ahead && shift == 3)
                    second_nibbles = second_nibbles && selects(model, ahead->second_nibble);
                static_cast<void>(model.predict());
                model.update((data[i] >> shift) & 1);
            }
            ahead = after;
        }
        CHECK(first_nibbles);
        CHECK(second_nibbles);
        CHECK(contexts);
    }
}

}  // namespace

void run_mix9_tests() {
    test_predictions_stay_in_range();
    test_deterministic();
    test_order2_cap_changes_nothing();
    test_next_words_matches_stage_3();
    test_hints_change_no_prediction();
    test_prefetched_keys_are_the_keys_used();
}

}  // namespace cpress::test
