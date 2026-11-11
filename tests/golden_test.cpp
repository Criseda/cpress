// Golden archives: one small archive per frozen codec, checked in as bytes.
// A frozen codec's id and output are permanent (docs/style.md, Format
// stability), so each archive here must keep decoding to its text, and its
// text must keep compressing to exactly these bytes, for as long as the
// project exists. If this test fails, the codec changed: revert the change
// or give it a new codec id. Never edit the bytes.
#include "check.hpp"

#include "cpress/cpress.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace cpress::test {
namespace {

// Codec 1, order0, frozen when stage 1 was merged. Produced at 5beef5f by:
//   printf '%s' '<kOrder0Text>' > golden.txt
//   cpress c --codec order0 golden.txt
// The text is too short for an order-0 model to learn, so the archive is
// three bytes longer than its input.
constexpr std::uint8_t kOrder0Id = 1;
constexpr std::string_view kOrder0Text =
    "cpress codec 1 (order0), frozen by stage 1: this text must decode from these bytes forever.";
constexpr std::array<std::uint8_t, 94> kOrder0Archive{
    0x43, 0x50, 0x52, 0x53, 0x01, 0x01, 0x00, 0x00, 0x5b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x77, 0xf7, 0xf4, 0x9c, 0x8c, 0x80, 0x69, 0x3e, 0x29, 0xc0, 0xed, 0x72, 0xbf, 0xb6, 0x60,
    0x7f, 0xb8, 0x7f, 0xa7, 0xf6, 0xab, 0xf7, 0xdf, 0x4d, 0x20, 0xad, 0x35, 0x4c, 0x22, 0xd4, 0xbb,
    0x60, 0xaf, 0xdd, 0xbf, 0x85, 0x02, 0x5f, 0x5a, 0xd4, 0xcf, 0xa0, 0xc9, 0x70, 0x2c, 0x2d, 0x68,
    0xb2, 0x3a, 0x1b, 0x96, 0x40, 0x76, 0xd2, 0x80, 0x0c, 0x41, 0x6b, 0x56, 0xac, 0x61, 0x4f, 0xa0,
    0x23, 0x49, 0x89, 0xd1, 0x3b, 0x5e, 0xb7, 0x8c, 0x35, 0x45, 0x78, 0x79, 0xc4, 0x2f,
};

void test_order0_golden_archive() {
    const cpress::Bytes text(kOrder0Text.begin(), kOrder0Text.end());
    const cpress::Bytes archive(kOrder0Archive.begin(), kOrder0Archive.end());

    CHECK(cpress::read_header(archive).codec_id == kOrder0Id);
    cpress::Bytes decoded;
    CHECK(!throws_error([&] { decoded = cpress::decompress(archive); }));
    CHECK(decoded == text);

    const cpress::Codec* codec = cpress::find_codec(kOrder0Id);
    CHECK(codec != nullptr);
    if (codec == nullptr) return;
    CHECK(cpress::compress(text, *codec) == archive);
}

}  // namespace

void run_golden_tests() {
    test_order0_golden_archive();
}

}  // namespace cpress::test
