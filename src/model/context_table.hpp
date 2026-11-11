// ContextTable: the model for one context order. It predicts each bit from
// what followed the same last n bytes earlier in the input.
//
// The table is an array of slots. A slot holds the predictions for one
// half-byte (nibble) in one context: the fifteen nodes of the binary tree
// over the nibble's four bits, numbered as Order0 numbers a whole byte
// (node 1 is the first bit; node * 2 + bit is the next), plus entry 0, which
// is unused and reserved for stage 3's collision detection. So a table is
// looked up twice per byte, not eight times, and the four bits of a nibble
// share one cache miss.
//
// A context is hashed straight to its slot. Nothing detects two contexts
// landing on the same one: they share its counters, and each sees the
// other's history as noise.
//
// Memory: 2^slot_bits slots of kSlotCounters * 2 = 32 bytes each. A slot is
// half a cache line and, until stage 4 aligns the table, may straddle two.
//
// Update rule: the counter at the current node learns the bit; see
// counter.hpp.
//
// Technique: one hashed slot per nibble is the layout of Matt Mahoney's
// lpaq1, which keeps bit-history states where this keeps counters. The hash
// is Fibonacci hashing: Knuth, The Art of Computer Programming, vol. 3,
// section 6.4.
#pragma once

#include "model/counter.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cpress::model {

// A nibble's tree has nodes 1 to 15. After its four bits the node is
// 16 + the nibble, one of kNibbleDone to 2 * kNibbleDone - 1.
inline constexpr std::uint32_t kNibbleDone = 16;

// What a slot is for, beyond its context: 0 for the first nibble of a byte,
// or, for the second, the node the first nibble ended on (16 + the high
// nibble). It takes kMarkerBits.
inline constexpr int kMarkerBits = 5;
static_assert((2 * kNibbleDone) - 1 < (1U << kMarkerBits));

// The longest context a 64-bit key has room for beside the marker: 7 bytes.
inline constexpr int kMaxOrder = (64 - kMarkerBits) / 8;

// The key of a slot: the last `order` bytes of `history` (newest in the low
// byte) and the marker.
[[nodiscard]] constexpr std::uint64_t context_key(std::uint64_t history, int order,
                                                  std::uint32_t marker) noexcept {
    assert(order >= 1 && order <= kMaxOrder);
    assert(marker == 0 || (marker >= kNibbleDone && marker < 2 * kNibbleDone));
    const std::uint64_t context = history & ((std::uint64_t{1} << (8 * order)) - 1);
    return (context << kMarkerBits) | marker;
}

// 2^64 divided by the golden ratio. Multiplying by it and keeping the top
// bits spreads keys that differ only in their low bits, as neighbouring
// contexts do, across the whole table.
inline constexpr std::uint64_t kHashMultiplier = 0x9E3779B97F4A7C15;

class ContextTable {
public:
    static constexpr std::size_t kSlotCounters = 16;
    static constexpr int kMinSlotBits = 1;
    static constexpr int kMaxSlotBits = 31;  // the slot count fits in 32 bits

    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int slot_bits) noexcept {
        return (std::uint64_t{1} << slot_bits) * sizeof(Slot);
    }

    // Allocates 2^slot_bits slots, every counter at one half, and selects
    // the slot of key 0. Throws std::bad_alloc if there is no memory.
    explicit ContextTable(int slot_bits)
        : hash_shift_(64 - slot_bits), slots_(std::size_t{1} << slot_bits) {
        assert(slot_bits >= kMinSlotBits && slot_bits <= kMaxSlotBits);
    }

    // Chooses the slot that p12() and update() use, until the next select().
    // Called once per nibble. The 64-bit product wraps; that is the hash.
    void select(std::uint64_t key) noexcept {
        selected_ = static_cast<std::size_t>((key * kHashMultiplier) >> hash_shift_);
    }

    // P(next bit = 1) at `node` of the selected slot, in [kProbMin, kProbMax].
    [[nodiscard]] std::uint32_t p12(std::uint32_t node) const noexcept {
        assert(node >= 1 && node < kNibbleDone);
        return slots_[selected_][node].p12();
    }

    void update(std::uint32_t node, int bit) noexcept {
        assert(node >= 1 && node < kNibbleDone);
        slots_[selected_][node].update(bit);
    }

    // For tests.
    [[nodiscard]] std::size_t slot_count() const noexcept {
        return slots_.size();
    }
    [[nodiscard]] std::size_t selected_slot() const noexcept {
        return selected_;
    }

private:
    using Slot = std::array<Counter, kSlotCounters>;
    static_assert(kSlotCounters == kNibbleDone, "one counter per node, and entry 0");
    static_assert(sizeof(Slot) == 32, "a slot is half a cache line");

    int hash_shift_;  // 64 - slot_bits, in [33, 63]: keeps the top slot_bits bits
    std::vector<Slot> slots_;
    std::size_t selected_ = 0;  // an index, not a pointer, so a table can be copied or moved
};

}  // namespace cpress::model
