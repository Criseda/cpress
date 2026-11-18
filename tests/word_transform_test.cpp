// The word transform of codec 5: it reverses every input, codes the words it
// should, depends on nothing but the input, and refuses every malformed
// stream with an Error rather than reading or writing out of bounds.
#include "check.hpp"

#include "text/word_transform.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cpress::test {
namespace {

cpress::Bytes bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

// The input back, through the transform and its inverse.
cpress::Bytes there_and_back(const cpress::Bytes& input) {
    const cpress::Bytes stream = text::forward(input);
    cpress::Bytes output;
    text::inverse(stream, input.size(), output);
    return output;
}

// Text whose words recur often enough to be coded, in every case the
// transform restores, beside words it must leave alone.
cpress::Bytes wordy_text() {
    std::string text;
    for (int i = 0; i < 40; ++i) {
        text += "The cat sat on the mat. THE END of the story, the cat said; ";
        text += "McDonald and iPhone stay as they are, and so does a and I. ";
        text += "Supercalifragilisticexpialidociousandthensome is too long. ";
        text += "caf\xC3\xA9 na\xC3\xAFve \x01\x02\x03\x1F tab\there\r\n";
    }
    return bytes_of(text);
}

void test_round_trips() {
    CHECK(there_and_back({}).empty());
    CHECK(there_and_back(bytes_of("a")) == bytes_of("a"));
    CHECK(there_and_back(bytes_of("no word recurs here")) == bytes_of("no word recurs here"));

    const cpress::Bytes text = wordy_text();
    CHECK(there_and_back(text) == text);

    cpress::Bytes every_byte;
    for (int round = 0; round < 20; ++round) {
        for (int byte = 0; byte < 256; ++byte)
            every_byte.push_back(static_cast<std::uint8_t>(byte));
        const cpress::Bytes words = bytes_of(" these words recur ");
        every_byte.insert(every_byte.end(), words.begin(), words.end());
    }
    CHECK(there_and_back(every_byte) == every_byte);

    const cpress::Bytes noise = random_bytes(std::size_t{256} * 1024, 0x7E47);
    CHECK(there_and_back(noise) == noise);
    const cpress::Bytes prose = sample_text(std::size_t{256} * 1024);
    CHECK(there_and_back(prose) == prose);
}

void test_codes_words() {
    const cpress::Bytes text = wordy_text();
    const cpress::Bytes stream = text::forward(text);
    CHECK(stream.size() < text.size());
    // The same input, the same stream: nothing depends on a hash table's order.
    CHECK(text::forward(text) == stream);
    // Words that recur fewer than kMinCount times are not coded, so the
    // stream is the input after the layout and an empty dictionary.
    const cpress::Bytes once = bytes_of("every word here occurs once only");
    const cpress::Bytes plain = text::forward(once);
    CHECK(plain.size() > once.size());
    CHECK(std::ranges::equal(std::span(plain).last(once.size()), once));
}

// Every prefix of a valid stream is refused: it is cut short, or it
// expands to less than the original size.
void test_truncation() {
    const cpress::Bytes text = wordy_text();
    const cpress::Bytes stream = text::forward(text);
    for (std::size_t size = 0; size < stream.size(); ++size) {
        cpress::Bytes output;
        CHECK(throws_error(
            [&] { text::inverse(std::span(stream).first(size), text.size(), output); }));
    }
}

// The stream's size claim is held exactly: one byte more or less is refused.
void test_original_size() {
    const cpress::Bytes text = wordy_text();
    const cpress::Bytes stream = text::forward(text);
    cpress::Bytes output;
    CHECK(throws_error([&] { text::inverse(stream, text.size() - 1, output); }));
    CHECK(output.size() <= text.size() - 1);
    output.clear();
    CHECK(throws_error([&] { text::inverse(stream, text.size() + 1, output); }));
}

// A stream with one field broken, each of which inverse() must refuse.
void test_malformed_layouts() {
    const cpress::Bytes text = wordy_text();
    const cpress::Bytes good = text::forward(text);
    const std::size_t k = std::size_t{good[0]} + 1;
    const auto refused = [&](cpress::Bytes stream) {
        cpress::Bytes output;
        return throws_error([&] { text::inverse(stream, text.size(), output); });
    };
    const auto with = [&](std::size_t at, std::uint8_t value) {
        cpress::Bytes stream = good;
        stream[at] = value;
        return stream;
    };
    CHECK(refused(with(0, 200)));                               // more than 128 code bytes
    CHECK(refused(with(1, 0x41)));                              // a code byte below 128
    CHECK(refused(with(2, good[1])));                           // code bytes not ascending
    CHECK(refused(with(1 + k, 'e')));                           // the escape a letter
    CHECK(refused(with(1 + k, 0x90)));                          // the escape a code byte
    CHECK(refused(with(2 + k, good[1 + k])));                   // two flags the same
    CHECK(refused(with(4 + k, static_cast<std::uint8_t>(k))));  // B1 + B2 past K
    CHECK(refused(with(6 + k, 'A')));                           // a word not lower case
    CHECK(refused(with(7 + k, '\n')));                          // a word of one letter
}

// Single-byte changes anywhere: never a crash, never more than the
// original size written. Most are refused; some decode to other bytes,
// which the container's CRC catches.
void test_mutations() {
    const cpress::Bytes text = wordy_text();
    const cpress::Bytes good = text::forward(text);
    std::uint64_t state = 0x9E3779B97F4A7C15;
    for (int trial = 0; trial < 2000; ++trial) {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        cpress::Bytes stream = good;
        const std::size_t at = static_cast<std::size_t>(state >> 33) % stream.size();
        stream[at] = static_cast<std::uint8_t>(stream[at] ^ (1U + ((state >> 13) % 255)));
        cpress::Bytes output;
        try {
            text::inverse(stream, text.size(), output);
            CHECK(output.size() == text.size());
        } catch (const cpress::Error&) {
            CHECK(output.size() <= text.size());
        }
    }
}

}  // namespace

void run_word_transform_tests() {
    test_round_trips();
    test_codes_words();
    test_truncation();
    test_original_size();
    test_malformed_layouts();
    test_mutations();
}

}  // namespace cpress::test
