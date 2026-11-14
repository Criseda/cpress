// The ITCH schema: every layout is the specification's, and nothing else is
// a message type.
#include "check.hpp"

#include "cpress/cpress.hpp"
#include "itch/schema.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace cpress::test {
namespace {

// What the specification says of each type: its length, and how many
// fields follow the type byte. Typed from the tables of the PDF (sections
// 1.1 to 1.8), independently of schema.hpp.
struct Specified {
    char type;
    std::size_t length;
    std::size_t fields;
};

constexpr std::array<Specified, 23> kSpecified{{
    {'S', 12, 4}, {'R', 39, 17}, {'H', 25, 7},  {'Y', 20, 5}, {'L', 26, 8},  {'V', 35, 6},
    {'W', 12, 4}, {'K', 28, 7},  {'J', 35, 8},  {'h', 21, 6}, {'A', 36, 8},  {'F', 40, 9},
    {'E', 31, 6}, {'C', 36, 8},  {'X', 23, 5},  {'D', 19, 4}, {'U', 35, 7},  {'P', 44, 9},
    {'Q', 40, 8}, {'B', 19, 4},  {'I', 50, 12}, {'N', 20, 5}, {'O', 48, 11},
}};

void test_layouts_are_the_specifications() {
    CHECK(itch::kLayouts.size() == kSpecified.size());
    for (const Specified& specified : kSpecified) {
        const itch::MessageLayout* layout =
            itch::layout_of(static_cast<std::uint8_t>(specified.type));
        CHECK(layout != nullptr);
        if (layout == nullptr) continue;
        CHECK(layout->type == specified.type);
        CHECK(layout->length == specified.length);
        CHECK(layout->fields.size() == specified.fields);

        // The fields tile the message: each starts where the last ended.
        std::size_t at = 1;
        for (const itch::Field& field : layout->fields) {
            CHECK(field.offset == at);
            CHECK(field.length > 0);
            at += field.length;
        }
        CHECK(at == layout->length);
        CHECK(layout->length <= itch::kMaxMessageBytes);

        // Every type starts with the same three fields.
        CHECK(layout->fields[0].kind == itch::FieldKind::Locate);
        CHECK(layout->fields[1].kind == itch::FieldKind::Tracking);
        CHECK(layout->fields[2].kind == itch::FieldKind::Timestamp);
        CHECK(std::size_t{layout->fields[2].offset} + layout->fields[2].length ==
              itch::kHeaderBytes);
    }
}

// A field from each of the longer tables, at the offset and length the
// specification prints.
void test_fields_are_where_the_specification_puts_them() {
    struct Placed {
        char type;
        std::string_view name;
        std::size_t offset;
        std::size_t length;
    };
    constexpr std::array<Placed, 12> kPlaced{{
        {'R', "etp_leverage_factor", 34, 4},
        {'L', "stock", 15, 8},
        {'K', "ipo_price", 24, 4},
        {'F', "attribution", 36, 4},
        {'C', "execution_price", 32, 4},
        {'U', "new_order_reference_number", 19, 8},
        {'U', "price", 31, 4},
        {'P', "match_number", 36, 8},
        {'Q', "cross_type", 39, 1},
        {'I', "current_reference_price", 44, 4},
        {'I', "price_variation_indicator", 49, 1},
        {'O', "near_execution_time", 32, 8},
    }};
    for (const Placed& placed : kPlaced) {
        const itch::Field field = itch::field_of(placed.type, placed.name);
        CHECK(field.offset == placed.offset);
        CHECK(field.length == placed.length);
    }
}

void test_no_other_byte_is_a_type() {
    std::size_t types = 0;
    for (std::uint32_t byte = 0; byte <= 0xFF; ++byte) {
        const itch::MessageLayout* layout = itch::layout_of(static_cast<std::uint8_t>(byte));
        const bool specified = std::ranges::any_of(kSpecified, [&](const Specified& s) {
            return static_cast<std::uint8_t>(s.type) == byte;
        });
        CHECK((layout != nullptr) == specified);
        if (layout != nullptr) {
            ++types;
            CHECK(&itch::kLayouts[itch::index_of(*layout)] == layout);
        }
    }
    CHECK(types == kSpecified.size());
    CHECK(itch::layout_of(0) == nullptr);  // the codec's escape relies on it
}

void test_big_endian() {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t width = 1; width <= bytes.size(); ++width) {
        const std::span<std::uint8_t> field(bytes.data(), width);
        const std::uint64_t value = 0x0123456789ABCDEF >> (8 * (8 - width));
        itch::store_big_endian(field, value);
        CHECK(field[0] == (value >> (8 * (width - 1))));  // most significant first
        CHECK(itch::load_big_endian(field) == value);
    }
}

// The file framing: a message is a prefix equal to its type's length, and
// that many bytes.
void test_framing() {
    cpress::Bytes framed(itch::kLengthPrefixBytes + 19);
    framed[1] = 19;
    framed[2] = 'D';
    const itch::MessageLayout* layout = itch::framed_message_at(framed);
    CHECK(layout != nullptr);
    if (layout != nullptr) CHECK(layout->type == 'D');

    const cpress::ByteView whole(framed);
    CHECK(itch::framed_message_at(whole.first(framed.size() - 1)) == nullptr);  // cut short
    CHECK(itch::framed_message_at(whole.first(2)) == nullptr);
    CHECK(itch::framed_message_at({}) == nullptr);

    cpress::Bytes wrong_length = framed;
    wrong_length[1] = 20;
    CHECK(itch::framed_message_at(wrong_length) == nullptr);
    cpress::Bytes wrong_high_byte = framed;
    wrong_high_byte[0] = 1;
    CHECK(itch::framed_message_at(wrong_high_byte) == nullptr);
    cpress::Bytes no_type = framed;
    no_type[2] = 0;
    CHECK(itch::framed_message_at(no_type) == nullptr);
}

}  // namespace

void run_itch_schema_tests() {
    test_layouts_are_the_specifications();
    test_fields_are_where_the_specification_puts_them();
    test_no_other_byte_is_a_type();
    test_big_endian();
    test_framing();
}

}  // namespace cpress::test
