// FieldCoder: symbols and numbers come back from a decoder given what an
// encoder wrote; every prediction is one the coder accepts; and symbols
// that should not share a slot do not.
#include "check.hpp"

#include "codec/two_way.hpp"
#include "cpress/cpress.hpp"
#include "itch/field_coder.hpp"
#include "itch/residual.hpp"
#include "model/predictor.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace cpress::test {
namespace {

using itch::Contexts;
using itch::FieldCoder;
using itch::NumberField;
using itch::Symbol;

constexpr int kSlotBits = 12;
constexpr std::size_t kSymbols = 64;

// One thing to code: a nibble, a byte, or a number of some width with some
// of its bytes modelled.
struct Item {
    enum class Kind : std::uint8_t { Nibble, Byte, Number };
    Kind kind;
    Symbol symbol;  // the first id, for a number
    int width;
    int modelled_bytes;
    Contexts contexts;
    std::uint64_t prefix;
    std::uint64_t value;
};

// A seeded mix of all three, with a few contexts that repeat, so that the
// tables learn, and values that are often small, as residuals are.
std::vector<Item> items(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Item> list;
    list.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Item item{};
        item.contexts = {rng() % 3, rng() % 17, rng(), rng() % 2};
        item.prefix = rng() % 5;
        switch (rng() % 3) {
            case 0:
                item.kind = Item::Kind::Nibble;
                item.symbol = static_cast<Symbol>(rng() % 8);
                item.value = rng() % 16;
                break;
            case 1:
                item.kind = Item::Kind::Byte;
                item.symbol = static_cast<Symbol>(8 + (rng() % 8));
                item.value = rng() % 256;
                break;
            default:
                item.kind = Item::Kind::Number;
                item.width = 1 + static_cast<int>(rng() % 8);
                // A number of width w takes w + 1 ids; nine fit from 16 on.
                item.symbol = static_cast<Symbol>(16 + (9 * (rng() % 5)));
                item.modelled_bytes = static_cast<int>(rng() % 9);
                item.value = rng() & itch::detail::field_mask(item.width);
                if (rng() % 2 == 0) item.value >>= rng() % 64;  // often short
                break;
        }
        list.push_back(item);
    }
    return list;
}

// Codes `item` and returns what the coder returned.
template <codec::TwoWayCoder C>
std::uint64_t code(FieldCoder& fields, C& coder, const Item& item, std::uint64_t value) {
    switch (item.kind) {
        case Item::Kind::Nibble:
            return fields.nibble(coder, item.symbol, item.contexts, item.prefix,
                                 static_cast<std::uint32_t>(value));
        case Item::Kind::Byte:
            return fields.byte(coder, item.symbol, item.contexts, item.prefix,
                               static_cast<std::uint32_t>(value));
        default:
            return fields.number(coder, NumberField{.first = item.symbol, .width = item.width},
                                 item.contexts, value, item.modelled_bytes);
    }
}

void test_round_trip() {
    const std::vector<Item> list = items(50'000, 0xF1E1D);
    cpress::Bytes payload;
    {
        FieldCoder fields(kSlotBits, kSymbols);
        codec::EncodingCoder coder(payload);
        std::size_t changed = 0;
        for (const Item& item : list) {
            // An encoder gets back the value it gave.
            if (code(fields, coder, item, item.value) != item.value) ++changed;
        }
        coder.finish();
        CHECK(changed == 0);
        CHECK(fields.valid());
    }

    FieldCoder fields(kSlotBits, kSymbols);
    codec::DecodingCoder coder(payload);
    std::size_t wrong = 0;
    for (const Item& item : list) {
        // The value passed in is ignored: pass one that is certainly not it.
        if (code(fields, coder, item, ~item.value & 0xF) != item.value) ++wrong;
    }
    CHECK(wrong == 0);
    CHECK(fields.valid());
    CHECK(!coder.overrun());
    CHECK(coder.consumed() == payload.size());
}

// A coder that codes nothing and keeps the range of the predictions it was
// handed.
struct RangeCoder {
    static constexpr bool kDecoding = false;
    void code(int& /*bit*/, std::uint32_t p12) noexcept {
        lowest = std::min(lowest, p12);
        highest = std::max(highest, p12);
        ++bits;
    }
    std::uint32_t lowest = model::kProbOne;
    std::uint32_t highest = 0;
    std::uint64_t bits = 0;
};
static_assert(codec::TwoWayCoder<RangeCoder>);

// A million symbols, half of them from a handful of contexts with the same
// value every time, which drives the predictions to both ends.
void test_predictions_are_in_range() {
    constexpr std::size_t kCount = 1'000'000;
    FieldCoder fields(kSlotBits, kSymbols);
    RangeCoder coder;
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(0x9A46E);
    for (std::size_t i = 0; i < kCount; ++i) {
        const auto symbol = static_cast<Symbol>(rng() % kSymbols);
        const bool certain = i % 2 == 0;
        const Contexts contexts{certain ? symbol : rng(), rng() % 7, 0, i % 3};
        // When certain, a byte that only the symbol decides.
        const std::uint32_t certain_value = symbol % 2 == 0 ? 0xFF : 0x00;
        const std::uint32_t value =
            certain ? certain_value : static_cast<std::uint32_t>(rng() % 256);
        static_cast<void>(fields.byte(coder, symbol, contexts, 0, value));
    }
    CHECK(coder.bits == kCount * 8);
    CHECK(coder.lowest >= model::kProbMin);
    CHECK(coder.highest <= model::kProbMax);
    // Both ends were approached: the certain symbols were learned.
    CHECK(coder.lowest < 64);
    CHECK(coder.highest > model::kProbOne - 64);
}

// The slot each table selected for the last nibble coded.
std::array<std::size_t, FieldCoder::kInputs> selected_slots(const FieldCoder& fields) {
    std::array<std::size_t, FieldCoder::kInputs> slots{};
    for (std::size_t i = 0; i < slots.size(); ++i) slots[i] = fields.table(i).selected_slot();
    return slots;
}

// How many tables selected the same slot in both.
std::size_t shared(const std::array<std::size_t, FieldCoder::kInputs>& a,
                   const std::array<std::size_t, FieldCoder::kInputs>& b) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) count += a[i] == b[i] ? 1 : 0;
    return count;
}

