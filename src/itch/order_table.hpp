// OrderTable: the orders that are alive, found by reference number, and for
// each stock the list of its live orders, newest first.
//
// What an order is. An add message puts an order on the exchange's book: so
// many shares of one stock, to buy or to sell, at a price. It gives the
// order a reference number, and every later message about the order (an
// execution, a cancel, a delete, a replace) carries that 64-bit number and
// little else. A model that remembers the order knows the rest: its stock,
// side, shares and price need no bits. And the number itself, which is
// nearly incompressible, can be replaced by something small: the order's
// place among its stock's live orders. Most messages are about recent
// orders, so most places are small.
//
//   entry     32 bytes: reference, newer and older in its stock's list (as
//             entry indices), shares, price, locate, side, and whether the
//             entry is in use
//   bucket    kWays = 8 entries, 256 bytes, four cache lines
//   table     2^order_bits entries; a reference's bucket is the top
//             order_bits - 3 bits of reference * kHashMultiplier
//   add       the first free entry of the bucket; if there is none, the
//             order with the smallest reference is removed first, exactly as
//             remove() would remove it
//
// Why bounded. The decoder allocates the table from a level stored in the
// archive, checked against a compile-time maximum, like every other table
// (docs/style.md, Errors). It cannot grow.
// Why the smallest reference goes. References grow with time, so it is the
// oldest order in the bucket and the one least likely to be named again.
// What an eviction costs. A later message that names the order does not
// find it, and the model codes the reference in full. Nothing breaks.
//
// The lists are doubly linked through the table. An entry never moves, so
// the indices stay good. The table keeps each stock's newest order and
// count itself, because an eviction, or an order named in full, can belong
// to a stock other than the one the message at hand is about.
//
// Invariant: a stock's list holds exactly its live entries, `live` of them;
// following `older` from `newest` reaches kNone after `live` steps. Every
// operation keeps it, whatever references it is given: a crafted archive
// can ask for nothing here that walks off a list.
//
// Memory: 2^order_bits * 32 bytes through TableAllocator, and 8 bytes per
// stock locate.
#pragma once

#include "itch/schema.hpp"
#include "model/context_table.hpp"
#include "model/table_allocator.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cpress::itch {

// A live order. The first five members are the table's; the rest are what
// the messages said about the order.
struct Order {
    std::uint64_t reference;
    std::uint32_t newer;  // the next newer order of the same stock, or OrderTable::kNone
    std::uint32_t older;  // the next older, or kNone
    std::uint32_t shares;
    std::uint32_t price;
    std::uint16_t locate;
    std::uint8_t side;  // kBuy or kSell
    bool in_use;

    [[nodiscard]] bool operator==(const Order&) const noexcept = default;
};

inline constexpr std::uint8_t kBuy = 0;
inline constexpr std::uint8_t kSell = 1;

// For the benchmark: the orders evicted from the order table of every
// compression this process has finished. A table too small for its file
// shows here. Like model::large_page_bytes_granted(), it counts for the
// whole process, so a caller takes the difference across what it measures.
// Defined in src/codecs.cpp, where codec 4 adds each compression's count.
[[nodiscard]] std::uint64_t evictions_recorded() noexcept;

class OrderTable {
public:
    static constexpr std::size_t kWays = 8;
    static constexpr std::uint32_t kNone = 0xFFFFFFFF;  // no order: the end of a list

