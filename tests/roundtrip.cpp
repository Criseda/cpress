// cpress tests: checksum vectors, round trips for every codec, and rejection
// of corrupted archives. No framework: a failed CHECK is counted and reported,
// and the process exits non-zero if any failed.
//
// Extra arguments are treated as files to round-trip through every codec:
//   cpress-tests corpus/enwik8
#include "cpress/cpress.hpp"
#include "cpress/io.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                          \
    do {                                                                                          \
        ++g_checks;                                                                               \
        if (!(condition)) {                                                                       \
            ++g_failures;                                                                         \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " << #condition << "\n"; \
        }                                                                                         \
    } while (false)

template <typename F>
bool throws_error(F&& f) {
    try {
        std::forward<F>(f)();
    } catch (const cpress::Error&) {
        return true;
    }
    return false;
}

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

        // Inverts every bit of the byte at `index`.
        auto flip = [](std::size_t index) {
            return [index](cpress::Bytes& a) { a[index] = static_cast<std::uint8_t>(~a[index]); };
        };

        CHECK(throws_error([] { (void)cpress::decompress(cpress::Bytes{}); }));
        CHECK(damaged([](cpress::Bytes& a) { a.resize(10); }));    // short header
        CHECK(damaged(flip(0)));                                   // magic
        CHECK(damaged([](cpress::Bytes& a) { a[4] = 2; }));        // version
        CHECK(damaged([](cpress::Bytes& a) { a[5] = 0xFF; }));     // codec id
        CHECK(damaged([](cpress::Bytes& a) { a[6] = 1; }));        // reserved
        CHECK(damaged(flip(8)));                                   // original size
        CHECK(damaged(flip(16)));                                  // checksum
        CHECK(damaged(flip(cpress::kHeaderSize)));                 // payload
        CHECK(damaged([](cpress::Bytes& a) { a.pop_back(); }));    // truncated
        CHECK(damaged([](cpress::Bytes& a) { a.push_back(0); }));  // trailing byte
    }
}

}  // namespace

int main(int argc, char** argv) {
    test_crc32();
    test_codec_registry();
    test_round_trips(samples());
    test_corruption();

    try {
        std::vector<Sample> files;
        for (int i = 1; i < argc; ++i) {
            files.push_back({argv[i], cpress::read_file(argv[i])});
        }
        test_round_trips(files);
    } catch (const std::exception& e) {
        ++g_failures;
        std::cerr << "file round trip failed: " << e.what() << "\n";
    }

    std::cout << g_checks << " checks, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
