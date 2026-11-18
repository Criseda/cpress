// MessageModel: codes one ITCH message, in either direction, as the
// difference between each field and what the market so far leads the model
// to expect.
//
// A general-purpose compressor sees bytes. This model parses the message,
// and codes it in the order that lets each field be predicted from the ones
// before it, which is not the order on the wire:
//
//   type        a byte, in the context of the last two types
//   stock       not its 16-bit locate, but its place in the list of stocks
//               in order of last use. Trading comes in bursts, so the stock
//               of a message was very often seen a moment ago, and a recent
//               stock is a small number: about six bits where the locate in
//               the context of the last two locates costs nine.
//   order       for a message about an existing order (an execution, cancel,
//               delete or replace): not its 64-bit reference, but its place
//               among the stock's live orders, newest first. Then the order
//               is known, and with it the shares, price and side the rest
//               of the message is measured against. That is why a delete is
//               cheap: once its order is found there is nothing left to say.
//   the rest    tracking number, timestamp, then the type's own fields
//
// Every field is coded through the FieldCoder as a residual (residual.hpp)
// against an expectation, with four contexts. Section 5 of
// docs/stages/stage-5.md has the table of them; the code below is that table.
//
// One function, both directions. Every coding function here is a template
// over the coder (codec/two_way.hpp) and written once: it reads a field
// from the message when encoding, codes it, and writes it there when
// decoding. The market is read while coding and changed only afterwards, by
// Market::apply() on the complete message.
//
// Nothing trusts a field. A wrong expectation costs bits; an order the
// table does not hold is coded in full; a locate, a place or a length is
// checked or cannot be out of range by construction. So any bytes framed as
// a message can be coded, and a crafted archive can make a decoder produce
// wrong bytes, which the checksum catches, but not misbehave.
//
// Memory: set by the level, kMinLevel to kMaxLevel, the log2 of the order
// table's entries; see memory_bytes().
#pragma once

#include "codec/two_way.hpp"
#include "itch/field_coder.hpp"
#include "itch/market.hpp"
#include "itch/order_table.hpp"
#include "itch/residual.hpp"
#include "itch/schema.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cpress::itch {

// Prices are in units of 1/10,000 dollar, and on the day the design was
// measured on most moved by whole cents. A price that is measured against
// another is coded as a number of cents and a remainder.
inline constexpr std::uint32_t kPriceStep = 100;

// The ids of a field coded as a stepped residual: its quotient, and its
// remainder, which is a nibble if the step is at most 16 and a byte
// otherwise.
struct SteppedField {
    NumberField quotient;
    Symbol remainder;
    std::uint32_t step;
};

// The ids of a field coded as its bytes: `first` is its first byte.
struct BytesField {
    Symbol first;
    int length;
};

// The three kinds of message the stock's place is coded for separately.
inline constexpr std::size_t kMessageKinds = 3;
[[nodiscard]] constexpr std::size_t kind_of(std::uint32_t type) noexcept {
    if (adds_order(type)) return 0;
    return names_order(type) ? 1 : 2;
}

// The types with a model of their own. Every other type is coded as the
// bytes of its fields.
[[nodiscard]] constexpr bool has_own_model(std::uint32_t type) noexcept {
    return adds_order(type) || names_order(type) || type == 'P' || type == 'I';
}

// Every symbol the model codes. Ids are dense: a symbol's id is its place
// in the list make_symbols() builds, so the mixer and the APM have exactly
// as many sets and curves as there are symbols.
struct Symbols {
    Symbol type;
    Symbol escaped_byte;
    std::array<NumberField, kMessageKinds> stock_place;
    NumberField order_place;
    NumberField order_reference;  // an order's reference in full
    NumberField tracking;
    NumberField timestamp;

    SteppedField add_reference;
    Symbol add_side;
    NumberField add_shares;
    NumberField add_stock;
    NumberField add_price;
    BytesField add_attribution;

    NumberField executed_shares;
    NumberField executed_match;
    Symbol executed_printable;
    SteppedField executed_price;

    NumberField cancelled_shares;

    SteppedField replace_reference;
    NumberField replace_shares;
    SteppedField replace_price;

    NumberField trade_reference;
    Symbol trade_side;
    NumberField trade_shares;
    NumberField trade_stock;
    SteppedField trade_price;
    NumberField trade_match;

