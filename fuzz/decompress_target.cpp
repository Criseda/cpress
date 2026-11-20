// The decoder's fuzz target. Every archive is untrusted input, so whatever
// bytes it is given, decoding must end in one of two ways: the original
// data, or cpress::Error (docs/style.md, Errors). Any other exception, a
// crash, a hang, or a sanitizer report is a bug.
//
// Each archive is decoded as decompress() decodes it, whole. An archive of
// a codec that streams is also decoded as decompress_file() decodes it, to
// a sink, through the codec's own streaming decoder, and the two must agree.
#include "fuzz_target.hpp"

#include "codec/buffer_stream.hpp"
#include "cpress/cpress.hpp"
#include "cpress/stream.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>

namespace {

using cpress::ByteView;

// An archive whose header claims more original bytes than this is skipped,
// and so is a word stream (codecs 5 and 6) claimed larger than this. A claim
// is held to the payload's size (the expansion limits in cpress.hpp), but a
// payload of a few kilobytes may still claim megabytes, which under the
// sanitizers takes seconds to decode. That costs throughput and proves
// nothing a smaller claim does not; the limits on claims are tested in
// tests/roundtrip.cpp.
constexpr std::uint64_t kMaxClaimedBytes = std::uint64_t{64} * 1024;

// An archive of a codec with a memory level that states a level above this
// is skipped. Each level sizes the model's tables differently, so levels 10
// to 12 exercise the hashing at three sizes; a higher one widens the same
// masks, and its tables, allocated for every input, would cost a run
// hundreds of megabytes and the time to clear them. The check of the level
// byte itself is tested in tests/roundtrip.cpp.
constexpr int kMaxFuzzedLevel = 12;

// From cpress.hpp: codecs 2 to 6 start their payload with the level, and
// codecs 5 and 6 follow it with the word stream's size, 8 bytes,
// little-endian.
constexpr std::uint8_t kFirstLevelledCodec = 2;
constexpr std::uint8_t kWordsCodec = 5;
constexpr std::uint8_t kWordsFlagCodec = 6;
constexpr std::size_t kStreamSizeOffset = 1;
constexpr std::size_t kStreamSizeBytes = 8;

// The word stream's size that `payload`, of codec 5 or 6, claims; none if
// the payload ends first.
std::optional<std::uint64_t> claimed_stream_size(ByteView payload) {
    if (payload.size() < kStreamSizeOffset + kStreamSizeBytes) return std::nullopt;
    std::uint64_t size = 0;
    for (std::size_t i = 0; i < kStreamSizeBytes; ++i) {
        size |= std::uint64_t{payload[kStreamSizeOffset + i]} << (8 * i);
    }
    return size;
}

// True if decoding `archive` could cost more than the limits above allow.
// An archive whose header is malformed is never skipped: it is rejected
// before anything is allocated.
bool too_costly(ByteView archive) {
    cpress::Header header{};
    try {
        header = cpress::read_header(archive);
    } catch (const cpress::Error&) {
        return false;
    }
    if (header.original_size > kMaxClaimedBytes) return true;

    const ByteView payload = archive.subspan(cpress::kHeaderSize);
    if (header.codec_id < kFirstLevelledCodec || payload.empty()) return false;
    if (payload.front() > kMaxFuzzedLevel) return true;

    if (header.codec_id != kWordsCodec && header.codec_id != kWordsFlagCodec) return false;
    const std::optional<std::uint64_t> stream_size = claimed_stream_size(payload);
    return stream_size.has_value() && *stream_size > kMaxClaimedBytes;
}

// The data a decode gave, or none if it rejected the archive.
using Decoded = std::optional<cpress::Bytes>;

Decoded decode_whole(ByteView archive) {
    try {
        return cpress::decompress(archive);
    } catch (const cpress::Error&) {
        return std::nullopt;
    }
}

Decoded decode_to_sink(ByteView archive) {
    cpress::Bytes data;
    cpress::codec::VectorSink sink(data);
    try {
        cpress::decompress(archive, sink);
    } catch (const cpress::Error&) {
        return std::nullopt;
    }
    return data;
}

// True if `archive` names a codec with its own streaming decoder, which
// decode_to_sink() reaches and decode_whole() does not. For any other codec
// both reach the same decoder, so one decode is enough.
bool streams(ByteView archive) {
    try {
        const cpress::Codec* codec = cpress::find_codec(cpress::read_header(archive).codec_id);
        return codec != nullptr && codec->decode_stream != nullptr;
    } catch (const cpress::Error&) {
        return false;
    }
}

[[noreturn]] void fail(std::string_view what) {
    std::cerr << "cpress-fuzz: " << what << "\n";
    std::abort();
}

void check_decoder(ByteView archive) {
    const Decoded whole = decode_whole(archive);
    if (!streams(archive)) return;
    if (decode_to_sink(archive) != whole) {
        fail("decoding whole and decoding to a sink disagree");
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const ByteView archive(data, size);
    if (too_costly(archive)) return cpress::fuzz::kSkipped;
    try {
        check_decoder(archive);
    } catch (const std::exception& e) {
        std::cerr << "cpress-fuzz: " << e.what() << "\n";
        fail("the decoder threw something other than cpress::Error");
    } catch (...) {
        fail("the decoder threw something that is not a std::exception");
    }
    return cpress::fuzz::kTested;
}
