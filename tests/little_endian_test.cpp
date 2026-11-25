// Little-endian integers: the least significant byte comes first, a store
// writes only the bytes it is given, and a load gives back what was stored
// for every width of 1 to 8 bytes.
#include "check.hpp"

#include "little_endian.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cpress::test {
namespace {

using detail::load_little_endian;
using detail::store_little_endian;

void test_least_significant_byte_first() {
    std::array<std::uint8_t, 4> bytes{};
    store_little_endian(bytes, 0x12345678);
    CHECK((bytes == std::array<std::uint8_t, 4>{0x78, 0x56, 0x34, 0x12}));
    CHECK(load_little_endian(bytes) == 0x12345678);
}

void test_store_keeps_the_low_bytes() {
    std::array<std::uint8_t, 2> bytes{};
    store_little_endian(bytes, 0xAABBCCDD);
    CHECK((bytes == std::array<std::uint8_t, 2>{0xDD, 0xCC}));
}

void test_load_inverts_store_at_every_width() {
    constexpr std::uint64_t kValue = 0x0123456789ABCDEF;
    for (std::size_t width = 1; width <= 8; ++width) {
        std::array<std::uint8_t, 8> buffer{};
        const std::span<std::uint8_t> bytes = std::span(buffer).first(width);
        store_little_endian(bytes, kValue);
        const std::uint64_t low_bytes =
            width == 8 ? kValue : kValue & ((std::uint64_t{1} << (8 * width)) - 1);
        CHECK(load_little_endian(bytes) == low_bytes);
    }
}

}  // namespace

void run_little_endian_tests() {
    test_least_significant_byte_first();
    test_store_keeps_the_low_bytes();
    test_load_inverts_store_at_every_width();
}

}  // namespace cpress::test
