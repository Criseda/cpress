// The archive header as bytes, shared by the whole-buffer and the streaming
// container code. Not part of the public API.
#pragma once

#include "cpress/cpress.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cpress::detail {

// Where each header field is, as laid out in cpress.hpp. Each field starts
// where the one before it ends, and the last ends at kHeaderSize.
inline constexpr std::size_t kVersionOffset = kMagic.size();
inline constexpr std::size_t kCodecIdOffset = kVersionOffset + 1;
inline constexpr std::size_t kReservedOffset = kCodecIdOffset + 1;
inline constexpr std::size_t kReservedBytes = 2;
inline constexpr std::size_t kOriginalSizeOffset = kReservedOffset + kReservedBytes;
inline constexpr std::size_t kOriginalSizeBytes = 8;
inline constexpr std::size_t kCrcOffset = kOriginalSizeOffset + kOriginalSizeBytes;
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
