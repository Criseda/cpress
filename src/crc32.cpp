#include "cpress/cpress.hpp"

namespace cpress {
namespace {

constexpr std::array<std::uint32_t, 256> make_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1u) != 0 ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr auto kTable = make_table();

}  // namespace

std::uint32_t crc32(ByteView data) noexcept {
    std::uint32_t c = 0xFFFFFFFFu;
    for (const std::uint8_t byte : data) {
        c = kTable[(c ^ byte) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

}  // namespace cpress
