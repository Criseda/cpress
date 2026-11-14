// The NASDAQ TotalView-ITCH 5.0 message schema: every message type, its
// length and its fields. This file holds what the specification says and
// nothing about how a message is modelled.
//
// Source: Nasdaq TotalView-ITCH 5.0, the specification PDF dated 28 February
// 2024 (the link is in docs/idea.md), sections 1.1 to 1.8. Every offset and
// length below was typed from its tables.
//
// A message is a type byte and then fields, each at a fixed offset. All
// integers are big-endian and unsigned; text is ASCII, padded on the right
// with spaces. Every type starts with the same three fields after the type
// byte: the stock locate, a small integer that names the stock for the day;
// a tracking number; and a timestamp in nanoseconds since midnight.
//
// The specification defines messages, not files. In the day files Nasdaq
// publishes, each message is preceded by its length as a 2-byte big-endian
// integer (kLengthPrefixBytes); that framing is all a file adds.
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

namespace cpress::itch {

// What a field holds, as the specification types it, with the fields every
// message shares and the two kinds of identifier told apart from plain
// integers.
enum class FieldKind : std::uint8_t {
    Alpha,      // text or a one-character code
    Integer,    // a count or a time
    Price,      // an integer with implied decimal places: Price (4) or Price (8)
    Locate,     // the stock locate
    Tracking,   // the tracking number
    Timestamp,  // nanoseconds since midnight
    Reference,  // an order reference number
    Match,      // a match number
};

struct Field {
    std::string_view name;
    std::uint8_t offset = 0;  // from the type byte
    std::uint8_t length = 0;
    FieldKind kind = FieldKind::Integer;
};

struct MessageLayout {
    char type = 0;
    std::uint8_t length = 0;  // the whole message, type byte included, without the prefix
    std::span<const Field> fields;
};

inline constexpr std::size_t kLengthPrefixBytes = 2;

// The longest message: net order imbalance, type I.
inline constexpr std::size_t kMaxMessageBytes = 50;

// The fields every message starts with, and the bytes they and the type
// byte take.
inline constexpr std::size_t kHeaderFields = 3;
inline constexpr std::size_t kHeaderBytes = 11;

// The stock locate is 16 bits, so there are this many stocks at most.
inline constexpr std::size_t kLocates = std::size_t{1} << 16;

// The tables of the specification, one per message type.
namespace detail::spec {

using enum FieldKind;

inline constexpr Field kStockLocate{"stock_locate", 1, 2, Locate};
inline constexpr Field kTrackingNumber{"tracking_number", 3, 2, Tracking};
inline constexpr Field kTimestampField{"timestamp", 5, 6, Timestamp};

// 1.1 System event.
inline constexpr std::array kSystemEvent{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"event_code", 11, 1, Alpha},
};

// 1.2.1 Stock directory.
inline constexpr std::array kStockDirectory{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"stock", 11, 8, Alpha},
    Field{"market_category", 19, 1, Alpha},
    Field{"financial_status_indicator", 20, 1, Alpha},
    Field{"round_lot_size", 21, 4, Integer},
    Field{"round_lots_only", 25, 1, Alpha},
    Field{"issue_classification", 26, 1, Alpha},
    Field{"issue_sub_type", 27, 2, Alpha},
    Field{"authenticity", 29, 1, Alpha},
    Field{"short_sale_threshold_indicator", 30, 1, Alpha},
    Field{"ipo_flag", 31, 1, Alpha},
    Field{"luld_reference_price_tier", 32, 1, Alpha},
    Field{"etp_flag", 33, 1, Alpha},
    Field{"etp_leverage_factor", 34, 4, Integer},
    Field{"inverse_indicator", 38, 1, Alpha},
};

// 1.2.2 Stock trading action.
inline constexpr std::array kStockTradingAction{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"stock", 11, 8, Alpha},
    Field{"trading_state", 19, 1, Alpha},
    Field{"reserved", 20, 1, Alpha},
    Field{"reason", 21, 4, Alpha},
};

// 1.2.3 Reg SHO short sale price test restricted indicator.
inline constexpr std::array kRegShoRestriction{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"stock", 11, 8, Alpha},
    Field{"reg_sho_action", 19, 1, Alpha},
};

