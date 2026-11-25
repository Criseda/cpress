// The word transform: choosing the code bytes and the dictionary, then
// forward() and inverse(). The stream's layout is in word_transform.hpp.
#include "text/word_transform.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cpress::text {
namespace {

constexpr std::size_t kByteValues = 256;
constexpr std::uint8_t kLineFeed = '\n';

// Code bytes are chosen from the values kFirstCodeByte to 255, which are
// never ASCII.
constexpr std::uint8_t kFirstCodeByte = 128;
constexpr std::size_t kMaxCodeBytes = kByteValues - kFirstCodeByte;

// The flags are chosen from the control bytes, 0 to kControlBytes - 1,
// except tab, line feed and carriage return, which text is full of.
constexpr std::uint8_t kControlBytes = 32;

// The escape, the capital flag and the all-capitals flag, in that order.
constexpr std::size_t kFlagCount = 3;
using Flags = std::array<std::uint8_t, kFlagCount>;

[[nodiscard]] constexpr bool is_upper(std::uint8_t byte) noexcept {
    return byte >= 'A' && byte <= 'Z';
}
[[nodiscard]] constexpr bool is_lower(std::uint8_t byte) noexcept {
    return byte >= 'a' && byte <= 'z';
}
[[nodiscard]] constexpr bool is_letter(std::uint8_t byte) noexcept {
    return is_upper(byte) || is_lower(byte);
}
[[nodiscard]] constexpr std::uint8_t to_lower(std::uint8_t byte) noexcept {
    return is_upper(byte) ? static_cast<std::uint8_t>(byte + ('a' - 'A')) : byte;
}
[[nodiscard]] constexpr std::uint8_t to_upper(std::uint8_t byte) noexcept {
    return is_lower(byte) ? static_cast<std::uint8_t>(byte - ('a' - 'A')) : byte;
}

// How a word is written. Only a mixed one, such as "McDonald", is never
// coded: its case would need more than a flag.
enum class Case : std::uint8_t { Lower, Capital, Upper, Mixed };

[[nodiscard]] Case case_of(ByteView word) noexcept {
    const auto capitals = static_cast<std::size_t>(std::ranges::count_if(word, is_upper));
    if (capitals == 0) return Case::Lower;
    if (capitals == 1 && is_upper(word.front())) return Case::Capital;
    if (capitals == word.size()) return Case::Upper;
    return Case::Mixed;
}

// The end of the run of letters that starts at `start`.
[[nodiscard]] std::size_t end_of_letters(ByteView input, std::size_t start) noexcept {
    std::size_t end = start;
    while (end < input.size() && is_letter(input[end])) ++end;
    return end;
}

// A run of letters the transform can code: the right length, and a case a
// flag can restore.
[[nodiscard]] bool codable(ByteView run) noexcept {
    return run.size() >= kMinWordLetters && run.size() <= kMaxWordLetters &&
           case_of(run) != Case::Mixed;
}

// A word's lower-case form, written into a buffer this owns, so that looking
// a word up costs no allocation.
class LowerCase {
public:
    [[nodiscard]] std::string_view of(ByteView word) noexcept {
        for (std::size_t i = 0; i < word.size(); ++i) {
            letters_[i] = static_cast<char>(to_lower(word[i]));
        }
        return {letters_.data(), word.size()};
    }

private:
    std::array<char, kMaxWordLetters> letters_{};
};

// A map keyed by words that can be searched with a string_view.
struct WordHash {
    using is_transparent = void;  // NOLINT(readability-identifier-naming): the standard's name
    [[nodiscard]] std::size_t operator()(std::string_view word) const noexcept {
        return std::hash<std::string_view>{}(word);
    }
};
template <typename Value>
using WordMap = std::unordered_map<std::string, Value, WordHash, std::equal_to<>>;

// The code bytes, flags and code lengths of one stream: K, B1 and B2 in
// word_transform.hpp.
//
// A code is a number written in base K, with the code bytes as its digits:
// the code byte at index i stands for the digit i. The first B1 code bytes
// are whole codes, one byte each. The next B2 lead two-byte codes, and the
// rest lead three-byte codes; every digit after a lead may be any code byte.
class Layout {
public:
    // `codes` ascending, and `one_byte_codes` + `two_byte_leads` at most
    // the number of codes.
    Layout(std::vector<std::uint8_t> codes, Flags flags, std::size_t one_byte_codes,
           std::size_t two_byte_leads)
        : codes_(std::move(codes)),
          escape_(flags[0]),
          capital_(flags[1]),
          upper_(flags[2]),
          one_byte_codes_(one_byte_codes),
          two_byte_leads_(two_byte_leads) {
        position_.fill(kNotACode);
        for (std::size_t i = 0; i < codes_.size(); ++i) {
            position_[codes_[i]] = static_cast<std::int16_t>(i);
        }
    }

