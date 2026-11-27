// Byte class tests: every byte value has the class the brief lists, the
// history keeps the newest class lowest, and each key depends on exactly
// the classes it is said to.
#include "check.hpp"

#include "model/byte_classes.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace cpress::test {
namespace {

namespace classes = model::classes;

// The brief's list (docs/stages/stage-9.md, section 2), by the characters
// of each class but the three ranges and the two catch-alls.
void test_classes_are_the_briefs() {
    constexpr std::array<std::string_view, classes::kClassCount> kMembers{
        "", "\n", " ", "", "", "", "", ".!?", ",;:", "'\"", "()", "[]", "{}|", "<>/\\", "=&#*", ""};
    std::array<std::uint32_t, 256> expected{};
    for (std::uint32_t byte = 0; byte <= 0xFF; ++byte) {
        if (byte < 0x20)
            expected[byte] = 0;
        else if (byte >= 0x80)
            expected[byte] = 6;
        else if (byte >= '0' && byte <= '9')
            expected[byte] = 3;
        else if (byte >= 'a' && byte <= 'z')
            expected[byte] = 4;
        else if (byte >= 'A' && byte <= 'Z')
            expected[byte] = 5;
        else
            expected[byte] = 15;
    }
    for (std::uint32_t c = 0; c < classes::kClassCount; ++c) {
        for (const char member : kMembers[c]) expected[static_cast<std::uint8_t>(member)] = c;
    }
    bool all_match = true;
    for (std::uint32_t byte = 0; byte <= 0xFF; ++byte) {
        all_match = all_match && classes::class_of(byte) == expected[byte];
    }
    CHECK(all_match);
}

// The class history after `text`, from the start.
std::uint64_t history_of(std::string_view text) {
    std::uint64_t history = 0;
    for (const char c : text)
        history = classes::next_history(history, static_cast<std::uint8_t>(c));
    return history;
}

// The newest class is in the lowest kClassBits; last_two() is the last two.
void test_history_keeps_the_newest_lowest() {
    const std::uint64_t history = history_of("a1 ");
    CHECK((history & (classes::kClassCount - 1)) == classes::class_of(' '));
    CHECK(classes::last_two(history) ==
          ((classes::class_of('1') << classes::kClassBits) | classes::class_of(' ')));
}

// Each key sees its own number of classes and no more: texts that differ
// only before them share a key, and texts that differ within them do not.
void test_keys_see_their_classes() {
    // "x" and "." differ in class; both are then followed by 12 bytes.
    constexpr std::string_view kTwelve = "the cat sat ";
    static_assert(kTwelve.size() == classes::kLongClasses);
    const std::uint64_t a = history_of(std::string("x").append(kTwelve));
    const std::uint64_t b = history_of(std::string(".").append(kTwelve));
    CHECK(classes::long_key(a) == classes::long_key(b));
    CHECK(classes::short_key(a) == classes::short_key(b));

    // Changing the class 12 bytes back moves the long key, not the short.
    const std::uint64_t c = history_of(std::string("Xhe cat sat "));
    const std::uint64_t d = history_of(std::string(".he cat sat "));
    CHECK(classes::long_key(c) != classes::long_key(d));
    CHECK(classes::short_key(c) == classes::short_key(d));

    // Changing it 6 bytes back moves both.
    const std::uint64_t e = history_of("the ca. sat ");
    CHECK(classes::long_key(e) != classes::long_key(c));
    CHECK(classes::short_key(e) != classes::short_key(c));
}

}  // namespace

void run_byte_classes_tests() {
    test_classes_are_the_briefs();
    test_history_keeps_the_newest_lowest();
    test_keys_see_their_classes();
}

}  // namespace cpress::test