// The id is part of every key, and so is which nibble of a byte is being
// coded. With 2^20 slots a chance collision in any one table is one in a
// million, and the inputs here are fixed.
void test_symbols_do_not_share_slots() {
    constexpr int kManySlots = 20;
    const Contexts contexts{11, 22, 33, 44};
    constexpr std::uint64_t kPrefix = 5;
    FieldCoder fields(kManySlots, kSymbols);
    RangeCoder coder;

    static_cast<void>(fields.nibble(coder, 3, contexts, kPrefix, 0x9));
    const auto first = selected_slots(fields);
    static_cast<void>(fields.nibble(coder, 3, contexts, kPrefix, 0x2));
    CHECK(shared(first, selected_slots(fields)) == FieldCoder::kInputs);  // the same symbol again

    // Another id, the same contexts and prefix: no table shares.
    static_cast<void>(fields.nibble(coder, 4, contexts, kPrefix, 0x9));
    CHECK(shared(first, selected_slots(fields)) == 0);

    // Another prefix: only the table keyed by the symbol alone shares.
    static_cast<void>(fields.nibble(coder, 3, contexts, kPrefix + 1, 0x9));
    CHECK(shared(first, selected_slots(fields)) == 1);

    // Another first context: the tables without it share.
    static_cast<void>(fields.nibble(coder, 3, Contexts{12, 22, 33, 44}, kPrefix, 0x9));
    CHECK(shared(first, selected_slots(fields)) == FieldCoder::kInputs - 1);

    // A byte's second nibble is keyed apart from its first, and by what the
    // first nibble was.
    static_cast<void>(fields.byte(coder, 3, contexts, kPrefix, 0x97));
    const auto after_nine = selected_slots(fields);
    CHECK(shared(first, after_nine) == 0);
    static_cast<void>(fields.byte(coder, 3, contexts, kPrefix, 0xA7));
    CHECK(shared(after_nine, selected_slots(fields)) == 0);
}

// What a decoder does with a length no encoder writes. The length of a
// number is a nibble, so a payload can say 9 bytes, or 15, for a field of
// 2: the number is cut to the field, nothing is indexed out of range (the
// sanitizer build would say), and the coder reports it.
void test_impossible_length_is_cut_and_reported() {
    const Contexts contexts{1, 2, 3, 4};
    constexpr NumberField kField{.first = 20, .width = 2};
    for (const std::uint32_t length : {3U, 9U, 15U}) {
        cpress::Bytes payload;
        {
            FieldCoder fields(kSlotBits, kSymbols);
            codec::EncodingCoder coder(payload);
            // The length symbol, written directly, and bytes enough for any
            // number the decoder goes on to read.
            static_cast<void>(fields.nibble(coder, kField.length(), contexts, 0, length));
            for (int i = 0; i < 16; ++i) {
                static_cast<void>(fields.byte(coder, 0, contexts, 0, 0xAB));
            }
            coder.finish();
        }
        FieldCoder fields(kSlotBits, kSymbols);
        codec::DecodingCoder coder(payload);
        const std::uint64_t number =
            fields.number(coder, kField, contexts, 0, FieldCoder::kEveryByte);
        CHECK(number <= itch::detail::field_mask(kField.width));
        CHECK(!fields.valid());
    }

    // A number whose first byte is zero is another: its length said the
    // byte was needed.
    cpress::Bytes payload;
    {
        FieldCoder fields(kSlotBits, kSymbols);
        codec::EncodingCoder coder(payload);
        static_cast<void>(fields.nibble(coder, kField.length(), contexts, 0, 2));
        static_cast<void>(fields.byte(coder, kField.byte(1), contexts, 2, 0x00));
        static_cast<void>(fields.byte(coder, kField.byte(0), contexts, itch::mix(2, 0), 0x07));
        coder.finish();
    }
    FieldCoder fields(kSlotBits, kSymbols);
    codec::DecodingCoder coder(payload);
    CHECK(fields.number(coder, kField, contexts, 0, FieldCoder::kEveryByte) == 0x07);
    CHECK(!fields.valid());
}

void test_helpers() {
    CHECK(itch::bytes_needed(0) == 0);
    CHECK(itch::bytes_needed(1) == 1);
    CHECK(itch::bytes_needed(0xFF) == 1);
    CHECK(itch::bytes_needed(0x100) == 2);
    CHECK(itch::bytes_needed(0xFFFFFFFFFFFFFF) == 7);
    CHECK(itch::bytes_needed(0x100000000000000) == 8);
    CHECK(itch::bytes_needed(~std::uint64_t{0}) == 8);

    // The hash tells its arguments apart, and their order.
    CHECK(itch::mix(1, 2) != itch::mix(2, 1));
    CHECK(itch::mix(1, 2, 3) != itch::mix(1, 2));
    CHECK(itch::mix(0, 0) != 0);
}

}  // namespace

void run_field_coder_tests() {
    test_round_trip();
    test_predictions_are_in_range();
    test_symbols_do_not_share_slots();
    test_impossible_length_is_cut_and_reported();
    test_helpers();
}

}  // namespace cpress::test
