// Counter tests: the update arithmetic, its bounds, and that it learns.
#include "check.hpp"

#include "model/counter.hpp"
#include "model/predictor.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>

namespace cpress::test {
namespace {

using model::Counter;
using model::kCountLimit;
using model::kP13Max;
using model::kP13Min;
using model::kP13One;

void test_starts_at_one_half() {
    const Counter counter;
    CHECK(counter.p13() == kP13One / 2);
    CHECK(counter.count() == 0);
    CHECK(counter.p12() == model::kProbOne / 2);
}

// The first steps from one half, worked out by hand from the rates
// 1/1.5, 1/2.5, 1/3.5 and 1/4.5.
void test_first_steps() {
    constexpr std::array<std::uint32_t, 4> kAfterOnes{6826, 7372, 7606, 7736};
    constexpr std::array<std::uint32_t, 4> kAfterZeros{1366, 820, 586, 456};
    Counter ones;
    Counter zeros;
    for (std::size_t i = 0; i < kAfterOnes.size(); ++i) {
        ones.update(1);
        zeros.update(0);
        CHECK(ones.p13() == kAfterOnes.at(i));
        CHECK(zeros.p13() == kAfterZeros.at(i));
        CHECK(ones.count() == i + 1);
    }
}

// A constant stream drives p13 to its bound, where it stays, and the count
// stops at its limit. The bounds mirror each other because the two branches
// of the update do.
void test_bounds_are_reached_and_held() {
    constexpr int kEnoughToSettle = 1000;
    for (const int bit : {1, 0}) {
        const std::uint32_t bound = bit != 0 ? kP13Max : kP13Min;
        Counter counter;
        for (int i = 0; i < kEnoughToSettle; ++i) counter.update(bit);
        CHECK(counter.p13() == bound);
        CHECK(counter.count() == kCountLimit);

        bool held = true;
        for (int i = 0; i < kEnoughToSettle; ++i) {
            counter.update(bit);
            held = held && counter.p13() == bound && counter.count() == kCountLimit;
        }
        CHECK(held);
        CHECK(counter.p12() >= model::kProbMin);
        CHECK(counter.p12() <= model::kProbMax);
    }
    CHECK(kP13Max == kP13One - kP13Min);
}

// On bits that are 1 nine times in ten, the prediction settles around 0.9.
// A single reading wanders, because a counter at its count limit weighs the
// last few bits heavily, so the test takes the mean over the run. The
// sampling error of 100,000 such bits is 0.001; the tolerance is ten times
// that.
void test_settles_on_the_frequency() {
    constexpr int kWarmUp = 100;
    constexpr int kBits = 100'000;
    constexpr double kTolerance = 0.01;
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(90);
    Counter counter;
    double sum = 0;
    for (int i = 0; i < kWarmUp + kBits; ++i) {
        counter.update(rng() % 10 != 0 ? 1 : 0);
        if (i >= kWarmUp) sum += static_cast<double>(counter.p13()) / kP13One;
    }
    const double mean = sum / kBits;
    CHECK(std::abs(mean - 0.9) < kTolerance);
}

}  // namespace

void run_counter_tests() {
    test_starts_at_one_half();
    test_first_steps();
    test_bounds_are_reached_and_held();
    test_settles_on_the_frequency();
}

}  // namespace cpress::test
