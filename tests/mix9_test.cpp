// Mix9 tests: valid predictions on any input, determinism, and the level
// arithmetic.
#include "check.hpp"

#include "model/mix9.hpp"
#include "model/predictor.hpp"

#include <cstdint>
#include <random>

namespace cpress::test {
namespace {

using model::Mix9;

static_assert(model::Predictor<Mix9>);

// level_for_size: the smallest level with 2^level >= size, within the limits.
static_assert(Mix9::level_for_size(0) == Mix9::kMinLevel);
static_assert(Mix9::level_for_size(1) == Mix9::kMinLevel);
static_assert(Mix9::level_for_size(1024) == 10);
static_assert(Mix9::level_for_size(1025) == 11);
static_assert(Mix9::level_for_size(std::uint64_t{1} << 23) == 23);
static_assert(Mix9::level_for_size(std::uint64_t{1} << 40) == Mix9::kMaxLevel);

// memory_bytes: the table in docs/stage-3.md, section 7.
static_assert(Mix9::memory_bytes(10) == 4'534'656);
static_assert(Mix9::memory_bytes(16) == 13'166'976);
static_assert(Mix9::memory_bytes(20) == 140'962'176);
static_assert(Mix9::memory_bytes(22) == 549'906'816);
static_assert(Mix9::memory_bytes(23) == 1'095'166'336);

// Random bits are the input a model is worst at, and at the smallest level
// contexts collide constantly. Predictions must stay valid all the same.
void test_predictions_stay_in_range() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(16);
    Mix9 model(Mix9::kMinLevel);
    bool in_range = true;
    for (int i = 0; i < 1'000'000; ++i) {
        const std::uint32_t p12 = model.predict();
        in_range = in_range && p12 >= model::kProbMin && p12 <= model::kProbMax;
        model.update(static_cast<int>(rng() & 1));
    }
    CHECK(in_range);
}

void test_deterministic() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(17);
    Mix9 a(Mix9::kMinLevel);
    Mix9 b(Mix9::kMinLevel);
    bool identical = true;
    for (int i = 0; i < 100'000; ++i) {
        identical = identical && a.predict() == b.predict();
        // Bytes with only two bits of entropy, so contexts, words and
        // matches repeat, and every part is exercised beyond its initial
        // state.
        const int bit = (rng() % 4) == 0 ? 1 : 0;
        a.update(bit);
        b.update(bit);
    }
    CHECK(identical);
}

}  // namespace

void run_mix9_tests() {
    test_predictions_stay_in_range();
    test_deterministic();
}

}  // namespace cpress::test
