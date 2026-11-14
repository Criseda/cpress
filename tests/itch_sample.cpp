#include "itch_sample.hpp"

#include "itch/schema.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace cpress::test {
namespace {

using itch::Field;
using itch::field_of;

// A market small enough to read in a debugger: a few stocks, each with its
// own price, whose orders come and go.
constexpr std::size_t kStocks = 6;
constexpr std::uint32_t kCounters = 4;  // as the day file has; see itch/market.hpp

struct LiveOrder {
    std::uint64_t reference;
    std::uint16_t locate;
    std::uint32_t shares;
    std::uint32_t price;
};

class Generator {
public:
    explicit Generator(const ItchSampleOptions& options)
        : options_(options), rng_(options.seed), reference_rng_(~options.seed) {}

    cpress::Bytes run() {
        system_event('O');
        for (std::uint16_t locate = 1; locate <= kStocks; ++locate) directory(locate);
        while (messages_ < options_.messages) next();
        return std::move(stream_);
    }

private:
    // A message under construction: its bytes, zero until a field is set.
    struct Message {
        explicit Message(char type) : layout(itch::layout_of(static_cast<std::uint8_t>(type))) {
            bytes[0] = static_cast<std::uint8_t>(type);
        }
        void set(std::string_view name, std::uint64_t value) {
            const Field field = field_of(layout->type, name);
            itch::store_big_endian(std::span(bytes).subspan(field.offset, field.length), value);
        }
        void set_text(std::string_view name, std::string_view text) {
            const Field field = field_of(layout->type, name);
            for (std::size_t i = 0; i < field.length; ++i) {
                bytes[field.offset + i] =
                    i < text.size() ? static_cast<std::uint8_t>(text[i]) : ' ';
            }
        }
        const itch::MessageLayout* layout;  // of an ITCH type: never null
        std::array<std::uint8_t, itch::kMaxMessageBytes> bytes{};
    };

    std::uint64_t below(std::uint64_t limit) {
        return rng_() % limit;
    }

    // Starts a message: its type and the fields every message has.
    Message begin(char type, std::uint16_t locate) {
        Message message(type);
        timestamp_ += 1 + below(below(8) == 0 ? 50'000'000 : 20'000);
        message.set("stock_locate", locate);
        message.set("tracking_number", below(16) == 0 ? below(4) : 0);
        message.set("timestamp", timestamp_);
        return message;
    }

    void emit(const Message& message) {
        const std::size_t length = message.layout->length;
        stream_.push_back(static_cast<std::uint8_t>(length >> 8));
        stream_.push_back(static_cast<std::uint8_t>(length));
        stream_.insert(stream_.end(), message.bytes.begin(),
                       message.bytes.begin() + static_cast<std::ptrdiff_t>(length));
        ++messages_;
    }

    static std::string_view symbol_of(std::uint16_t locate) {
        constexpr std::array<std::string_view, kStocks> kSymbols{"AAPL", "MSFT", "SPY",
                                                                 "QQQ",  "TSLA", "ZVZZT"};
        return kSymbols.at(locate - 1U);
    }

    void system_event(char code) {
        Message message = begin('S', 0);
        message.set("event_code", static_cast<std::uint8_t>(code));
        emit(message);
    }

    void directory(std::uint16_t locate) {
        Message message = begin('R', locate);
        message.set_text("stock", symbol_of(locate));
        message.set("market_category", 'Q');
        message.set("financial_status_indicator", 'N');
        message.set("round_lot_size", 100);
        message.set("round_lots_only", 'N');
        message.set("issue_classification", 'C');
        message.set_text("issue_sub_type", "Z");
        message.set("authenticity", 'P');
        message.set("short_sale_threshold_indicator", 'N');
        message.set("ipo_flag", ' ');
        message.set("luld_reference_price_tier", '1');
        message.set("etp_flag", 'N');
        message.set("inverse_indicator", 'N');
        emit(message);
    }

    // The stock of the next message: usually the last one, as in a burst.
    std::uint16_t next_stock() {
        if (below(3) != 0) return last_stock_;
        last_stock_ = static_cast<std::uint16_t>(1 + below(kStocks));
        return last_stock_;
    }

    // A price near the stock's own, in whole cents, rarely not.
    std::uint32_t price_near(std::uint16_t locate) {
        const std::uint64_t cents = (locate * 2'000ULL) + below(40);
        return static_cast<std::uint32_t>((cents * 100) + (below(50) == 0 ? below(100) : 0));
    }

    // The reference of a new order in this stock: the next of its counter,
    // or a little further on.
    std::uint64_t new_reference(std::uint16_t locate) {
        std::uint64_t& head = heads_.at(locate % kCounters);
        head += kCounters * (1 + (below(4) == 0 ? below(6) : 0));
        return head + (locate % kCounters);
    }

    // The reference an order message carries for `order`. A random one
    // comes from a generator of its own, so that the rest of the stream is
    // the same either way.
    std::uint64_t named(const LiveOrder& order) {
        return options_.random_references ? reference_rng_() : order.reference;
    }

    void add(std::uint16_t locate) {
        const bool attributed = below(20) == 0;
        Message message = begin(attributed ? 'F' : 'A', locate);
        const LiveOrder order{.reference = new_reference(locate),
                              .locate = locate,
                              .shares = static_cast<std::uint32_t>(100 * (1 + below(5))),
                              .price = price_near(locate)};
        message.set("order_reference_number", order.reference);
        message.set("buy_sell_indicator", below(2) == 0 ? 'B' : 'S');
        message.set("shares", order.shares);
        message.set_text("stock", symbol_of(locate));
        message.set("price", order.price);
        if (attributed) message.set_text("attribution", below(2) == 0 ? "NSDQ" : "GSCO");
        emit(message);
        orders_.push_back(order);
    }

    // Takes a live order of the stock out of the list: a recent one, most
    // often. Returns false if the stock has none.
    bool take_order(std::uint16_t locate, LiveOrder& taken) {
        std::size_t skip = below(4) == 0 ? below(40) : below(3);
        for (std::size_t i = orders_.size(); i > 0; --i) {
            if (orders_[i - 1].locate != locate) continue;
            if (skip-- != 0) continue;
            taken = orders_[i - 1];
            orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i - 1));
            return true;
        }
        return false;
    }

