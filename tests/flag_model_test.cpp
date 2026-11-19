// FlagModel tests: a fresh model has no favourite, it learns where a guess
// is right and where it is not, its contexts are the last five bytes and
// no more, and a prefetch changes nothing.
#include "check.hpp"

#include "model/flag_model.hpp"
#include "model/predictor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace cpress::test {
namespace {

using model::FlagModel;

constexpr int kTableBits = 16;

// A fresh model gives every guess the same probability, whatever the
// bytes before it and however long the match: every input starts at one
// half, every weight set alike.
void test_fresh_model_has_no_favourite() {
    FlagModel flag(kTableBits);
    const std::uint32_t first = flag.predict(0, 0, 12);
    bool same = true;
    for (const std::uint64_t history : {std::uint64_t{0x6874}, std::uint64_t{0x0102030405060708}}) {
        for (const std::uint32_t guess : {0U, std::uint32_t{'e'}, 0xFFU}) {
            for (const std::uint32_t length : {12U, 40U, 65535U}) {
                same = same && flag.predict(history, guess, length) == first;
            }
        }
    }
    CHECK(same);
}

// Hits for one guess after one context and misses for another after
// another, in turn: each is learned in its own entries, so the first
// guess ends far more likely than the second, either side of one half.
void test_learns_where_the_guess_is_right() {
    FlagModel flag(kTableBits);
    constexpr std::uint64_t kRight = 0x2074686520;  // " the "
    constexpr std::uint64_t kWrong = 0x2061206220;  // " a b "
    constexpr std::uint32_t kLength = 20;
    for (int i = 0; i < 200; ++i) {
        static_cast<void>(flag.predict(kRight, 'c', kLength));
        flag.update(1);
        static_cast<void>(flag.predict(kWrong, 'c', kLength));
        flag.update(0);
    }
    const std::uint32_t right = flag.predict(kRight, 'c', kLength);
    const std::uint32_t wrong = flag.predict(kWrong, 'c', kLength);
    CHECK(right > model::kProbOne / 2);
    CHECK(wrong < model::kProbOne / 2);
}

// The longer context is the last five bytes: bytes before them change no
// prediction, however the model has been trained.
void test_contexts_are_the_last_five_bytes() {
    FlagModel flag(kTableBits);
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(0xF1A6);
    for (int i = 0; i < 5000; ++i) {
        static_cast<void>(flag.predict(rng() & 0xFFFF'FFFF'FF,
                                       static_cast<std::uint32_t>(rng() & 3),
                                       12 + static_cast<std::uint32_t>(rng() % 50)));
        flag.update(static_cast<int>(rng() & 1));
    }
    bool same = true;
    for (std::uint64_t low = 0; low < 64; ++low) {
        const std::uint64_t history = low * 0x0101'0101'01;
        const std::uint32_t p12 = flag.predict(history, 1, 30);
        same = same && flag.predict(history | 0xABCD'EF00'0000'0000, 1, 30) == p12;
    }
    CHECK(same);
}

// Two models fed the same outcomes, one prefetching before every
// prediction, predict the same throughout.
void test_prefetch_changes_nothing() {
    FlagModel plain(kTableBits);
    FlagModel hinted(kTableBits);
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(0x9E7C);
    bool same = true;
    for (int i = 0; i < 20'000; ++i) {
        const std::uint64_t history = rng();
        const auto guess = static_cast<std::uint32_t>(rng() & 0xFF);
        const auto length = static_cast<std::uint32_t>(12 + (rng() % 100));
        hinted.prefetch(history, guess);
        hinted.prefetch(rng(), guess ^ 1);  // a hint for a guess not made
        same =
            same && plain.predict(history, guess, length) == hinted.predict(history, guess, length);
        const int hit = (rng() % 4) != 0 ? 1 : 0;
        plain.update(hit);
        hinted.update(hit);
    }
    CHECK(same);
}

// Memory is the two tables, a byte an entry, and under 8 KiB of maps and
// weights, as flag_model.hpp says.
void test_memory() {
    const std::uint64_t tables = 2 * (std::uint64_t{1} << kTableBits);
    CHECK(FlagModel::memory_bytes(kTableBits) > tables);
    CHECK(FlagModel::memory_bytes(kTableBits) - tables < std::uint64_t{8} * 1024);
}

}  // namespace

void run_flag_model_tests() {
    test_fresh_model_has_no_favourite();
    test_learns_where_the_guess_is_right();
    test_contexts_are_the_last_five_bytes();
    test_prefetch_changes_nothing();
    test_memory();
}

}  // namespace cpress::test