    NumberField paired_shares;
    NumberField imbalance_shares;
    Symbol imbalance_direction;
    NumberField imbalance_stock;
    SteppedField far_price;
    SteppedField near_price;
    SteppedField current_reference_price;
    Symbol cross_type;
    Symbol price_variation;

    // For each type coded as plain bytes, by layout index: the id of its
    // first byte after the shared fields. The rest follow, one id a byte.
    std::array<Symbol, kLayoutCount> other;

    Symbol count;  // how many ids there are
};

namespace detail {

// Hands out ids in order.
class SymbolList {
public:
    [[nodiscard]] constexpr Symbol symbol() noexcept {
        return next_++;
    }
    [[nodiscard]] constexpr NumberField number(const Field& field) noexcept {
        return number(field.length);
    }
    [[nodiscard]] constexpr NumberField number(int width) noexcept {
        const NumberField field{.first = next_, .width = width};
        next_ += field.ids();
        return field;
    }
    [[nodiscard]] constexpr SteppedField stepped(const Field& field, std::uint32_t step) noexcept {
        const NumberField quotient = number(field);
        return {.quotient = quotient, .remainder = symbol(), .step = step};
    }
    [[nodiscard]] constexpr BytesField bytes(int length) noexcept {
        const BytesField field{.first = next_, .length = length};
        next_ += static_cast<Symbol>(length);
        return field;
    }
    [[nodiscard]] constexpr Symbol size() const noexcept {
        return next_;
    }

private:
    Symbol next_ = 0;
};

// The width of the number an order's place is coded as: it is below
// kMaxWalk, and four bytes leave room for any limit a test may set.
inline constexpr int kPlaceWidth = 4;

[[nodiscard]] constexpr Symbols make_symbols() noexcept {
    using namespace fields;
    SymbolList list;
    Symbols s{};
    s.type = list.symbol();
    s.escaped_byte = list.symbol();
    for (NumberField& place : s.stock_place) place = list.number(kLocate);
    s.order_place = list.number(kPlaceWidth);
    s.order_reference = list.number(kOrderReference);
    s.tracking = list.number(kTracking);
    s.timestamp = list.number(kTimestamp);

    s.add_reference = list.stepped(kAddReference, kReferenceCounters);
    s.add_side = list.symbol();
    s.add_shares = list.number(kAddShares);
    s.add_stock = list.number(kAddStock);
    s.add_price = list.number(kAddPrice);
    s.add_attribution = list.bytes(kAddAttribution.length);

    s.executed_shares = list.number(kExecutedShares);
    s.executed_match = list.number(kExecutedMatch);
    s.executed_printable = list.symbol();
    s.executed_price = list.stepped(kExecutedPrice, kPriceStep);

    s.cancelled_shares = list.number(kCancelledShares);

    s.replace_reference = list.stepped(kReplaceReference, kReferenceCounters);
    s.replace_shares = list.number(kReplaceShares);
    s.replace_price = list.stepped(kReplacePrice, kPriceStep);

    s.trade_reference = list.number(kTradeReference);
    s.trade_side = list.symbol();
    s.trade_shares = list.number(kTradeShares);
    s.trade_stock = list.number(kTradeStock);
    s.trade_price = list.stepped(kTradePrice, kPriceStep);
    s.trade_match = list.number(kTradeMatch);

    s.paired_shares = list.number(kPairedShares);
    s.imbalance_shares = list.number(kImbalanceShares);
    s.imbalance_direction = list.symbol();
    s.imbalance_stock = list.number(kImbalanceStock);
    s.far_price = list.stepped(kFarPrice, kPriceStep);
    s.near_price = list.stepped(kNearPrice, kPriceStep);
    s.current_reference_price = list.stepped(kCurrentReferencePrice, kPriceStep);
    s.cross_type = list.symbol();
    s.price_variation = list.symbol();

    for (std::size_t i = 0; i < kLayouts.size(); ++i) {
        const MessageLayout& layout = kLayouts[i];
        if (has_own_model(static_cast<std::uint8_t>(layout.type))) continue;
        s.other[i] = list.bytes(static_cast<int>(layout.length - kHeaderBytes)).first;
    }
    s.count = list.size();
    return s;
}

}  // namespace detail

inline constexpr Symbols kSymbols = detail::make_symbols();

class MessageModel {
public:
    // The level is log2 of the order table's entries.
    static constexpr int kMinLevel = 10;
    static constexpr int kMaxLevel = 23;

