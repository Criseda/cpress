#include "container.hpp"

#include "cpress/cpress.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace cpress {
namespace {

// Explicit little-endian serialisation: the format does not depend on the
// host's byte order or struct layout.
void put_le(std::span<std::uint8_t> out, std::uint64_t value) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

std::uint64_t get_le(ByteView in, std::size_t offset, int bytes) {
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) {
        value |= static_cast<std::uint64_t>(in[offset + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return value;
}

}  // namespace

namespace detail {

std::array<std::uint8_t, kHeaderSize> header_bytes(std::uint8_t codec_id,
                                                   std::uint64_t original_size,
                                                   std::uint32_t crc) noexcept {
    std::array<std::uint8_t, kHeaderSize> header{};  // the reserved field stays zero
    const std::span<std::uint8_t> bytes(header);
    std::ranges::copy(kMagic, bytes.begin());
    header[4] = kFormatVersion;
    header[5] = codec_id;
    put_le(bytes.subspan(8, 8), original_size);
    put_le(bytes.subspan(kCrcOffset, kCrcBytes), crc);
    return header;
}

const Codec& codec_of(const Header& header) {
    const Codec* codec = find_codec(header.codec_id);
    if (codec == nullptr) {
        throw Error("unknown codec id " + std::to_string(header.codec_id));
    }
    return *codec;
}

}  // namespace detail

Header read_header(ByteView archive) {
    if (archive.size() < kHeaderSize) {
        throw Error("not a cpress archive: shorter than the header");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), archive.begin())) {
        throw Error("not a cpress archive: bad magic");
    }

    Header header{};
    header.version = archive[4];
    header.codec_id = archive[5];
    if (header.version != kFormatVersion) {
        throw Error("unsupported format version " + std::to_string(header.version));
    }
    if (get_le(archive, 6, 2) != 0) {
        throw Error("corrupt archive: reserved header field is not zero");
    }
    header.original_size = get_le(archive, 8, 8);
    header.crc32 = static_cast<std::uint32_t>(get_le(archive, 16, 4));
    return header;
}

Bytes compress(ByteView input, const Codec& codec) {
    const auto header = detail::header_bytes(codec.id, input.size(), crc32(input));
    Bytes out(header.begin(), header.end());
    codec.encode(input, out);
    return out;
}

Bytes decompress(ByteView archive) {
    const Header header = read_header(archive);

    const Codec& codec = detail::codec_of(header);
    if (header.original_size > std::numeric_limits<std::size_t>::max()) {
        throw Error("archive is too large for this platform");
    }

    Bytes out;
    codec.decode(archive.subspan(kHeaderSize), header.original_size, out);

    if (out.size() != header.original_size) {
        throw Error("corrupt archive: decoded size does not match the header");
    }
    if (crc32(out) != header.crc32) {
        throw Error("corrupt archive: checksum mismatch");
    }
    return out;
}

}  // namespace cpress
