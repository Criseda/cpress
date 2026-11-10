// Container-level tests: checksum vectors, round trips for every codec, and
// rejection of corrupted archives.
#include "check.hpp"

#include "codec/bitwise.hpp"
#include "cpress/cpress.hpp"
#include "cpress/io.hpp"
#include "model/order0.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
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

cpress::Bytes random_bytes(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    cpress::Bytes data(count);
    for (std::uint8_t& byte : data) {
        byte = static_cast<std::uint8_t>(rng() >> 56);
    }
    return data;
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

// The untrusted-input rules of decode_bitwise, exercised directly rather
// than through the container.
void test_decode_bitwise_limits() {
    using Decode = void (*)(cpress::ByteView, std::uint64_t, cpress::Bytes&);
    const Decode decode = &codec::decode_bitwise<model::Order0>;
    // The text of the Error `decode` throws, or "" if it does not throw.
    auto error_of = [&](const cpress::Bytes& payload, std::uint64_t size) -> std::string {
        try {
            cpress::Bytes out;
            decode(payload, size, out);
        } catch (const cpress::Error& e) {
            return e.what();
        }
        return {};
    };
    auto rejects = [&](const cpress::Bytes& payload, std::uint64_t size) {
        return !error_of(payload, size).empty();
    };
    auto over_limit = [&](const cpress::Bytes& payload, std::uint64_t size) {
        return error_of(payload, size).find("more data") != std::string::npos;
    };

    // Bounded expansion: four payload bytes may stand for at most 16,384
    // bytes, and a huge claim is rejected before anything is allocated.
    // (Within the limit a bogus payload is still rejected, by the overrun
    // check, once the decoder runs out of bytes.)
    const cpress::Bytes four_zeros(4, 0);
    constexpr std::uint64_t kLimit = std::uint64_t{4} * 4096;
    CHECK(!rejects(four_zeros, 0));
    CHECK(!over_limit(four_zeros, kLimit));
    CHECK(over_limit(four_zeros, kLimit + 1));
    CHECK(over_limit(four_zeros, std::numeric_limits<std::uint64_t>::max()));

    // No overrun, even when there is nothing to decode.
    CHECK(rejects({}, 0));
    CHECK(rejects(cpress::Bytes(3, 0), 0));
    CHECK(rejects(four_zeros, 100));  // runs out of payload

    // Exact consumption: a byte too many or too few is noticed.
    const cpress::Bytes data = random_bytes(1000, 0xD474);
    cpress::Bytes payload;
    codec::encode_bitwise<model::Order0>(data, payload);
    cpress::Bytes decoded;
    decode(payload, data.size(), decoded);
    CHECK(decoded == data);

    cpress::Bytes extended = payload;
    extended.push_back(0);
    CHECK(rejects(extended, data.size()));
    cpress::Bytes truncated = payload;
    truncated.pop_back();
    CHECK(rejects(truncated, data.size()));
}

// 10,000 random mutations of one valid archive per codec. Each must either be
// rejected with cpress::Error or decode normally; anything else (another
// exception type, a crash, a hang, a sanitizer report) is a bug. Seeded, so a
// failure reproduces.
void test_mutations() {
    constexpr int kMutations = 10'000;
    std::string text;
    for (int i = 0; i < 100; ++i) {
        text += "mutation testing: the quick brown fox " + std::to_string(i * i) + ". ";
    }
    const cpress::Bytes data = bytes_of(text);

    for (const cpress::Codec& codec : cpress::codecs()) {
        const cpress::Bytes good = cpress::compress(data, codec);
        std::mt19937_64 rng(0xFACADE + codec.id);
        int rejected = 0;
        for (int i = 0; i < kMutations; ++i) {
            cpress::Bytes archive = good;
            switch (rng() % 4) {
                case 0:  // flip one bit anywhere
                    archive.at(rng() % archive.size()) ^=
                        static_cast<std::uint8_t>(1U << (rng() % 8));
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
                    archive.at(rng() % cpress::kHeaderSize) =
                        static_cast<std::uint8_t>(rng() >> 56);
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
        CHECK(rejected > kMutations / 2);
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

constexpr std::array<ArchiveChecksum, 6> kOrder0Checksums{{
    {"empty", 0xe2ccc027},
    {"one byte", 0x87d73a85},
    {"zeros 64 KiB", 0x0825ae3a},
    {"random 1 MiB", 0xea105de5},
    {"every byte value", 0x7455b768},
    {"repetitive text", 0xe13ff503},
}};

void test_determinism() {
    const cpress::Codec* codec = cpress::find_codec(std::string_view{"order0"});
    CHECK(codec != nullptr);
    if (codec == nullptr) return;

    const std::vector<Sample> inputs = samples();
    CHECK(inputs.size() == kOrder0Checksums.size());
    for (const Sample& sample : inputs) {
        const auto expected = std::ranges::find_if(
            kOrder0Checksums, [&](const auto& c) { return c.sample == sample.name; });
        CHECK(expected != kOrder0Checksums.end());
        if (expected == kOrder0Checksums.end()) continue;

        const std::uint32_t actual = cpress::crc32(cpress::compress(sample.data, *codec));
        CHECK(actual == expected->crc32);
        if (actual != expected->crc32) {
            std::cerr << "  archive CRC-32 of '" << sample.name << "' is 0x" << std::hex << actual
                      << std::dec << "\n";
        }
    }
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

// order0 must actually compress. An order-0 model sees only byte frequencies,
// not repetition, so the target is the byte-histogram entropy (55% of the
// repetitive-text sample, not less) plus what adapting costs: under 3% on
// this sample with the adaptation shift of 5. Random data cannot be
// compressed at all, so it grows by about 1.2% (2.5% with a shift of 4).
void test_compression_happens() {
    const cpress::Codec* codec = cpress::find_codec(std::string_view{"order0"});
    CHECK(codec != nullptr);
    if (codec == nullptr) return;

    for (const Sample& sample : samples()) {
        const auto archive_size = static_cast<double>(cpress::compress(sample.data, *codec).size());
        const auto original_size = static_cast<double>(sample.data.size());
        if (sample.name == "repetitive text") {
            CHECK(archive_size < 0.6 * original_size);
            CHECK(archive_size < (1.05 * order0_entropy_bytes(sample.data)) + 24);
        } else if (sample.name == "random 1 MiB") {
            CHECK(archive_size > original_size);
            CHECK(archive_size < 1.015 * original_size);
        }
    }
}

}  // namespace

void run_roundtrip_tests(std::span<const char* const> files) {
    test_crc32();
    test_codec_registry();
    test_round_trips(samples());
    test_corruption();
    test_decode_bitwise_limits();
    test_mutations();
    test_determinism();
    test_compression_happens();

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
