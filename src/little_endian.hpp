// Little-endian integers: how the archive stores every number wider than a
// byte, least significant byte first. Spelling the bytes out, rather than
// copying an integer's memory, makes the format the same on every machine,
// whatever its own byte order. Not part of the public API.
#pragma once

#include "cpress/cpress.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cpress::detail {

// The unsigned integer in `bytes`, 1 to 8 of them, least significant first.
// Byte i is worth 256^i, so it is shifted up by 8 * i bits.
[[nodiscard]] constexpr std::uint64_t load_little_endian(ByteView bytes) noexcept {
    assert(!bytes.empty() && bytes.size() <= 8);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= std::uint64_t{bytes[i]} << (8 * i);
    }
    return value;
}

// Writes the low bytes of `value`, least significant first, to fill `bytes`.
// Byte i is `value` shifted down by 8 * i bits, cut to its lowest 8.
constexpr void store_little_endian(std::span<std::uint8_t> bytes, std::uint64_t value) noexcept {
    assert(!bytes.empty() && bytes.size() <= 8);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

}  // namespace cpress::detail
