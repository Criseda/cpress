// Binary arithmetic coder: turns a stream of (bit, probability) pairs into
// bytes and back. Header-only so it inlines into the codec driver.
//
// Technique: the carryless 32-bit coder of Matt Mahoney's lpaq and paq8
// family. The state is an inclusive interval [low, high]; each coded bit
// narrows it, and a byte is emitted as soon as the top bytes of low and high
// agree, because they can never change again.
//
// Probabilities are P(bit = 1) as 12-bit integers (`p12`) in [1, 4095]; see
// model/predictor.hpp. The coder is correct for *any* p12 in that range on
// any bit: a poor prediction costs bits, never correctness.
#pragma once

#include "cpress/cpress.hpp"
#include "model/predictor.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace cpress::coder {

// The coder's state is 32 bits wide. finish() writes this many bytes, and the
// decoder always holds this many that it has read but not yet used up.
inline constexpr std::size_t kCodeBytes = 4;

namespace detail {

inline constexpr std::uint32_t kTopByteMask = 0xFF000000u;

// The point that divides [low, high] in proportion p12 : (4096 - p12).
// Bit 1 takes [low, mid] and bit 0 takes [mid + 1, high].
//
// Requires low < high and p12 in [1, 4095]. The 64-bit product is exact
// (at most 2^32 * 2^12). Because p12 < 4096, floor((high - low) * p12 / 4096)
// is strictly less than high - low, so low <= mid < high and both halves are
// never empty.
[[nodiscard]] constexpr std::uint32_t split(std::uint32_t low, std::uint32_t high,
                                            std::uint32_t p12) noexcept {
    assert(low < high);
    assert(p12 >= model::kProbMin && p12 <= model::kProbMax);
    const std::uint64_t range = high - low;
    return low + static_cast<std::uint32_t>((range * p12) >> model::kProbBits);
}

// True while the top bytes of low and high agree, i.e. while a byte can be
// shifted out. After the loop that drains this, low and high differ in their
// top byte, which is what guarantees low < high.
[[nodiscard]] constexpr bool top_byte_settled(std::uint32_t low, std::uint32_t high) noexcept {
    return ((low ^ high) & kTopByteMask) == 0;
}

}  // namespace detail

class Encoder {
public:
    // Appends to `output`; bytes already in it are left alone.
    explicit Encoder(Bytes& output) noexcept : output_(&output) {}

    // Codes `bit` (0 or 1) given p12 = P(bit = 1) in [1, 4095].
    //
    // Appends to a std::vector, so this can allocate and, on memory
    // exhaustion, terminate. It is the one tolerated exception to "the
    // per-bit loop does not allocate" at this stage: the driver reserves
    // ahead so growth is rare. The coder itself never throws.
    // NOLINTNEXTLINE(bugprone-exception-escape): allocation failure terminates, by design.
    void encode(int bit, std::uint32_t p12) noexcept {
        assert(bit == 0 || bit == 1);
        const std::uint32_t mid = detail::split(low_, high_, p12);
        if (bit != 0) {
            high_ = mid;
        } else {
            low_ = mid + 1;
        }
        while (detail::top_byte_settled(low_, high_)) {
            output_->push_back(static_cast<std::uint8_t>(high_ >> 24));
            low_ <<= 8;
            high_ = (high_ << 8) | 0xFFu;
        }
        assert(low_ < high_);
    }

    // Writes the four bytes of `low`, most significant first. Any value in
    // [low, high] would do; `low` needs no arithmetic. Call exactly once,
    // after the last bit. The payload is then (bytes emitted by encode) + 4.
    // NOLINTNEXTLINE(bugprone-exception-escape): allocation failure terminates, by design.
    void finish() noexcept {
        for (int shift = 24; shift >= 0; shift -= 8) {
            output_->push_back(static_cast<std::uint8_t>(low_ >> shift));
        }
    }

private:
    Bytes* output_;  // not owned
    std::uint32_t low_ = 0;
    std::uint32_t high_ = 0xFFFFFFFFu;
};

class Decoder {
public:
    // Reads the first kCodeBytes payload bytes. The payload must outlive the
    // decoder.
    explicit Decoder(ByteView payload) noexcept : payload_(payload) {
        for (std::size_t i = 0; i < kCodeBytes; ++i) {
            code_ = (code_ << 8) | next_byte();
        }
    }

    // Returns the next bit given the same p12 the encoder used.
    //
    // Invariant, true for every payload including corrupt ones: low <= code
    // <= high. Each step keeps the half that contains `code`, and shifting
    // preserves containment because the shifted-out top bytes are equal.
    [[nodiscard]] int decode(std::uint32_t p12) noexcept {
        const std::uint32_t mid = detail::split(low_, high_, p12);
        assert(low_ <= code_ && code_ <= high_);
        const int bit = code_ <= mid ? 1 : 0;
        if (bit != 0) {
            high_ = mid;
        } else {
            low_ = mid + 1;
        }
        while (detail::top_byte_settled(low_, high_)) {
            low_ <<= 8;
            high_ = (high_ << 8) | 0xFFu;
            code_ = (code_ << 8) | next_byte();
        }
        assert(low_ < high_);
        return bit;
    }

    // True once a read went past the end of the payload. Sticky. The caller
    // checks it outside the bit loop: this class never throws.
    [[nodiscard]] bool overrun() const noexcept {
        return overrun_;
    }

    // Payload bytes actually read. For a correct payload of N + 4 bytes this
    // is exactly N + 4 once the last bit is decoded, so a truncated or
    // extended payload is detectable.
    [[nodiscard]] std::size_t consumed() const noexcept {
        return position_;
    }

private:
    // Past the end it returns 0 and sets the overrun flag; position_ then
    // stops counting, so consumed() never exceeds the payload size.
    [[nodiscard]] std::uint32_t next_byte() noexcept {
        if (position_ >= payload_.size()) {
            overrun_ = true;
            return 0;
        }
        return payload_[position_++];
    }

    ByteView payload_;
    std::size_t position_ = 0;
    bool overrun_ = false;
    std::uint32_t low_ = 0;
    std::uint32_t high_ = 0xFFFFFFFFu;
    std::uint32_t code_ = 0;
};

}  // namespace cpress::coder