// 1.2.4 Market participant position.
inline constexpr std::array kMarketParticipantPosition{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"mpid", 11, 4, Alpha},
    Field{"stock", 15, 8, Alpha},
    Field{"primary_market_maker", 23, 1, Alpha},
    Field{"market_maker_mode", 24, 1, Alpha},
    Field{"market_participant_state", 25, 1, Alpha},
};

// 1.2.5.1 Market-wide circuit breaker decline levels.
inline constexpr std::array kMwcbDeclineLevel{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"level_1", 11, 8, Price},
    Field{"level_2", 19, 8, Price},
    Field{"level_3", 27, 8, Price},
};

// 1.2.5.2 Market-wide circuit breaker status.
inline constexpr std::array kMwcbStatus{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"breached_level", 11, 1, Alpha},
};

// 1.2.6 IPO quoting period update.
inline constexpr std::array kIpoQuotingPeriodUpdate{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"stock", 11, 8, Alpha},
    Field{"ipo_quotation_release_time", 19, 4, Integer},
    Field{"ipo_quotation_release_qualifier", 23, 1, Alpha},
    Field{"ipo_price", 24, 4, Price},
};

// 1.2.7 Limit up, limit down auction collar.
inline constexpr std::array kLuldAuctionCollar{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"stock", 11, 8, Alpha},
    Field{"auction_collar_reference_price", 19, 4, Price},
    Field{"upper_auction_collar_price", 23, 4, Price},
    Field{"lower_auction_collar_price", 27, 4, Price},
    Field{"auction_collar_extension", 31, 4, Integer},
};

// 1.2.8 Operational halt.
inline constexpr std::array kOperationalHalt{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"stock", 11, 8, Alpha},
    Field{"market_code", 19, 1, Alpha},
    Field{"operational_halt_action", 20, 1, Alpha},
};

// 1.3.1 Add order, no attribution.
inline constexpr std::array kAddOrder{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"order_reference_number", 11, 8, Reference},
    Field{"buy_sell_indicator", 19, 1, Alpha},
    Field{"shares", 20, 4, Integer},
    Field{"stock", 24, 8, Alpha},
    Field{"price", 32, 4, Price},
};

// 1.3.2 Add order with attribution.
inline constexpr std::array kAddOrderAttributed{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"order_reference_number", 11, 8, Reference},
    Field{"buy_sell_indicator", 19, 1, Alpha},
    Field{"shares", 20, 4, Integer},
    Field{"stock", 24, 8, Alpha},
    Field{"price", 32, 4, Price},
    Field{"attribution", 36, 4, Alpha},
};

// 1.4.1 Order executed.
inline constexpr std::array kOrderExecuted{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"order_reference_number", 11, 8, Reference},
    Field{"executed_shares", 19, 4, Integer},
    Field{"match_number", 23, 8, Match},
};

// 1.4.2 Order executed with price.
inline constexpr std::array kOrderExecutedWithPrice{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"order_reference_number", 11, 8, Reference},
    Field{"executed_shares", 19, 4, Integer},
    Field{"match_number", 23, 8, Match},
    Field{"printable", 31, 1, Alpha},
    Field{"execution_price", 32, 4, Price},
};

// 1.4.3 Order cancel.
inline constexpr std::array kOrderCancel{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"order_reference_number", 11, 8, Reference},
    Field{"cancelled_shares", 19, 4, Integer},
};

// 1.4.4 Order delete.
inline constexpr std::array kOrderDelete{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"order_reference_number", 11, 8, Reference},
};

// 1.4.5 Order replace.
inline constexpr std::array kOrderReplace{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"original_order_reference_number", 11, 8, Reference},
    Field{"new_order_reference_number", 19, 8, Reference},
    Field{"shares", 27, 4, Integer},
    Field{"price", 31, 4, Price},
};

// 1.5.1 Trade, non-cross.
inline constexpr std::array kTrade{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"order_reference_number", 11, 8, Reference},
    Field{"buy_sell_indicator", 19, 1, Alpha},
    Field{"shares", 20, 4, Integer},
    Field{"stock", 24, 8, Alpha},
    Field{"price", 32, 4, Price},
    Field{"match_number", 36, 8, Match},
};

// 1.5.2 Cross trade.
inline constexpr std::array kCrossTrade{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"shares", 11, 8, Integer},
    Field{"stock", 19, 8, Alpha},
    Field{"cross_price", 27, 4, Price},
    Field{"match_number", 31, 8, Match},
    Field{"cross_type", 39, 1, Alpha},
};

