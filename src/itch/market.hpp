// Market: what the messages so far imply. The message model reads it to
// say what the next message's fields are expected to be; nothing else is
// remembered.
//
// It changes in one place only: apply(), called once per message after the
// message has been coded, with the complete message. When decoding, the
// message is complete once its last field is decoded, so both directions
// run the same update on the same bytes, and the two markets cannot drift
// apart. The model reads the market through a const reference while it
// codes.
//
// Per stock, by its 16-bit locate, a Stock. Across stocks, the members of
// Market below. And the order table (order_table.hpp).
//
// Three of the things kept are facts about the day the design was measured
// on (30 December 2019), not about ITCH. A file that breaks them still
// round-trips, and costs more.
//   - Order references come from kReferenceCounters interleaved counters,
//     told apart by the reference modulo 4, and a stock uses one counter all
//     day. Within a counter the next reference is usually the last plus 4.
//   - Messages cluster by stock: the stock of the next message is very
//     often one seen a moment ago. So a stock is named by its place in the
//     list of stocks in order of last use, a move-to-front list.
//   - Timestamps never go backwards.
//
// Memory: the order table, a Stock per locate and two bytes per locate for
// the list; see memory_bytes().
#pragma once

#include "itch/order_table.hpp"
#include "itch/schema.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace cpress::itch {

// The counters order references come from: a new reference belongs to
// counter `reference % kReferenceCounters`. Measured on the day file: no
// stock changed counter in 140,270,523 new references.
inline constexpr std::uint32_t kReferenceCounters = 4;

// The fields the model and the market both read, by type. A and F share the
// offsets of every field they both have, as do E and C.
namespace fields {

inline constexpr Field kLocate = field_of('A', "stock_locate");
inline constexpr Field kTracking = field_of('A', "tracking_number");
inline constexpr Field kTimestamp = field_of('A', "timestamp");

// Every order message names its order at the same place.
inline constexpr Field kOrderReference = field_of('D', "order_reference_number");

inline constexpr Field kAddReference = field_of('A', "order_reference_number");
inline constexpr Field kAddSide = field_of('A', "buy_sell_indicator");
inline constexpr Field kAddShares = field_of('A', "shares");
inline constexpr Field kAddStock = field_of('A', "stock");
inline constexpr Field kAddPrice = field_of('A', "price");
inline constexpr Field kAddAttribution = field_of('F', "attribution");

inline constexpr Field kExecutedShares = field_of('E', "executed_shares");
inline constexpr Field kExecutedMatch = field_of('E', "match_number");
inline constexpr Field kExecutedPrintable = field_of('C', "printable");
inline constexpr Field kExecutedPrice = field_of('C', "execution_price");

inline constexpr Field kCancelledShares = field_of('X', "cancelled_shares");

inline constexpr Field kReplaceReference = field_of('U', "new_order_reference_number");
inline constexpr Field kReplaceShares = field_of('U', "shares");
inline constexpr Field kReplacePrice = field_of('U', "price");

inline constexpr Field kTradeReference = field_of('P', "order_reference_number");
inline constexpr Field kTradeSide = field_of('P', "buy_sell_indicator");
inline constexpr Field kTradeShares = field_of('P', "shares");
inline constexpr Field kTradeStock = field_of('P', "stock");
inline constexpr Field kTradePrice = field_of('P', "price");
inline constexpr Field kTradeMatch = field_of('P', "match_number");

inline constexpr Field kPairedShares = field_of('I', "paired_shares");
inline constexpr Field kImbalanceShares = field_of('I', "imbalance_shares");
inline constexpr Field kImbalanceDirection = field_of('I', "imbalance_direction");
inline constexpr Field kImbalanceStock = field_of('I', "stock");
inline constexpr Field kFarPrice = field_of('I', "far_price");
inline constexpr Field kNearPrice = field_of('I', "near_price");
inline constexpr Field kCurrentReferencePrice = field_of('I', "current_reference_price");
inline constexpr Field kCrossType = field_of('I', "cross_type");
inline constexpr Field kPriceVariation = field_of('I', "price_variation_indicator");

inline constexpr Field kDirectoryStock = field_of('R', "stock");

[[nodiscard]] constexpr bool same_place(const Field& a, const Field& b) noexcept {
    return a.offset == b.offset && a.length == b.length;
}

static_assert(same_place(kAddReference, field_of('F', "order_reference_number")) &&
              same_place(kAddSide, field_of('F', "buy_sell_indicator")) &&
              same_place(kAddShares, field_of('F', "shares")) &&
              same_place(kAddStock, field_of('F', "stock")) &&
              same_place(kAddPrice, field_of('F', "price")));
static_assert(same_place(kExecutedShares, field_of('C', "executed_shares")) &&
              same_place(kExecutedMatch, field_of('C', "match_number")));
static_assert(same_place(kOrderReference, field_of('E', "order_reference_number")) &&
              same_place(kOrderReference, field_of('C', "order_reference_number")) &&
              same_place(kOrderReference, field_of('X', "order_reference_number")) &&
              same_place(kOrderReference, field_of('U', "original_order_reference_number")));

}  // namespace fields

