// The bit-history states: what a context remembers of the bits it has seen,
// in one byte.
//
// A probability says what the bits in a context averaged to. It cannot say
// how much evidence that average rests on, or whether the bits have just
// changed their mind. A state records how many zeros and how many ones the
// context has seen, (n0, n1), with older evidence discounted, and leaves
// what that means to a learned table, a state map (see history_table.hpp).
// The same state can then mean different things in different tables: three
// ones and no zeros is strong evidence after six bytes of context and weak
// evidence after one.
//
// Transitions. On a bit b, with o the other bit:
//
//   n_b = n_b + 1
//   n_o = discount(n_o)        n, or n / 2 + 1 above 2
//   n_b = min(n_b, cap(n_o))   kRunCap if n_o is 0, kMixedCap otherwise
//
// When the bit changes, the evidence for the old bit is halved rather than
// kept, so a context that switches behaviour settles on the new one within
// a few bits. Counts of 1 and 2 are too few to halve. A run, all zeros or
// all ones, may count higher than a mixed history, because how long a
// deterministic run has lasted is the most useful thing a state can tell
// its map. The cap applies only to the count just incremented: a zero after
// a run of ones gives (1, discount(kRunCap)), with n1 above kMixedCap.
//
// Numbering. State 0 is (0, 0), so a zeroed slot holds fresh states. The
// rest are numbered in the order a breadth-first search from state 0 finds
// them, following bit 0 before bit 1. The table is generated from the rule
// above at compile time; nothing in it is typed in.
//
// Memory: kStateCount entries of two transitions and two counts, 724 bytes.
//
// Technique: a pair of counts in a byte, with the opposite count discounted
// when a bit is seen, is the idea behind the state tables of Matt Mahoney's
// PAQ8 (paq8l, 2007) and lpaq1 (2007). Theirs also remember the last bit
// and spell out every history of up to four bits, which gives 253 states.
// The rule, caps and numbering here are this project's own.
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace cpress::model {

// The most a count can reach in a run, where the other count is 0.
inline constexpr std::uint32_t kRunCap = 40;

// The most a count can reach once the other count is not 0.
inline constexpr std::uint32_t kMixedCap = 20;

static_assert(kMixedCap <= kRunCap);

namespace detail {

// A state's counts: how many zeros and ones it stands for.
struct BitCounts {
    std::uint32_t zeros = 0;
    std::uint32_t ones = 0;

    friend constexpr bool operator==(const BitCounts&, const BitCounts&) = default;
};

// The count of a bit after the other bit has been seen: halved, plus one,
// unless it is too small to halve.
[[nodiscard]] constexpr std::uint32_t discount(std::uint32_t count) noexcept {
    return count <= 2 ? count : (count / 2) + 1;
}

// The most a count can reach, given the other count.
[[nodiscard]] constexpr std::uint32_t count_cap(std::uint32_t other) noexcept {
    return other == 0 ? kRunCap : kMixedCap;
}

// The counts after `bit` has been seen: the rule at the top of this file.
[[nodiscard]] constexpr BitCounts next_counts(BitCounts counts, int bit) noexcept {
    assert(bit == 0 || bit == 1);
    std::uint32_t& seen = bit != 0 ? counts.ones : counts.zeros;
    std::uint32_t& other = bit != 0 ? counts.zeros : counts.ones;
    seen += 1;
    other = discount(other);
    seen = std::min(seen, count_cap(other));
    return counts;
}

// A state must fit in a byte.
inline constexpr std::size_t kMaxStates = 256;

// The search's working table, sized for the most states a byte can name.
struct GeneratedStates {
    std::array<BitCounts, kMaxStates> counts{};
    std::array<std::array<std::uint8_t, 2>, kMaxStates> next{};
    std::size_t size = 0;
};

// Every state reachable from (0, 0), numbered breadth first. The states
// found so far are the search's queue: each is expanded, bit 0 first, after
// every state found before it. No count exceeds kRunCap, so `index_of`
// covers every pair the rule can produce. Indexing either array out of
// range, which more than kMaxStates states would do, is not a constant
// expression, so a rule with too many states fails to compile.
[[nodiscard]] constexpr GeneratedStates generate_states() noexcept {
    constexpr std::size_t kCountValues = kRunCap + 1;
    constexpr std::size_t kNotFound = kMaxStates;
    std::array<std::array<std::size_t, kCountValues>, kCountValues> index_of{};
    for (auto& row : index_of) row.fill(kNotFound);

    GeneratedStates states;
    states.size = 1;  // state 0, (0, 0), as value-initialised
    index_of[0][0] = 0;
    for (std::size_t state = 0; state < states.size; ++state) {
        for (int bit = 0; bit <= 1; ++bit) {
            const BitCounts next = next_counts(states.counts[state], bit);
            std::size_t& index = index_of[next.zeros][next.ones];
            if (index == kNotFound) {
                index = states.size;
                states.counts[states.size] = next;
                ++states.size;
            }
            states.next[state][static_cast<std::size_t>(bit)] = static_cast<std::uint8_t>(index);
        }
    }
    return states;
}

inline constexpr GeneratedStates kGeneratedStates = generate_states();

}  // namespace detail

// The number of states the rule produces.
inline constexpr int kStateCount = static_cast<int>(detail::kGeneratedStates.size);
static_assert(kStateCount == 181, "the rule and caps of docs/stages/stage-3.md give 181 states");
static_assert(kStateCount <= static_cast<int>(detail::kMaxStates), "a state fits a byte");
static_assert(detail::kGeneratedStates.counts[0] == detail::BitCounts{},
              "state 0 is (0, 0), so a zeroed slot holds fresh states");

namespace detail {

// One state, as kept at run time: where each bit leads, and its counts.
// Every count is at most kRunCap, so it fits a byte.
struct State {
    std::array<std::uint8_t, 2> next;
    std::uint8_t zeros;
    std::uint8_t ones;
};

static_assert(kRunCap <= std::numeric_limits<std::uint8_t>::max());

[[nodiscard]] constexpr std::array<State, kStateCount> compact_states() noexcept {
    std::array<State, kStateCount> states{};
    for (std::size_t i = 0; i < states.size(); ++i) {
        states[i] = {.next = kGeneratedStates.next[i],
                     .zeros = static_cast<std::uint8_t>(kGeneratedStates.counts[i].zeros),
                     .ones = static_cast<std::uint8_t>(kGeneratedStates.counts[i].ones)};
    }
    return states;
}

inline constexpr std::array<State, kStateCount> kStates = compact_states();

}  // namespace detail

// The state after `bit` has been seen in `state`.
[[nodiscard]] constexpr std::uint8_t next_state(std::uint8_t state, int bit) noexcept {
    assert(state < kStateCount);
    assert(bit == 0 || bit == 1);
    return detail::kStates[state].next[static_cast<std::size_t>(bit)];
}

// n0: the zeros `state` stands for.
[[nodiscard]] constexpr std::uint32_t zeros(std::uint8_t state) noexcept {
    assert(state < kStateCount);
    return detail::kStates[state].zeros;
}

// n1: the ones `state` stands for.
[[nodiscard]] constexpr std::uint32_t ones(std::uint8_t state) noexcept {
    assert(state < kStateCount);
    return detail::kStates[state].ones;
}

}  // namespace cpress::model
