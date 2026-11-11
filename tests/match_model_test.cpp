// MatchModel tests: a repeat is predicted to its end, a wrong bit ends the
// match for its byte, and nothing older than the buffer is used.
#include "check.hpp"

#include "model/match_model.hpp"
#include "model/mix9.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cpress::test {
namespace {

using model::MatchModel;

// What the model did on each bit of a byte fed to it.
struct ByteResult {
    int predicted = 0;  // bits on which a match was running
    int right = 0;      // of those, bits whose expected bit was the bit
};

// A match model and what Mix9 keeps for it: the bytes so far.
struct Driven {
    MatchModel model;
    std::uint64_t history = 0;
};

// Feeds a byte as Mix9 does: each bit predicted from the partial byte and
// learned, then the whole byte pushed with the history.
ByteResult feed_byte(Driven& driven, std::uint8_t byte) {
    ByteResult result;
    std::uint32_t partial = 1;
    for (int shift = 7; shift >= 0; --shift) {
        const int bit = (byte >> shift) & 1;
        (void)driven.model.predict(partial);
        if (driven.model.length() > 0) {
            ++result.predicted;
            if (driven.model.expected_bit() == static_cast<std::uint32_t>(bit)) ++result.right;
        }
        driven.model.update(bit);
        partial = (partial * 2) + static_cast<std::uint32_t>(bit);
    }
    driven.history = (driven.history << 8) | byte;
    driven.model.push_byte(driven.history);
    return result;
}

void feed(Driven& driven, std::string_view text) {
    for (const char c : text) (void)feed_byte(driven, static_cast<std::uint8_t>(c));
}

// Sizes with room to spare, so that no two contexts of these short texts
// share a hash slot. If an edit to a text makes two collide, the tests below
// fail, and the text needs changing, not the test.
constexpr int kBufferBits = 16;
constexpr int kHashBits = 16;

// A text, then the text again. Once kMinLength bytes of the second copy are
// in, the lookup finds the first copy, and from there to the end every bit
// is predicted, rightly. The match outgrows 32 bytes, the top mixer class.
void test_predicts_a_repeat_to_its_end() {
    constexpr std::string_view kText =
        "A match model bets that the byte which followed these seven bytes last time "
        "follows them again; this sentence is long enough to prove it.";
    Driven driven{.model = MatchModel(kBufferBits, kHashBits)};
    feed(driven, kText);

    bool all_predicted = true;
    bool all_right = true;
    std::uint32_t top_class = 0;
    for (std::size_t i = 0; i < kText.size(); ++i) {
        const ByteResult result = feed_byte(driven, static_cast<std::uint8_t>(kText[i]));
        if (i >= MatchModel::kMinLength) {
            all_predicted = all_predicted && result.predicted == 8;
            all_right = all_right && result.right == 8;
        }
        top_class = std::max(top_class, driven.model.mixer_context());
    }
    CHECK(all_predicted);
    CHECK(all_right);
    CHECK(top_class == MatchModel::kMixerContexts - 1);
}

// Three pieces. The third follows the second until "middle", where the
// second had 'P' and the third has 'p'. They differ in their third bit, so
// two bits are predicted rightly, the third wrongly, and the rest of the
// byte not at all. At the next boundary a lookup finds "middle p" in the
// first piece, and that match runs to the end.
void test_a_wrong_bit_ends_the_match() {
    constexpr std::string_view kFirst = "[one] the shared middle p and its own ending; ";
    constexpr std::string_view kSecond = "[two] the shared middle P and another ending; ";
    constexpr std::string_view kThird = "[two] the shared middle p and its own ending; ";
    static_assert(('P' ^ 'p') == 0x20, "the third bit, most significant first");
    Driven driven{.model = MatchModel(kBufferBits, kHashBits)};
    feed(driven, kFirst);
    feed(driven, kSecond);

    const std::size_t change = kThird.find('p');
    bool followed = true;
    for (std::size_t i = 0; i < change; ++i) {
        const ByteResult result = feed_byte(driven, static_cast<std::uint8_t>(kThird[i]));
        if (i >= MatchModel::kMinLength) followed = followed && result.right == 8;
    }
    CHECK(followed);

    const ByteResult at_change = feed_byte(driven, static_cast<std::uint8_t>(kThird[change]));
    CHECK(at_change.predicted == 3);
    CHECK(at_change.right == 2);
    CHECK(driven.model.length() >= MatchModel::kMinLength);  // found again at the boundary

    bool resumed = true;
    for (std::size_t i = change + 1; i < kThird.size(); ++i) {
        resumed = resumed && feed_byte(driven, static_cast<std::uint8_t>(kThird[i])).right == 8;
    }
    CHECK(resumed);
}

// Feeds `block`, `gap` zeros and `block` again. Returns the bits of the
// second copy predicted after its first byte (the first byte ends the match
// the zeros made).
int predicted_after_gap(int buffer_bits, int hash_bits, std::size_t block_size, std::size_t gap) {
    const cpress::Bytes block = random_bytes(block_size, 0xB10C);
    Driven driven{.model = MatchModel(buffer_bits, hash_bits)};
    for (const std::uint8_t byte : block) (void)feed_byte(driven, byte);
    for (std::size_t i = 0; i < gap; ++i) (void)feed_byte(driven, 0);
    (void)feed_byte(driven, block[0]);
    int predicted = 0;
    for (std::size_t i = 1; i < block.size(); ++i)
        predicted += feed_byte(driven, block[i]).predicted;
    return predicted;
}

// At the smallest level a model can have, a block that has left the buffer
// is not matched, though its positions are still in the hash table (zeros
// touch only a few of its slots); one still in the buffer is. With ASan
// this also shows that wrapping round the buffer reads nothing outside it.
void test_nothing_older_than_the_buffer_is_used() {
    constexpr int kLevel = model::Mix9::kMinLevel;
    constexpr int kBuffer = model::Mix9::match_buffer_bits(kLevel);
    constexpr int kHash = model::Mix9::match_hash_bits(kLevel);
    constexpr std::size_t kBufferSize = std::size_t{1} << kBuffer;
    constexpr std::size_t kBlock = 100;
    CHECK(predicted_after_gap(kBuffer, kHash, kBlock, kBufferSize / 2) > 0);
    CHECK(predicted_after_gap(kBuffer, kHash, kBlock, kBufferSize) == 0);
}

// Verification never reads a byte the buffer no longer holds. With a buffer
// of B bytes, a copy at distance d has B - d bytes before it still held, so
// a repeat is found at a distance of B - kMinLength and never at one more.
// (A large hash table keeps the block's positions; the buffer is the limit.)
void test_verification_stops_at_the_oldest_byte() {
    constexpr int kBuffer = 10;
    constexpr std::size_t kBufferSize = std::size_t{1} << kBuffer;
    constexpr std::size_t kFurthest = kBufferSize - MatchModel::kMinLength;
    CHECK(predicted_after_gap(kBuffer, kHashBits, kFurthest, 0) > 0);
    CHECK(predicted_after_gap(kBuffer, kHashBits, kFurthest + 1, 0) == 0);
}

}  // namespace

void run_match_model_tests() {
    test_predicts_a_repeat_to_its_end();
    test_a_wrong_bit_ends_the_match();
    test_nothing_older_than_the_buffer_is_used();
    test_verification_stops_at_the_oldest_byte();
}

}  // namespace cpress::test
