// Streaming: file to file makes the same archive and the same data as
// buffer to buffer, for every codec; block edges are not special; and a bad
// archive leaves nothing behind.
#include "check.hpp"
#include "itch_sample.hpp"

#include "codec/buffer_stream.hpp"
#include "codec/messages.hpp"
#include "cpress/cpress.hpp"
#include "cpress/io.hpp"
#include "cpress/stream.hpp"
#include "itch/message_model.hpp"
#include "itch/schema.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cpress::test {
namespace {

namespace fs = std::filesystem;

// A directory of its own for the files a test writes, removed with
// everything in it when the test ends.
class ScratchDirectory {
public:
    ScratchDirectory()
        : path_(fs::temp_directory_path() /
                ("cpress-tests-" + std::to_string(std::random_device{}()))) {
        fs::create_directories(path_);
    }
    // Owns a directory on disk: not copied, and removing it is the
    // destructor's job.
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;
    // NOLINTNEXTLINE(bugprone-exception-escape): only allocation failure can throw here.
    ~ScratchDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    [[nodiscard]] fs::path file(const std::string& name) const {
        return path_ / name;
    }

    // The names of the files in it, sorted.
    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> found;
        for (const fs::directory_entry& entry : fs::directory_iterator(path_)) {
            found.push_back(entry.path().filename().string());
        }
        std::ranges::sort(found);
        return found;
    }

private:
    fs::path path_;
};

// The archive compress_file() makes of `data` and what decompress_file()
// makes of that, both through files.
struct ThroughFiles {
    cpress::Bytes archive;
    cpress::Bytes restored;
};

ThroughFiles through_files(const ScratchDirectory& scratch, const cpress::Bytes& data,
                           const cpress::Codec& codec) {
    const fs::path input = scratch.file("input");
    const fs::path archive = scratch.file("archive.cpz");
    const fs::path output = scratch.file("output");
    cpress::write_file(input, data);
    cpress::compress_file(input, archive, codec);
    cpress::decompress_file(archive, output);
    return {.archive = cpress::read_file(archive), .restored = cpress::read_file(output)};
}