// The types that add an order, and the types that name one already added.
[[nodiscard]] constexpr bool adds_order(std::uint32_t type) noexcept {
    return type == 'A' || type == 'F';
}
[[nodiscard]] constexpr bool names_order(std::uint32_t type) noexcept {
    return type == 'E' || type == 'C' || type == 'X' || type == 'D' || type == 'U';
}

// The side a buy/sell indicator names. Anything but 'B' counts as a sell.
[[nodiscard]] constexpr std::uint8_t side_of(std::uint64_t indicator) noexcept {
    return indicator == 'B' ? kBuy : kSell;
}

// What is kept for one stock. Every member starts at zero.
struct Stock {
    std::uint64_t symbol;                          // its name, from the stock directory
    std::uint64_t paired_shares;                   // of its last imbalance message, with
    std::uint64_t imbalance_shares;                // the seven members marked `I` below
    std::array<std::uint32_t, 2> last_add_shares;  // of the last add on each side
    std::uint32_t last_trade_price;                // of the last execution or trade
    std::uint32_t last_price;                      // of the last order added, replaced or deleted
    std::uint32_t last_place;                      // where the last order message found its order
    std::uint32_t last_attribution;                // of the last attributed add
    std::uint32_t far_price;                       // I
    std::uint32_t near_price;                      // I
    std::uint32_t current_reference_price;         // I
    std::uint8_t last_side;                        // the indicator of the last add, 'B' or 'S'
    std::uint8_t counter;                          // the reference counter of its last new order
    std::uint8_t imbalance_direction;              // I
    std::uint8_t cross_type;                       // I
    std::uint8_t price_variation;                  // I

    [[nodiscard]] bool operator==(const Stock&) const noexcept = default;
};

// What coding a message found, which apply() needs and the message does not
// hold.
struct Found {
    std::uint32_t stock_place = 0;  // the stock's place in the order of last use
    std::uint32_t order_place = 0;  // for an order message: the place coded for its order
    Order* order = nullptr;         // for an order message: its order, if the table has it
};

