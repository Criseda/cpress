// OrderTable: an order added is found and one removed is not; each stock's
// list holds its live orders newest first; a full bucket evicts its oldest
// order exactly as a delete would; and no sequence of operations takes a
// walk off a list.
#include "check.hpp"

#include "itch/order_table.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

namespace cpress::test {
namespace {

using itch::Order;
using itch::OrderTable;

// The references of a stock's live orders, newest first, read through at().
std::vector<std::uint64_t> listed(OrderTable& table, std::uint16_t locate) {
    std::vector<std::uint64_t> references;
    references.reserve(table.live(locate));
    for (std::uint32_t place = 0; place < table.live(locate); ++place) {
        references.push_back(table.at(locate, place).reference);
    }
    return references;
}

void test_add_find_remove() {
    OrderTable table(10);
    CHECK(table.entry_count() == 1024);
    CHECK(table.find(42) == nullptr);
    CHECK(table.live(7) == 0);

    table.add(42, 300, 1'250'000, 7, itch::kSell);
    const Order* order = table.find(42);
    CHECK(order != nullptr);
    if (order == nullptr) return;
    CHECK(order->reference == 42);
    CHECK(order->shares == 300);
    CHECK(order->price == 1'250'000);
    CHECK(order->locate == 7);
    CHECK(order->side == itch::kSell);
    CHECK(table.live(7) == 1);
    CHECK(table.find(43) == nullptr);

    table.remove(*table.find(42));
    CHECK(table.find(42) == nullptr);
    CHECK(table.live(7) == 0);
    CHECK(table.evictions() == 0);
}

// Against the obvious model, a vector of references per stock: after any
// mix of adds and removes each stock's list is its live orders, newest
// first. The table is large enough that nothing is evicted.
void test_lists_are_newest_first() {
    constexpr std::size_t kStocks = 5;
    OrderTable table(14);
    std::array<std::vector<std::uint64_t>, kStocks> expected;  // newest first
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(0x0BDE5);
    std::uint64_t next_reference = 1'000;
    std::size_t mismatches = 0;

    for (int i = 0; i < 20'000; ++i) {
        const auto locate = static_cast<std::uint16_t>(rng() % kStocks);
        std::vector<std::uint64_t>& orders = expected.at(locate);
        if (orders.empty() || rng() % 5 < 3) {
            next_reference += 4;
            table.add(next_reference, 100, 5'000, locate, itch::kBuy);
            orders.insert(orders.begin(), next_reference);
        } else {
            const std::size_t place = rng() % orders.size();
            Order* order = table.find(orders[place]);
            if (order == nullptr) {
                ++mismatches;
                continue;
            }
            table.remove(*order);
            orders.erase(orders.begin() + static_cast<std::ptrdiff_t>(place));
        }
        if (i % 500 == 0 && listed(table, locate) != orders) ++mismatches;
    }
    for (std::size_t locate = 0; locate < kStocks; ++locate) {
        if (listed(table, static_cast<std::uint16_t>(locate)) != expected.at(locate)) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    CHECK(table.evictions() == 0);
}

// `count` references that all hash to the bucket of the first, found by
// trying them in order.
std::vector<std::uint64_t> references_in_one_bucket(const OrderTable& table, std::size_t count) {
    std::vector<std::uint64_t> references{1'000};
    const std::size_t bucket = table.bucket_index(references.front());
    for (std::uint64_t reference = 1'001; references.size() < count; ++reference) {
        if (table.bucket_index(reference) == bucket) references.push_back(reference);
    }
    return references;
}

// A ninth order in a bucket of eight evicts the one with the smallest
// reference, and leaves everything as if that order had been deleted.
void test_full_bucket_evicts_the_smallest_reference() {
    constexpr std::uint16_t kLocate = 3;
    constexpr std::uint16_t kOther = 9;
    OrderTable table(10);
    OrderTable deleted(10);  // the same orders, with the delete done by hand
    const std::vector<std::uint64_t> references =
        references_in_one_bucket(table, OrderTable::kWays + 1);

    // The smallest reference is added third, so that it is neither the
    // first entry of the bucket nor the newest order, and belongs to
    // another stock than the ninth.
    std::vector<std::uint64_t> order_added(references.begin() + 1, references.end() - 1);
    order_added.insert(order_added.begin() + 2, references.front());
    for (const std::uint64_t reference : order_added) {
        const std::uint16_t locate = reference == references.front() ? kOther : kLocate;
        table.add(reference, 100, 5'000, locate, itch::kBuy);
        deleted.add(reference, 100, 5'000, locate, itch::kBuy);
    }
    CHECK(table.live(kLocate) == OrderTable::kWays - 1);
    CHECK(table.live(kOther) == 1);
    CHECK(table.evictions() == 0);

    deleted.remove(*deleted.find(references.front()));
    table.add(references.back(), 200, 6'000, kLocate, itch::kSell);
    deleted.add(references.back(), 200, 6'000, kLocate, itch::kSell);

    CHECK(table.evictions() == 1);
    CHECK(table.find(references.front()) == nullptr);
    CHECK(table.live(kOther) == 0);
    CHECK(table.live(kLocate) == OrderTable::kWays);
    for (std::size_t i = 1; i < references.size(); ++i) {
        CHECK(table.find(references[i]) != nullptr);
    }
    CHECK(listed(table, kLocate) == listed(deleted, kLocate));
    CHECK(listed(table, kLocate).front() == references.back());
    // The same table but for the count of evictions.
    CHECK(deleted.evictions() == 0);
    CHECK(!(table == deleted));
}

// place_of() gives the place at() walks to, and the same order: within a
// limit, just inside one and at one. Beyond the limit, or for an order the
// stock does not have, it gives the limit and no order.
void test_place_of_and_at_agree() {
    constexpr std::uint16_t kLocate = 12;
    constexpr std::uint16_t kNeighbour = 13;
    constexpr std::uint16_t kEmpty = 14;
    constexpr std::uint32_t kOrders = 40;
    OrderTable table(12);
    for (std::uint32_t i = 0; i < kOrders; ++i) {
        table.add(500 + (4 * std::uint64_t{i}), 100, 5'000, kLocate, itch::kBuy);
    }
    table.add(9'999, 100, 5'000, kNeighbour, itch::kBuy);  // another stock's

    std::size_t disagreements = 0;
    const auto disagree = [](OrderTable::Placed placed, std::uint32_t place, const Order* order) {
        return placed.place != place || placed.order != order;
    };
    for (std::uint32_t place = 0; place < kOrders; ++place) {
        Order& order = table.at(kLocate, place);
        const std::uint64_t reference = order.reference;
        if (disagree(table.place_of(kLocate, reference, kOrders), place, &order)) ++disagreements;
        // Just inside a limit, and just outside it.
        if (disagree(table.place_of(kLocate, reference, place + 1), place, &order)) {
            ++disagreements;
        }
        if (disagree(table.place_of(kLocate, reference, place), place, nullptr)) ++disagreements;
    }
    CHECK(disagreements == 0);

    constexpr std::uint32_t kLimit = 10;
    const auto not_found = [](OrderTable::Placed placed, std::uint32_t limit) {
        return placed.place == limit && placed.order == nullptr;
    };
    CHECK(not_found(table.place_of(kLocate, 1, kLimit), kLimit));        // not there
    CHECK(not_found(table.place_of(kLocate, 9'999, kOrders), kOrders));  // in another stock
    CHECK(not_found(table.place_of(kLocate, 500, kLimit), kLimit));      // the oldest: beyond
    CHECK(not_found(table.place_of(kLocate, 500, 0), 0));
    CHECK(not_found(table.place_of(kEmpty, 500, kLimit), kLimit));  // a stock with no orders
}

// An order added under a reference that is already live replaces it.
void test_reused_reference_replaces_the_order() {
    OrderTable table(10);
    table.add(77, 100, 5'000, 1, itch::kBuy);
    table.add(81, 100, 5'000, 1, itch::kBuy);
    table.add(77, 900, 7'000, 2, itch::kSell);
    CHECK(table.live(1) == 1);
    CHECK(table.live(2) == 1);
    const Order* order = table.find(77);
    CHECK(order != nullptr);
    if (order == nullptr) return;
    CHECK(order->locate == 2);
    CHECK(order->shares == 900);
    CHECK(listed(table, 1) == std::vector<std::uint64_t>{81});
    CHECK(table.evictions() == 0);
}

// The invariant, under everything at once: the smallest table, sixteen
// entries in two buckets, so that almost every add evicts; references drawn
// from a small range, so that they are reused; a few stocks. After every
// operation the lists are walked: each has `live` orders, all in use, of
// that stock, newest first, each found by its reference at its own place.
// Under ASan this is also the test that nothing is read outside the table.
void test_invariant_under_random_operations() {
    constexpr int kOperations = 100'000;
    constexpr std::uint16_t kStocks = 4;
    constexpr std::uint64_t kReferences = 64;
    OrderTable table(OrderTable::kMinOrderBits);
    std::map<std::uint64_t, int> added_at;  // when each reference was last added
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(0x1A7A);
    std::size_t broken = 0;

    for (int i = 0; i < kOperations; ++i) {
        const std::uint64_t reference = rng() % kReferences;
        if (rng() % 3 != 0) {
            table.add(reference, static_cast<std::uint32_t>(rng()),
                      static_cast<std::uint32_t>(rng()),
                      static_cast<std::uint16_t>(rng() % kStocks), itch::kBuy);
            added_at[reference] = i;
        } else if (Order* order = table.find(reference)) {
            table.remove(*order);
        }

        std::size_t listed_orders = 0;
        for (std::uint16_t locate = 0; locate < kStocks; ++locate) {
            int newer = kOperations;
            for (std::uint32_t place = 0; place < table.live(locate); ++place) {
                const Order& order = table.at(locate, place);
                const int added = added_at[order.reference];
                const OrderTable::Placed placed =
                    table.place_of(locate, order.reference, table.live(locate));
                if (!order.in_use || order.locate != locate || added >= newer ||
                    table.find(order.reference) != &order || placed.place != place ||
                    placed.order != &order) {
                    ++broken;
                }
                newer = added;
                ++listed_orders;
            }
        }
        std::size_t found = 0;
        for (std::uint64_t r = 0; r < kReferences; ++r) found += table.find(r) != nullptr ? 1 : 0;
        if (found != listed_orders || found > table.entry_count()) ++broken;
    }
    CHECK(broken == 0);
    CHECK(table.evictions() > 1'000);
}

}  // namespace

void run_order_table_tests() {
    test_add_find_remove();
    test_lists_are_newest_first();
    test_full_bucket_evicts_the_smallest_reference();
    test_place_of_and_at_agree();
    test_reused_reference_replaces_the_order();
    test_invariant_under_random_operations();
}

}  // namespace cpress::test
