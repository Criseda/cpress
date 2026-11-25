// The archive container: the header that every archive starts with, and
// compress() and decompress() on whole buffers. See cpress.hpp for the
// layout.
#include "container.hpp"

#include "cpress/cpress.hpp"
#include "little_endian.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace cpress {

namespace detail {

std::array<std::uint8_t, kHeaderSize> header_bytes(std::uint8_t codec_id,
                                                   std::uint64_t original_size,
                                                   std::uint32_t crc) noexcept {
    std::array<std::uint8_t, kHeaderSize> header{};  // the reserved field stays zero
    const std::span<std::uint8_t> bytes(header);
    std::ranges::copy(kMagic, bytes.begin());
    header[kVersionOffset] = kFormatVersion;
    header[kCodecIdOffset] = codec_id;
    store_little_endian(bytes.subspan(kOriginalSizeOffset, kOriginalSizeBytes), original_size);
    store_little_endian(bytes.subspan(kCrcOffset, kCrcBytes), crc);
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
    using namespace detail;
    if (archive.size() < kHeaderSize) {
        throw Error("not a cpress archive: shorter than the header");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), archive.begin())) {
        throw Error("not a cpress archive: bad magic");
    }

    Header header{};
    header.version = archive[kVersionOffset];
    header.codec_id = archive[kCodecIdOffset];
    if (header.version != kFormatVersion) {
        throw Error("unsupported format version " + std::to_string(header.version));
    }
    if (load_little_endian(archive.subspan(kReservedOffset, kReservedBytes)) != 0) {
        throw Error("corrupt archive: reserved header field is not zero");
    }
    header.original_size =
        load_little_endian(archive.subspan(kOriginalSizeOffset, kOriginalSizeBytes));
    header.crc32 =
        static_cast<std::uint32_t>(load_little_endian(archive.subspan(kCrcOffset, kCrcBytes)));
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
