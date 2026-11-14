// MessageFile, the benchmark tools' reader: every message of a file comes
// back, whole, however the buffer's edges fall; it stops at the first bytes
// that are not a message; and it sums the CRC-32 of the whole file.
#include "check.hpp"
#include "itch_sample.hpp"

#include "cpress/cpress.hpp"
#include "cpress/io.hpp"
#include "itch/schema.hpp"
#include "message_file.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <system_error>

namespace cpress::test {
namespace {

namespace fs = std::filesystem;

// A file of its own in the temporary directory, removed with this object.
class ScratchFile {
public:
    explicit ScratchFile(const cpress::Bytes& data)
        : path_(fs::temp_directory_path() /
                ("cpress-message-file-" + std::to_string(std::random_device{}()))) {
        cpress::write_file(path_, data);
    }
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    ScratchFile(ScratchFile&&) = delete;
    ScratchFile& operator=(ScratchFile&&) = delete;
    ~ScratchFile() {
        std::error_code ignored;
        fs::remove(path_, ignored);
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

// What reading a file to its first non-message found: the messages, joined,
// and whether that was the end.
struct Read {
    cpress::Bytes messages;
    std::size_t count = 0;
    bool at_end = false;
    std::uint64_t bytes_read = 0;
    std::uint32_t crc32 = 0;
};

Read read_all(const fs::path& path) {
    cpress::bench::MessageFile file(path);
    Read read;
    std::size_t not_whole = 0;
    for (cpress::ByteView message = file.next(); !message.empty(); message = file.next()) {
        const itch::MessageLayout* layout = itch::framed_message_at(message);
        if (layout == nullptr || message.size() != itch::kLengthPrefixBytes + layout->length) {
            ++not_whole;
        }
        read.messages.insert(read.messages.end(), message.begin(), message.end());
        ++read.count;
    }
    CHECK(not_whole == 0);
    read.at_end = file.at_end();
    file.read_rest();
    read.bytes_read = file.bytes_read();
    read.crc32 = file.crc32();
    return read;
}

// More than two of the reader's 1 MiB buffers, so that messages straddle
// its edges: each comes back whole and in order.
void test_every_message_comes_back() {
    constexpr std::size_t kMessages = 80'000;
    const cpress::Bytes data = itch_sample({.messages = kMessages, .seed = 0xF11E5});
    CHECK(data.size() > std::size_t{2} << 20);
    const ScratchFile file(data);

    const Read read = read_all(file.path());
    CHECK(read.count == kMessages);
    CHECK(read.messages == data);
    CHECK(read.at_end);
    CHECK(read.bytes_read == data.size());
    CHECK(read.crc32 == cpress::crc32(data));
}

// Bytes that are not a message stop it there, and are not the end; the
// size and the checksum are still of the whole file. So is a file that ends
// in half a message.
void test_stops_at_what_is_not_a_message() {
    constexpr std::size_t kMessages = 1'000;
    const cpress::Bytes messages = itch_sample({.messages = kMessages, .seed = 0x570B});
    CHECK(count_itch_messages(messages) == kMessages);

    struct Case {
        cpress::Bytes data;
        std::size_t messages;  // before the first thing that is not one
    };
    Case junk_after{.data = messages, .messages = kMessages};
    junk_after.data.insert(junk_after.data.end(), {0x00, 0x01, 0x02, 0x03});  // no type 2
    junk_after.data.insert(junk_after.data.end(), messages.begin(), messages.end());
    Case cut_short{.data = messages, .messages = kMessages - 1};
    cut_short.data.resize(messages.size() - 3);

    for (const Case& c : {junk_after, cut_short}) {
        const ScratchFile file(c.data);
        const Read read = read_all(file.path());
        CHECK(!read.at_end);
        CHECK(read.count == c.messages);
        CHECK(read.bytes_read == c.data.size());
        CHECK(read.crc32 == cpress::crc32(c.data));
    }

    const ScratchFile empty(cpress::Bytes{});
    const Read read = read_all(empty.path());
    CHECK(read.count == 0);
    CHECK(read.at_end);
    CHECK(read.bytes_read == 0);
}

}  // namespace

void run_message_file_tests() {
    try {
        test_every_message_comes_back();
        test_stops_at_what_is_not_a_message();
        CHECK(
            throws_error([] { const cpress::bench::MessageFile file("no-such-file-for-cpress"); }));
    } catch (const std::exception& e) {
        ++g_failures;
        std::cerr << "message file tests threw: " << e.what() << "\n";
    }
}

}  // namespace cpress::test
