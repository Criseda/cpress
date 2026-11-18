// cpress: public API.
//
// An archive is a fixed 20-byte header followed by a codec-specific payload.
// All multi-byte fields are little-endian.
//
//   offset  size  field
//   0       4     magic "CPRS"
//   4       1     format version
//   5       1     codec id
//   6       2     reserved, must be zero
//   8       8     original size in bytes
//   16      4     CRC-32 of the original data
//   20      ...   payload
//
// The codec id selects how the payload is read:
//
//   id  name    payload
//   0   stored  The original bytes.
//   1   order0  Arithmetic-coded bits, each byte most significant bit first,
//               then the coder's 4 flush bytes. The payload must be consumed
//               exactly, and the original size may not exceed 4096 times the
//               payload size. Frozen at v0.1.
//   2   mix6    One byte, the model's memory level, 10 to 22; then
//               arithmetic-coded bits and 4 flush bytes as for order0, with
//               the same two rules applied to the bytes after the level.
//               Any other level is corrupt. The decoder allocates the tables
//               the level calls for, from 192 KiB at 10 to 640.5 MiB at 22,
//               whatever the original size. Frozen at v0.2.
//   3   mix9    As mix6, with a level of 10 to 23. The decoder allocates
//               what the level calls for, from 4.3 MiB at 10 to 980.4 MiB at
//               23, whatever the original size. Frozen at v0.3.
//   4   itch    One byte, the model's memory level, 10 to 23; then
//               arithmetic-coded bits and 4 flush bytes. The bits code a
//               sequence of things, each either an ITCH 5.0 message without
//               its 2-byte length prefix, or one byte that is not part of a
//               message (docs/stages/stage-5.md). The payload after the
//               level must be consumed exactly, and the original size may
//               not exceed 33,792 times its size. The decoder allocates what
//               the level calls for, from 12.3 MiB at 10 to 364 MiB at 23,
//               whatever the original size. Frozen at v0.5.
//   5   words   One byte, the model's memory level, 10 to 25; then the size
//               of a word stream, 8 bytes; then that stream, as arithmetic-
//               coded bits and 4 flush bytes, with the same two rules for
//               the bytes after the size. The model is mix9's with an
//               order-7 table (Mix10). The stream is the original with its
//               frequent words replaced by codes, and a dictionary of them
//               at its head (src/text/word_transform.hpp); it must expand
//               to exactly the original size. The decoder allocates what the
//               level calls for, from 4.3 MiB at 10 to 4,228.4 MiB at 25,
//               and the stream. Frozen at v1.1.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace cpress {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

// Thrown for malformed archives, unknown codecs and I/O failures.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ByteSource;  // cpress/stream.hpp
class ByteSink;

// ---------------------------------------------------------------- codecs

// A codec turns input bytes into a payload and back. Dispatch happens once
// per file, so plain function pointers are enough here; the per-bit model
// code inside a codec is composed at compile time.
struct Codec {
    std::uint8_t id;
    std::string_view name;
    std::string_view summary;
    // Appends the payload for `input` to `output`.
    void (*encode)(ByteView input, Bytes& output);
    // Appends the decoded bytes to `output`. `original_size` comes from an
    // untrusted header: never allocate from it without a sanity check.
    void (*decode)(ByteView payload, std::uint64_t original_size, Bytes& output);
    // The same two, a block at a time, for a codec that can work without
    // holding its input or its output; null for one that cannot. They make
    // and read exactly the payload encode and decode do. `size` is how many
    // bytes `input` will give.
    void (*encode_stream)(ByteSource& input, std::uint64_t size, ByteSink& output) = nullptr;
    void (*decode_stream)(ByteView payload, std::uint64_t original_size,
                          ByteSink& output) = nullptr;
};

std::span<const Codec> codecs() noexcept;
const Codec* find_codec(std::uint8_t id) noexcept;
const Codec* find_codec(std::string_view name) noexcept;
const Codec& default_codec() noexcept;

// ------------------------------------------------------------- container

inline constexpr std::array<std::uint8_t, 4> kMagic{'C', 'P', 'R', 'S'};
inline constexpr std::uint8_t kFormatVersion = 1;
inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::string_view kExtension = ".cpz";

struct Header {
    std::uint8_t version;
    std::uint8_t codec_id;
    std::uint64_t original_size;
    std::uint32_t crc32;
};

// Parses and validates the header. Throws Error if it is malformed.
Header read_header(ByteView archive);

// Returns a complete archive (header + payload) for `input`.
Bytes compress(ByteView input, const Codec& codec = default_codec());

// Returns the original data. Throws Error if the archive is malformed or
// the decoded data fails the size or checksum check.
Bytes decompress(ByteView archive);

// ---------------------------------------------------------------- checksum

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320). `previous` is the
// CRC-32 of the bytes before `data`, so that data arriving in pieces can be
// summed as it comes: crc32(b, crc32(a)) is the CRC-32 of a followed by b.
std::uint32_t crc32(ByteView data, std::uint32_t previous = 0) noexcept;

}  // namespace cpress
