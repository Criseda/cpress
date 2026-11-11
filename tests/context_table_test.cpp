// ContextTable tests: keys, slot selection at both ends of the size range,
// and the independence of a slot's counters.
#include "check.hpp"

#include "model/context_table.hpp"
#include "model/mix6.hpp"
#include "model/predictor.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <random>

namespace cpress::test {
namespace {

using model::context_key;
using model::ContextTable;
using model::kNibbleDone;

constexpr std::uint32_t kHalf = model::kProbOne / 2;
constexpr std::uint64_t kAllOnes = std::numeric_limits<std::uint64_t>::max();

// A key is the last `order` bytes and the marker, and nothing else.
constexpr std::uint64_t kHistory = 0x0807060504030201;
static_assert(model::kMaxOrder == 7);
static_assert(context_key(kHistory, 1, 0) == (std::uint64_t{0x01} << model::kMarkerBits));
static_assert(context_key(kHistory, 3, 0) == (std::uint64_t{0x030201} << model::kMarkerBits));
static_assert(context_key(kHistory, 6, 31) ==
              ((std::uint64_t{0x060504030201} << model::kMarkerBits) | 31));
// The longest order still fits: every bit of its context and marker is kept.
static_assert(context_key(kAllOnes, model::kMaxOrder, 31) ==
              kAllOnes >> (64 - (8 * model::kMaxOrder) - model::kMarkerBits));
static_assert(context_key(kHistory, 2, 0) != context_key(kHistory, 2, kNibbleDone));

// The same key always selects the same slot, and every key selects a slot
// inside the table: at the smallest table there can be, and at the smallest
// and largest a model asks for.
void test_selection_is_stable_and_in_range() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(8);
    for (const int slot_bits :
         {ContextTable::kMinSlotBits, model::Mix6::kMinLevel, model::Mix6::kMaxLevel}) {
        ContextTable table(slot_bits);
        CHECK(table.slot_count() == std::size_t{1} << slot_bits);
        CHECK(ContextTable::memory_bytes(slot_bits) == table.slot_count() * 32);

        bool in_range = true;
        bool stable = true;
        auto check_key = [&](std::uint64_t key) {
            table.select(key);
            const std::size_t slot = table.selected_slot();
            in_range = in_range && slot < table.slot_count();
            table.select(~key);
            table.select(key);
            stable = stable && table.selected_slot() == slot;
        };
        check_key(0);
        check_key(1);
        check_key(kAllOnes);
        for (int i = 0; i < 10'000; ++i) check_key(rng());
        CHECK(in_range);
        CHECK(stable);
    }
}

// The hash must spread keys that differ only in their low bits, as the
// contexts of neighbouring bytes do: 256 consecutive order-1 keys land in
// 256 different slots of a table with room for them.
void test_neighbouring_keys_are_spread() {
    ContextTable table(model::Mix6::kMinLevel);
    std::array<bool, std::size_t{1} << model::Mix6::kMinLevel> used{};
    int distinct = 0;
    for (std::uint64_t byte = 0; byte < 256; ++byte) {
        table.select(context_key(byte, 1, 0));
        if (!used.at(table.selected_slot())) ++distinct;
        used.at(table.selected_slot()) = true;
    }
    CHECK(distinct == 256);
}

void test_update_touches_one_counter() {
    ContextTable table(model::Mix6::kMinLevel);
    constexpr std::uint64_t kKey = 0x1234;
    constexpr std::uint32_t kNode = 5;
    table.select(kKey);
    table.update(kNode, 1);

    CHECK(table.p12(kNode) > kHalf);
    int untouched = 0;
    for (std::uint32_t node = 1; node < kNibbleDone; ++node) {
        if (node != kNode && table.p12(node) == kHalf) ++untouched;
    }
    CHECK(untouched == 14);

    // Another slot has seen nothing; coming back finds the update.
    table.select(kKey + 1);
    CHECK(table.selected_slot() != 0);
    CHECK(table.p12(kNode) == kHalf);
    table.select(kKey);
    CHECK(table.p12(kNode) > kHalf);
}

// In the smallest table every key shares one of two slots. The contexts
// spoil each other's statistics, but every prediction is still valid.
void test_shared_slots_stay_valid() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(10);
    ContextTable table(ContextTable::kMinSlotBits);
    bool valid = true;
    for (int i = 0; i < 100'000; ++i) {
        table.select(rng());
        const auto node = static_cast<std::uint32_t>(1 + (rng() % (kNibbleDone - 1)));
        table.update(node, static_cast<int>(rng() & 1));
        const std::uint32_t p12 = table.p12(node);
        valid = valid && p12 >= model::kProbMin && p12 <= model::kProbMax;
    }
    CHECK(valid);
}

}  // namespace

void run_context_table_tests() {
    test_selection_is_stable_and_in_range();
    test_neighbouring_keys_are_spread();
    test_update_touches_one_counter();
    test_shared_slots_stay_valid();
}

}  // namespace cpress::test