// 1.5.3 Broken trade.
inline constexpr std::array kBrokenTrade{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"match_number", 11, 8, Match},
};

// 1.6 Net order imbalance indicator.
inline constexpr std::array kNetOrderImbalance{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"paired_shares", 11, 8, Integer},
    Field{"imbalance_shares", 19, 8, Integer},
    Field{"imbalance_direction", 27, 1, Alpha},
    Field{"stock", 28, 8, Alpha},
    Field{"far_price", 36, 4, Price},
    Field{"near_price", 40, 4, Price},
    Field{"current_reference_price", 44, 4, Price},
    Field{"cross_type", 48, 1, Alpha},
    Field{"price_variation_indicator", 49, 1, Alpha},
};

// 1.7 Retail price improvement indicator.
inline constexpr std::array kRetailInterest{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"stock", 11, 8, Alpha},
    Field{"interest_flag", 19, 1, Alpha},
};

// 1.8 Direct listing with capital raise price discovery.
inline constexpr std::array kDirectListing{
    kStockLocate,
    kTrackingNumber,
    kTimestampField,
    Field{"stock", 11, 8, Alpha},
    Field{"open_eligibility_status", 19, 1, Alpha},
    Field{"minimum_allowable_price", 20, 4, Price},
    Field{"maximum_allowable_price", 24, 4, Price},
    Field{"near_execution_price", 28, 4, Price},
    Field{"near_execution_time", 32, 8, Integer},
    Field{"lower_price_range_collar", 40, 4, Price},
    Field{"upper_price_range_collar", 44, 4, Price},
};

}  // namespace detail::spec

// Every message type, in the order the specification gives them.
inline constexpr std::size_t kLayoutCount = 23;
inline constexpr std::array<MessageLayout, kLayoutCount> kLayouts{{
    {'S', 12, detail::spec::kSystemEvent},
    {'R', 39, detail::spec::kStockDirectory},
    {'H', 25, detail::spec::kStockTradingAction},
    {'Y', 20, detail::spec::kRegShoRestriction},
    {'L', 26, detail::spec::kMarketParticipantPosition},
    {'V', 35, detail::spec::kMwcbDeclineLevel},
    {'W', 12, detail::spec::kMwcbStatus},
    {'K', 28, detail::spec::kIpoQuotingPeriodUpdate},
    {'J', 35, detail::spec::kLuldAuctionCollar},
    {'h', 21, detail::spec::kOperationalHalt},
    {'A', 36, detail::spec::kAddOrder},
    {'F', 40, detail::spec::kAddOrderAttributed},
    {'E', 31, detail::spec::kOrderExecuted},
    {'C', 36, detail::spec::kOrderExecutedWithPrice},
    {'X', 23, detail::spec::kOrderCancel},
    {'D', 19, detail::spec::kOrderDelete},
    {'U', 35, detail::spec::kOrderReplace},
    {'P', 44, detail::spec::kTrade},
    {'Q', 40, detail::spec::kCrossTrade},
    {'B', 19, detail::spec::kBrokenTrade},
    {'I', 50, detail::spec::kNetOrderImbalance},
    {'N', 20, detail::spec::kRetailInterest},
    {'O', 48, detail::spec::kDirectListing},
}};

namespace detail {

// True if the layout starts with the three shared fields and its fields
// cover the message after the type byte exactly once, in order, with no gap.
[[nodiscard]] constexpr bool tiles_its_message(const MessageLayout& layout) noexcept {
    if (layout.fields.size() < kHeaderFields) return false;
    std::size_t at = 1;  // after the type byte
    for (const Field& field : layout.fields) {
        if (field.offset != at || field.length == 0) return false;
        at += field.length;
    }
    const Field& last_header_field = layout.fields[kHeaderFields - 1];
    return at == layout.length &&
           std::size_t{last_header_field.offset} + last_header_field.length == kHeaderBytes;
}

[[nodiscard]] constexpr bool schema_is_consistent() noexcept {
    std::size_t longest = 0;
    for (const MessageLayout& layout : kLayouts) {
        if (!tiles_its_message(layout)) return false;
        longest = std::max<std::size_t>(longest, layout.length);
    }
    return longest == kMaxMessageBytes;
}

static_assert(schema_is_consistent(),
              "every layout's fields tile its message, and the longest is kMaxMessageBytes");

inline constexpr std::uint8_t kNoLayout = 0xFF;

// For every byte value, the index in kLayouts of the type it names, or
// kNoLayout.
[[nodiscard]] constexpr std::array<std::uint8_t, 256> make_layout_index() noexcept {
    std::array<std::uint8_t, 256> index{};
    index.fill(kNoLayout);
    for (std::size_t i = 0; i < kLayouts.size(); ++i) {
        index[static_cast<std::uint8_t>(kLayouts[i].type)] = static_cast<std::uint8_t>(i);
    }
    return index;
}

inline constexpr std::array<std::uint8_t, 256> kLayoutIndex = make_layout_index();

}  // namespace detail

