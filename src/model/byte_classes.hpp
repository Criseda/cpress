// Byte classes: a coarse name for each byte value, and the keys codec 7
// makes from the classes of the last few bytes.
//
// A context of exact bytes sees "the " and "and " as strangers. The classes
// of the same bytes, lower case three times and a space, say what both
// have in common: a short word has just ended. Twelve classes reach back
// further than any byte order, in a context that has far fewer values, so
// it learns fast and is found again often.
//
// The sixteen classes are this project's own grouping, for the word stream
// that codec 5's transform writes, where control bytes are its flags and
// escape, and bytes of 128 and up are its codes as well as UTF-8.
//
//   0   control bytes, but line feed        8   , ; :
//   1   line feed                           9   ' "
//   2   space                               10  ( )
//   3   digits                              11  [ ]
//   4   lower case                          12  { } |
//   5   upper case                          13  < > \ /
//   6   128 and up                          14  = & # *
//   7   . ! ?                               15  the rest
//
// The history of classes is a 64-bit word of 4-bit classes, the newest in
// the lowest four bits; it starts at 0, as if sixteen control bytes came
// first.
//
// Memory: a 256-byte table, built at compile time.
//
// Technique: a context of recent character classes, and mixer weights
// chosen by the classes of the last bytes, are mcm's (Mathieu Chartier,
// 2015), read and not copied (docs/stages/stage-9.md, section 1).
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cpress::model::classes {

inline constexpr int kClassBits = 4;
inline constexpr std::uint32_t kClassCount = 1U << kClassBits;

namespace detail {

// The punctuation classes, 7 to 14, by their members.
inline constexpr std::uint8_t kFirstPunctuation = 7;
inline constexpr std::array<std::string_view, 8> kPunctuation{".!?", ",;:", "'\"",   "()",
                                                              "[]",  "{}|", "<>\\/", "=&#*"};

// Every byte value's class, as the table at the top of this file lists.
[[nodiscard]] constexpr std::array<std::uint8_t, 256> make_classes() noexcept {
    std::array<std::uint8_t, 256> table{};
    for (std::uint32_t byte = 0; byte < table.size(); ++byte) {
        std::uint8_t value = 15;
        if (byte < 0x20) value = 0;
        if (byte >= '0' && byte <= '9') value = 3;
        if (byte >= 'a' && byte <= 'z') value = 4;
        if (byte >= 'A' && byte <= 'Z') value = 5;
        if (byte >= 0x80) value = 6;
        table[byte] = value;
    }
    table['\n'] = 1;
    table[' '] = 2;
    for (std::size_t i = 0; i < kPunctuation.size(); ++i) {
        for (const char member : kPunctuation[i]) {
            table[static_cast<std::uint8_t>(member)] =
                static_cast<std::uint8_t>(kFirstPunctuation + i);
        }
    }
    return table;
}

inline constexpr std::array<std::uint8_t, 256> kClassOf = make_classes();

// The last `count` classes of `history`, the rest cleared.
[[nodiscard]] constexpr std::uint64_t last(std::uint64_t history, int count) noexcept {
    assert(count > 0 && count * kClassBits < 64);
    return history & ((std::uint64_t{1} << (count * kClassBits)) - 1);
}

// Folds the high bits of a product into its low bits, as words::mix() in
// mix9.hpp does for the word keys, so that the top bits the marker shift
// drops still count.
[[nodiscard]] constexpr std::uint64_t fold(std::uint64_t hash) noexcept {
    return hash ^ (hash >> 29);
}

}  // namespace detail

// The class of `byte`, 0 to kClassCount - 1.
[[nodiscard]] constexpr std::uint32_t class_of(std::uint32_t byte) noexcept {
    assert(byte <= 0xFF);
    return detail::kClassOf[byte];
}

// The class history after `byte`, if `history` is the one before it.
[[nodiscard]] constexpr std::uint64_t next_history(std::uint64_t history,
                                                   std::uint32_t byte) noexcept {
    return (history << kClassBits) | class_of(byte);
}

// The classes the two class tables are keyed by: twelve and six. Section 2
// of the brief found 10 and 5 to 16 and 7 within 0.2% on e30.
inline constexpr int kLongClasses = 12;
inline constexpr int kShortClasses = 6;

// Each key multiplies the classes by an odd constant and adds a small one
// of its own, as the word keys add 1 and 2, then folds. The short key adds
// kShortOffset to the classes first. These are the constants the prototype
// was measured with; other odd ones would do as well.
inline constexpr std::uint64_t kLongKey = 0x9FB21C651E98DF25;
inline constexpr std::uint64_t kShortKey = 0xD6E8FEB86659FD93;
inline constexpr std::uint64_t kShortOffset = 0x100000;

// The long class table's key, before the marker.
[[nodiscard]] constexpr std::uint64_t long_key(std::uint64_t history) noexcept {
    return detail::fold((detail::last(history, kLongClasses) * kLongKey) + 3);
}

// The short class table's key, before the marker.
[[nodiscard]] constexpr std::uint64_t short_key(std::uint64_t history) noexcept {
    return detail::fold(((detail::last(history, kShortClasses) + kShortOffset) * kShortKey) + 5);
}

// The classes of the last two bytes, 0 to kClassCount^2 - 1: what codec 7's
// mixer weights and its first APM are chosen by.
[[nodiscard]] constexpr std::uint32_t last_two(std::uint64_t history) noexcept {
    return static_cast<std::uint32_t>(detail::last(history, 2));
}

}  // namespace cpress::model::classes