struct Market {
    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int order_bits) noexcept {
        return OrderTable::memory_bytes(order_bits) + (kLocates * sizeof(Stock)) +
               (kLocates * sizeof(std::uint16_t));
    }

    // Allocates an order table of 2^order_bits entries and everything else,
    // in the state before any message. Throws std::bad_alloc if there is no
    // memory.
    explicit Market(int order_bits)
        : orders(order_bits), stocks(kLocates), recent_stocks(kLocates) {
        for (std::size_t locate = 0; locate < kLocates; ++locate) {
            recent_stocks[locate] = static_cast<std::uint16_t>(locate);
        }
    }

    // The update for a complete message of an ITCH type. `found` is what
    // coding it found. Work: one move within the list of stocks, of at most
    // kLocates entries, and a fixed number of order table operations.
    void apply(std::span<const std::uint8_t> message, const Found& found) noexcept {
        const std::uint32_t type = message[0];
        const auto locate = static_cast<std::uint16_t>(value_of(message, fields::kLocate));
        const std::uint64_t timestamp = value_of(message, fields::kTimestamp);
        Stock& stock = stocks[locate];

        switch (type) {
            case 'A':
            case 'F':
                apply_add(message, locate, stock);
                break;
            case 'E':
            case 'C':
                apply_execution(message, stock, found);
                break;
            case 'X':
                stock.last_place = found.order_place;
                if (found.order != nullptr) {
                    take_shares(*found.order, value_of(message, fields::kCancelledShares));
                }
                break;
            case 'D':
                stock.last_place = found.order_place;
                if (found.order != nullptr) {
                    stock.last_price = found.order->price;
                    orders.remove(*found.order);
                }
                break;
            case 'U':
                apply_replace(message, locate, stock, found);
                break;
            case 'P':
                stock.last_trade_price =
                    static_cast<std::uint32_t>(value_of(message, fields::kTradePrice));
                last_match = value_of(message, fields::kTradeMatch);
                break;
            case 'I':
                apply_imbalance(message, stock);
                break;
            default:
                apply_other(message, stock);
                break;
        }

        use_stock(found.stock_place);
        type_before = last_type;
        last_type = type;
        locate_before = last_locate;
        last_locate = locate;
        last_tracking = static_cast<std::uint32_t>(value_of(message, fields::kTracking));
        gap_width_before = gap_width;
        gap_width = static_cast<std::uint32_t>(
            std::bit_width((timestamp - last_timestamp) &
                           ((std::uint64_t{1} << (8 * fields::kTimestamp.length)) - 1)));
        last_timestamp = timestamp;
        for (const std::uint8_t byte : message.last(kInputHistoryBytes)) remember_input(byte);
    }

    // The update for one byte that was not part of a message.
    void apply_escape(std::uint32_t byte) noexcept {
        type_before = last_type;
        last_type = kEscape;
        remember_input(byte);
    }

    [[nodiscard]] bool operator==(const Market&) const noexcept = default;

    // What code_type() returns for a byte that is not part of a message.
    // No ITCH type is 0.
    static constexpr std::uint32_t kEscape = 0;

    // The last bytes of the input kept for coding a byte outside a message.
    // Every message is longer than this.
    static constexpr std::size_t kInputHistoryBytes = 4;

    OrderTable orders;
    std::vector<Stock> stocks;  // by locate

    // Every locate, most recently used first; at first in numeric order.
    std::vector<std::uint16_t> recent_stocks;
    std::uint32_t last_stock_place = 0;  // where the last message found its stock in it

    std::uint32_t last_type = kEscape;    // of the last message, or kEscape
    std::uint32_t type_before = kEscape;  // of the one before it
    std::uint32_t last_locate = 0;
    std::uint32_t locate_before = 0;
    std::uint32_t last_tracking = 0;
    std::uint64_t last_timestamp = 0;
    std::uint32_t gap_width = 0;         // the bit width of the last gap between timestamps
    std::uint32_t gap_width_before = 0;  // and of the gap before it
    std::uint64_t last_match = 0;        // the last match number
    std::uint32_t last_attribution = 0;  // of the last attributed add, in any stock
    std::uint32_t input_history = 0;     // the last kInputHistoryBytes bytes, newest lowest

    // The largest reference seen on each counter. The largest and not the
    // last: at 09:30 orders entered earlier appear with their old, smaller
    // references, and must not drag the head back.
    std::array<std::uint64_t, kReferenceCounters> heads{};

    // The last message of each type that is coded as plain bytes, by its
    // layout's index.
    std::array<std::array<std::uint8_t, kMaxMessageBytes>, kLayoutCount> last_of_type{};