    void remove(std::uint16_t locate, const LiveOrder& order) {
        Message message = begin('D', locate);
        message.set("order_reference_number", named(order));
        emit(message);
    }

    void replace(std::uint16_t locate, LiveOrder order) {
        Message message = begin('U', locate);
        message.set("original_order_reference_number", named(order));
        order.reference = new_reference(locate);
        order.price += 100 * static_cast<std::uint32_t>(below(3));
        message.set("new_order_reference_number", order.reference);
        message.set("shares", order.shares);
        message.set("price", order.price);
        emit(message);
        orders_.push_back(order);
    }

    // An execution or a cancel of part of the order, or all of it.
    void reduce(std::uint16_t locate, LiveOrder order) {
        const std::uint64_t kind = below(10);
        const bool all = order.shares == 1 || below(2) == 0;
        const auto shares =
            all ? order.shares : static_cast<std::uint32_t>(1 + below(order.shares - 1));
        if (kind < 6) {
            Message message = begin('E', locate);
            message.set("order_reference_number", named(order));
            message.set("executed_shares", shares);
            message.set("match_number", ++match_);
            emit(message);
        } else if (kind < 7) {
            Message message = begin('C', locate);
            message.set("order_reference_number", named(order));
            message.set("executed_shares", shares);
            message.set("match_number", ++match_);
            message.set("printable", 'Y');
            message.set("execution_price", order.price + 100);
            emit(message);
        } else {
            Message message = begin('X', locate);
            message.set("order_reference_number", named(order));
            message.set("cancelled_shares", shares);
            emit(message);
        }
        if (shares < order.shares) {
            order.shares -= shares;
            orders_.push_back(order);
        }
    }

    void trade(std::uint16_t locate) {
        Message message = begin('P', locate);
        message.set("buy_sell_indicator", 'B');
        message.set("shares", 100 * (1 + below(3)));
        message.set_text("stock", symbol_of(locate));
        message.set("price", price_near(locate));
        message.set("match_number", ++match_);
        emit(message);
    }

    void imbalance(std::uint16_t locate) {
        Message message = begin('I', locate);
        message.set("paired_shares", 1'000 * below(50));
        message.set("imbalance_shares", 100 * below(50));
        message.set("imbalance_direction", below(2) == 0 ? 'B' : 'S');
        message.set_text("stock", symbol_of(locate));
        message.set("far_price", price_near(locate));
        message.set("near_price", price_near(locate));
        message.set("current_reference_price", price_near(locate));
        message.set("cross_type", 'O');
        message.set("price_variation_indicator", 'L');
        emit(message);
    }

    void trading_action(std::uint16_t locate) {
        Message message = begin('H', locate);
        message.set_text("stock", symbol_of(locate));
        message.set("trading_state", below(2) == 0 ? 'T' : 'H');
        message.set("reserved", ' ');
        message.set_text("reason", "");
        emit(message);
    }

    void next() {
        const std::uint16_t locate = next_stock();
        const std::uint64_t roll = below(100);
        LiveOrder order{};
        if (roll < 46) {
            add(locate);
        } else if (roll < 78) {
            if (take_order(locate, order)) remove(locate, order);
        } else if (roll < 88) {
            if (take_order(locate, order)) replace(locate, order);
        } else if (roll < 96) {
            if (take_order(locate, order)) reduce(locate, order);
        } else if (roll < 98) {
            trade(locate);
        } else if (roll < 99) {
            imbalance(locate);
        } else {
            trading_action(locate);
        }
    }

    ItchSampleOptions options_;
    std::mt19937_64 rng_;
    std::mt19937_64 reference_rng_;
    cpress::Bytes stream_;
    std::size_t messages_ = 0;
    std::vector<LiveOrder> orders_;  // oldest first
    std::array<std::uint64_t, kCounters> heads_{};
    std::uint64_t timestamp_ = 0;
    std::uint64_t match_ = 0;
    std::uint16_t last_stock_ = 1;
};

}  // namespace

cpress::Bytes itch_sample(const ItchSampleOptions& options) {
    return Generator(options).run();
}

cpress::Bytes random_itch_message(char type, std::mt19937_64& rng) {
    const itch::MessageLayout& layout = *itch::layout_of(static_cast<std::uint8_t>(type));
    cpress::Bytes framed(itch::kLengthPrefixBytes + layout.length);
    const std::span<std::uint8_t> bytes(framed);
    itch::store_big_endian(bytes.first(itch::kLengthPrefixBytes), layout.length);
    for (std::uint8_t& byte : bytes.subspan(itch::kLengthPrefixBytes)) {
        byte = static_cast<std::uint8_t>(rng() >> 56);
    }
    framed[itch::kLengthPrefixBytes] = static_cast<std::uint8_t>(type);
    return framed;
}

std::size_t count_itch_messages(cpress::ByteView stream) {
    std::size_t count = 0;
    while (const itch::MessageLayout* layout = itch::framed_message_at(stream)) {
        stream = stream.subspan(itch::kLengthPrefixBytes + layout->length);
        ++count;
    }
    return stream.empty() ? count : 0;
}

}  // namespace cpress::test
