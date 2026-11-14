// A look-ahead over a ByteSource that always holds the whole of the next
// ITCH message: how the message codec reads its input, and how the
// benchmark tools read a day file a message at a time.
#pragma once

#include "cpress/cpress.hpp"
#include "cpress/stream.hpp"
#include "itch/schema.hpp"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>

namespace cpress::codec::detail {

// The input from the current position on, read from a source a block at a
// time. ahead() always holds the whole of the next message, or everything
// that is left if that is less.
class InputWindow {
public:
    // `block_bytes` is at least itch::kMaxFramedBytes.
    InputWindow(ByteSource& source, std::size_t block_bytes)
        : source_(&source), buffer_(block_bytes) {
        assert(block_bytes >= itch::kMaxFramedBytes);
    }

    // The bytes from the current position: at least itch::kMaxFramedBytes
    // of them, unless the input ends sooner, and then all that is left.
    [[nodiscard]] ByteView ahead() {
        if (end_ - position_ < itch::kMaxFramedBytes && !ended_) refill();
        return ByteView(buffer_).subspan(position_, end_ - position_);
    }

    // Moves the position on by `count` bytes of ahead().
    void advance(std::size_t count) noexcept {
        position_ += count;
    }

    // True once the source has ended and every byte of it has been passed.
    [[nodiscard]] bool at_end() const noexcept {
        return ended_ && position_ == end_;
    }

private:
    // Moves what is left to the front and reads until the buffer is full or
    // the source ends.
    void refill() {
        std::memmove(buffer_.data(), buffer_.data() + position_, end_ - position_);
        end_ -= position_;
        position_ = 0;
        while (end_ < buffer_.size()) {
            const std::size_t count = source_->read(std::span(buffer_).subspan(end_));
            if (count == 0) {
                ended_ = true;
                return;
            }
            end_ += count;
        }
    }

    ByteSource* source_;  // not owned
    Bytes buffer_;
    std::size_t position_ = 0;  // of the next byte to code, in buffer_
    std::size_t end_ = 0;       // of the bytes read, in buffer_
    bool ended_ = false;        // the source has no more
};

}  // namespace cpress::codec::detail