    // Each history table has 2^min(level, kHistoryBits) slots. Larger
    // tables gained 0.1% on the day file for four times the memory.
    static constexpr int kHistoryBits = 20;

    // An order is named by its place among its stock's live orders if that
    // is below kMaxWalk, and by its reference otherwise. It bounds the work
    // an order message can cost, in both directions, and a crafted archive
    // cannot raise it. On the day file 0.11% of order messages are beyond it.
    static constexpr std::uint32_t kMaxWalk = 4096;

    // The smallest level with 8 * 2^level >= size, within the two limits. A
    // message that adds an order is 38 bytes with its prefix, so below the
    // largest level no more than one entry in five is ever in use.
    [[nodiscard]] static constexpr int level_for_size(std::uint64_t size) noexcept {
        const std::uint64_t entries =
            (size / kBytesPerEntry) + (size % kBytesPerEntry != 0 ? 1 : 0);
        const int bits = std::bit_width(entries == 0 ? 0 : entries - 1);  // 2^bits >= entries
        return std::clamp(bits, kMinLevel, kMaxLevel);
    }

    [[nodiscard]] static constexpr int history_bits(int level) noexcept {
        return std::min(level, kHistoryBits);
    }

    // Bytes a model at `level` allocates: every table, map, weight and list.
    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int level) noexcept {
        return FieldCoder::memory_bytes(history_bits(level), kSymbols.count) +
               Market::memory_bytes(level);
    }

    // Allocates everything for `level`, in [kMinLevel, kMaxLevel]. Throws
    // std::bad_alloc if there is no memory. `max_walk` is kMaxWalk except in
    // tests.
    explicit MessageModel(int level, std::uint32_t max_walk = kMaxWalk)
        : fields_(history_bits(level), kSymbols.count), market_(level), max_walk_(max_walk) {
        assert(level >= kMinLevel && level <= kMaxLevel);
    }

    // Codes what the next thing in the input is: the type of a message, or
    // Market::kEscape for a byte that is not part of one. Returns it. A
    // decoder gets any byte value back, and must check it.
    template <codec::TwoWayCoder C>
    [[nodiscard]] std::uint32_t code_type(C& coder, std::uint32_t type) noexcept {
        const Market& market = market_;
        const std::uint32_t last = market.last_type;
        const std::uint32_t before = market.type_before;
        const bool stock_has_orders = market.orders.live(narrow_locate(market.last_locate)) != 0;
        const bool same_stock_twice = market.last_locate == market.locate_before;
        const Contexts contexts{last, mix(last, before), mix(last, stock_has_orders ? 1 : 0),
                                mix(last, before, same_stock_twice ? 1 : 0)};
        return fields_.byte(coder, kSymbols.type, contexts, 0, type);
    }

    // Codes the rest of a message whose type code_type() just coded.
    // `message` is the whole message, as long as its type's layout says,
    // with the type in its first byte; the rest is read when encoding and
    // filled when decoding. Returns false if what was decoded cannot have
    // come from the encoder: the archive is corrupt.
    //
    // Work per message is bounded whatever the bytes: one scan of the list
    // of stocks, at most kLocates long, when encoding; one walk of at most
    // max_walk orders; and a fixed number of symbols.
    template <codec::TwoWayCoder C>
    [[nodiscard]] bool code_message(C& coder, std::span<std::uint8_t> message) noexcept {
        const std::uint32_t type = message[0];
        assert(layout_of(message[0]) != nullptr && message.size() == layout_of(message[0])->length);

        Found found;
        found.stock_place = code_stock(coder, type, message);
        const std::uint16_t locate = narrow_locate(value_of(message, fields::kLocate));
        if (names_order(type) && !code_order(coder, type, locate, message, found)) return false;
        code_tracking(coder, type, locate, message);
        code_timestamp(coder, type, locate, message);

        switch (type) {
            case 'A':
            case 'F':
                code_add(coder, type, locate, message);
                break;
            case 'E':
            case 'C':
                code_execution(coder, type, locate, message, found);
                break;
            case 'X':
                code_cancel(coder, type, locate, message, found);
                break;
            case 'D':
                break;  // the order says everything
            case 'U':
                code_replace(coder, type, locate, message, found);
                break;
            case 'P':
                code_trade(coder, type, locate, message);
                break;
            case 'I':
                code_imbalance(coder, type, locate, message);
                break;
            default:
                code_other(coder, locate, message);
                break;
        }

        market_.apply(message, found);
        return fields_.valid();
    }

    // Codes one byte that is not part of a message, in the context of the
    // last one to four bytes of the input.
    template <codec::TwoWayCoder C>
    [[nodiscard]] std::uint32_t code_escaped_byte(C& coder, std::uint32_t byte) noexcept {
        const std::uint32_t history = market_.input_history;
        const Contexts contexts{history & 0xFF, history & 0xFFFF, history & 0xFFFFFF, history};
        byte = fields_.byte(coder, kSymbols.escaped_byte, contexts, 0, byte);
        market_.apply_escape(byte);
        return byte;
    }

    // The fewest bits code_type() and code_message() can code for a message
    // of this layout: every number as expected, which is its length nibble
    // alone, and every byte symbol in full. It mirrors the functions below,
    // and message_model_test.cpp counts the bits they really code against
    // it. It is what bounds how far a payload can expand (codec/messages.hpp).
    [[nodiscard]] static constexpr int min_coded_bits(const MessageLayout& layout) noexcept {
        constexpr int kNumber = kNibbleBits;                   // its length, 0
        constexpr int kReference = kNibbleBits + kNibbleBits;  // and a remainder below 16
        constexpr int kPrice = kNibbleBits + kByteBits;        // and a remainder below 100
        const int shared = kByteBits + (3 * kNumber);          // type, stock, tracking, timestamp
        const int order = kNumber;                             // the order's place
        switch (layout.type) {
            case 'A':
                return shared + kReference + kByteBits + (3 * kNumber);
            case 'F':
                return shared + kReference + kByteBits + (3 * kNumber) +
                       (kByteBits * fields::kAddAttribution.length);
            case 'E':
                return shared + order + (2 * kNumber);
            case 'C':
                return shared + order + (2 * kNumber) + kByteBits + kPrice;
            case 'X':
                return shared + order + kNumber;
            case 'D':
                return shared + order;
            case 'U':
                return shared + order + kReference + kNumber + kPrice;
            case 'P':
                return shared + (4 * kNumber) + kByteBits + kPrice;
            case 'I':
                return shared + (3 * kNumber) + (3 * kByteBits) + (3 * kPrice);
            default:
                return shared + (kByteBits * (layout.length - static_cast<int>(kHeaderBytes)));
        }
    }

    // The fewest bits an escaped byte takes: the escape and the byte.
    static constexpr int kEscapedByteBits = 16;

    // For tests and the benchmark.
    [[nodiscard]] const Market& market() const noexcept {
        return market_;
    }
    [[nodiscard]] std::uint64_t evictions() const noexcept {
        return market_.orders.evictions();
    }

