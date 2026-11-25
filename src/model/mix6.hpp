// Mix6: the stage 2 predictor. It predicts each bit six times, from the last
// one to six bytes, and mixes the six predictions into one.
//
//   order n table   what followed the same last n bytes before (ContextTable)
//   mixer           how far to trust each order, given the previous byte
//
// Short contexts have seen a lot and know little; long contexts know a lot
// when they have been seen at all. Which to believe changes from byte to
// byte: after a space the long contexts know little, in the middle of a word
// they know a lot. So the mixer keeps one weight set per value of the
// previous byte and learns each separately.
//
// Memory: set by the level, kMinLevel to kMaxLevel. Each order's table has
// 2^level slots of 32 bytes, except that no table has more than twice as
// many slots as it has possible keys, which caps order 1 at 2^14. That is
// 192 KiB at level 10 and 640.5 MiB at level 22; see memory_bytes(). The
// mixer adds kWeightSets * kOrders * 4 bytes, 6 KiB.
//
// Update rule: every table's counter and the mixer's selected weights learn
// each bit; see counter.hpp and mixer.hpp.
//
// Technique: hashed context orders mixed in the logistic domain are the core
// of Matt Mahoney's lpaq1. Its contexts hold bit-history states where these
// hold counters, and it adds a match model and two secondary estimators:
// the subject of stage 3.
#pragma once

#include "model/context_table.hpp"
#include "model/logistic.hpp"
#include "model/mixer.hpp"
#include "model/predictor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace cpress::model {

namespace detail {

// log2 of the slots in the table for `order` at `level`: the level, but
// never more than the order can use; see usable_slot_bits().
[[nodiscard]] constexpr int slot_bits(int order, int level) noexcept {
    return std::min(level, usable_slot_bits(order));
}

}  // namespace detail

class Mix6 {
public:
    // The orders mixed: contexts of 1 to kOrders bytes.
    static constexpr int kOrders = 6;
    static_assert(kOrders <= kMaxOrder);

    // The level is log2 of the slots in each table but the first: order 1
    // has so few contexts that 2^14 slots are all it can use.
    static constexpr int kMinLevel = 10;
    static constexpr int kMaxLevel = 22;
    static_assert(detail::slot_bits(1, kMaxLevel) == 14);
    static_assert(detail::slot_bits(2, kMaxLevel) == kMaxLevel, "only order 1 is capped");

    // The smallest level with 2^level >= size, within the two limits: about
    // one slot per input byte in each table.
    [[nodiscard]] static constexpr int level_for_size(std::uint64_t size) noexcept {
        const int bits = std::bit_width(size == 0 ? 0 : size - 1);  // 2^bits >= size
        return std::clamp(bits, kMinLevel, kMaxLevel);
    }

    // Bytes of table a model at `level` allocates.
    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int level) noexcept {
        std::uint64_t bytes = 0;
        for (int order = 1; order <= kOrders; ++order) {
            bytes += ContextTable::memory_bytes(detail::slot_bits(order, level));
        }
        return bytes;
    }

    // Allocates the tables for `level`, in [kMinLevel, kMaxLevel]. Throws
    // std::bad_alloc if there is no memory.
    explicit Mix6(int level)
        : tables_(make_tables(level, std::make_index_sequence<kOrders>())), mixer_(kWeightSets) {
        select_slots();
    }

    [[nodiscard]] std::uint32_t predict() noexcept {
        Mixer<kOrders>::Stretched st{};
        for (std::size_t i = 0; i < tables_.size(); ++i) {
            st[i] = stretch(tables_[i].p12(node_));
        }
        return mixer_.mix(st);
    }

    void update(int bit) noexcept {
        assert(bit == 0 || bit == 1);
        mixer_.update(bit);
        for (ContextTable& table : tables_) {
            table.update(node_, bit);
        }
        node_ = (node_ * 2) + static_cast<std::uint32_t>(bit);
        if (node_ < kNibbleDone) return;

        // A nibble is complete, and node_ is 16 + its value.
        if (marker_ == kFirstNibble) {
            marker_ = node_;  // exactly the second nibble's marker
        } else {
            push_byte(((marker_ - kNibbleDone) << 4) | (node_ - kNibbleDone));
            marker_ = kFirstNibble;
        }
        select_slots();
        node_ = 1;
    }

private:
    // One weight set per value of the previous byte.
    static constexpr std::size_t kWeightSets = 256;

    // The marker of a first-nibble slot; see context_key().
    static constexpr std::uint32_t kFirstNibble = 0;

    // The table for each order, 1 to kOrders, in that order.
    template <std::size_t... Index>
    [[nodiscard]] static std::array<ContextTable, kOrders> make_tables(
        int level, std::index_sequence<Index...> /*orders*/) {
        assert(level >= kMinLevel && level <= kMaxLevel);
        return {ContextTable(detail::slot_bits(static_cast<int>(Index) + 1, level))...};
    }

    // Points every table at its slot for the nibble about to be coded.
    void select_slots() noexcept {
        for (std::size_t i = 0; i < tables_.size(); ++i) {
            tables_[i].select(context_key(history_, static_cast<int>(i) + 1, marker_));
        }
    }

    // A whole byte has been coded: it joins the history and picks the weight
    // set for the byte after it.
    void push_byte(std::uint32_t byte) noexcept {
        assert(byte <= 0xFF);
        history_ = (history_ << 8) | byte;
        mixer_.select(byte);
    }

    std::array<ContextTable, kOrders> tables_;  // tables_[i] is order i + 1
    Mixer<kOrders> mixer_;
    std::uint64_t history_ = 0;  // the last 8 bytes, newest in the low byte; 0 before the input
    std::uint32_t marker_ = kFirstNibble;  // which nibble is being coded; see context_key()
    std::uint32_t node_ = 1;               // position in that nibble's tree, 1 to 15
};

static_assert(Predictor<Mix6>);

}  // namespace cpress::model