// The two ways in and the three ways out agree, for every codec: those that
// stream and those that are run on whole buffers behind the same functions.
void test_files_and_buffers_agree() {
    const ScratchDirectory scratch;
    const std::vector<cpress::Bytes> inputs{
        {},
        {0x42},
        sample_text(3'000),
        random_bytes(5'000, 0x57E4),
        itch_sample({.messages = 2'000, .seed = 0xF11E}),
    };
    for (const cpress::Codec& codec : cpress::codecs()) {
        for (const cpress::Bytes& data : inputs) {
            try {
                const cpress::Bytes archive = cpress::compress(data, codec);
                const ThroughFiles files = through_files(scratch, data, codec);
                CHECK(files.archive == archive);
                CHECK(files.restored == data);
                CHECK(cpress::decompress(archive) == data);

                cpress::Bytes streamed;
                codec::VectorSink sink(streamed);
                cpress::decompress(archive, sink);
                CHECK(streamed == data);
            } catch (const std::exception& e) {
                ++g_failures;
                std::cerr << "codec '" << codec.name << "' through files threw: " << e.what()
                          << "\n";
            }
        }
    }
    // Only the three files of the last round are left: no temporary one.
    CHECK(scratch.names() == (std::vector<std::string>{"archive.cpz", "input", "output"}));
}

// A sink that keeps what it is given and how it came.
class RecordingSink final : public ByteSink {
public:
    void write(cpress::ByteView bytes) override {
        all.insert(all.end(), bytes.begin(), bytes.end());
        largest = std::max(largest, bytes.size());
        ++writes;
    }
    cpress::Bytes all;
    std::size_t largest = 0;
    std::size_t writes = 0;
};

// A source that gives a few bytes at a time, never a full buffer.
class TrickleSource final : public ByteSource {
public:
    explicit TrickleSource(cpress::ByteView bytes) : left_(bytes) {}
    std::size_t read(std::span<std::uint8_t> buffer) override {
        const std::size_t count = std::min({buffer.size(), left_.size(), std::size_t{7}});
        std::ranges::copy(left_.first(count), buffer.begin());
        left_ = left_.subspan(count);
        return count;
    }

private:
    cpress::ByteView left_;
};

// The driver reads and writes a block at a time, and the block edges are
// nowhere in what it writes. With blocks as small as the driver allows,
// every message of a stream straddles an edge somewhere, the last block is
// part full, and the source gives short counts; the payload is the one made
// with a single block, and it decodes to the stream at every block size.
void test_block_edges_change_nothing() {
    constexpr int kLevel = itch::MessageModel::kMinLevel;
    cpress::Bytes stream = itch_sample({.messages = 1'500, .seed = 0xED6E});
    stream.resize(stream.size() - 5);                              // half a message at the end
    stream.insert(stream.begin() + 4'000, {0, 1, 2, 3, 4, 5, 6});  // and bytes that are not one

    cpress::Bytes one_block;
    {
        itch::MessageModel model(kLevel);
        codec::SpanSource source(stream);
        codec::VectorSink sink(one_block);
        CHECK(codec::encode_messages(model, source, sink) == stream.size());
    }

    for (const std::size_t block :
         {itch::kMaxFramedBytes, itch::kMaxFramedBytes + 1, std::size_t{100}, std::size_t{4'096},
          stream.size() - 1, stream.size(), stream.size() + 1}) {
        RecordingSink coded;
        {
            itch::MessageModel model(kLevel);
            TrickleSource source(stream);
            CHECK(codec::encode_messages(model, source, coded, block) == stream.size());
        }
        CHECK(coded.all == one_block);

        RecordingSink decoded;
        itch::MessageModel model(kLevel);
        codec::decode_messages(model, coded.all, stream.size(), decoded, block);
        CHECK(decoded.all == stream);
        // Handed over a block at a time: never much more than a block.
        CHECK(decoded.largest < block + itch::kMaxFramedBytes);
        // More than one write, unless one block holds everything.
        if (block < stream.size()) CHECK(decoded.writes > 1);
    }
}

// And at the real block size, through files: an input of one block and one
// byte, so that the last read finds a single byte.
void test_more_than_a_block_through_files() {
    const cpress::Codec* codec = cpress::find_codec(std::string_view{"itch"});
    CHECK(codec != nullptr);
    if (codec == nullptr) return;
    CHECK(codec->encode_stream != nullptr);
    CHECK(codec->decode_stream != nullptr);

    cpress::Bytes data = itch_sample({.messages = 40'000, .seed = 0xB10C});
    CHECK(data.size() > codec::kBlockBytes + 1);
    data.resize(codec::kBlockBytes + 1);

    const ScratchDirectory scratch;
    const ThroughFiles files = through_files(scratch, data, *codec);
    CHECK(files.restored == data);
    CHECK(files.archive == cpress::compress(data, *codec));
}

// True if decompress_file() throws cpress::Error for `archive` and leaves
// the directory holding the archive alone.
bool rejected_leaving_nothing(const ScratchDirectory& scratch, const cpress::Bytes& archive) {
    const fs::path path = scratch.file("bad.cpz");
    cpress::write_file(path, archive);
    const bool threw = throws_error([&] { cpress::decompress_file(path, scratch.file("output")); });
    return threw && scratch.names() == std::vector<std::string>{"bad.cpz"};
}

// A bad archive leaves no output and no temporary file, whichever check
// catches it: the codec's own, the size, or the checksum, which only fails
// once everything has been written.
void test_bad_archive_leaves_nothing() {
    const ScratchDirectory scratch;
    const cpress::Bytes data = itch_sample({.messages = 3'000, .seed = 0xBAD});
    for (const cpress::Codec& codec : cpress::codecs()) {
        const cpress::Bytes good = cpress::compress(data, codec);

        cpress::Bytes corrupt = good;
        corrupt.at(good.size() / 2) ^= 0x40;
        CHECK(rejected_leaving_nothing(scratch, corrupt));

        cpress::Bytes truncated = good;
        truncated.resize(good.size() - 7);
        CHECK(rejected_leaving_nothing(scratch, truncated));

        cpress::Bytes wrong_checksum = good;
        wrong_checksum.at(cpress::kHeaderSize - 1) ^= 0x01;
        CHECK(rejected_leaving_nothing(scratch, wrong_checksum));

        CHECK(rejected_leaving_nothing(scratch, {}));
    }

    // And compress_file() leaves no archive when it cannot read its input.
    CHECK(throws_error(
        [&] { cpress::compress_file(scratch.file("no-such-input"), scratch.file("never.cpz")); }));
    CHECK(scratch.names() == std::vector<std::string>{"bad.cpz"});

    // An output that was there before a failed decompression is still there.
    const fs::path kept = scratch.file("output");
    cpress::write_file(kept, cpress::Bytes{1, 2, 3});
    CHECK(throws_error([&] { cpress::decompress_file(scratch.file("bad.cpz"), kept); }));
    CHECK(cpress::read_file(kept) == (cpress::Bytes{1, 2, 3}));
}

// The checksum of data that arrives in pieces is the checksum of the whole.
void test_crc32_in_pieces() {
    const cpress::Bytes data = random_bytes(10'000, 0xC2C);
    const cpress::ByteView whole(data);
    const std::uint32_t expected = cpress::crc32(whole);
    for (const std::size_t cut :
         {std::size_t{0}, std::size_t{1}, std::size_t{4'096}, data.size()}) {
        CHECK(cpress::crc32(whole.subspan(cut), cpress::crc32(whole.first(cut))) == expected);
    }
    std::uint32_t crc = 0;
    for (const std::uint8_t& byte : data) crc = cpress::crc32({&byte, 1}, crc);
    CHECK(crc == expected);
}

}  // namespace

void run_stream_tests() {
    try {
        test_crc32_in_pieces();
        test_files_and_buffers_agree();
        test_block_edges_change_nothing();
        test_more_than_a_block_through_files();
        test_bad_archive_leaves_nothing();
    } catch (const std::exception& e) {
        ++g_failures;
        std::cerr << "stream tests threw: " << e.what() << "\n";
    }
}

}  // namespace cpress::test