private:
    static_assert(kInputHistoryBytes * 8 == 32, "the history is one std::uint32_t");

    void remember_input(std::uint32_t byte) noexcept {
        input_history = (input_history << 8) | (byte & 0xFF);
    }

    // Moves the stock at `place` in the order of last use to the front.
    void use_stock(std::uint32_t place) noexcept {
        assert(place < recent_stocks.size());
        const std::uint16_t locate = recent_stocks[place];
        std::memmove(recent_stocks.data() + 1, recent_stocks.data(), place * sizeof(std::uint16_t));
        recent_stocks[0] = locate;
        last_stock_place = place;
    }

    // A new order's reference moves its counter's head, and says which
    // counter its stock uses.
    void note_new_reference(std::uint64_t reference, Stock& stock) noexcept {
        const auto counter = static_cast<std::uint8_t>(reference % kReferenceCounters);
        heads[counter] = std::max(heads[counter], reference);
        stock.counter = counter;
    }

    // An execution or cancel takes shares from an order; one that leaves
    // none removes it, as the specification says a reader of the feed must.
    void take_shares(Order& order, std::uint64_t shares) noexcept {
        if (shares >= order.shares) {
            orders.remove(order);
        } else {
            order.shares -= static_cast<std::uint32_t>(shares);
        }
    }

    void apply_add(std::span<const std::uint8_t> message, std::uint16_t locate,
                   Stock& stock) noexcept {
        const std::uint64_t reference = value_of(message, fields::kAddReference);
        const std::uint64_t indicator = value_of(message, fields::kAddSide);
        const std::uint8_t side = side_of(indicator);
        const auto shares = static_cast<std::uint32_t>(value_of(message, fields::kAddShares));
        const auto price = static_cast<std::uint32_t>(value_of(message, fields::kAddPrice));

        note_new_reference(reference, stock);
        orders.add(reference, shares, price, locate, side);
        stock.last_side = static_cast<std::uint8_t>(indicator);
        stock.last_add_shares[side] = shares;
        stock.last_price = price;
        if (message[0] == 'F') {
            last_attribution =
                static_cast<std::uint32_t>(value_of(message, fields::kAddAttribution));
            stock.last_attribution = last_attribution;
        }
    }

    void apply_execution(std::span<const std::uint8_t> message, Stock& stock,
                         const Found& found) noexcept {
        stock.last_place = found.order_place;
        last_match = value_of(message, fields::kExecutedMatch);
        if (message[0] == 'C') {
            stock.last_trade_price =
                static_cast<std::uint32_t>(value_of(message, fields::kExecutedPrice));
        } else if (found.order != nullptr) {
            stock.last_trade_price = found.order->price;
        }
        if (found.order != nullptr) {
            take_shares(*found.order, value_of(message, fields::kExecutedShares));
        }
    }

    // A replace removes its order and adds another, on the same side, under
    // a new reference.
    void apply_replace(std::span<const std::uint8_t> message, std::uint16_t locate, Stock& stock,
                       const Found& found) noexcept {
        const std::uint64_t reference = value_of(message, fields::kReplaceReference);
        const auto shares = static_cast<std::uint32_t>(value_of(message, fields::kReplaceShares));
        const auto price = static_cast<std::uint32_t>(value_of(message, fields::kReplacePrice));
        const std::uint8_t side = found.order != nullptr ? found.order->side : kBuy;

        stock.last_place = found.order_place;
        note_new_reference(reference, stock);
        if (found.order != nullptr) orders.remove(*found.order);
        orders.add(reference, shares, price, locate, side);
        stock.last_price = price;
    }

    static void apply_imbalance(std::span<const std::uint8_t> message, Stock& stock) noexcept {
        stock.paired_shares = value_of(message, fields::kPairedShares);
        stock.imbalance_shares = value_of(message, fields::kImbalanceShares);
        stock.imbalance_direction =
            static_cast<std::uint8_t>(value_of(message, fields::kImbalanceDirection));
        stock.far_price = static_cast<std::uint32_t>(value_of(message, fields::kFarPrice));
        stock.near_price = static_cast<std::uint32_t>(value_of(message, fields::kNearPrice));
        stock.current_reference_price =
            static_cast<std::uint32_t>(value_of(message, fields::kCurrentReferencePrice));
        stock.cross_type = static_cast<std::uint8_t>(value_of(message, fields::kCrossType));
        stock.price_variation =
            static_cast<std::uint8_t>(value_of(message, fields::kPriceVariation));
    }

    // Every other type is remembered whole, for the next message of its
    // type. The stock directory also names the stock.
    void apply_other(std::span<const std::uint8_t> message, Stock& stock) noexcept {
        const MessageLayout* layout = layout_of(message[0]);
        assert(layout != nullptr && message.size() == layout->length);
        std::ranges::copy(message, last_of_type[index_of(*layout)].begin());
        if (message[0] == 'R') stock.symbol = value_of(message, fields::kDirectoryStock);
    }
};

}  // namespace cpress::itch
