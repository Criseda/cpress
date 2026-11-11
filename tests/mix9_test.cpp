// Mix9 tests: valid predictions on any input, determinism, and the level
// arithmetic.
#include "check.hpp"

#include "model/context_table.hpp"
#include "model/history_table.hpp"
#include "model/mix9.hpp"
#include "model/predictor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
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

}  // namespace

void run_mix9_tests() {
    test_predictions_stay_in_range();
    test_deterministic();
    test_order2_cap_changes_nothing();
}

}  // namespace cpress::test