    // At least two buckets, since a shift by 64 is undefined; and every
    // entry index is below kNone.
    static constexpr int kMinOrderBits = 4;
    static constexpr int kMaxOrderBits = 31;

    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int order_bits) noexcept {
        return ((std::uint64_t{1} << order_bits) * sizeof(Order)) + (kLocates * sizeof(List));
    }

    // Allocates 2^order_bits entries, none in use. Throws std::bad_alloc if
    // there is no memory.
    explicit OrderTable(int order_bits)
        : bucket_shift_(64 - (order_bits - kWayBits)),
          entries_(std::size_t{1} << order_bits),
          lists_(kLocates) {
        assert(order_bits >= kMinOrderBits && order_bits <= kMaxOrderBits);
    }

    // The live order with this reference, or nullptr.
    [[nodiscard]] Order* find(std::uint64_t reference) noexcept {
        const std::size_t first = bucket_of(reference);
        for (std::size_t way = 0; way < kWays; ++way) {
            Order& order = entries_[first + way];
            if (order.in_use && order.reference == reference) return &order;
        }
        return nullptr;
    }

    // Adds an order as its stock's newest. An order already there under the
    // same reference is removed first: the feed never reuses one, but other
    // input can.
    void add(std::uint64_t reference, std::uint32_t shares, std::uint32_t price,
             std::uint16_t locate, std::uint8_t side) noexcept {
        if (Order* same = find(reference)) remove(*same);
        const std::size_t index = free_entry(bucket_of(reference));
        List& list = lists_[locate];
        entries_[index] = {.reference = reference,
                           .newer = kNone,
                           .older = list.newest,
                           .shares = shares,
                           .price = price,
                           .locate = locate,
                           .side = side,
                           .in_use = true};
        if (list.newest != kNone) entries_[list.newest].newer = static_cast<std::uint32_t>(index);
        list.newest = static_cast<std::uint32_t>(index);
        ++list.live;
    }

    // Removes a live order of this table from the table and from its
    // stock's list.
    void remove(Order& order) noexcept {
        assert(order.in_use);
        List& list = lists_[order.locate];
        if (order.newer != kNone) {
            entries_[order.newer].older = order.older;
        } else {
            list.newest = order.older;
        }
        if (order.older != kNone) entries_[order.older].newer = order.newer;
        --list.live;
        order.in_use = false;
    }

    // How many live orders the stock has.
    [[nodiscard]] std::uint32_t live(std::uint16_t locate) const noexcept {
        return lists_[locate].live;
    }

    // The order `place` steps from the stock's newest; place < live(locate).
    // The walk is `place` steps, which the invariant keeps on the list.
    [[nodiscard]] Order& at(std::uint16_t locate, std::uint32_t place) noexcept {
        assert(place < lists_[locate].live);
        std::uint32_t index = lists_[locate].newest;
        for (std::uint32_t step = 0; step < place; ++step) {
            assert(index != kNone);
            index = entries_[index].older;
        }
        assert(index != kNone);
        return entries_[index];
    }

    // Where place_of() found an order.
    struct Placed {
        std::uint32_t place;  // steps from the stock's newest, or the limit
        Order* order;         // the order at that place, or nullptr at the limit
    };

    // The place of `reference` among the stock's live orders and the order
    // there, if it is within `limit` of the newest; otherwise `limit` and no
    // order. At most `limit` steps: the order comes from the same walk, so
    // a caller that wants both does not walk again with at().
    [[nodiscard]] Placed place_of(std::uint16_t locate, std::uint64_t reference,
                                  std::uint32_t limit) noexcept {
        std::uint32_t index = lists_[locate].newest;
        for (std::uint32_t place = 0; place < limit && index != kNone; ++place) {
            if (entries_[index].reference == reference) return {place, &entries_[index]};
            index = entries_[index].older;
        }
        return {limit, nullptr};
    }

    // Orders removed to make room for another.
    [[nodiscard]] std::uint64_t evictions() const noexcept {
        return evictions_;
    }

    // The same orders in the same entries, and the same lists.
    [[nodiscard]] bool operator==(const OrderTable& other) const noexcept {
        return lists_ == other.lists_ && evictions_ == other.evictions_ &&
               std::ranges::equal(entries_, other.entries_, same_entry);
    }

    // For tests.
    [[nodiscard]] std::size_t entry_count() const noexcept {
        return entries_.size();
    }
    [[nodiscard]] std::size_t bucket_index(std::uint64_t reference) const noexcept {
        return bucket_of(reference) / kWays;
    }

private:
    // A stock's live orders.
    struct List {
        std::uint32_t newest = kNone;
        std::uint32_t live = 0;

        [[nodiscard]] bool operator==(const List&) const noexcept = default;
    };

    static constexpr int kWayBits = 3;
    static_assert(kWays == std::size_t{1} << kWayBits);
    static_assert(sizeof(Order) == 32, "eight entries are four cache lines");
    static_assert((sizeof(Order) * kWays) % model::kCacheLineBytes == 0);

    // An entry not in use may hold what its last order left behind, which
    // is not part of the table's state.
    [[nodiscard]] static bool same_entry(const Order& a, const Order& b) noexcept {
        return a.in_use == b.in_use && (!a.in_use || a == b);
    }

    // The index of the first entry of the reference's bucket. The 64-bit
    // product wraps; that is the hash.
    [[nodiscard]] std::size_t bucket_of(std::uint64_t reference) const noexcept {
        const std::uint64_t hash = reference * model::kHashMultiplier;
        return static_cast<std::size_t>(hash >> bucket_shift_) * kWays;
    }

    // An entry of the bucket that is not in use: the first one, or, if all
    // are in use, the one with the smallest reference, evicted.
    [[nodiscard]] std::size_t free_entry(std::size_t first) noexcept {
        std::size_t oldest = first;
        for (std::size_t way = 0; way < kWays; ++way) {
            const Order& order = entries_[first + way];
            if (!order.in_use) return first + way;
            if (order.reference < entries_[oldest].reference) oldest = first + way;
        }
        remove(entries_[oldest]);
        ++evictions_;
        return oldest;
    }

    int bucket_shift_;  // 64 - (order_bits - kWayBits), in [36, 63]
    model::TableArray<Order> entries_;
    std::vector<List> lists_;  // by stock locate
    std::uint64_t evictions_ = 0;
};

}  // namespace cpress::itch
