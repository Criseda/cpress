// Order-0 model tests: it learns, its table stays in range, and it is
// deterministic.
#include "check.hpp"

#include "model/order0.hpp"
#include "model/predictor.hpp"

#include <algorithm>
#include <cstdint>
#include <random>

namespace cpress::test {
namespace {

using model::Order0;

static_assert(model::Predictor<Order0>);
static_assert(!model::Predictor<int>);

// Feeds the bits of `byte`, most significant first.
void feed_byte(Order0& model, std::uint8_t byte) {
    for (int shift = 7; shift >= 0; --shift) {
        model.update((byte >> shift) & 1);
    }
}

void test_learns_a_repeated_byte() {
    constexpr std::uint8_t kByte = 0xA5;  // mixed ones and zeros
    Order0 model;
    CHECK(model.predict() == 2048);
    for (int i = 0; i < 10'000; ++i) {
        feed_byte(model, kByte);
    }
    // Each bit is predicted with confidence above 4000/4096.
    bool confident = true;
    for (int shift = 7; shift >= 0; --shift) {
        const int bit = (kByte >> shift) & 1;
        const std::uint32_t p12 = model.predict();
        const std::uint32_t confidence = bit != 0 ? p12 : 4096 - p12;
        confident = confident && confidence > 4000;
        model.update(bit);
    }
    CHECK(confident);
}

void test_entries_stay_in_range() {
    for (const int bit : {1, 0}) {
        Order0 model;
        bool in_range = true;
        for (int i = 0; i < 100'000; ++i) {
            model.update(bit);
            const std::uint32_t p12 = model.predict();
            in_range = in_range && p12 >= model::kProbMin && p12 <= model::kProbMax;
        }
        CHECK(in_range);
        const auto entries = model.entries();
        // Entry 0 is unused. A constant stream drives the entries it visits
        // all the way to one bound, and must leave the rest inside both.
        const auto [low, high] = std::minmax_element(entries.begin() + 1, entries.end());
        CHECK(*low >= Order0::kEntryMin);
        CHECK(*high <= Order0::kEntryMax);
        CHECK(bit != 0 ? *high == Order0::kEntryMax : *low == Order0::kEntryMin);
        // Contexts the all-ones or all-zeros stream never visits stay untouched.
        CHECK(std::count(entries.begin(), entries.end(), Order0::kP16Half) > 200);
    }
}

void test_deterministic() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(7);
    Order0 a;
    Order0 b;
    bool identical = true;
    for (int i = 0; i < 100'000; ++i) {
        identical = identical && a.predict() == b.predict();
        const int bit = static_cast<int>(rng() & 1);
        a.update(bit);
        b.update(bit);
    }
    CHECK(identical);
    CHECK(std::equal(a.entries().begin(), a.entries().end(), b.entries().begin()));
}

}  // namespace

void run_order0_tests() {
    test_learns_a_repeated_byte();
    test_entries_stay_in_range();
    test_deterministic();
}

}  // namespace cpress::test
