// stretch and squash tests: the tables are the real functions rounded, and
// they are monotonic, symmetric and inverse to each other.
#include "check.hpp"

#include "model/logistic.hpp"
#include "model/predictor.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace cpress::test {
namespace {

using model::kProbMax;
using model::kProbMin;
using model::kProbOne;
using model::kStretchMax;
using model::kStretchOne;
using model::squash;
using model::stretch;

// Known values: ln(p / (1 - p)) * 256 and its inverse, worked out by hand.
static_assert(squash(0) == 2048);
static_assert(squash(256) == 2994);
static_assert(squash(-256) == 1102);
static_assert(squash(1024) == 4022);
static_assert(squash(2047) == 4095);
static_assert(squash(-2047) == 1);
static_assert(stretch(2048) == 0);
static_assert(stretch(3000) == 258);
static_assert(stretch(4095) == 2026);
static_assert(stretch(1) == -2026);

void test_squash_is_the_real_function_rounded() {
    // Floating point is fine in tests. Rounding to nearest is off by at most
    // a half; the 0.01 covers the error of the table's own arithmetic.
    constexpr double kTolerance = 0.51;
    bool close = true;
    for (std::int32_t st = -kStretchMax; st <= kStretchMax; ++st) {
        const double real = kProbOne / (1.0 + std::exp(-static_cast<double>(st) / kStretchOne));
        close = close && std::abs(static_cast<double>(squash(st)) - real) <= kTolerance;
    }
    CHECK(close);
}

void test_squash_is_monotonic_and_symmetric() {
    bool non_decreasing = true;
    bool symmetric = true;
    for (std::int32_t st = -kStretchMax; st < kStretchMax; ++st) {
        non_decreasing = non_decreasing && squash(st) <= squash(st + 1);
    }
    for (std::int32_t st = 0; st <= kStretchMax; ++st) {
        symmetric = symmetric && squash(-st) == kProbOne - squash(st);
    }
    CHECK(non_decreasing);
    CHECK(symmetric);
}

void test_squash_clamps_its_argument() {
    CHECK(squash(kStretchMax + 1) == kProbMax);
    CHECK(squash(-kStretchMax - 1) == kProbMin);
    CHECK(squash(std::numeric_limits<std::int32_t>::max()) == kProbMax);
    CHECK(squash(std::numeric_limits<std::int32_t>::min()) == kProbMin);
}

void test_stretch_is_monotonic_and_odd() {
    bool non_decreasing = true;
    bool odd = true;
    bool in_range = true;
    for (std::uint32_t p12 = kProbMin; p12 < kProbMax; ++p12) {
        non_decreasing = non_decreasing && stretch(p12) <= stretch(p12 + 1);
    }
    for (std::uint32_t p12 = kProbMin; p12 <= kProbMax; ++p12) {
        odd = odd && stretch(kProbOne - p12) == -stretch(p12);
        in_range = in_range && stretch(p12) >= -kStretchMax && stretch(p12) <= kStretchMax;
    }
    CHECK(non_decreasing);
    CHECK(odd);
    CHECK(in_range);
}

// stretch(p) is the least st that squashes to p or more, and squash rises by
// at most 4 per step (its slope at the centre is kProbOne / 4 per
// kStretchOne), so the round trip lands on p or less than 4 above it.
void test_squash_inverts_stretch() {
    bool inverse = true;
    for (std::uint32_t p12 = kProbOne / 2; p12 <= kProbMax; ++p12) {
        const std::uint32_t back = squash(stretch(p12));
        inverse = inverse && back >= p12 && back - p12 < 4;
    }
    CHECK(inverse);
}

}  // namespace

void run_logistic_tests() {
    test_squash_is_the_real_function_rounded();
    test_squash_is_monotonic_and_symmetric();
    test_squash_clamps_its_argument();
    test_stretch_is_monotonic_and_odd();
    test_squash_inverts_stretch();
}

}  // namespace cpress::test
