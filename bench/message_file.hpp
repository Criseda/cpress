// A file of ITCH messages read a message at a time through one buffer, for
// the tools that go through a day file: it is gigabytes, and they need a
// message of it at once.
#pragma once

#include "cpress/cpress.hpp"
#include "itch/schema.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>

namespace cpress::bench {

class MessageFile {
public:
    // Throws cpress::Error if the file cannot be opened.
    explicit MessageFile(const std::filesystem::path& path)
        : file_(path, std::ios::binary), buffer_(kBufferBytes) {
        if (!file_) throw Error("cannot open '" + path.string() + "' for reading");
    }

    // The next message, with its length prefix, as itch::framed_message_at()
    // sees it. The bytes are good until the next call. Empty if what comes
    // next is not a message: the end of the file, or bytes that are not
    // ITCH, at which the position stays.
    [[nodiscard]] ByteView next() {
        if (end_ - position_ < itch::kMaxFramedBytes && !ended_) refill();
        const ByteView ahead = ByteView(buffer_).subspan(position_, end_ - position_);
        const itch::MessageLayout* layout = itch::framed_message_at(ahead);
        if (layout == nullptr) return {};
        const ByteView message = ahead.first(itch::kLengthPrefixBytes + layout->length);
        position_ += message.size();
        return message;
    }

    // True once next() has gone through the whole file: it was messages
    // and nothing else.
    [[nodiscard]] bool at_end() const noexcept {
        return ended_ && position_ == end_;
    }

    // Reads whatever is left without looking at it, so that bytes_read()
    // and crc32() are of the whole file.
    void read_rest() {
        while (!ended_) {
            position_ = end_;
            refill();
        }
    }

    // How much of the file has been read into the buffer so far, and the
    // CRC-32 of that.
    [[nodiscard]] std::uint64_t bytes_read() const noexcept {
        return bytes_read_;
    }
    [[nodiscard]] std::uint32_t crc32() const noexcept {
        return crc32_;
    }

private:
    static constexpr std::size_t kBufferBytes = std::size_t{1} << 20;

    // Moves what is left to the front and reads until the buffer is full or
    // the file ends.
    void refill() {
        std::memmove(buffer_.data(), buffer_.data() + position_, end_ - position_);
        end_ -= position_;
        position_ = 0;
        const std::span<std::uint8_t> room = std::span(buffer_).subspan(end_);
        file_.read(reinterpret_cast<char*>(room.data()), static_cast<std::streamsize>(room.size()));
        const auto count = static_cast<std::size_t>(file_.gcount());
        crc32_ = cpress::crc32(room.first(count), crc32_);
        bytes_read_ += count;
        end_ += count;
        if (count < room.size()) ended_ = true;
    }

    std::ifstream file_;
    Bytes buffer_;
    std::size_t position_ = 0;  // of the next message, in buffer_
    std::size_t end_ = 0;       // of the bytes read, in buffer_
    bool ended_ = false;        // the file has no more
    std::uint64_t bytes_read_ = 0;
    std::uint32_t crc32_ = 0;
};

}  // namespace cpress::bench
