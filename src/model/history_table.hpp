// HistoryTable: the bit histories of one context input. It says which state
// (state_table.hpp) a context is in at each bit of the next byte; the state
// map beside it says what that state predicts.
//
// The table is an array of slots. A slot holds the states of one nibble in
// one context, numbered as stage 2 numbers them (node 1 is the first bit;
// node * 2 + bit is the next), and a check byte that says which context owns
// the slot:
//
//   slot     16 bytes: byte 0 the check, bytes 1 to 15 the states of nodes 1 to 15
//   bucket   kWays = 4 slots, 64 bytes
//
// A context's key is hashed to a bucket and to a check, the eight bits of
// the hash below the bucket index. A lookup takes the first slot of the
// bucket whose check matches. If none does, the slot that has learned least
// is cleared and given the check. A slot learns at node 1 on every visit,
// so the total count of its node 1 state measures how much it knows.
//
// Why a check. Two contexts that land on one unchecked slot share its
// history, and each sees the other's bits as noise. With a check a
// stranger is noticed, 255 times in 256, and gets a fresh slot instead.
// Why four ways. A bucket is one cache line once the table is aligned, so
// searching it costs no more memory traffic than reading one slot.
//
// An empty slot has check 0 and every state 0. A context whose check is 0
// adopts one as its own, which is harmless: the slot is in its initial
// state either way.
//
// Memory: 2^slot_bits * 16 bytes. Buckets are not yet aligned to cache
// lines, so one may straddle two; aligning them changes no prediction.
//
// Update rule: the state at the current node follows next_state().
//
// Technique: a 16-byte slot per nibble with an 8-bit check, searched among
// neighbouring slots within a 64-byte line, and replacing the slot whose
// first state has the lowest count, is the hash table of Matt Mahoney's
// lpaq1 (2007), which tries three slots where this tries four.
#pragma once

#include "model/context_table.hpp"
#include "model/probability_map.hpp"
#include "model/state_table.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cpress::model {

namespace detail {

// The p22 a state's own counts suggest: (2 * n1 + 1) / (2 * (n0 + n1) + 2),
// rounded down. For state 0 that is one half. The numerator is below
// 2^7 * 2^22, so 64 bits are plenty.
[[nodiscard]] constexpr std::uint32_t counts_p22(std::uint8_t state) noexcept {
    const std::uint64_t numerator = (2 * std::uint64_t{ones(state)}) + 1;
    const std::uint64_t denominator = (2 * std::uint64_t{zeros(state) + ones(state)}) + 2;
    return static_cast<std::uint32_t>((numerator << kP22Bits) / denominator);
}

[[nodiscard]] constexpr std::array<std::uint32_t, kStateCount> make_state_map_p22() noexcept {
    std::array<std::uint32_t, kStateCount> p22{};
    for (std::size_t state = 0; state < p22.size(); ++state) {
        p22[state] = counts_p22(static_cast<std::uint8_t>(state));
    }
    return p22;
}

}  // namespace detail

// Where a state map starts: each state's entry at the estimate its counts
// suggest, with count 0, so that its first update replaces most of the
// guess.
inline constexpr std::array<std::uint32_t, kStateCount> kStateMapInitialP22 =
    detail::make_state_map_p22();

// A state map: what each state predicts in one table, learned online.
[[nodiscard]] inline ProbabilityMap make_state_map() {
    return ProbabilityMap(kStateMapInitialP22);
}

class HistoryTable {
public:
    static constexpr std::size_t kSlotBytes = 16;
    static constexpr std::size_t kWays = 4;

