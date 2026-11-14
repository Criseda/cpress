// The archive header as bytes, shared by the whole-buffer and the streaming
// container code. Not part of the public API.
#pragma once

#include "cpress/cpress.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cpress::detail {

// Where the CRC-32 of the original data is in the header; see cpress.hpp.
inline constexpr std::size_t kCrcOffset = 16;
inline constexpr std::size_t kCrcBytes = 4;
static_assert(kCrcOffset + kCrcBytes == kHeaderSize);

// The header of an archive made by codec `codec_id` from `original_size`
// bytes whose CRC-32 is `crc`.
[[nodiscard]] std::array<std::uint8_t, kHeaderSize> header_bytes(std::uint8_t codec_id,
                                                                 std::uint64_t original_size,
                                                                 std::uint32_t crc) noexcept;

// The codec an archive's header names. Throws Error if there is none.
[[nodiscard]] const Codec& codec_of(const Header& header);

}  // namespace cpress::detail