// The layout of a type, or nullptr if the byte is not an ITCH 5.0 type.
[[nodiscard]] constexpr const MessageLayout* layout_of(std::uint8_t type) noexcept {
    const std::uint8_t index = detail::kLayoutIndex[type];
    return index == detail::kNoLayout ? nullptr : &kLayouts[index];
}

// The place of a layout in kLayouts: a small dense number for tables that
// hold something per type.
[[nodiscard]] constexpr std::size_t index_of(const MessageLayout& layout) noexcept {
    return detail::kLayoutIndex[static_cast<std::uint8_t>(layout.type)];
}

// The field called `name` in messages of `type`. For constants: a name the
// schema does not have throws, which in a constant expression is a compile
// error.
//
// It searches kLayouts rather than asking layout_of(): GCC with
// -fsanitize=undefined does not accept the comparison of that pointer with
// nullptr as a constant expression.
[[nodiscard]] constexpr Field field_of(char type, std::string_view name) {
    for (const MessageLayout& layout : kLayouts) {
        if (layout.type == type) {
            for (const Field& field : layout.fields) {
                if (field.name == name) return field;
            }
        }
    }
    throw std::logic_error("the ITCH schema has no such field");
}

// A big-endian unsigned integer of 1 to 8 bytes.
[[nodiscard]] constexpr std::uint64_t load_big_endian(
    std::span<const std::uint8_t> bytes) noexcept {
    assert(!bytes.empty() && bytes.size() <= 8);
    std::uint64_t value = 0;
    for (const std::uint8_t byte : bytes) value = (value << 8) | byte;
    return value;
}

// Writes the low bytes of `value`, most significant first, to fill `bytes`.
constexpr void store_big_endian(std::span<std::uint8_t> bytes, std::uint64_t value) noexcept {
    assert(!bytes.empty() && bytes.size() <= 8);
    for (std::size_t i = bytes.size(); i > 0; --i) {
        bytes[i - 1] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

// The bytes of `field` in `message`.
[[nodiscard]] constexpr std::span<std::uint8_t> bytes_of(std::span<std::uint8_t> message,
                                                         const Field& field) noexcept {
    return message.subspan(field.offset, field.length);
}
[[nodiscard]] constexpr std::span<const std::uint8_t> bytes_of(
    std::span<const std::uint8_t> message, const Field& field) noexcept {
    return message.subspan(field.offset, field.length);
}

// The value of an integer field of `message`.
[[nodiscard]] constexpr std::uint64_t value_of(std::span<const std::uint8_t> message,
                                               const Field& field) noexcept {
    return load_big_endian(bytes_of(message, field));
}

// The layout of the message at the front of `input`, if one is there whole:
// a length prefix that equals the length of the type that follows it, and
// that many bytes. Otherwise nullptr. This is the file framing, and the one
// definition of "the next thing is a message".
[[nodiscard]] constexpr const MessageLayout* framed_message_at(
    std::span<const std::uint8_t> input) noexcept {
    if (input.size() <= kLengthPrefixBytes) return nullptr;
    const MessageLayout* layout = layout_of(input[kLengthPrefixBytes]);
    if (layout == nullptr) return nullptr;
    if (load_big_endian(input.first(kLengthPrefixBytes)) != layout->length) return nullptr;
    if (input.size() < kLengthPrefixBytes + layout->length) return nullptr;
    return layout;
}

// A message with its prefix is at most this long.
inline constexpr std::size_t kMaxFramedBytes = kLengthPrefixBytes + kMaxMessageBytes;

}  // namespace cpress::itch