    // The hash keeps 64 - bucket_bits and 56 - bucket_bits bits, and a shift
    // by 64 is undefined, so there are at least two buckets. The slot count
    // fits in 32 bits.
    static constexpr int kMinSlotBits = 3;
    static constexpr int kMaxSlotBits = 31;

    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int slot_bits) noexcept {
        return (std::uint64_t{1} << slot_bits) * kSlotBytes;
    }

    // A key's place in the table: its bucket, and the check that identifies
    // it there.
    struct Location {
        std::size_t bucket;
        std::uint8_t check;
    };

    // Allocates 2^slot_bits empty slots and selects slot 0. Throws
    // std::bad_alloc if there is no memory.
    explicit HistoryTable(int slot_bits)
        : bucket_bits_(slot_bits - kWayBits), slots_(std::size_t{1} << slot_bits) {
        assert(slot_bits >= kMinSlotBits && slot_bits <= kMaxSlotBits);
    }

    // Where `key` belongs. The 64-bit product wraps; that is the hash. Its
    // top bucket_bits bits are the bucket and the next eight the check.
    [[nodiscard]] Location locate(std::uint64_t key) const noexcept {
        const std::uint64_t hash = key * kHashMultiplier;
        return {.bucket = static_cast<std::size_t>(hash >> (64 - bucket_bits_)),
                .check = static_cast<std::uint8_t>((hash >> (56 - bucket_bits_)) & 0xFF)};
    }

    // Chooses the slot that state() and update() use, until the next
    // select(): the key's own slot, or a cleared one. Called once per nibble.
    void select(std::uint64_t key) noexcept {
        const Location location = locate(key);
        const std::size_t first = location.bucket * kWays;
        const std::span<Slot, kWays> bucket(&slots_[first], kWays);
        selected_ = first + claim(bucket, location.check);
    }

    // The state of `node`, 1 to 15, in the selected slot.
    [[nodiscard]] std::uint8_t state(std::uint32_t node) const noexcept {
        assert(node >= 1 && node < kNibbleDone);
        return slots_[selected_][node];
    }

    // Moves the state of `node` in the selected slot on by `bit`.
    void update(std::uint32_t node, int bit) noexcept {
        assert(node >= 1 && node < kNibbleDone);
        std::uint8_t& state = slots_[selected_][node];
        state = next_state(state, bit);
    }

    // For tests.
    [[nodiscard]] std::size_t slot_count() const noexcept {
        return slots_.size();
    }
    [[nodiscard]] std::size_t selected_slot() const noexcept {
        return selected_;
    }
    [[nodiscard]] std::uint8_t selected_check() const noexcept {
        return slots_[selected_][kCheckByte];
    }

private:
    // Byte 0 is the check; byte `node` is the state of that node.
    using Slot = std::array<std::uint8_t, kSlotBytes>;
    static constexpr std::size_t kCheckByte = 0;
    static constexpr int kWayBits = 2;
    static_assert(kWays == std::size_t{1} << kWayBits);
    static_assert(kSlotBytes == kNibbleDone, "the check, then one state per node");
    static_assert(sizeof(Slot) * kWays == 64, "a bucket is one cache line");

    // The way of `bucket` that `check` owns: the first whose check matches,
    // or else the one whose node 1 state has the lowest total count (the
    // first, on a tie), cleared and given the check.
    [[nodiscard]] static std::size_t claim(std::span<Slot, kWays> bucket,
                                           std::uint8_t check) noexcept {
        for (std::size_t way = 0; way < kWays; ++way) {
            if (bucket[way][kCheckByte] == check) return way;
        }
        std::size_t least = 0;
        for (std::size_t way = 1; way < kWays; ++way) {
            if (priority(bucket[way]) < priority(bucket[least])) least = way;
        }
        bucket[least].fill(0);
        bucket[least][kCheckByte] = check;
        return least;
    }

    // How much a slot has learned: the total count of its node 1 state,
    // which moves on every visit.
    [[nodiscard]] static std::uint32_t priority(const Slot& slot) noexcept {
        const std::uint8_t first = slot[1];
        return zeros(first) + ones(first);
    }

    int bucket_bits_;  // slot_bits - kWayBits, in [1, 29]: the shifts above stay below 64
    std::vector<Slot> slots_;
    std::size_t selected_ = 0;  // an index, not a pointer, so a table can be copied or moved
};

}  // namespace cpress::model
