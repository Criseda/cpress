// Apm tests: a fresh curve is the identity within its interpolation error,
// a bias is learned in its own context only, and points stay in range.
#include "check.hpp"

#include "model/apm.hpp"
#include "model/logistic.hpp"
#include "model/predictor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>

namespace cpress::test {
namespace {

using model::Apm;

constexpr std::uint32_t kP16Max = 0xFFFF;
constexpr std::int32_t kOffset = 2048;  // the st of point 0 is -kOffset
constexpr std::int32_t kSpacing = 128;  // st between points
static_assert(kSpacing * static_cast<std::int32_t>(Apm::kPoints - 1) == 2 * kOffset);

// The most a fresh curve's linear interpolation between points departs from
// squash itself, in p12 units, over every stretched value: computed from
// the curve's own points.
double interpolation_error(const Apm& apm) {
    double worst = 0;
    for (std::int32_t st = -model::kStretchMax; st <= model::kStretchMax; ++st) {
        const std::int32_t x = st + kOffset;
        const auto j = static_cast<std::size_t>(x / kSpacing);
        const double w = static_cast<double>(x % kSpacing) / kSpacing;
        const double p16 = (static_cast<double>(apm.p16(0, j)) * (1 - w)) +
                           (static_cast<double>(apm.p16(0, j + 1)) * w);
        worst = std::max(worst, std::abs((p16 / 16) - static_cast<double>(model::squash(st))));
    }
    return worst;
}

// A fresh APM returns its input, give or take: the interpolation error
// above; how far squash(stretch(p)) is from p, which stretch's rounding
// decides; and one for the shifts that drop fractions. It never decreases
// as its input increases.
void test_fresh_curve_is_the_identity() {
    Apm apm(1);
    const double error = interpolation_error(apm);

    bool within = true;
    bool non_decreasing = true;
    std::uint32_t last = 0;
    for (std::uint32_t p12 = model::kProbMin; p12 <= model::kProbMax; ++p12) {
        const std::uint32_t refined = apm.refine(model::stretch(p12), 0);
        const double rounding = std::abs(static_cast<double>(model::squash(model::stretch(p12))) -
                                         static_cast<double>(p12));
        within = within && std::abs(static_cast<double>(refined) - static_cast<double>(p12)) <=
                               error + rounding + 1;
        non_decreasing = non_decreasing && refined >= last;
        last = refined;
    }
    CHECK(within);
    CHECK(non_decreasing);
}

// An input of one half and bits that are 1 nine times in ten: the curve at
// one half moves to 0.9. One point learns, at a rate of 1/128, so a reading
// has a standard deviation of about sqrt(0.09 / 255) = 0.019; the mean of
// 100,000 is within 0.01. No other context, and no other point of this
// one, changes.
void test_learns_a_bias_in_its_context() {
    constexpr std::size_t kContexts = 4;
    constexpr std::size_t kContext = 2;
    constexpr int kWarmUp = 2'000;
    constexpr int kBits = 100'000;
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(92);
    Apm apm(kContexts);
    const Apm fresh(kContexts);
    double sum = 0;
    for (int i = 0; i < kWarmUp + kBits; ++i) {
        const std::uint32_t p12 = apm.refine(model::stretch(model::kProbOne / 2), kContext);
        if (i >= kWarmUp) sum += static_cast<double>(p12) / model::kProbOne;
        apm.update(rng() % 10 != 0 ? 1 : 0);
    }
    CHECK(std::abs((sum / kBits) - 0.9) < 0.01);

    constexpr std::size_t kCentre = Apm::kPoints / 2;  // where one half lands
    std::size_t changed = 0;
    for (std::size_t context = 0; context < kContexts; ++context) {
        for (std::size_t point = 0; point < Apm::kPoints; ++point) {
            if (apm.p16(context, point) != fresh.p16(context, point)) ++changed;
        }
    }
    CHECK(changed == 1);
    CHECK(apm.p16(kContext, kCentre) != fresh.p16(kContext, kCentre));
}

// 1,000,000 ones, then 1,000,000 zeros in another context, with inputs
// sweeping every p12 so that every point learns. Arithmetic that wrapped
// would leave a point on the wrong side of where it started. The centre
// point, which the most inputs reach, comes to rest: towards a 1 where
// 65535 - p < 2^kApmRate, towards a 0 at 0 exactly.
void test_points_stay_in_range() {
    constexpr int kBits = 1'000'000;
    constexpr std::uint32_t kHighest = kP16Max - ((1U << model::kApmRate) - 1);
    Apm apm(2);
    const Apm fresh(2);
    for (int bit = 0; bit <= 1; ++bit) {
        const auto context = static_cast<std::size_t>(bit);
        for (int i = 0; i < kBits; ++i) {
            const std::uint32_t p12 =
                model::kProbMin + (static_cast<std::uint32_t>(i) % model::kProbMax);
            (void)apm.refine(model::stretch(p12), context);
            apm.update(bit);
        }
        bool moved_towards_the_bit = true;
        for (std::size_t point = 0; point < Apm::kPoints; ++point) {
            const std::uint32_t p16 = apm.p16(context, point);
            const std::uint32_t start = fresh.p16(context, point);
            moved_towards_the_bit =
                moved_towards_the_bit && (bit != 0 ? p16 >= start : p16 <= start);
        }
        CHECK(moved_towards_the_bit);
        const std::uint32_t centre = apm.p16(context, Apm::kPoints / 2);
        CHECK(bit != 0 ? centre >= kHighest : centre == 0);
        const std::uint32_t refined = apm.refine(model::stretch(model::kProbOne / 2), context);
        CHECK(bit != 0 ? refined >= (kHighest >> 4) : refined == model::kProbMin);
    }
}

}  // namespace

void run_apm_tests() {
    test_fresh_curve_is_the_identity();
    test_learns_a_bias_in_its_context();
    test_points_stay_in_range();
}

}  // namespace cpress::test
