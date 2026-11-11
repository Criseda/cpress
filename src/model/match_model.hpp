// MatchModel: predicts that the data repeats. It finds the most recent place
// where the last kMinLength bytes occurred before, and bets that the byte
// which followed them then follows them now.
//
// Long repeats are common in real data: a template, a quoted line, a
// boilerplate sentence. The context tables stop at six bytes; a match can be
// thousands long, and the longer it has run the more it deserves belief.
//
// State: the most recent 2^buffer_bits bytes, in a ring; a hash table of
// 2^hash_bits positions, keyed by the last kMinLength bytes; and the current
// match, a pointer to the byte it predicts and how many bytes it has agreed.
// The caller keeps the position within the byte and the recent bytes, and
// passes them in: predict() takes the partial byte, push_byte() the history.
//
// At each byte boundary:
//   - a running match that predicted the byte right grows by one;
//   - if none is running, the hash table gives the position after the last
//     occurrence of the last kMinLength bytes. Going back from there and
//     from here, the bytes that agree are counted; kMinLength or more start
//     a match;
//   - the hash table learns the current position.
//
// Per bit, while a match runs: the expected bit is the next bit of the byte
// it points to, and the prediction is a probability learned for each length
// class and expected bit (ProbabilityMap), stretched. A bit that disagrees
// ends the match, and nothing more is predicted until the next boundary.
// Otherwise the model says nothing: its input is 0, which a mixer ignores.
//
// Memory: 2^buffer_bits bytes of buffer, 4 * 2^hash_bits bytes of hash
// table, and 4 * kMapContexts bytes of map.
//
// Positions are kept modulo 2^32 in the hash table. A stale or aliased
// position costs compression, never safety: every read goes through the
// ring's mask, and every candidate is verified against the buffer.
//
// Technique: a hash of the last few bytes pointing into a ring buffer,
// verified by comparing backwards, and a probability learned per match
// length and expected bit, is the match model of Matt Mahoney's lpaq1
// (2007). Data Compression Explained, section 4.3.5, describes match models
// in general. lpaq1 tries two context lengths, caps the length at 62, and
// adds the last byte to its probability's context; the single length of 7,
// the cap and the length classes here are this project's own.
#pragma once

#include "model/context_table.hpp"
#include "model/logistic.hpp"
#include "model/probability_map.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace cpress::model {

namespace detail {

// Matches shorter than this get a class each; longer ones one class per
// power of two.
inline constexpr std::uint32_t kExactMatchLengths = 16;

// The class of a match `length` long, for the probability map: the length
// itself below kExactMatchLengths, then 16 for 16 to 31, 17 for 32 to 63 and
// so on. Long matches differ little from their neighbours, so they share.
[[nodiscard]] constexpr std::uint32_t length_class(std::uint32_t length) noexcept {
    if (length < kExactMatchLengths) return length;
    return kExactMatchLengths + static_cast<std::uint32_t>(std::bit_width(length)) -
           static_cast<std::uint32_t>(std::bit_width(kExactMatchLengths));
}

}  // namespace detail

class MatchModel {
public:
    // How many bytes must agree before a match is used: the length of the
    // context hashed to find one.
    static constexpr std::uint32_t kMinLength = 7;

    // The longest a match counts. A lookup compares at most this many bytes.
    static constexpr std::uint32_t kMaxLength = 65535;

    // Classes of match length, for the mixer: none, under 16, under 32, and
    // 32 or more.
    static constexpr std::uint32_t kMixerContexts = 4;

