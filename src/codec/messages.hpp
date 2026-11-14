// The message codec driver: runs a MessageModel over an input, a message at
// a time, in both directions. It owns the framing, the escape and the size
// rules; the model owns everything about what a message means.
//
// At each position in the input the next thing is either a message or one
// byte:
//
//   a message   if itch::framed_message_at() says so: a 2-byte length that
//               equals the length of the type that follows it, and that
//               many bytes left. Coded: the type, then the message.
//   one byte    otherwise. Coded: type 0, the escape, then the byte.
//
// So any bytes round-trip, ITCH or not, without a second format. The length
// prefix is never coded: the type implies it, and a prefix that does not
// match its type is not a message. Bytes that are not ITCH go through the
// escape one at a time and compress badly, which is the right amount of
// effort for them.
//
// Both directions stream: the encoder reads its input from a ByteSource
// through a window that always holds the whole of the next message, and
// both write to a ByteSink a block at a time. The decoder's payload is one
// span: the coder reads from it bit by bit, and refilling it mid-bit would
// put a call in the per-bit loop.
//
// Payload layout: the arithmetic coder's output, then its four flush bytes,
// as for the bitwise codecs. The level byte before it belongs to the codec
// (src/codecs.cpp).
#pragma once

#include "codec/bitwise.hpp"
#include "codec/two_way.hpp"
#include "coder/arithmetic_coder.hpp"
#include "cpress/cpress.hpp"
#include "cpress/stream.hpp"
#include "itch/market.hpp"
#include "itch/message_model.hpp"
#include "itch/schema.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace cpress::codec {

namespace detail {

// The most output bytes one coded bit can stand for, as a fraction: the
// largest, over every message type and the escape, of the bytes a message
// takes in the file to the fewest bits the model can code it in.
struct BytesPerBit {
    std::uint64_t bytes;
    std::uint64_t bits;
};

[[nodiscard]] constexpr BytesPerBit most_bytes_per_coded_bit() noexcept {
    BytesPerBit most{.bytes = 1, .bits = itch::MessageModel::kEscapedByteBits};
    for (const itch::MessageLayout& layout : itch::kLayouts) {
        const BytesPerBit ratio{
            .bytes = itch::kLengthPrefixBytes + layout.length,
            .bits = static_cast<std::uint64_t>(itch::MessageModel::min_coded_bits(layout))};
        if (ratio.bytes * most.bits > most.bytes * ratio.bits) most = ratio;
    }
    return most;
}

inline constexpr BytesPerBit kMostBytesPerCodedBit = most_bytes_per_coded_bit();

// An order executed message, type E, sets it: 33 bytes with its prefix from
// 32 coded bits, when its stock, its order, its shares, its match number,
// its tracking number and its timestamp are all as expected.
static_assert(kMostBytesPerCodedBit.bytes == 33 && kMostBytesPerCodedBit.bits == 32);

}  // namespace detail

// How many output bytes a payload byte can stand for. A coded bit leaves at
// most 4095/4096 of the coder's interval whatever it is, so a payload byte
// stands for a bounded number of coded bits: kMaxExpansion counts that as
// 8 * kMaxExpansion, since the bitwise codecs spend 8 bits on every byte.
// This codec can spend fewer. The bound holds from any point in a stream,
// as kMaxExpansion's does.
inline constexpr std::uint64_t kMaxMessageExpansion =
    ((8 * kMaxExpansion * detail::kMostBytesPerCodedBit.bytes) +
     detail::kMostBytesPerCodedBit.bits - 1) /
    detail::kMostBytesPerCodedBit.bits;

// How much is read from a source or handed to a sink at once. The two
// functions below take another size only so that tests can put a block
// edge wherever they like; it changes no byte of what is written.
inline constexpr std::size_t kBlockBytes = std::size_t{1} << 20;

namespace detail {

// Throws unless `output_bytes` of output could come from `payload_bytes` of
// payload.
inline void require_within_message_expansion(std::uint64_t output_bytes,
                                             std::uint64_t payload_bytes) {
    if (output_bytes > saturating_multiply(payload_bytes, kMaxMessageExpansion)) {
        throw Error("corrupt archive: header claims more data than the payload can hold");
    }
}

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

// Bytes on their way to a sink, handed over a block at a time.
class OutputBlocks {
public:
    OutputBlocks(ByteSink& sink, std::size_t block_bytes)
        : sink_(&sink), block_bytes_(block_bytes) {
        // A block and the little one message or one coded message adds
        // before the block is handed over.
        block_.reserve(block_bytes + kSlackBytes);
    }

    // The bytes not yet handed over. Append to it, then call pass_on().
    [[nodiscard]] Bytes& block() noexcept {
        return block_;
    }

    // Hands the block over if it is full.
    void pass_on() {
        if (block_.size() >= block_bytes_) flush();
    }

