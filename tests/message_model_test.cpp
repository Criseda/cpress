// MessageModel: a stream of messages comes back from a decoder given what
// an encoder wrote; the market is the same in both directions after every
// message; the order table is worth what it costs; and what no encoder
// writes is refused.
#include "check.hpp"
#include "itch_sample.hpp"

#include "codec/buffer_stream.hpp"
#include "codec/messages.hpp"
#include "codec/two_way.hpp"
#include "cpress/cpress.hpp"
#include "itch/field_coder.hpp"
#include "itch/market.hpp"
#include "itch/message_model.hpp"
#include "itch/schema.hpp"
#include "model/apm.hpp"
#include "model/probability_map.hpp"
#include "model/state_table.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <string>

namespace cpress::test {
namespace {

using itch::MessageModel;

// The payload codec 4 would write for `stream` after its level byte, with
// the model at `level`.
cpress::Bytes coded(cpress::ByteView stream, int level,
                    std::uint32_t max_walk = MessageModel::kMaxWalk) {
    MessageModel model(level, max_walk);
    codec::SpanSource source(stream);
    cpress::Bytes payload;
    codec::VectorSink sink(payload);
    CHECK(codec::encode_messages(model, source, sink) == stream.size());
    return payload;
}

// What decode_messages makes of `payload`, or the text of the Error it
// throws.
struct Decoded {
    cpress::Bytes bytes;
    std::string error;
};

Decoded decoded(cpress::ByteView payload, std::uint64_t size, int level,
                std::uint32_t max_walk = MessageModel::kMaxWalk) {
    Decoded result;
    try {
        MessageModel model(level, max_walk);
        codec::VectorSink sink(result.bytes);
        codec::decode_messages(model, payload, size, sink);
    } catch (const cpress::Error& e) {
        result.error = e.what();
    }
    return result;
}

// A stream comes back, at the smallest level, where the order table is
// smaller than the orders alive and evicts, and at one where it never does.
void test_round_trip() {
    const cpress::Bytes stream = itch_sample({.messages = 30'000, .seed = 0x17C4});
    CHECK(count_itch_messages(stream) == 30'000);

    constexpr int kRoomyLevel = 16;
    for (const int level : {MessageModel::kMinLevel, kRoomyLevel}) {
        const cpress::Bytes payload = coded(stream, level);
        const Decoded result = decoded(payload, stream.size(), level);
        CHECK(result.error.empty());
        CHECK(result.bytes == stream);
    }

    // The roomy level is roomy: nothing is evicted. The smallest can hold
    // 1,024 orders and this stream keeps more alive.
    MessageModel roomy(kRoomyLevel);
    MessageModel small(MessageModel::kMinLevel);
    for (MessageModel* model : {&roomy, &small}) {
        codec::SpanSource source(stream);
        cpress::Bytes payload;
        codec::VectorSink sink(payload);
        static_cast<void>(codec::encode_messages(*model, source, sink));
    }
    CHECK(roomy.evictions() == 0);
    CHECK(small.evictions() > 0);
}

// A coder that codes nothing: for running a model as an encoder runs it,
// without an output.
struct NoCoder {
    static constexpr bool kDecoding = false;
    void code(int& /*bit*/, std::uint32_t /*p12*/) noexcept {
        ++bits;
    }
    std::uint64_t bits = 0;
};
static_assert(codec::TwoWayCoder<NoCoder>);

// The market is the same in both directions. One model is run as the
// encoder runs it, one decodes what an encoder wrote, and the two markets
// are compared: after each of the first messages, then at intervals (a
// comparison reads every stock and every order), and at the end.
void test_market_is_the_same_both_ways() {
    constexpr int kLevel = MessageModel::kMinLevel;
    constexpr std::size_t kMessages = 6'000;
    constexpr std::size_t kCompareEach = 200;  // messages compared one by one
    constexpr std::size_t kInterval = 97;      // and then one in this many
    const cpress::Bytes stream = itch_sample({.messages = kMessages, .seed = 0xB07});
    const cpress::Bytes payload = coded(stream, kLevel);

    const auto encoding = std::make_unique<MessageModel>(kLevel);
    const auto decoding = std::make_unique<MessageModel>(kLevel);
    NoCoder no_coder;
    codec::DecodingCoder decoder(payload);
    CHECK(encoding->market() == decoding->market());

    cpress::ByteView left = stream;
    std::size_t differences = 0;
    for (std::size_t i = 0; i < kMessages; ++i) {
        const itch::MessageLayout* layout = itch::framed_message_at(left);
        CHECK(layout != nullptr);
        if (layout == nullptr) return;
        std::array<std::uint8_t, itch::kMaxMessageBytes> original{};
        std::array<std::uint8_t, itch::kMaxMessageBytes> restored{};
        const std::span<std::uint8_t> read(original.data(), layout->length);
        const std::span<std::uint8_t> filled(restored.data(), layout->length);
        std::memcpy(read.data(), left.data() + itch::kLengthPrefixBytes, read.size());

        static_cast<void>(encoding->code_type(no_coder, read[0]));
        CHECK(encoding->code_message(no_coder, read));
        filled[0] = static_cast<std::uint8_t>(decoding->code_type(decoder, 0));
        CHECK(filled[0] == read[0]);
        if (filled[0] != read[0]) return;
        CHECK(decoding->code_message(decoder, filled));
        if (original != restored) ++differences;

        if (i < kCompareEach || i % kInterval == 0 || i + 1 == kMessages) {
            if (!(encoding->market() == decoding->market())) ++differences;
        }
        left = left.subspan(itch::kLengthPrefixBytes + layout->length);
    }
    CHECK(differences == 0);
    CHECK(left.empty());
}

// The order table earns its place. The same stream, with every reference
// to an existing order replaced by a random one, still round-trips: no
// order is ever found, and each is named in full. A random reference is 64
// bits that nothing predicts, and about four messages in ten name an order,
// so the archive grows by more than two bytes a message.
void test_unknown_orders_cost_bits() {
    constexpr std::size_t kMessages = 20'000;
    constexpr int kLevel = 16;
    const cpress::Bytes known = itch_sample({.messages = kMessages, .seed = 0x0DE});
    const cpress::Bytes unknown =
        itch_sample({.messages = kMessages, .seed = 0x0DE, .random_references = true});
    CHECK(known.size() == unknown.size());

    const cpress::Bytes known_payload = coded(known, kLevel);
    const cpress::Bytes unknown_payload = coded(unknown, kLevel);
    const Decoded result = decoded(unknown_payload, unknown.size(), kLevel);
    CHECK(result.error.empty());
    CHECK(result.bytes == unknown);
    CHECK(unknown_payload.size() > known_payload.size() + (2 * kMessages));
}

// A place beyond the limit is refused. An encoder that walks further than
// the decoder writes places the decoder's limit does not allow; the first
// one ends the decode.
void test_place_beyond_limit_is_rejected() {
    constexpr int kLevel = 16;
    constexpr std::uint32_t kLongWalk = 64;
    constexpr std::uint32_t kShortWalk = 2;
    const cpress::Bytes stream = itch_sample({.messages = 5'000, .seed = 0x9A7});
    const cpress::Bytes payload = coded(stream, kLevel, kLongWalk);

    CHECK(decoded(payload, stream.size(), kLevel, kLongWalk).bytes == stream);
    const Decoded result = decoded(payload, stream.size(), kLevel, kShortWalk);
    CHECK(result.error.find("no encoder writes") != std::string::npos);
    CHECK(result.bytes != stream);
}

void test_levels_and_memory() {
    CHECK(MessageModel::level_for_size(0) == MessageModel::kMinLevel);
    CHECK(MessageModel::level_for_size(1) == MessageModel::kMinLevel);
    CHECK(MessageModel::level_for_size(8'192) == 10);
    CHECK(MessageModel::level_for_size(8'193) == 11);
    CHECK(MessageModel::level_for_size(std::uint64_t{1} << 26) == 23);
    CHECK(MessageModel::level_for_size((std::uint64_t{1} << 26) + 1) == MessageModel::kMaxLevel);
    CHECK(MessageModel::level_for_size(std::uint64_t{1} << 40) == MessageModel::kMaxLevel);
    // The benchmark and its first 30,000,000 messages both get the largest.
    CHECK(MessageModel::level_for_size(8'251'407'909) == MessageModel::kMaxLevel);
    CHECK(MessageModel::level_for_size(951'200'923) == MessageModel::kMaxLevel);

    // The order table and the six history tables, as section 7 of the
    // brief lists them.
    struct Row {
        int level;
        std::uint64_t order_table;
        std::uint64_t history_tables;
    };
    constexpr std::array<Row, 4> kRows{{{10, 32'768, 98'304},
                                        {16, 2'097'152, 6'291'456},
                                        {20, 33'554'432, 100'663'296},
                                        {23, 268'435'456, 100'663'296}}};
    constexpr std::uint64_t kStateMaps =
        itch::FieldCoder::kInputs * model::ProbabilityMap::memory_bytes(model::kStateCount);
    constexpr std::uint64_t kMixer =
        2 * std::uint64_t{itch::kSymbols.count} * itch::FieldCoder::kInputs * sizeof(std::int32_t);
    constexpr std::uint64_t kApm =
        model::Apm::memory_bytes(std::size_t{256} * itch::kSymbols.count);
    constexpr std::uint64_t kPerLocate =
        sizeof(itch::Stock) + sizeof(std::uint16_t) + (2 * sizeof(std::uint32_t));
    constexpr std::uint64_t kFixed = kStateMaps + kMixer + kApm + (itch::kLocates * kPerLocate);
    for (const Row& row : kRows) {
        CHECK(MessageModel::memory_bytes(row.level) ==
              row.order_table + row.history_tables + kFixed);
    }
    // "About 12 MiB", most of it the APM and the stocks.
    constexpr std::uint64_t kMiB = std::uint64_t{1} << 20;
    CHECK(kFixed > 11 * kMiB);
    CHECK(kFixed < 13 * kMiB);
    CHECK(itch::kSymbols.count == 429);
}

// The fewest bits a message can take, which bounds how far a payload can
// expand. Counted on the generated stream and on a message of every type
// with random fields: no message is coded in fewer bits than
// min_coded_bits() says its type needs.
void test_min_coded_bits_is_a_lower_bound() {
    const auto model = std::make_unique<MessageModel>(MessageModel::kMinLevel);
    NoCoder coder;
    auto code = [&](cpress::ByteView framed) {
        const itch::MessageLayout* layout = itch::framed_message_at(framed);
        CHECK(layout != nullptr);
        if (layout == nullptr) return std::uint64_t{0};
        std::array<std::uint8_t, itch::kMaxMessageBytes> message{};
        const std::span<std::uint8_t> body(message.data(), layout->length);
        std::memcpy(body.data(), framed.data() + itch::kLengthPrefixBytes, body.size());
        const std::uint64_t before = coder.bits;
        static_cast<void>(model->code_type(coder, body[0]));
        static_cast<void>(model->code_message(coder, body));
        return coder.bits - before;
    };

    const cpress::Bytes stream = itch_sample({.messages = 5'000, .seed = 0x817});
    cpress::ByteView left = stream;
    std::size_t too_few = 0;
    while (const itch::MessageLayout* layout = itch::framed_message_at(left)) {
        if (code(left) < static_cast<std::uint64_t>(MessageModel::min_coded_bits(*layout))) {
            ++too_few;
        }
        left = left.subspan(itch::kLengthPrefixBytes + layout->length);
    }
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(0xA11);
    for (const itch::MessageLayout& layout : itch::kLayouts) {
        const cpress::Bytes message = random_itch_message(layout.type, rng);
        if (code(message) < static_cast<std::uint64_t>(MessageModel::min_coded_bits(layout))) {
            ++too_few;
        }
    }
    CHECK(too_few == 0);

    const std::uint64_t before = coder.bits;
    static_cast<void>(model->code_type(coder, itch::Market::kEscape));
    static_cast<void>(model->code_escaped_byte(coder, 0x5A));
    CHECK(coder.bits - before == MessageModel::kEscapedByteBits);
}

// The bound is reached, by the message that sets the expansion ceiling: an
// execution whose every field is as expected takes exactly the 32 bits
// min_coded_bits() gives its type.
void test_cheapest_message_takes_its_minimum() {
    const itch::MessageLayout& add = *itch::layout_of('A');
    const itch::MessageLayout& executed = *itch::layout_of('E');
    constexpr std::uint64_t kLocate = 7;
    constexpr std::uint64_t kReference = 4'000;
    constexpr std::uint64_t kShares = 300;
    constexpr std::uint64_t kTime = 1'000'000;

    auto set = [](std::span<std::uint8_t> message, const itch::Field& field, std::uint64_t value) {
        itch::store_big_endian(itch::bytes_of(message, field), value);
    };
    std::array<std::uint8_t, itch::kMaxMessageBytes> bytes{};
    const auto model = std::make_unique<MessageModel>(MessageModel::kMinLevel);
    NoCoder coder;

    // An order is added.
    const std::span<std::uint8_t> first(bytes.data(), add.length);
    first[0] = 'A';
    set(first, itch::fields::kLocate, kLocate);
    set(first, itch::fields::kTimestamp, kTime);
    set(first, itch::fields::kAddReference, kReference);
    set(first, itch::fields::kAddSide, 'B');
    set(first, itch::fields::kAddShares, kShares);
    static_cast<void>(model->code_type(coder, first[0]));
    CHECK(model->code_message(coder, first));

    // And executed in full at once: the same stock, the newest order, all
    // its shares, the first match number, the same nanosecond.
    bytes.fill(0);
    const std::span<std::uint8_t> second(bytes.data(), executed.length);
    second[0] = 'E';
    set(second, itch::fields::kLocate, kLocate);
    set(second, itch::fields::kTimestamp, kTime);
    set(second, itch::fields::kOrderReference, kReference);
    set(second, itch::fields::kExecutedShares, kShares);
    set(second, itch::fields::kExecutedMatch, 1);
    const std::uint64_t before = coder.bits;
    static_cast<void>(model->code_type(coder, second[0]));
    CHECK(model->code_message(coder, second));
    CHECK(coder.bits - before ==
          static_cast<std::uint64_t>(MessageModel::min_coded_bits(executed)));
    CHECK(MessageModel::min_coded_bits(executed) == 32);
    CHECK(model->market().orders.live(static_cast<std::uint16_t>(kLocate)) == 0);

    // 33 bytes from 32 bits, at 8 * kMaxExpansion bits to a payload byte.
    CHECK(codec::kMaxMessageExpansion == 8 * codec::kMaxExpansion * 33 / 32);
}

}  // namespace

void run_message_model_tests() {
    test_round_trip();
    test_market_is_the_same_both_ways();
    test_unknown_orders_cost_bits();
    test_place_beyond_limit_is_rejected();
    test_levels_and_memory();
    test_min_coded_bits_is_a_lower_bound();
    test_cheapest_message_takes_its_minimum();
}

}  // namespace cpress::test
