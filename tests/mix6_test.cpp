// Mix6 tests: valid predictions on any input, determinism, the level
// arithmetic, and that it learns a repeated text as fast as its parts allow.
#include "check.hpp"

#include "model/counter.hpp"
#include "model/logistic.hpp"
#include "model/mix6.hpp"
#include "model/mixer.hpp"
#include "model/predictor.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string_view>

namespace cpress::test {
namespace {

using model::Mix6;

static_assert(model::Predictor<Mix6>);

// level_for_size: the smallest level with 2^level >= size, within the limits.
static_assert(Mix6::level_for_size(0) == Mix6::kMinLevel);
static_assert(Mix6::level_for_size(1) == Mix6::kMinLevel);
static_assert(Mix6::level_for_size(1024) == 10);
static_assert(Mix6::level_for_size(1025) == 11);
static_assert(Mix6::level_for_size(std::uint64_t{1} << 22) == 22);
static_assert(Mix6::level_for_size(std::uint64_t{1} << 40) == Mix6::kMaxLevel);

// memory_bytes: the table in docs/stages/stage-2.md.
constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kMiB = 1024 * kKiB;
static_assert(Mix6::memory_bytes(10) == 192 * kKiB);
static_assert(Mix6::memory_bytes(16) == (10 * kMiB) + (512 * kKiB));
static_assert(Mix6::memory_bytes(20) == (160 * kMiB) + (512 * kKiB));
static_assert(Mix6::memory_bytes(22) == (640 * kMiB) + (512 * kKiB));

// Random bits are the input a model is worst at, and at the smallest level
// contexts collide constantly. Predictions must stay valid all the same.
void test_predictions_stay_in_range() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(14);
    Mix6 model(Mix6::kMinLevel);
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
    std::mt19937_64 rng(15);
    Mix6 a(Mix6::kMinLevel);
    Mix6 b(Mix6::kMinLevel);
    bool identical = true;
    for (int i = 0; i < 100'000; ++i) {
        identical = identical && a.predict() == b.predict();
        // Bytes with only two bits of entropy, so contexts repeat and the
        // tables and weights are exercised, not only their initial values.
        const int bit = (rng() % 4) == 0 ? 1 : 0;
        a.update(bit);
        b.update(bit);
    }
    CHECK(identical);
}

// Feeds `text` once. Returns the least confidence given to any of its bits:
// the p12 of the bit that came, out of kProbOne.
std::uint32_t least_confidence(Mix6& model, std::string_view text) {
    std::uint32_t least = model::kProbOne;
    for (const char c : text) {
        const auto byte = static_cast<std::uint8_t>(c);
        for (int shift = 7; shift >= 0; --shift) {
            const int bit = (byte >> shift) & 1;
            const std::uint32_t p12 = model.predict();
            least = std::min(least, bit != 0 ? p12 : model::kProbOne - p12);
            model.update(bit);
        }
    }
    return least;
}

// A text repeated: by the third repetition every bit is predicted with a
// confidence that follows from the parts.
//
// No byte occurs twice in the text, so at every order a context has exactly
// one successor and no counter ever sees both a 0 and a 1. By the third
// repetition each counter consulted has been updated at least once, by the
// second repetition, with the bit that comes again now:
//   - it predicts that bit at least as strongly as a counter does after one
//     update, kOnceSeenP12;
//   - so all six inputs are at least stretch(kOnceSeenP12), on the right side;
//   - no weight is below kWeightInitial, because an input that is right or
//     silent never loses weight.
// The mix is therefore at least squash(6 * stretch(kOnceSeenP12) / 4).
//
// This assumes no two of the text's contexts share a slot. At this level and
// for this text none do; if the hash or the text changes and two collide,
// the test fails and the text needs changing, not the threshold.
void test_learns_a_repeated_text() {
    constexpr std::string_view kText = "abcdefghijklmnopqrstuvwxyz";
    constexpr int kLevel = 16;

    constexpr std::uint32_t kOnceSeenP12 = [] {
        model::Counter counter;
        counter.update(1);
        return counter.p12();
    }();
    constexpr std::uint32_t kThreshold = model::squash(
        Mix6::kOrders * model::stretch(kOnceSeenP12) * model::kWeightInitial / model::kWeightOne);
    static_assert(kThreshold > 3700, "the derivation should give over 90%");

    Mix6 model(kLevel);
    // The first time through nothing is known, and nothing is guessed.
    CHECK(least_confidence(model, kText) == model::kProbOne / 2);
    (void)least_confidence(model, kText);
    CHECK(least_confidence(model, kText) >= kThreshold);
}

}  // namespace

void run_mix6_tests() {
    test_predictions_stay_in_range();
    test_deterministic();
    test_learns_a_repeated_text();
}

}  // namespace cpress::test
