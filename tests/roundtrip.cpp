// Container-level tests: checksum vectors, round trips for every codec, and
// rejection of corrupted archives.
#include "check.hpp"
#include "itch_sample.hpp"

#include "codec/bitwise.hpp"
#include "codec/buffer_stream.hpp"
#include "codec/messages.hpp"
#include "cpress/cpress.hpp"
#include "cpress/io.hpp"
#include "itch/message_model.hpp"
#include "itch/schema.hpp"
#include "model/mix6.hpp"
#include "model/mix9.hpp"
#include "model/order0.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpress::test {
namespace {

cpress::Bytes bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

struct Sample {
    std::string name;
    cpress::Bytes data;
};

std::vector<Sample> samples() {
    std::vector<Sample> out;
    out.push_back({"empty", {}});
    out.push_back({"one byte", {0x42}});
    out.push_back({"zeros 64 KiB", cpress::Bytes(std::size_t{64} * 1024, 0)});
    out.push_back({"random 1 MiB", random_bytes(std::size_t{1024} * 1024, 0x5EED)});

    cpress::Bytes every_byte(256);
    for (std::size_t i = 0; i < every_byte.size(); ++i) {
        every_byte[i] = static_cast<std::uint8_t>(i);
    }
    out.push_back({"every byte value", std::move(every_byte)});

    std::string text;
    for (int i = 0; i < 2000; ++i) {
        text += "the quick brown fox jumps over the lazy dog. ";
    }
    out.push_back({"repetitive text", bytes_of(text)});

    // Not text: market data, which codec 4 parses and the others see as
    // bytes. The six samples above are the other way round: none is ITCH,
    // so codec 4 takes them through its escape.
    out.push_back({"itch messages", itch_sample({.messages = 20'000, .seed = 0x17C4})});
    return out;
}

void test_crc32() {
    CHECK(cpress::crc32(bytes_of("")) == 0x00000000u);
    CHECK(cpress::crc32(bytes_of("123456789")) == 0xCBF43926u);
    CHECK(cpress::crc32(bytes_of("The quick brown fox jumps over the lazy dog")) == 0x414FA339u);
}

void test_codec_registry() {
    CHECK(!cpress::codecs().empty());
    for (const cpress::Codec& codec : cpress::codecs()) {
        CHECK(cpress::find_codec(codec.id) == &codec);
        CHECK(cpress::find_codec(codec.name) == &codec);
    }
    CHECK(cpress::find_codec(std::uint8_t{0xFF}) == nullptr);
    CHECK(cpress::find_codec(std::string_view{"no-such-codec"}) == nullptr);
}

void round_trip(const cpress::Codec& codec, const std::string& name, const cpress::Bytes& data) {
    try {
        const cpress::Bytes archive = cpress::compress(data, codec);
        const cpress::Header header = cpress::read_header(archive);
        CHECK(header.version == cpress::kFormatVersion);
        CHECK(header.codec_id == codec.id);
        CHECK(header.original_size == data.size());
        CHECK(header.crc32 == cpress::crc32(data));
        CHECK(cpress::decompress(archive) == data);
    } catch (const std::exception& e) {
        ++g_failures;
        std::cerr << "round trip threw for codec '" << codec.name << "', sample '" << name
                  << "': " << e.what() << "\n";
    }
}

void test_round_trips(const std::vector<Sample>& inputs) {
    for (const cpress::Codec& codec : cpress::codecs()) {
        for (const Sample& sample : inputs) {
            round_trip(codec, sample.name, sample.data);
        }
    }
}

// Every kind of damage must be rejected with cpress::Error, never accepted
// and never allowed to crash.
void test_corruption() {
    const cpress::Bytes data = random_bytes(4096, 0xBAD);
    for (const cpress::Codec& codec : cpress::codecs()) {
        const cpress::Bytes good = cpress::compress(data, codec);
        CHECK(cpress::decompress(good) == data);

        auto damaged = [&](auto&& mutate) {
            cpress::Bytes copy = good;
            mutate(copy);
            return throws_error([&] { (void)cpress::decompress(copy); });
        };

        // Mutations use at(): a mistake in an offset fails loudly instead of
        // writing out of bounds.
        auto flip = [](std::size_t index) {  // inverts every bit of one byte
            return [index](cpress::Bytes& a) {
                a.at(index) = static_cast<std::uint8_t>(~a.at(index));
            };
        };
        auto set = [](std::size_t index, std::uint8_t value) {
            return [index, value](cpress::Bytes& a) { a.at(index) = value; };
        };

        CHECK(throws_error([] { (void)cpress::decompress(cpress::Bytes{}); }));
        CHECK(damaged([](cpress::Bytes& a) { a.resize(10); }));    // short header
        CHECK(damaged(flip(0)));                                   // magic
        CHECK(damaged(set(4, 2)));                                 // version
        CHECK(damaged(set(5, 0xFF)));                              // codec id
        CHECK(damaged(set(6, 1)));                                 // reserved
        CHECK(damaged(flip(8)));                                   // original size
        CHECK(damaged(flip(16)));                                  // checksum
        CHECK(damaged(flip(cpress::kHeaderSize)));                 // payload
        CHECK(damaged([](cpress::Bytes& a) { a.pop_back(); }));    // truncated
        CHECK(damaged([](cpress::Bytes& a) { a.push_back(0); }));  // trailing byte
    }
}

// A driver's decoder, as the limit tests call it: appends the `size` bytes
// that `payload` stands for to `output`, or throws cpress::Error.
using DecodePayload =
    std::function<void(cpress::ByteView payload, std::uint64_t size, cpress::Bytes& output)>;

// The untrusted-input rules that every driver's decoder keeps, whatever it
// decodes. `max_expansion` is the driver's limit on output bytes per payload
// byte, and `payload` is what its encoder wrote for `data`.
void check_decode_limits(const DecodePayload& decode, std::uint64_t max_expansion,
                         const cpress::Bytes& data, const cpress::Bytes& payload) {
    // The text of the Error `decode` throws, or "" if it does not throw.
    auto error_of = [&](const cpress::Bytes& bytes, std::uint64_t size) -> std::string {
        try {
            cpress::Bytes out;
            decode(bytes, size, out);
        } catch (const cpress::Error& e) {
            return e.what();
        }
        return {};
    };
    auto rejects = [&](const cpress::Bytes& bytes, std::uint64_t size) {
        return !error_of(bytes, size).empty();
    };
    auto over_limit = [&](const cpress::Bytes& bytes, std::uint64_t size) {
        return error_of(bytes, size).find("more data") != std::string::npos;
    };

    // Bounded expansion: four payload bytes may stand for at most four
    // times the limit, and a huge claim is rejected before anything is
    // allocated. (Within the limit a bogus payload is still rejected, by
    // another rule, once the decoder is given it.)
    const cpress::Bytes four_zeros(4, 0);
    const std::uint64_t limit = std::uint64_t{4} * max_expansion;
    CHECK(!rejects(four_zeros, 0));
    CHECK(!over_limit(four_zeros, limit));
    CHECK(over_limit(four_zeros, limit + 1));
    CHECK(over_limit(four_zeros, std::numeric_limits<std::uint64_t>::max()));

    // The limit holds for what is left all the way through, not only on
    // entry. A random payload claiming the most the limit allows passes the
    // entry check, then spends its bytes far faster than the claim requires
    // and is rejected with next to nothing decoded.
    const cpress::Bytes garbage = random_bytes(std::size_t{64} * 1024, 0xB16);
    cpress::Bytes output;
    CHECK(throws_error([&] { decode(garbage, garbage.size() * max_expansion, output); }));
    CHECK(output.size() < max_expansion);

    // No overrun, even when there is nothing to decode.
    CHECK(rejects({}, 0));
    CHECK(rejects(cpress::Bytes(3, 0), 0));
    CHECK(rejects(four_zeros, 100));  // runs out of payload

    // Exact consumption: a byte too many or too few is noticed.
    cpress::Bytes decoded;
    CHECK(!throws_error([&] { decode(payload, data.size(), decoded); }));
    CHECK(decoded == data);

    cpress::Bytes extended = payload;
    extended.push_back(0);
    CHECK(rejects(extended, data.size()));
    cpress::Bytes truncated = payload;
    truncated.pop_back();
    CHECK(rejects(truncated, data.size()));
}

// The rules of decode_bitwise, exercised directly rather than through the
// container.
void test_decode_bitwise_limits() {
    const DecodePayload decode = [](cpress::ByteView payload, std::uint64_t size,
                                    cpress::Bytes& output) {
        codec::decode_bitwise<model::Order0>(payload, size, output);
    };
    const cpress::Bytes data = random_bytes(1000, 0xD474);
    cpress::Bytes payload;
    codec::encode_bitwise<model::Order0>(data, payload);
    check_decode_limits(decode, codec::kMaxExpansion, data, payload);

    // Memory follows the payload and the bytes actually decoded, not the
    // claim. This claim is too modest to be caught early, so the decoder runs
    // until the payload ends. What was allocated by then is the capped
    // reserve, or one step of vector growth past the bytes decoded.
    constexpr std::uint64_t kModestExpansion = 64;
    static_assert(kModestExpansion > codec::kReserveExpansion);
    const cpress::Bytes garbage = random_bytes(std::size_t{64} * 1024, 0xB16);
    cpress::Bytes output;
    CHECK(throws_error([&] { decode(garbage, garbage.size() * kModestExpansion, output); }));
    CHECK(output.size() < garbage.size() * kModestExpansion);
    CHECK(output.capacity() <=
          std::max<std::uint64_t>(garbage.size() * codec::kReserveExpansion, output.size() * 2));
}

// What codec 4 writes after its level byte for `data`, with the model at
// `level`. The encoder's model is gone before this returns.
cpress::Bytes message_payload(const cpress::Bytes& data, int level) {
    cpress::Bytes payload;
    itch::MessageModel model(level);
    codec::SpanSource source(data);
    codec::VectorSink sink(payload);
    CHECK(codec::encode_messages(model, source, sink) == data.size());
    return payload;
}

// The same rules for decode_messages, whose limit is higher: a message can
// cost fewer bits than it has bytes.
void test_decode_messages_limits() {
    constexpr int kLevel = itch::MessageModel::kMinLevel;
    static_assert(codec::kMaxMessageExpansion > codec::kMaxExpansion);
    const DecodePayload decode = [](cpress::ByteView payload, std::uint64_t size,
                                    cpress::Bytes& output) {
        itch::MessageModel model(kLevel);
        codec::VectorSink sink(output);
        codec::decode_messages(model, payload, size, sink);
    };
    const cpress::Bytes data = itch_sample({.messages = 200, .seed = 0x11D});
    check_decode_limits(decode, codec::kMaxMessageExpansion, data, message_payload(data, kLevel));
}

// The most confident model the coder allows: every bit is a 1, predicted at
// kProbMax. No model can expand a payload further.
struct AlwaysOne {
    [[nodiscard]] static std::uint32_t predict() noexcept {
        return model::kProbMax;
    }
    static void update(int /*bit*/) noexcept {}
};

// Codes 1 MiB of `fill` through P and checks that it expands by more than
// `least_expansion`, stays under kMaxExpansion, and still decodes.
template <model::Predictor P>
void check_extreme_expansion(std::uint8_t fill, std::uint64_t least_expansion) {
    const cpress::Bytes data(std::size_t{1024} * 1024, fill);
    cpress::Bytes payload;
    codec::encode_bitwise<P>(data, payload);
    CHECK(data.size() > least_expansion * payload.size());
    CHECK(data.size() < codec::kMaxExpansion * payload.size());

    cpress::Bytes decoded;
    CHECK(!throws_error([&] { codec::decode_bitwise<P>(payload, data.size(), decoded); }));
    CHECK(decoded == data);
}

// kMaxExpansion must sit above the greatest expansion a valid payload can
// reach, or decode_bitwise would reject real archives. All-ones bytes through
// AlwaysOne are that extreme; all-zeros bytes through order0 show how near a
// real model comes to it.
void test_expansion_ceiling() {
    check_extreme_expansion<AlwaysOne>(0xFF, 2000);
    check_extreme_expansion<model::Order0>(0x00, 1000);
}

// The payload of a codec with a memory level for `data`, with the model at
// `level`: what the codec writes, except that the level is the caller's
// choice. The encoder's model is gone before it returns, so a decoder that
// follows does not hold two at once.
using PayloadAtLevel = cpress::Bytes (*)(const cpress::Bytes& data, int level);

// For codecs 2 and 3, which run their model through the bitwise driver.
template <model::Predictor Model>
cpress::Bytes bitwise_payload_at(const cpress::Bytes& data, int level) {
    cpress::Bytes payload{static_cast<std::uint8_t>(level)};
    Model predictor(level);
    codec::encode_bitwise(predictor, data, payload);
    return payload;
}

// For codec 4.
cpress::Bytes message_payload_at(const cpress::Bytes& data, int level) {
    cpress::Bytes payload{static_cast<std::uint8_t>(level)};
    const cpress::Bytes coded = message_payload(data, level);
    payload.insert(payload.end(), coded.begin(), coded.end());
    return payload;
}

// The level byte is untrusted: one outside the model's limits is rejected,
// as is a payload too short to have one.
template <typename Model>
void test_level_byte(std::string_view codec_name, PayloadAtLevel payload_at) {
    const cpress::Codec* codec = cpress::find_codec(codec_name);
    CHECK(codec != nullptr);
    if (codec == nullptr) return;

    const cpress::Bytes data = bytes_of("the level byte comes first");
    const cpress::Bytes good = payload_at(data, Model::kMinLevel);
    auto rejects = [&](const cpress::Bytes& payload) {
        cpress::Bytes output;
        return throws_error([&] { codec->decode(payload, data.size(), output); });
    };
    CHECK(!rejects(good));
    CHECK(rejects({}));

    for (const int level : {0, Model::kMinLevel - 1, Model::kMaxLevel + 1, 255}) {
        cpress::Bytes payload = good;
        payload.at(0) = static_cast<std::uint8_t>(level);
        CHECK(rejects(payload));
    }
}

// The decoder uses the level the archive states, not the one this encoder
// would have chosen: the same small input decodes at the smallest level and
// at the largest. The second allocates everything a level byte can ask for,
// which makes it the test that reaches the decoder's allocation limit.
template <typename Model>
void test_decodes_any_level(std::string_view codec_name, PayloadAtLevel payload_at) {
    const cpress::Codec* codec = cpress::find_codec(codec_name);
    CHECK(codec != nullptr);
    if (codec == nullptr) return;

    const cpress::Bytes data = bytes_of("a small input, coded with a model of any size");
    for (const int level : {Model::kMinLevel, Model::kMaxLevel}) {
        const cpress::Bytes payload = payload_at(data, level);
        CHECK(payload.at(0) == static_cast<std::uint8_t>(level));
        cpress::Bytes decoded;
        CHECK(!throws_error([&] { codec->decode(payload, data.size(), decoded); }));
        CHECK(decoded == data);
    }
}

// The offsets at which the framed messages of `stream` start: the places
// where something can go in without cutting a message in two.
std::vector<std::size_t> message_starts(cpress::ByteView stream) {
    std::vector<std::size_t> starts;
    std::size_t at = 0;
    while (const itch::MessageLayout* layout = itch::framed_message_at(stream.subspan(at))) {
        starts.push_back(at);
        at += itch::kLengthPrefixBytes + layout->length;
    }
    return starts;
}

// Codec 4 takes any bytes. What is a message it parses; everything else goes
// through its escape a byte at a time, and the two alternate freely. Each
// input here leaves the stream of well-formed messages in a different way.
void test_itch_mixed_input() {
    const cpress::Codec* codec = cpress::find_codec(std::string_view{"itch"});
    CHECK(codec != nullptr);
    if (codec == nullptr) return;

    constexpr std::size_t kMessages = 3'000;
    const cpress::Bytes messages = itch_sample({.messages = kMessages, .seed = 0x3D});
    const std::vector<std::size_t> starts = message_starts(messages);
    CHECK(starts.size() == kMessages);
    if (starts.size() != kMessages) return;
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(0x5911CE);

    // Random bytes between messages, a few at a time, at about one message
    // in fifty.
    cpress::Bytes spliced;
    for (std::size_t i = 0; i < kMessages; ++i) {
        if (rng() % 50 == 0) {
            for (std::uint64_t n = 1 + (rng() % 16); n > 0; --n) {
                spliced.push_back(static_cast<std::uint8_t>(rng() >> 56));
            }
        }
        const std::size_t end = i + 1 < kMessages ? starts[i + 1] : messages.size();
        spliced.insert(spliced.end(), messages.begin() + static_cast<std::ptrdiff_t>(starts[i]),
                       messages.begin() + static_cast<std::ptrdiff_t>(end));
    }
    CHECK(spliced.size() > messages.size());

    cpress::Bytes cut_short = messages;
    cut_short.resize(messages.size() - 5);

    // A length that is not the length of the type after it: not a message.
    cpress::Bytes wrong_prefix = messages;
    wrong_prefix.at(starts[1'000] + 1) += 1;

    // The byte that codec 4 uses for its escape, where a type should be.
    cpress::Bytes type_zero = messages;
    type_zero.at(starts[2'000] + itch::kLengthPrefixBytes) = 0;

    // Well-formed messages that make no sense: every type, with every field
    // random, so that no order is known, no stock repeats and no difference
    // is small.
    constexpr std::size_t kSensible = 500;
    constexpr std::size_t kRounds = 40;
    cpress::Bytes senseless(messages.begin(),
                            messages.begin() + static_cast<std::ptrdiff_t>(starts[kSensible]));
    for (std::size_t round = 0; round < kRounds; ++round) {
        for (const itch::MessageLayout& layout : itch::kLayouts) {
            const cpress::Bytes message = random_itch_message(layout.type, rng);
            senseless.insert(senseless.end(), message.begin(), message.end());
        }
    }
    CHECK(count_itch_messages(senseless) == kSensible + (kRounds * itch::kLayouts.size()));

    round_trip(*codec, "random bytes between messages", spliced);
    round_trip(*codec, "a message cut short", cut_short);
    round_trip(*codec, "a prefix that is not the length of its type", wrong_prefix);
    round_trip(*codec, "a type byte of zero", type_zero);
    round_trip(*codec, "messages with random fields", senseless);
}

// `count` random mutations of the archive `codec` makes of `data`. Each
// must either be rejected with cpress::Error or decode normally; anything
// else (another exception type, a crash, a hang, a sanitizer report) is a
// bug. Seeded, so a failure reproduces.
void check_mutations(const cpress::Codec& codec, const cpress::Bytes& data, int count) {
    const cpress::Bytes good = cpress::compress(data, codec);
    std::mt19937_64 rng(0xFACADE + codec.id);
    int rejected = 0;
    for (int i = 0; i < count; ++i) {
        cpress::Bytes archive = good;
        switch (rng() % 4) {
            case 0:  // flip one bit anywhere
                archive.at(rng() % archive.size()) ^= static_cast<std::uint8_t>(1U << (rng() % 8));
                break;
            case 1:  // truncate
                archive.resize(rng() % archive.size());
                break;
            case 2:  // append bytes
                for (std::uint64_t n = 1 + (rng() % 8); n > 0; --n) {
                    archive.push_back(static_cast<std::uint8_t>(rng() >> 56));
                }
                break;
            default:  // overwrite one header byte
                archive.at(rng() % cpress::kHeaderSize) = static_cast<std::uint8_t>(rng() >> 56);
                break;
        }
        try {
            const cpress::Bytes out = cpress::decompress(archive);
            CHECK(out.size() == cpress::read_header(archive).original_size);
        } catch (const cpress::Error&) {
            ++rejected;
        } catch (const std::exception& e) {
            CHECK(false && "decompress threw something other than cpress::Error");
            std::cerr << "codec '" << codec.name << "': " << e.what() << "\n";
        }
    }
    // Almost every mutation damages the archive; if few were rejected the
    // test is not exercising anything.
    CHECK(rejected > count / 2);
}

// 10,000 mutations of one archive for each of the bitwise codecs.
//
// The text is short because every mutation costs a decode of it, and a
// decode through mix6 costs many times one through order0. Through mix9 the
// larger cost is building the model, 4.3 MiB at its smallest, which no
// length of text changes. The length does not change what the test proves:
// every kind of mutation still lands on the header and on a payload that
// takes the decoder through whole bytes.
//
// Codec 4 gets fewer, of another input. Its smallest model is 12.3 MiB, so
// a decode costs several times one of mix9's whatever it decodes: with
// MSVC its 1,500 take 2.7 seconds, against 4.0 for mix9's 10,000. Its input
// is messages, with a few bytes that are not, so that a damaged payload
// takes the decoder through the fields of a message, the order table and
// the escape. A hundred messages make an archive of 608 bytes, so every
// byte of it is still hit more than once.
void test_mutations() {
    constexpr int kMutations = 10'000;
    constexpr int kMessageMutations = 1'500;
    constexpr int kSentences = 5;
    std::string text;
    for (int i = 0; i < kSentences; ++i) {
        text += "mutation testing: the quick brown fox " + std::to_string(i * i) + ". ";
    }

    cpress::Bytes messages = itch_sample({.messages = 100, .seed = 0xFACADE});
    messages.insert(messages.end(), {0x00, 0x24, 0x41});

    for (const cpress::Codec& codec : cpress::codecs()) {
        if (codec.encode_stream == nullptr) {
            check_mutations(codec, bytes_of(text), kMutations);
        } else {
            check_mutations(codec, messages, kMessageMutations);
        }
    }
}

// The archive bytes must not depend on the compiler or platform. These are
// CRC-32s of whole archives, recorded once; CI runs them on MSVC, GCC and
// Clang. A change here means the output of the codec changed: from the first
// tag on, that needs a new codec id (see docs/style.md, Format stability).
struct ArchiveChecksum {
    std::string_view sample;
    std::uint32_t crc32;
};

using ArchiveChecksums = std::array<ArchiveChecksum, 7>;

constexpr ArchiveChecksums kOrder0Checksums{{
    {"empty", 0xe2ccc027},
    {"one byte", 0x87d73a85},
    {"zeros 64 KiB", 0x0825ae3a},
    {"random 1 MiB", 0xea105de5},
    {"every byte value", 0x7455b768},
    {"repetitive text", 0xe13ff503},
    {"itch messages", 0x5d52c615},
}};

constexpr ArchiveChecksums kMix6Checksums{{
    {"empty", 0x92f3c34d},
    {"one byte", 0xdbd12293},
    {"zeros 64 KiB", 0x8ea6845f},
    {"random 1 MiB", 0x0ef3d68f},
    {"every byte value", 0xe3c6c63a},
    {"repetitive text", 0x09956ca7},
    {"itch messages", 0x6c282edd},
}};

constexpr ArchiveChecksums kMix9Checksums{{
    {"empty", 0xf794f80b},
    {"one byte", 0x2ca7f407},
    {"zeros 64 KiB", 0xdb995407},
    {"random 1 MiB", 0x92dff496},
    {"every byte value", 0x8f56f7c7},
    {"repetitive text", 0xe3662933},
    {"itch messages", 0xda7541e1},
}};

constexpr ArchiveChecksums kItchChecksums{{
    {"empty", 0x16d05e98},
    {"one byte", 0x72501d59},
    {"zeros 64 KiB", 0x0743596d},
    {"random 1 MiB", 0x2b9af7ca},
    {"every byte value", 0x891e1357},
    {"repetitive text", 0x40582d6f},
    {"itch messages", 0xe250f8f1},
}};

constexpr ArchiveChecksums kWordsChecksums{{
    {"empty", 0xe0298fae},
    {"one byte", 0x34c2376a},
    {"zeros 64 KiB", 0x573b1dfb},
    {"random 1 MiB", 0x21926f0e},
    {"every byte value", 0x7ec09e64},
    {"repetitive text", 0x0fd7f7d7},
    {"itch messages", 0x04309642},
}};

void check_determinism(std::string_view codec_name, const ArchiveChecksums& checksums,
                       const std::vector<Sample>& inputs) {
    const cpress::Codec* codec = cpress::find_codec(codec_name);
    CHECK(codec != nullptr);
    if (codec == nullptr) return;

    CHECK(inputs.size() == checksums.size());
    for (const Sample& sample : inputs) {
        const auto expected =
            std::ranges::find_if(checksums, [&](const auto& c) { return c.sample == sample.name; });
        CHECK(expected != checksums.end());
        if (expected == checksums.end()) continue;

        const std::uint32_t actual = cpress::crc32(cpress::compress(sample.data, *codec));
        CHECK(actual == expected->crc32);
        if (actual != expected->crc32) {
            std::cerr << "  " << codec_name << " archive CRC-32 of '" << sample.name << "' is 0x"
                      << std::hex << actual << std::dec << "\n";
        }
    }
}

void test_determinism(const std::vector<Sample>& inputs) {
    check_determinism("order0", kOrder0Checksums, inputs);
    check_determinism("mix6", kMix6Checksums, inputs);
    check_determinism("mix9", kMix9Checksums, inputs);
    check_determinism("itch", kItchChecksums, inputs);
    check_determinism("words", kWordsChecksums, inputs);
}

// Bytes needed to code `data` with the best possible order-0 model, i.e.
// its byte-histogram entropy. Floating point is fine in tests.
double order0_entropy_bytes(const cpress::Bytes& data) {
    std::array<std::size_t, 256> counts{};
    for (const std::uint8_t byte : data) ++counts[byte];
    double bits = 0;
    for (const std::size_t count : counts) {
        if (count != 0) {
            const auto n = static_cast<double>(count);
            bits -= n * std::log2(n / static_cast<double>(data.size()));
        }
    }
    return bits / 8;
}

// The sample called `name`. Every caller names one that exists.
const Sample& sample_named(const std::vector<Sample>& inputs, std::string_view name) {
    return *std::ranges::find_if(inputs, [&](const Sample& s) { return s.name == name; });
}

// The size of the archive the codec called `codec_name` makes of `data`.
double archive_size(std::string_view codec_name, const cpress::Bytes& data) {
    const cpress::Codec* codec = cpress::find_codec(codec_name);
    CHECK(codec != nullptr);
    if (codec == nullptr) return 0;
    return static_cast<double>(cpress::compress(data, *codec).size());
}

// order0 must actually compress. An order-0 model sees only byte frequencies,
// not repetition, so the target is the byte-histogram entropy (55% of the
// repetitive-text sample, not less) plus what adapting costs: under 3% on
// this sample with the adaptation shift of 5. Random data cannot be
// compressed at all, so it grows by about 1.2% (2.5% with a shift of 4).
void test_order0_compresses(const std::vector<Sample>& inputs) {
    const cpress::Bytes& text = sample_named(inputs, "repetitive text").data;
    const double text_archive = archive_size("order0", text);
    CHECK(text_archive < 0.6 * static_cast<double>(text.size()));
    CHECK(text_archive < (1.05 * order0_entropy_bytes(text)) + 24);

    const cpress::Bytes& random = sample_named(inputs, "random 1 MiB").data;
    const double random_archive = archive_size("order0", random);
    CHECK(random_archive > static_cast<double>(random.size()));
    CHECK(random_archive < 1.015 * static_cast<double>(random.size()));
}

// mix6 must compress far better. Its long contexts see the repetition order0
// cannot: once the 45-byte sentence has gone by a few times every bit is
// predicted with near certainty, and the rest of the sample's 90,000 bytes
// costs almost nothing. Random data still cannot be compressed, but six
// models that know nothing agree on one half, so it grows by about 0.3%, a
// quarter of what adaptation noise costs order0.
void test_mix6_compresses(const std::vector<Sample>& inputs) {
    const cpress::Bytes& text = sample_named(inputs, "repetitive text").data;
    CHECK(archive_size("mix6", text) < 0.005 * static_cast<double>(text.size()));

    const cpress::Bytes& random = sample_named(inputs, "random 1 MiB").data;
    const double random_archive = archive_size("mix6", random);
    CHECK(random_archive > static_cast<double>(random.size()));
    CHECK(random_archive < 1.005 * static_cast<double>(random.size()));
}

// mix9 must do at least as well. On the repetitive text the match model
// locks on once the sentence has gone by once and the contexts agree with
// it: the archive is 116 bytes, against mix6's 121, so 0.2% (180 bytes)
// leaves a margin of half again. On random data the APMs learn that the mix
// knows nothing and pull it back towards one half: the archive grows by
// 0.12%, against mix6's 0.29%, and the bound is half of mix6's.
void test_mix9_compresses(const std::vector<Sample>& inputs) {
    const cpress::Bytes& text = sample_named(inputs, "repetitive text").data;
    CHECK(archive_size("mix9", text) < 0.002 * static_cast<double>(text.size()));

    const cpress::Bytes& random = sample_named(inputs, "random 1 MiB").data;
    const double random_archive = archive_size("mix9", random);
    CHECK(random_archive > static_cast<double>(random.size()));
    CHECK(random_archive < 1.0025 * static_cast<double>(random.size()));
}

// itch must compress market data far better than the text model does, which
// is the reason it exists. The generated messages are harder than a real
// day, since their prices, sizes and gaps are drawn at random, and the gap
// is narrower than on a day: the archive is 71,443 bytes against mix9's
// 156,913, so 46% of it, and the bound is 55%.
//
// What is not a message goes through the escape, a byte at a time with the
// four bytes before it as contexts. That is enough to see the repetition in
// the text, and random data grows by 0.11%, the type byte that says "not a
// message" having cost next to nothing.
void test_itch_compresses(const std::vector<Sample>& inputs) {
    const cpress::Bytes& messages = sample_named(inputs, "itch messages").data;
    CHECK(archive_size("itch", messages) < 0.55 * archive_size("mix9", messages));

    const cpress::Bytes& text = sample_named(inputs, "repetitive text").data;
    CHECK(archive_size("itch", text) < 0.005 * static_cast<double>(text.size()));

    const cpress::Bytes& random = sample_named(inputs, "random 1 MiB").data;
    const double random_archive = archive_size("itch", random);
    CHECK(random_archive > static_cast<double>(random.size()));
    CHECK(random_archive < 1.0025 * static_cast<double>(random.size()));
}

}  // namespace

void run_roundtrip_tests(std::span<const char* const> files) {
    const std::vector<Sample> inputs = samples();
    test_crc32();
    test_codec_registry();
    test_round_trips(inputs);
    test_corruption();
    test_decode_bitwise_limits();
    test_decode_messages_limits();
    test_expansion_ceiling();
    test_level_byte<model::Mix6>("mix6", &bitwise_payload_at<model::Mix6>);
    test_level_byte<model::Mix9>("mix9", &bitwise_payload_at<model::Mix9>);
    test_level_byte<itch::MessageModel>("itch", &message_payload_at);
    test_decodes_any_level<model::Mix6>("mix6", &bitwise_payload_at<model::Mix6>);
    test_decodes_any_level<model::Mix9>("mix9", &bitwise_payload_at<model::Mix9>);
    test_decodes_any_level<itch::MessageModel>("itch", &message_payload_at);
    test_itch_mixed_input();
    test_mutations();
    test_determinism(inputs);
    test_order0_compresses(inputs);
    test_mix6_compresses(inputs);
    test_mix9_compresses(inputs);
    test_itch_compresses(inputs);

    try {
        std::vector<Sample> loaded;
        for (const char* path : files) {
            loaded.push_back({path, cpress::read_file(path)});
        }
        test_round_trips(loaded);
    } catch (const std::exception& e) {
        ++g_failures;
        std::cerr << "file round trip failed: " << e.what() << "\n";
    }
}

}  // namespace cpress::test