    [[nodiscard]] const std::vector<std::uint8_t>& codes() const noexcept {
        return codes_;
    }
    [[nodiscard]] std::uint8_t escape() const noexcept {
        return escape_;
    }
    [[nodiscard]] std::uint8_t capital() const noexcept {
        return capital_;
    }
    [[nodiscard]] std::uint8_t upper() const noexcept {
        return upper_;
    }

    // K: the number of code bytes, and so the base codes are written in.
    [[nodiscard]] std::size_t code_count() const noexcept {
        return codes_.size();
    }
    // B1.
    [[nodiscard]] std::size_t one_byte_codes() const noexcept {
        return one_byte_codes_;
    }
    // B2.
    [[nodiscard]] std::size_t two_byte_leads() const noexcept {
        return two_byte_leads_;
    }
    // K - B1 - B2.
    [[nodiscard]] std::size_t three_byte_leads() const noexcept {
        return code_count() - one_byte_codes_ - two_byte_leads_;
    }
    // How many words have a code: a lead of a two-byte code is followed by
    // any of K digits, and a lead of a three-byte code by any of K * K.
    [[nodiscard]] std::size_t capacity() const noexcept {
        const std::size_t base = code_count();
        return one_byte_codes_ + (two_byte_leads_ * base) + (three_byte_leads() * base * base);
    }

    // The index of a code byte among the codes, or -1.
    [[nodiscard]] int position(std::uint8_t byte) const noexcept {
        return position_[byte];
    }
    // True for a byte the text can only hold escaped.
    [[nodiscard]] bool special(std::uint8_t byte) const noexcept {
        return position(byte) >= 0 || byte == escape_ || byte == capital_ || byte == upper_;
    }