    // The map's contexts: an expected bit for each length class.
    static constexpr std::uint32_t kLengthClasses = detail::length_class(kMaxLength) + 1;
    static constexpr std::size_t kMapContexts = 2 * std::size_t{kLengthClasses};
    static_assert(kLengthClasses == 28);

    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int buffer_bits,
                                                              int hash_bits) noexcept {
        return (std::uint64_t{1} << buffer_bits) +
               ((std::uint64_t{1} << hash_bits) * sizeof(std::uint32_t)) +
               ProbabilityMap::memory_bytes(kMapContexts);
    }

    // Allocates a buffer of 2^buffer_bits bytes and a hash table of
    // 2^hash_bits positions, all zero. Throws std::bad_alloc if there is no
    // memory.
    MatchModel(int buffer_bits, int hash_bits)
        : buffer_(std::size_t{1} << buffer_bits),
          positions_(std::size_t{1} << hash_bits),
          map_(kHalfMap),
          hash_shift_(64 - hash_bits) {
        assert(buffer_bits >= 1 && buffer_bits <= 31);  // a distance within it fits 32 bits
        assert(hash_bits >= 1 && hash_bits <= 32);
    }

    // The match's prediction of the next bit, stretched, or 0 if there is no
    // match. `partial` is the byte's bits so far after a leading 1, 1 to 255.
    // Remembers the expected bit and the map's context for update().
    [[nodiscard]] std::int32_t predict(std::uint32_t partial) noexcept {
        assert(partial >= 1 && partial <= 0xFF);
        if (length_ == 0) return 0;
        // bit_width(partial) - 1 bits of the byte are known, so the next is
        // bit 8 - bit_width(partial) of the predicted byte.
        expected_bit_ = (byte_at(pointer_) >> (8 - std::bit_width(partial))) & 1;
        return stretch(map_.p12((2 * std::size_t{detail::length_class(length_)}) + expected_bit_));
    }

    // Learns `bit`, if a match predicted it; a bit that disagrees ends the
    // match until the next lookup.
    void update(int bit) noexcept {
        assert(bit == 0 || bit == 1);
        if (length_ == 0) return;
        map_.update(bit);
        if (std::cmp_not_equal(bit, expected_bit_)) length_ = 0;
    }

    // A whole byte is known: the newest byte of `history`, whose low
    // kMinLength bytes are the most recent, newest lowest. It joins the
    // buffer; a running match that predicted it grows (every bit agreed, or
    // update() would have ended it); and if none is running, one is looked
    // for.
    void push_byte(std::uint64_t history) noexcept {
        buffer_[static_cast<std::size_t>(position_ & (buffer_.size() - 1))] =
            static_cast<std::uint8_t>(history & 0xFF);
        ++position_;
        if (length_ > 0) {
            length_ = std::min(length_ + 1, kMaxLength);
            ++pointer_;
        }
        if (position_ < kMinLength) return;

        // The 64-bit product wraps; that is the hash, as in context_table.hpp.
        const auto slot =
            static_cast<std::size_t>(((history & kContextMask) * kHashMultiplier) >> hash_shift_);
        if (length_ == 0) find_match(positions_[slot]);
        positions_[slot] = static_cast<std::uint32_t>(position_);
    }

    // The match length class, 0 to kMixerContexts - 1: none (or ended this
    // byte), under 16, under 32, 32 or more.
    [[nodiscard]] std::uint32_t mixer_context() const noexcept {
        if (length_ == 0) return 0;
        if (length_ < kShortMatch) return 1;
        if (length_ < kLongMatch) return 2;
        return 3;
    }

    // For tests.
    [[nodiscard]] std::uint32_t length() const noexcept {
        return length_;
    }
    // The bit the last predict() expected. Only meaningful if length() was
    // more than 0 then.
    [[nodiscard]] std::uint32_t expected_bit() const noexcept {
        return expected_bit_;
    }

private:
    static constexpr std::uint32_t kShortMatch = 16;
    static constexpr std::uint32_t kLongMatch = 32;
    static_assert(kMinLength < kShortMatch && kLongMatch <= kMaxLength);

    // The last kMinLength bytes of a history.
    static constexpr std::uint64_t kContextMask = (std::uint64_t{1} << (8 * kMinLength)) - 1;
    static_assert(kMinLength < 8, "the context fits in a 64-bit history");

    // Every map entry starts at one half, which stretches to 0.
    static constexpr std::array<std::uint32_t, kMapContexts> kHalfMap = [] {
        std::array<std::uint32_t, kMapContexts> p22{};
        p22.fill(kP22One / 2);
        return p22;
    }();

    [[nodiscard]] std::uint32_t byte_at(std::uint64_t position) const noexcept {
        return buffer_[static_cast<std::size_t>(position & (buffer_.size() - 1))];
    }

    // Starts a match at `candidate`, a position modulo 2^32 where the last
    // kMinLength bytes may have occurred before, if at least kMinLength bytes
    // before it agree with the bytes before here.
    //
    // Cost: at most kMaxLength comparisons. A lookup happens on any byte
    // where no match is running, so that is the bound per byte, not per
    // input; on real data a long agreement starts a long match, and while it
    // runs there are no lookups.
    void find_match(std::uint32_t candidate) noexcept {
        // The bytes the buffer holds, all of them before position_.
        const std::uint64_t held = std::min<std::uint64_t>(position_, buffer_.size());
        const std::uint32_t distance = static_cast<std::uint32_t>(position_) - candidate;
        if (candidate == 0 || distance >= held) return;  // empty, or older than the buffer

        // Never read a byte older than the buffer holds: the earlier side
        // has held - distance bytes before it.
        const std::uint64_t earlier = position_ - distance;
        const auto limit =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(held - distance, kMaxLength));
        std::uint32_t agreed = 0;
        while (agreed < limit && byte_at(earlier - 1 - agreed) == byte_at(position_ - 1 - agreed)) {
            ++agreed;
        }
        if (agreed < kMinLength) return;
        length_ = agreed;
        pointer_ = earlier;
    }

    std::vector<std::uint8_t> buffer_;      // byte at position p is at p mod its size
    std::vector<std::uint32_t> positions_;  // context hash -> position after it, or 0
    ProbabilityMap map_;                    // length class and expected bit -> P(1)
    int hash_shift_;                        // 64 - hash_bits
    std::uint64_t position_ = 0;            // bytes seen
    std::uint64_t pointer_ = 0;             // the position of the byte the match predicts
    std::uint32_t length_ = 0;              // bytes the match has agreed, or 0 for none
    std::uint32_t expected_bit_ = 0;        // the bit the last predict() expected
};

}  // namespace cpress::model
