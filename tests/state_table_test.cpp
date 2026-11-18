// State table tests: its size, that every transition follows the rule, and
// the path of a run and of the change that ends it.
#include "check.hpp"

#include "model/state_table.hpp"

#include <algorithm>
#include <cstdint>

namespace cpress::test {
namespace {

using model::kMixedCap;
using model::kRunCap;
using model::kStateCount;

// The rule of docs/stages/stage-3.md, written out again here so that the
// table is checked against it rather than against itself.
struct Counts {
    std::uint32_t zeros;
    std::uint32_t ones;
    friend constexpr bool operator==(const Counts&, const Counts&) = default;
};

constexpr std::uint32_t halve(std::uint32_t n) {
    return n <= 2 ? n : (n / 2) + 1;
}

constexpr Counts rule(Counts counts, int bit) {
    if (bit != 0) {
        const std::uint32_t zeros = halve(counts.zeros);
        return {zeros, std::min(counts.ones + 1, zeros == 0 ? kRunCap : kMixedCap)};
    }
    const std::uint32_t ones = halve(counts.ones);
    return {std::min(counts.zeros + 1, ones == 0 ? kRunCap : kMixedCap), ones};
}

Counts counts_of(std::uint8_t state) {
    return {model::zeros(state), model::ones(state)};
}

static_assert(kStateCount <= 256, "a state fits a byte");
static_assert(model::zeros(0) == 0 && model::ones(0) == 0, "state 0 is (0, 0)");

// Every state leads, on each bit, to a state inside the table whose counts
// are what the rule gives; and no two states have the same counts.
void test_transitions_follow_the_rule() {
    bool in_range = true;
    bool follows = true;
    bool distinct = true;
    for (int state = 0; state < kStateCount; ++state) {
        const auto from = static_cast<std::uint8_t>(state);
        for (int bit = 0; bit <= 1; ++bit) {
            const std::uint8_t to = model::next_state(from, bit);
            in_range = in_range && to < kStateCount;
            follows = follows && counts_of(to) == rule(counts_of(from), bit);
        }
        for (int other = 0; other < state; ++other) {
            distinct =
                distinct && !(counts_of(from) == counts_of(static_cast<std::uint8_t>(other)));
        }
    }
    CHECK(in_range);
    CHECK(follows);
    CHECK(distinct);
}

// Breadth first, bit 0 first: from state 0, a 0 finds state 1 and a 1
// finds state 2.
void test_numbering_is_breadth_first() {
    CHECK(model::next_state(0, 0) == 1);
    CHECK(model::next_state(0, 1) == 2);
    CHECK((counts_of(1) == Counts{1, 0}));
    CHECK((counts_of(2) == Counts{0, 1}));
}

// A run of ones reaches (0, kRunCap) and stays there. A zero then gives
// (1, halve(kRunCap)) = (1, 21): the cap applies only to the count just
// incremented. Each further zero halves the ones, (2, 11), (3, 6), (4, 4),
// (5, 3), (6, 2), until two ones remain, which are never halved, and the
// zeros climb to kMixedCap.
void test_a_run_and_its_end() {
    std::uint8_t state = 0;
    for (int i = 0; i < 100; ++i) state = model::next_state(state, 1);
    CHECK((counts_of(state) == Counts{0, kRunCap}));
    CHECK(model::next_state(state, 1) == state);

    Counts expected{0, kRunCap};
    bool follows = true;
    for (std::uint32_t zeros = 1; zeros <= kMixedCap + 5; ++zeros) {
        state = model::next_state(state, 0);
        expected = {std::min(zeros, kMixedCap), halve(expected.ones)};
        follows = follows && counts_of(state) == expected;
    }
    CHECK(follows);
    CHECK((counts_of(state) == Counts{kMixedCap, 2}));
    CHECK(model::next_state(state, 0) == state);

    // The worked figures above, for the constants as they are.
    static_assert(kRunCap == 40 && kMixedCap == 20);
    CHECK((rule({0, kRunCap}, 0) == Counts{1, 21}));
    CHECK((rule({5, 3}, 0) == Counts{6, 2}));
}

}  // namespace

void run_state_table_tests() {
    test_transitions_follow_the_rule();
    test_numbering_is_breadth_first();
    test_a_run_and_its_end();
}

}  // namespace cpress::test
