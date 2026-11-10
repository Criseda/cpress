// Arithmetic coder tests: size against the entropy bound, extreme
// probabilities, and exact byte accounting.
#include "check.hpp"

#include "coder/arithmetic_coder.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace cpress::test {
namespace {

struct BitStream {
    std::vector<int> bits;
    std::vector<std::uint32_t> p12s;
};

BitStream constant_p(const std::vector<int>& bits, std::uint32_t p12) {
    return {bits, std::vector<std::uint32_t>(bits.size(), p12)};
}

// Encodes the stream, decodes it, and checks the decoder reproduces every bit
// and reads exactly the payload. Returns the payload size.
std::size_t round_trip(const BitStream& stream) {
    Bytes payload;
    coder::Encoder encoder(payload);
    for (std::size_t i = 0; i < stream.bits.size(); ++i) {
        encoder.encode(stream.bits[i], stream.p12s[i]);
    }
    encoder.finish();

    coder::Decoder decoder(payload);
    bool all_match = true;
    for (std::size_t i = 0; i < stream.bits.size(); ++i) {
        all_match = all_match && decoder.decode(stream.p12s[i]) == stream.bits[i];
    }
    CHECK(all_match);
    CHECK(!decoder.overrun());
    CHECK(decoder.consumed() == payload.size());
    return payload.size();
}

std::vector<int> random_bits(std::size_t count, std::uint64_t seed, std::uint64_t one_in) {
    // Raw generator output and modulo, not std::uniform_int_distribution,
    // whose results differ between standard libraries.
    std::mt19937_64 rng(seed);
    std::vector<int> bits(count);
    for (int& bit : bits) {
        bit = rng() % one_in == 0 ? 1 : 0;
    }
    return bits;
}

constexpr std::size_t kFlushBytes = 4;
constexpr std::size_t kMillion = 1'000'000;

void test_uniform() {
    // Fair bits at p = 1/2 cannot be compressed: 125,000 bytes plus the flush.
    const std::size_t size = round_trip(constant_p(random_bits(kMillion, 1, 2), 2048));
    const double expected = 125'000.0 + static_cast<double>(kFlushBytes);
    CHECK(std::abs(static_cast<double>(size) - expected) <= 0.001 * 125'000.0);
}

void test_skewed() {
    // P(1) = 0.1 coded at p12 = 410 (0.1001): within 1% of the entropy.
    const std::size_t size = round_trip(constant_p(random_bits(kMillion, 2, 10), 410));
    const double entropy_bits = -((0.1 * std::log2(0.1)) + (0.9 * std::log2(0.9)));
    const double expected = static_cast<double>(kMillion) * entropy_bits / 8.0;
    const auto payload = static_cast<double>(size - kFlushBytes);
    CHECK(std::abs(payload - expected) <= 0.01 * expected);
}

void test_extremes() {
    const std::vector<int> ones(100'000, 1);
    const std::vector<int> zeros(100'000, 0);
    round_trip(constant_p(ones, 4095));
    round_trip(constant_p(zeros, 1));
    // Worst case: every bit is the one the model called nearly impossible.
    round_trip(constant_p(ones, 1));
    round_trip(constant_p(zeros, 4095));
}

// A fresh probability on every bit, and bits independent of it, so
// predictions are often badly wrong. Seeded, so every run is identical.
BitStream varying_stream(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    BitStream stream;
    for (std::size_t i = 0; i < kMillion; ++i) {
        stream.p12s.push_back(1 + static_cast<std::uint32_t>(rng() % 4095));
        stream.bits.push_back(static_cast<int>(rng() & 1));
    }
    return stream;
}

void test_varying() {
    round_trip(varying_stream(3));
}

void test_empty() {
    CHECK(round_trip(BitStream{}) == kFlushBytes);
}

void test_overrun() {
    // A decoder given too few bytes reports it, and never reads past the end.
    const Bytes short_payload{0x12, 0x34};
    coder::Decoder decoder(short_payload);
    CHECK(decoder.overrun());
    CHECK(decoder.consumed() == short_payload.size());
    (void)decoder.decode(2048);
    CHECK(decoder.consumed() == short_payload.size());

    const Bytes exact_payload{0, 0, 0, 0};
    CHECK(!coder::Decoder(exact_payload).overrun());
}

}  // namespace

void run_coder_tests() {
    test_uniform();
    test_skewed();
    test_extremes();
    test_varying();
    test_empty();
    test_overrun();
}

}  // namespace cpress::test