private:
    static constexpr std::uint64_t kBytesPerEntry = 8;
    static constexpr int kNibbleBits = 4;
    static constexpr int kByteBits = 8;

    // The timestamp's top two coded bytes are predicted. Below them there
    // is nothing to learn: modelling every byte measured 16.39 bits a
    // timestamp against 16.38, and ran slower.
    static constexpr int kTimestampModelledBytes = 2;

    // A locate is 16 bits wherever it came from.
    [[nodiscard]] static constexpr std::uint16_t narrow_locate(std::uint64_t locate) noexcept {
        return static_cast<std::uint16_t>(locate);
    }

    // A place in the list of stocks as a context: itself if under 4, and
    // otherwise its bit width, so that near and far are told apart without
    // one context per place.
    [[nodiscard]] static constexpr std::uint32_t place_class(std::uint32_t place) noexcept {
        constexpr std::uint32_t kExact = 4;
        return place < kExact ? place : kExact + static_cast<std::uint32_t>(std::bit_width(place));
    }

    // ------------------------------------------------- one field, both ways
    //
    // Each takes the field's ids, its bytes in the message, what the field
    // is expected to be and its contexts; codes it; and returns its value.
    // These four and code_stock() and code_order() are the only places that
    // read a field to be coded or write a decoded one.

    template <codec::TwoWayCoder C>
    std::uint64_t code_wrapping(C& coder, NumberField symbols, std::span<std::uint8_t> bytes,
                                std::uint64_t expected, const Contexts& contexts,
                                int modelled_bytes = FieldCoder::kEveryByte) noexcept {
        const int width = width_of(bytes, symbols);
        std::uint64_t u = 0;
        if (!C::kDecoding) u = wrapping_residual(load_big_endian(bytes), expected, width);
        u = fields_.number(coder, symbols, contexts, u, modelled_bytes);
        const std::uint64_t value = wrapping_value(u, expected, width);
        if (C::kDecoding) store_big_endian(bytes, value);
        return value;
    }

    template <codec::TwoWayCoder C>
    std::uint64_t code_signed(C& coder, NumberField symbols, std::span<std::uint8_t> bytes,
                              std::uint64_t expected, const Contexts& contexts) noexcept {
        const int width = width_of(bytes, symbols);
        std::uint64_t u = 0;
        if (!C::kDecoding) u = signed_residual(load_big_endian(bytes), expected, width);
        u = fields_.number(coder, symbols, contexts, u, FieldCoder::kEveryByte);
        const std::uint64_t value = signed_value(u, expected, width);
        if (C::kDecoding) store_big_endian(bytes, value);
        return value;
    }

    // The remainder has the field's contexts and a prefix that says only
    // whether the quotient was zero. A decoded remainder of `step` or more
    // is one no encoder writes.
    template <codec::TwoWayCoder C>
    std::uint64_t code_stepped(C& coder, const SteppedField& symbols, std::span<std::uint8_t> bytes,
                               std::uint64_t expected, const Contexts& contexts) noexcept {
        constexpr std::uint32_t kLargestNibbleStep = 16;
        const int width = width_of(bytes, symbols.quotient);
        Stepped residual{};
        if (!C::kDecoding) {
            residual = stepped_residual(load_big_endian(bytes), expected, width, symbols.step);
        }
        residual.quotient = fields_.number(coder, symbols.quotient, contexts, residual.quotient,
                                           FieldCoder::kEveryByte);
        const std::uint64_t prefix = residual.quotient == 0 ? 0 : 1;
        residual.remainder =
            symbols.step <= kLargestNibbleStep
                ? fields_.nibble(coder, symbols.remainder, contexts, prefix, residual.remainder)
                : fields_.byte(coder, symbols.remainder, contexts, prefix, residual.remainder);
        if (residual.remainder >= symbols.step) fields_.mark_invalid();
        const std::uint64_t value = stepped_value(residual, expected, width, symbols.step);
        if (C::kDecoding) store_big_endian(bytes, value);
        return value;
    }

    // A field as it is, for text: one byte symbol per byte, each in the
    // context of the bytes before it. Returns nothing: text predicts
    // nothing else.
    template <codec::TwoWayCoder C>
    void code_bytes(C& coder, BytesField symbols, std::span<std::uint8_t> bytes,
                    const Contexts& contexts) noexcept {
        assert(bytes.size() == static_cast<std::size_t>(symbols.length));
        std::uint64_t prefix = 0;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const std::uint32_t byte = fields_.byte(coder, symbols.first + static_cast<Symbol>(i),
                                                    contexts, prefix, bytes[i]);
            if (C::kDecoding) bytes[i] = static_cast<std::uint8_t>(byte);
            prefix = mix(prefix, byte);
        }
    }

    // A field of one byte, as it is. Returns it.
    template <codec::TwoWayCoder C>
    std::uint32_t code_flag(C& coder, Symbol symbol, std::span<std::uint8_t> bytes,
                            const Contexts& contexts) noexcept {
        code_bytes(coder, BytesField{.first = symbol, .length = 1}, bytes, contexts);
        return bytes[0];
    }

    [[nodiscard]] static int width_of(std::span<const std::uint8_t> bytes,
                                      NumberField symbols) noexcept {
        assert(bytes.size() == static_cast<std::size_t>(symbols.width));
        static_cast<void>(bytes);
        return symbols.width;
    }

    // ------------------------------------------------ every message's fields

    // The stock, as its place in the order of last use. Returns the place.
    // When encoding, finding it is a scan of at most kLocates entries; the
    // mean place is small.
    template <codec::TwoWayCoder C>
    std::uint32_t code_stock(C& coder, std::uint32_t type,
                             std::span<std::uint8_t> message) noexcept {
        const Market& market = market_;
        const std::span<std::uint8_t> bytes = bytes_of(message, fields::kLocate);
        std::uint64_t place = 0;
        if (!C::kDecoding) {
            const std::uint16_t locate = narrow_locate(load_big_endian(bytes));
            // The list holds every locate, so this ends before its end.
            place = static_cast<std::uint64_t>(std::ranges::find(market.recent_stocks, locate) -
                                               market.recent_stocks.begin());
        }
        const std::uint32_t last = market.last_type;
        const std::uint32_t recency = place_class(market.last_stock_place);
        const Contexts contexts{type, mix(type, last), mix(type, recency),
                                mix(type, last, recency)};
        // Two bytes: any decoded place is in the list.
        place = fields_.number(coder, kSymbols.stock_place[kind_of(type)], contexts, place,
                               FieldCoder::kEveryByte);
        static_assert(fields::kLocate.length == 2 && kLocates == std::size_t{1} << 16);
        if (C::kDecoding) store_big_endian(bytes, market.recent_stocks[place]);
        return static_cast<std::uint32_t>(place);
    }

    // The order an order message names: its place among the stock's live
    // orders, newest first, if that is under the limit, and otherwise the
    // limit and then its reference in full. Fills `found`. Returns false
    // for a decoded place beyond the limit, which no encoder writes.
    template <codec::TwoWayCoder C>
    [[nodiscard]] bool code_order(C& coder, std::uint32_t type, std::uint16_t locate,
                                  std::span<std::uint8_t> message, Found& found) noexcept {
        const Market& market = market_;
        const Stock& stock = market.stocks[locate];
        const std::span<std::uint8_t> bytes = bytes_of(message, fields::kOrderReference);
        const std::uint32_t limit = std::min(market.orders.live(locate), max_walk_);

        // The encoder finds the order on the walk that finds its place.
        OrderTable::Placed placed{.place = limit, .order = nullptr};
        if (!C::kDecoding) placed = market_.orders.place_of(locate, load_big_endian(bytes), limit);
        std::uint64_t place = placed.place;
        const bool same_stock = locate == market.last_locate;
        const Contexts contexts{type, mix(type, locate), mix(type, stock.last_place),
                                mix(type, market.last_type, same_stock ? 1 : 0)};
        place =
            fields_.number(coder, kSymbols.order_place, contexts, place, FieldCoder::kEveryByte);
        found.order_place = static_cast<std::uint32_t>(place);
        if (place > limit) return false;

        if (place < limit) {
            if (C::kDecoding) {
                // A walk of `place` steps, under max_walk.
                found.order = &market_.orders.at(locate, found.order_place);
                store_big_endian(bytes, found.order->reference);
            } else {
                found.order = placed.order;
            }
            return true;
        }
        const std::uint64_t reference =
            code_signed(coder, kSymbols.order_reference, bytes, market.heads[stock.counter],
                        {type, locate, 0, 0});
        found.order = market_.orders.find(reference);
        return true;
    }

    template <codec::TwoWayCoder C>
    void code_tracking(C& coder, std::uint32_t type, std::uint16_t locate,
                       std::span<std::uint8_t> message) noexcept {
        const Market& market = market_;
        code_wrapping(coder, kSymbols.tracking, bytes_of(message, fields::kTracking), 0,
                      {type, mix(type, market.last_tracking), locate, mix(type, market.last_type)});
    }

    // The gap since the last timestamp, in the context of how wide the last
    // two gaps were: a burst of messages has small gaps, and stays a burst.
    template <codec::TwoWayCoder C>
    void code_timestamp(C& coder, std::uint32_t type, std::uint16_t locate,
                        std::span<std::uint8_t> message) noexcept {
        const Market& market = market_;
        const std::uint32_t last = market.last_type;
        const std::uint64_t same_stock = locate == market.last_locate ? 1 : 0;
        const Contexts contexts{mix(type, last), mix(type, last, same_stock),
                                mix(type, market.gap_width, same_stock),
                                mix(type, market.gap_width, market.gap_width_before, last)};
        code_wrapping(coder, kSymbols.timestamp, bytes_of(message, fields::kTimestamp),
                      market.last_timestamp, contexts, kTimestampModelledBytes);
    }

    // ------------------------------------------------------ each type's own

    // A new order's reference: the head of its stock's counter plus one
    // step, or near it.
    template <codec::TwoWayCoder C>
    void code_new_reference(C& coder, const SteppedField& symbols, std::span<std::uint8_t> bytes,
                            std::uint32_t type, std::uint16_t locate) noexcept {
        const Market& market = market_;
        const std::uint32_t counter = market.stocks[locate].counter;
        code_stepped(coder, symbols, bytes, market.heads[counter] + kReferenceCounters,
                     {type, mix(type, counter), mix(type, market.last_type), locate});
    }

    // A and F. The price has nothing good to be measured against: it is
    // coded as it is, and its contexts find the handful of prices its stock
    // is trading at.
    template <codec::TwoWayCoder C>
    void code_add(C& coder, std::uint32_t type, std::uint16_t locate,
                  std::span<std::uint8_t> message) noexcept {
        using namespace fields;
        const Market& market = market_;
        const Stock& stock = market.stocks[locate];
        const std::uint32_t last = market.last_type;

        code_new_reference(coder, kSymbols.add_reference, bytes_of(message, kAddReference), type,
                           locate);
        const std::uint64_t side =
            side_of(code_flag(coder, kSymbols.add_side, bytes_of(message, kAddSide),
                              {locate, mix(locate, stock.last_side), mix(type, last), type}));
        const std::uint64_t shares = code_wrapping(
            coder, kSymbols.add_shares, bytes_of(message, kAddShares), 0,
            {locate, mix(locate, side), mix(stock.last_add_shares[side], side), side});
        code_wrapping(coder, kSymbols.add_stock, bytes_of(message, kAddStock), stock.symbol,
                      {type, 0, 0, 0});
        code_wrapping(coder, kSymbols.add_price, bytes_of(message, kAddPrice), 0,
                      {mix(locate, side), side, mix(shares, side), mix(locate, last)});
        if (type == 'F') {
            code_bytes(coder, kSymbols.add_attribution, bytes_of(message, kAddAttribution),
                       {locate, stock.last_attribution, mix(locate, stock.last_attribution),
                        market.last_attribution});
        }
    }

    // E and C. If the order was not found, its shares are taken as 0, and
    // C's price is measured against the stock's last trade.
    template <codec::TwoWayCoder C>
    void code_execution(C& coder, std::uint32_t type, std::uint16_t locate,
                        std::span<std::uint8_t> message, const Found& found) noexcept {
        using namespace fields;
        const Market& market = market_;
        const std::uint32_t last = market.last_type;
        const std::uint32_t remaining = found.order != nullptr ? found.order->shares : 0;

        code_signed(coder, kSymbols.executed_shares, bytes_of(message, kExecutedShares), remaining,
                    {type, mix(type, remaining), locate, mix(type, last)});
        code_signed(coder, kSymbols.executed_match, bytes_of(message, kExecutedMatch),
                    market.last_match + 1, {type, mix(type, last), 0, 0});
        if (type != 'C') return;

        const std::uint32_t printable =
            code_flag(coder, kSymbols.executed_printable, bytes_of(message, kExecutedPrintable),
                      {type, locate, 0, 0});
        const std::uint32_t expected =
            found.order != nullptr ? found.order->price : market.stocks[locate].last_trade_price;
        code_stepped(coder, kSymbols.executed_price, bytes_of(message, kExecutedPrice), expected,
                     {locate, printable, 0, 0});
    }

    template <codec::TwoWayCoder C>
    void code_cancel(C& coder, std::uint32_t type, std::uint16_t locate,
                     std::span<std::uint8_t> message, const Found& found) noexcept {
        const std::uint32_t remaining = found.order != nullptr ? found.order->shares : 0;
        code_wrapping(coder, kSymbols.cancelled_shares, bytes_of(message, fields::kCancelledShares),
                      0, {type, mix(type, remaining), locate, mix(locate, remaining)});
    }

    // U. If the old order was not found, its shares and price are taken as
    // 0 and its side as buy.
    template <codec::TwoWayCoder C>
    void code_replace(C& coder, std::uint32_t type, std::uint16_t locate,
                      std::span<std::uint8_t> message, const Found& found) noexcept {
        using namespace fields;
        const std::uint32_t old_shares = found.order != nullptr ? found.order->shares : 0;
        const std::uint32_t old_price = found.order != nullptr ? found.order->price : 0;
        const std::uint64_t side = found.order != nullptr ? found.order->side : kBuy;

        code_new_reference(coder, kSymbols.replace_reference, bytes_of(message, kReplaceReference),
                           type, locate);
        code_signed(coder, kSymbols.replace_shares, bytes_of(message, kReplaceShares), old_shares,
                    {type, locate, mix(old_shares, side), 0});
        code_stepped(coder, kSymbols.replace_price, bytes_of(message, kReplacePrice), old_price,
                     {mix(locate, side), side, type, mix(side, market_.last_type)});
    }

    // P: a trade against an order that was never displayed.
    template <codec::TwoWayCoder C>
    void code_trade(C& coder, std::uint32_t type, std::uint16_t locate,
                    std::span<std::uint8_t> message) noexcept {
        using namespace fields;
        const Market& market = market_;
        const Stock& stock = market.stocks[locate];
        const std::uint32_t last = market.last_type;
        const std::uint32_t expected_price =
            stock.last_trade_price != 0 ? stock.last_trade_price : stock.last_price;

        code_wrapping(coder, kSymbols.trade_reference, bytes_of(message, kTradeReference), 0,
                      {type, 0, 0, 0});
        code_flag(coder, kSymbols.trade_side, bytes_of(message, kTradeSide), {type, locate, 0, 0});
        code_wrapping(coder, kSymbols.trade_shares, bytes_of(message, kTradeShares), 0,
                      {type, locate, mix(type, last), 0});
        code_wrapping(coder, kSymbols.trade_stock, bytes_of(message, kTradeStock), stock.symbol,
                      {type, 0, 0, 0});
        code_stepped(coder, kSymbols.trade_price, bytes_of(message, kTradePrice), expected_price,
                     {locate, type, mix(type, last), 0});
        code_signed(coder, kSymbols.trade_match, bytes_of(message, kTradeMatch),
                    market.last_match + 1, {type, mix(type, last), 0, 0});
    }

    // I: each field against the same field of the stock's last imbalance
    // message.
    template <codec::TwoWayCoder C>
    void code_imbalance(C& coder, std::uint32_t type, std::uint16_t locate,
                        std::span<std::uint8_t> message) noexcept {
        using namespace fields;
        const Stock& stock = market_.stocks[locate];
        const std::uint32_t direction = stock.imbalance_direction;
        const std::uint32_t variation = stock.price_variation;
        const Contexts price_contexts{locate, stock.cross_type, 0, 0};

        code_signed(coder, kSymbols.paired_shares, bytes_of(message, kPairedShares),
                    stock.paired_shares, {type, locate, 0, 0});
        code_signed(coder, kSymbols.imbalance_shares, bytes_of(message, kImbalanceShares),
                    stock.imbalance_shares, {type, locate, direction, 0});
        code_flag(coder, kSymbols.imbalance_direction, bytes_of(message, kImbalanceDirection),
                  {direction, locate, mix(locate, direction), 0});
        code_wrapping(coder, kSymbols.imbalance_stock, bytes_of(message, kImbalanceStock),
                      stock.symbol, {type, 0, 0, 0});
        code_stepped(coder, kSymbols.far_price, bytes_of(message, kFarPrice), stock.far_price,
                     price_contexts);
        code_stepped(coder, kSymbols.near_price, bytes_of(message, kNearPrice), stock.near_price,
                     price_contexts);
        code_stepped(coder, kSymbols.current_reference_price,
                     bytes_of(message, kCurrentReferencePrice), stock.current_reference_price,
                     price_contexts);
        code_flag(coder, kSymbols.cross_type, bytes_of(message, kCrossType),
                  {stock.cross_type, locate, 0, 0});
        code_flag(coder, kSymbols.price_variation, bytes_of(message, kPriceVariation),
                  {variation, locate, mix(locate, variation), 0});
    }

    // Every other type: each field after the shared ones as its bytes, in
    // the context of the same field in the last message of the type. They
    // are 0.09% of the day file; a model for each would not show.
    template <codec::TwoWayCoder C>
    void code_other(C& coder, std::uint16_t locate, std::span<std::uint8_t> message) noexcept {
        const MessageLayout& layout = *layout_of(message[0]);
        const std::size_t index = index_of(layout);
        const std::span<const std::uint8_t> last = market_.last_of_type[index];
        for (const Field& field : layout.fields.subspan(kHeaderFields)) {
            std::uint64_t before = 0;
            for (const std::uint8_t byte : bytes_of(last, field)) before = mix(before, byte);
            const BytesField symbols{
                .first = kSymbols.other[index] + (field.offset - static_cast<Symbol>(kHeaderBytes)),
                .length = field.length};
            code_bytes(coder, symbols, bytes_of(message, field),
                       {before, locate, mix(locate, before), 0});
        }
    }

    FieldCoder fields_;
    Market market_;
    std::uint32_t max_walk_;
};

}  // namespace cpress::itch
