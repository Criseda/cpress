// HistoryTable tests: a context keeps its slot, the least-trained slot of a
// full bucket is the one replaced, and every index is in range.
#include "check.hpp"

#include "model/context_table.hpp"
#include "model/history_table.hpp"
#include "model/mix9.hpp"
#include "model/state_table.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace cpress::test {
namespace {

using model::HistoryTable;
using model::kNibbleDone;

constexpr std::uint64_t kAllOnes = std::numeric_limits<std::uint64_t>::max();

// Feeds `bits` ones to every node of the selected slot.
void train(HistoryTable& table, int bits) {
    for (int i = 0; i < bits; ++i) {
        for (std::uint32_t node = 1; node < kNibbleDone; ++node) table.update(node, 1);
    }
}

// True if every node of the selected slot is in the state `bits` ones lead
// to from state 0.
bool holds_training(const HistoryTable& table, int bits) {
    std::uint8_t expected = 0;
    for (int i = 0; i < bits; ++i) expected = model::next_state(expected, 1);
    bool holds = true;
    for (std::uint32_t node = 1; node < kNibbleDone; ++node) {
        holds = holds && table.state(node) == expected;
    }
    return holds;
}

// A trained context finds its slot, states intact, after thousands of other
// keys have been looked up. Those keys learn nothing, so wherever one shares
// the bucket it is the one replaced.
void test_a_context_keeps_its_slot() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(7);
    HistoryTable table(model::Mix9::kMinLevel);
    constexpr std::uint64_t kKey = 0xC0FFEE;
    table.select(kKey);
    const std::size_t slot = table.selected_slot();
    train(table, 3);

    for (int i = 0; i < 10'000; ++i) table.select(rng());
    table.select(kKey);
    CHECK(table.selected_slot() == slot);
    CHECK(holds_training(table, 3));
}

// Five keys that share a bucket, with five different nonzero checks, found
// by searching. The bucket of a key does not depend on what the table holds.
std::array<std::uint64_t, 5> keys_in_one_bucket(const HistoryTable& table) {
    std::array<std::uint64_t, 5> keys{};
    std::vector<std::uint8_t> checks;
    const std::size_t bucket = table.locate(0).bucket;
    std::size_t found = 0;
    for (std::uint64_t key = 1; found < keys.size(); ++key) {
        const HistoryTable::Location location = table.locate(key);
        if (location.bucket != bucket || location.check == 0) continue;
        if (std::ranges::find(checks, location.check) != checks.end()) continue;
        checks.push_back(location.check);
        keys.at(found++) = key;
    }
    return keys;
}

// The first four keys fill the bucket, trained by different amounts, the
// least not in the first way. The fifth replaces the least-trained, and
// gets its slot zeroed and with its own check; the other three keep theirs.
void test_the_least_trained_slot_is_replaced() {
    HistoryTable table(model::Mix9::kMinLevel);
    const std::array<std::uint64_t, 5> keys = keys_in_one_bucket(table);
    constexpr std::array<int, 4> kTraining{3, 1, 4, 2};

    std::array<std::size_t, 4> slots{};
    for (std::size_t i = 0; i < kTraining.size(); ++i) {
        table.select(keys.at(i));
        slots.at(i) = table.selected_slot();
        train(table, kTraining.at(i));
    }
    CHECK(slots[0] / HistoryTable::kWays == slots[3] / HistoryTable::kWays);

    // Key 1 has the fewest bits at node 1, so its slot goes.
    table.select(keys[4]);
    CHECK(table.selected_slot() == slots[1]);
    CHECK(table.selected_check() == table.locate(keys[4]).check);
    CHECK(holds_training(table, 0));

    for (const std::size_t survivor : {0U, 2U, 3U}) {
        table.select(keys.at(survivor));
        CHECK(table.selected_slot() == slots.at(survivor));
        CHECK(holds_training(table, kTraining.at(survivor)));
    }
}

// Every key, including 0 and all ones, selects a slot inside the table, in
// the bucket locate() names: at the smallest table there can be and at the
// largest a model asks for.
void test_indices_are_in_range() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(9);
    for (const int slot_bits : {HistoryTable::kMinSlotBits, model::Mix9::kMaxLevel}) {
        HistoryTable table(slot_bits);
        CHECK(table.slot_count() == std::size_t{1} << slot_bits);
        CHECK(HistoryTable::memory_bytes(slot_bits) ==
              table.slot_count() * HistoryTable::kSlotBytes);

        bool in_range = true;
        auto check_key = [&](std::uint64_t key) {
            const HistoryTable::Location location = table.locate(key);
            table.select(key);
            in_range = in_range && location.bucket < table.slot_count() / HistoryTable::kWays &&
                       table.selected_slot() / HistoryTable::kWays == location.bucket;
        };
        check_key(0);
        check_key(1);
        check_key(kAllOnes);
        for (int i = 0; i < 10'000; ++i) check_key(rng());
        CHECK(in_range);
    }
}

}  // namespace

void run_history_table_tests() {
    test_a_context_keeps_its_slot();
    test_the_least_trained_slot_is_replaced();
    test_indices_are_in_range();
}

}  // namespace cpress::test