    void flush() {
        sink_->write(block_);
        block_.clear();
    }

private:
    static constexpr std::size_t kSlackBytes = 4096;

    ByteSink* sink_;  // not owned
    std::size_t block_bytes_;
    Bytes block_;
};

}  // namespace detail

// Writes the payload for everything `input` has left to `output`, coded
// with `model`, which must be freshly constructed. Returns how many bytes
// the input held.
inline std::uint64_t encode_messages(itch::MessageModel& model, ByteSource& input, ByteSink& output,
                                     std::size_t block_bytes = kBlockBytes) {
    detail::InputWindow window(input, block_bytes);
    detail::OutputBlocks coded(output, block_bytes);
    EncodingCoder coder(coded.block());
    std::array<std::uint8_t, itch::kMaxMessageBytes> message{};
    std::uint64_t size = 0;

    for (ByteView ahead = window.ahead(); !ahead.empty(); ahead = window.ahead()) {
        std::size_t taken = 1;
        if (const itch::MessageLayout* layout = itch::framed_message_at(ahead)) {
            // The model takes a message it may write to; this one it only
            // reads, but the copy keeps one signature for both directions.
            const std::span<std::uint8_t> body(message.data(), layout->length);
            std::memcpy(body.data(), ahead.data() + itch::kLengthPrefixBytes, body.size());
            static_cast<void>(model.code_type(coder, body[0]));
            // False only for decoded data.
            static_cast<void>(model.code_message(coder, body));
            taken = itch::kLengthPrefixBytes + body.size();
        } else {
            static_cast<void>(model.code_type(coder, itch::Market::kEscape));
            static_cast<void>(model.code_escaped_byte(coder, ahead[0]));
        }
        window.advance(taken);
        size += taken;
        coded.pass_on();
    }
    coder.finish();
    coded.flush();
    return size;
}

// Writes `original_size` decoded bytes to `output`, decoded with `model`,
// which must be freshly constructed, as the encoder's was and with the same
// arguments. Everything here comes from an untrusted archive, so it throws
// cpress::Error unless:
//   1. every decoded type is the escape or an ITCH type;
//   2. a decoded message fits in what is left of original_size;
//   3. the model accepts every decoded message;
//   4. original_size is at most kMaxMessageExpansion times the payload
//      size, on entry and for what is left after every message, which
//      bounds the work by the size of the payload;
//   5. the decoder never reads past the payload, and consumes it exactly.
// Memory does not depend on original_size: the output leaves a block at a
// time. Errors are thrown here, never from inside the coder or the model.
inline void decode_messages(itch::MessageModel& model, ByteView payload,
                            std::uint64_t original_size, ByteSink& output,
                            std::size_t block_bytes = kBlockBytes) {
    const std::uint64_t payload_size = payload.size();
    detail::require_within_message_expansion(original_size, payload_size);

    DecodingCoder coder(payload);
    detail::OutputBlocks decoded(output, block_bytes);
    std::uint64_t produced = 0;
    while (produced < original_size) {
        const std::uint64_t payload_left = payload_size - coder.consumed() + coder::kCodeBytes;
        detail::require_within_message_expansion(original_size - produced, payload_left);

        Bytes& block = decoded.block();
        const std::uint32_t type = model.code_type(coder, 0);
        if (type == itch::Market::kEscape) {
            block.push_back(static_cast<std::uint8_t>(model.code_escaped_byte(coder, 0)));
            produced += 1;
        } else {
            // A byte symbol: any value up to 255 can come back.
            const itch::MessageLayout* layout = itch::layout_of(static_cast<std::uint8_t>(type));
            if (layout == nullptr) {
                throw Error("corrupt archive: a message of no ITCH type");
            }
            const std::size_t framed = itch::kLengthPrefixBytes + layout->length;
            if (framed > original_size - produced) {
                throw Error("corrupt archive: a message runs past the end of the data");
            }
            const std::size_t start = block.size();
            block.resize(start + framed);
            const std::span<std::uint8_t> prefix(&block[start], itch::kLengthPrefixBytes);
            const std::span<std::uint8_t> body(&block[start + itch::kLengthPrefixBytes],
                                               layout->length);
            itch::store_big_endian(prefix, layout->length);
            body[0] = static_cast<std::uint8_t>(type);
            if (!model.code_message(coder, body)) {
                throw Error("corrupt archive: a message no encoder writes");
            }
            produced += framed;
        }
        if (coder.overrun()) {
            throw Error("corrupt archive: payload ended before the data did");
        }
        decoded.pass_on();
    }
    decoded.flush();

    // Also catches an empty or short payload when there is nothing to decode.
    if (coder.overrun()) {
        throw Error("corrupt archive: payload is shorter than the coder's header");
    }
    if (coder.consumed() != payload.size()) {
        throw Error("corrupt archive: payload has bytes left over");
    }
}

}  // namespace cpress::codec