    // Appends the code of word `index`, below capacity(). Words are numbered
    // through the one-byte codes, then the two-byte codes, then the
    // three-byte codes, each in order of their digits.
    void append_code(std::size_t index, Bytes& out) const {
        const std::size_t base = code_count();
        if (index < one_byte_codes_) {
            out.push_back(codes_[index]);
            return;
        }
        index -= one_byte_codes_;
        if (index < two_byte_leads_ * base) {
            out.push_back(codes_[one_byte_codes_ + (index / base)]);
            out.push_back(codes_[index % base]);
            return;
        }
        index -= two_byte_leads_ * base;
        out.push_back(codes_[one_byte_codes_ + two_byte_leads_ + (index / (base * base))]);
        out.push_back(codes_[(index / base) % base]);
        out.push_back(codes_[index % base]);
    }

private:
    static constexpr std::int16_t kNotACode = -1;
    std::vector<std::uint8_t> codes_;
    std::uint8_t escape_;
    std::uint8_t capital_;
    std::uint8_t upper_;
    std::size_t one_byte_codes_;
    std::size_t two_byte_leads_;
    std::array<std::int16_t, kByteValues> position_{};
};

// The `count` of `candidates` that occur least, by `occurrences`; among
// equals, the one earlier in `candidates`.
[[nodiscard]] std::vector<std::uint8_t> rarest(
    const std::array<std::uint64_t, kByteValues>& occurrences, std::vector<std::uint8_t> candidates,
    std::size_t count) {
    std::ranges::stable_sort(candidates, [&](std::uint8_t a, std::uint8_t b) {
        return occurrences[a] < occurrences[b];
    });
    candidates.resize(count);
    return candidates;
}

[[nodiscard]] Layout choose_layout(ByteView input) {
    std::array<std::uint64_t, kByteValues> occurrences{};
    for (const std::uint8_t byte : input) ++occurrences[byte];

    std::vector<std::uint8_t> high(kMaxCodeBytes);
    std::iota(high.begin(), high.end(), kFirstCodeByte);
    std::vector<std::uint8_t> control;
    for (std::uint8_t byte = 0; byte < kControlBytes; ++byte) {
        if (byte != '\t' && byte != '\n' && byte != '\r') control.push_back(byte);
    }

    std::vector<std::uint8_t> codes = rarest(occurrences, std::move(high), kCodeBytes);
    std::ranges::sort(codes);
    const std::vector<std::uint8_t> flags = rarest(occurrences, std::move(control), kFlagCount);
    return {std::move(codes), {flags[0], flags[1], flags[2]}, kOneByteCodes, kTwoByteLeads};
}

// The dictionary: every word seen at least kMinCount times, as many as the
// layout has codes for, the commonest first, then alphabetical within each
// code length. Ties in count go to the alphabetically first word, so the
// result depends on nothing but the input.
[[nodiscard]] std::vector<std::string> choose_words(ByteView input, const Layout& layout) {
    WordMap<std::uint64_t> counts;
    LowerCase lower;
    std::size_t i = 0;
    while (i < input.size()) {
        if (!is_letter(input[i])) {
            ++i;
            continue;
        }
        const std::size_t end = end_of_letters(input, i);
        const ByteView run = input.subspan(i, end - i);
        if (codable(run)) {
            const std::string_view word = lower.of(run);
            if (const auto found = counts.find(word); found != counts.end()) {
                ++found->second;
            } else {
                counts.emplace(std::string(word), 1);
            }
        }
        i = end;
    }

    std::vector<std::pair<std::string, std::uint64_t>> frequent;
    for (const auto& [word, count] : counts) {
        if (count >= kMinCount) frequent.emplace_back(word, count);
    }
    std::ranges::sort(frequent, [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    frequent.resize(std::min(frequent.size(), layout.capacity()));

    std::vector<std::string> words;
    words.reserve(frequent.size());
    for (auto& entry : frequent) words.push_back(std::move(entry.first));
    const std::size_t one_byte_end = std::min(words.size(), layout.one_byte_codes());
    const std::size_t two_byte_end =
        std::min(words.size(), one_byte_end + (layout.two_byte_leads() * layout.code_count()));
    const auto at = [&](std::size_t n) { return words.begin() + static_cast<std::ptrdiff_t>(n); };
    std::sort(at(0), at(one_byte_end));
    std::sort(at(one_byte_end), at(two_byte_end));
    std::sort(at(two_byte_end), words.end());
    return words;
}

// Writes the layout at the head of the stream; read_layout() reads it.
void write_layout(const Layout& layout, Bytes& out) {
    out.push_back(static_cast<std::uint8_t>(layout.code_count() - 1));
    out.insert(out.end(), layout.codes().begin(), layout.codes().end());
    out.push_back(layout.escape());
    out.push_back(layout.capital());
    out.push_back(layout.upper());
    out.push_back(static_cast<std::uint8_t>(layout.one_byte_codes()));
    out.push_back(static_cast<std::uint8_t>(layout.two_byte_leads()));
}

// Writes the dictionary after the layout; read_words() reads it.
void write_words(const std::vector<std::string>& words, Bytes& out) {
    for (const std::string& word : words) {
        out.insert(out.end(), word.begin(), word.end());
        out.push_back(kLineFeed);
    }
    out.push_back(kLineFeed);
}

// Reads a transformed stream; every read is checked.
class Reader {
public:
    explicit Reader(ByteView stream) noexcept : stream_(stream) {}

    [[nodiscard]] bool done() const noexcept {
        return position_ == stream_.size();
    }
    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }
    [[nodiscard]] std::uint8_t next(std::string_view inside) {
        if (done()) {
            throw Error("corrupt archive: the word stream ends inside " + std::string(inside));
        }
        return stream_[position_++];
    }
    [[nodiscard]] ByteView span(std::size_t start, std::size_t end) const noexcept {
        return stream_.subspan(start, end - start);
    }

private:
    ByteView stream_;
    std::size_t position_ = 0;
};

[[noreturn]] void corrupt(std::string_view what) {
    throw Error("corrupt archive: " + std::string(what));
}

[[nodiscard]] Layout read_layout(Reader& in) {
    const std::size_t code_count = std::size_t{in.next("the layout")} + 1;
    if (code_count > kMaxCodeBytes) {
        corrupt("the word stream has more than " + std::to_string(kMaxCodeBytes) + " code bytes");
    }
    std::vector<std::uint8_t> codes;
    for (std::size_t i = 0; i < code_count; ++i) {
        const std::uint8_t code = in.next("the layout");
        if (code < kFirstCodeByte || (!codes.empty() && code <= codes.back())) {
            corrupt("the word stream's code bytes are not ascending values of " +
                    std::to_string(kFirstCodeByte) + " or more");
        }
        codes.push_back(code);
    }
    Flags flags{};
    for (std::size_t i = 0; i < flags.size(); ++i) {
        flags[i] = in.next("the layout");
        if (flags[i] >= kFirstCodeByte || is_letter(flags[i])) {
            corrupt("a word stream flag is a letter or a code byte");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (flags[i] == flags[j]) corrupt("two word stream flags are the same byte");
        }
    }
    const std::size_t one_byte_codes = in.next("the layout");
    const std::size_t two_byte_leads = in.next("the layout");
    if (one_byte_codes + two_byte_leads > code_count) {
        corrupt("the word stream's code lengths exceed its code bytes");
    }
    return {std::move(codes), flags, one_byte_codes, two_byte_leads};
}

// The dictionary, as views of the stream.
[[nodiscard]] std::vector<ByteView> read_words(Reader& in, const Layout& layout) {
    std::vector<ByteView> words;
    for (;;) {
        const std::size_t start = in.position();
        std::uint8_t byte = in.next("the dictionary");
        if (byte == kLineFeed) return words;
        while (byte != kLineFeed) {
            if (!is_lower(byte)) corrupt("a dictionary word is not lower-case letters");
            byte = in.next("the dictionary");
        }
        const ByteView word = in.span(start, in.position() - 1);
        if (word.size() < kMinWordLetters || word.size() > kMaxWordLetters) {
            corrupt("a dictionary word is too short or too long");
        }
        if (words.size() == layout.capacity()) corrupt("the dictionary has more words than codes");
        words.push_back(word);
    }
}

// The index of the word whose code starts with `lead`, a code byte: the
// inverse of Layout::append_code().
[[nodiscard]] std::size_t read_code(Reader& in, const Layout& layout, std::uint8_t lead) {
    const auto digit = [&](std::uint8_t byte) {
        const int position = layout.position(byte);
        if (position < 0) corrupt("a word code continues with a byte that is not a code byte");
        return static_cast<std::size_t>(position);
    };
    const std::size_t base = layout.code_count();
    const std::size_t one_byte = layout.one_byte_codes();
    const std::size_t two_byte_leads = layout.two_byte_leads();

    const std::size_t first = digit(lead);
    if (first < one_byte) return first;
    if (first < one_byte + two_byte_leads) {
        const std::size_t second = digit(in.next("a word code"));
        return one_byte + ((first - one_byte) * base) + second;
    }
    const std::size_t second = digit(in.next("a word code"));
    const std::size_t third = digit(in.next("a word code"));
    return one_byte + (two_byte_leads * base) +
           ((first - one_byte - two_byte_leads) * base * base) + (second * base) + third;
}

}  // namespace

Bytes forward(ByteView input) {
    const Layout layout = choose_layout(input);
    const std::vector<std::string> words = choose_words(input, layout);
    WordMap<std::uint32_t> code_of;
    code_of.reserve(words.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
        code_of.emplace(words[i], static_cast<std::uint32_t>(i));
    }

    // Not reserved ahead. With a reserve() here, GCC 15 warns that the
    // push_back() after it frees a pointer operator new did not return
    // (-Wfree-nonheap-object), which is not so, and warnings are errors in
    // CI. Growing by doubling instead costs a fraction of a second on a
    // gigabyte.
    Bytes out;
    write_layout(layout, out);
    write_words(words, out);

    LowerCase lower;
    std::size_t i = 0;
    while (i < input.size()) {
        const std::uint8_t byte = input[i];
        if (!is_letter(byte)) {
            if (layout.special(byte)) out.push_back(layout.escape());
            out.push_back(byte);
            ++i;
            continue;
        }
        const std::size_t end = end_of_letters(input, i);
        const ByteView run = input.subspan(i, end - i);
        const auto found = codable(run) ? code_of.find(lower.of(run)) : code_of.end();
        if (found != code_of.end()) {
            const Case word_case = case_of(run);
            if (word_case == Case::Capital) out.push_back(layout.capital());
            if (word_case == Case::Upper) out.push_back(layout.upper());
            layout.append_code(found->second, out);
        } else {
            out.insert(out.end(), run.begin(), run.end());  // letters are never special
        }
        i = end;
    }
    return out;
}

void inverse(ByteView transformed, std::uint64_t original_size, Bytes& output) {
    Reader in(transformed);
    const Layout layout = read_layout(in);
    const std::vector<ByteView> words = read_words(in, layout);

    // A stream byte stands for at most kMaxWordLetters output bytes, so this
    // reserves no more than the stream could fill, whatever the header says.
    const std::uint64_t at_most = static_cast<std::uint64_t>(transformed.size()) * kMaxWordLetters;
    output.reserve(output.size() + static_cast<std::size_t>(std::min(original_size, at_most)));
    std::uint64_t written = 0;
    const auto room_for = [&](std::size_t bytes) {
        if (bytes > original_size - written) {
            corrupt("the word stream expands to more than the original size");
        }
        written += bytes;
    };
    const auto write_word = [&](std::size_t index, Case word_case) {
        if (index >= words.size()) corrupt("a word code is past the end of the dictionary");
        const ByteView word = words[index];
        room_for(word.size());
        for (std::size_t i = 0; i < word.size(); ++i) {
            const bool raise = word_case == Case::Upper || (word_case == Case::Capital && i == 0);
            output.push_back(raise ? to_upper(word[i]) : word[i]);
        }
    };

    while (!in.done()) {
        const std::uint8_t byte = in.next("the text");
        if (byte == layout.escape()) {
            room_for(1);
            output.push_back(in.next("an escape"));
        } else if (byte == layout.capital() || byte == layout.upper()) {
            const std::uint8_t lead = in.next("a flagged word");
            if (layout.position(lead) < 0) corrupt("a case flag is not followed by a word code");
            write_word(read_code(in, layout, lead),
                       byte == layout.capital() ? Case::Capital : Case::Upper);
        } else if (layout.position(byte) >= 0) {
            write_word(read_code(in, layout, byte), Case::Lower);
        } else {
            room_for(1);
            output.push_back(byte);
        }
    }
    if (written != original_size) corrupt("the word stream expands to less than the original size");
}

}  // namespace cpress::text
