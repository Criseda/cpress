// ProbabilityMap tests: the clamp holds from every starting point, the map
// learns a frequency, and contexts are independent.
#include "check.hpp"

#include "model/history_table.hpp"
#include "model/predictor.hpp"
#include "model/probability_map.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>

namespace cpress::test {
namespace {

using model::kP22One;
using model::ProbabilityMap;

// The state maps' initial values are the counts' own estimate: one half
// for state 0, and always strictly between 0 and 1.
void test_state_map_starts_from_the_counts() {
    CHECK(model::kStateMapInitialP22[0] == kP22One / 2);
    bool in_range = true;
    for (const std::uint32_t p22 : model::kStateMapInitialP22) {
        in_range = in_range && p22 > 0 && p22 < kP22One;
    }
    CHECK(in_range);
}

// From every value a state map starts at, 100,000 zeros or 100,000 ones
// never take a prediction outside [kProbMin, kProbMax]. That is where the
// clamp in p12() earns its place: zeros take p22 below 2^10.
void test_constant_bits_stay_in_range() {
    constexpr int kUpdates = 100'000;
    bool in_range = true;
    bool reached_floor = false;
    for (const std::uint32_t initial : model::kStateMapInitialP22) {
        for (int bit = 0; bit <= 1; ++bit) {
            const std::array<std::uint32_t, 1> start{initial};
            ProbabilityMap map(start);
            for (int i = 0; i < kUpdates; ++i) {
                const std::uint32_t p12 = map.p12(0);
                in_range = in_range && p12 >= model::kProbMin && p12 <= model::kProbMax;
                map.update(bit);
            }
            CHECK(map.count(0) == model::kMapCountLimit);
            reached_floor = reached_floor || map.p22(0) < (1U << 10);
        }
    }
    CHECK(in_range);
    CHECK(reached_floor);
}

// On bits that are 1 nine times in ten, the prediction settles near 0.9.
// At the count limit an update moves p by about 1/1024.5 of the error, so a
// reading has a standard deviation of about sqrt(0.09 / 2049) = 0.0066; the
// mean of 100,000 readings is far steadier. The tolerance is 0.01.
void test_settles_on_the_frequency() {
    constexpr int kWarmUp = 10'000;
    constexpr int kBits = 100'000;
    constexpr double kTolerance = 0.01;
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(91);
    const std::array<std::uint32_t, 1> start{kP22One / 2};
    ProbabilityMap map(start);
    double sum = 0;
    for (int i = 0; i < kWarmUp + kBits; ++i) {
        const std::uint32_t p12 = map.p12(0);
        if (i >= kWarmUp) sum += static_cast<double>(p12) / model::kProbOne;
        map.update(rng() % 10 != 0 ? 1 : 0);
    }
    CHECK(std::abs((sum / kBits) - 0.9) < kTolerance);
}

// An update changes only the context of the last p12().
void test_contexts_are_independent() {
    constexpr std::size_t kContexts = 8;
    constexpr std::size_t kTrained = 5;
    std::array<std::uint32_t, kContexts> start{};
    start.fill(kP22One / 2);
    ProbabilityMap map(start);
    for (int i = 0; i < 100; ++i) {
        (void)map.p12(kTrained);
        map.update(1);
    }
    CHECK(map.p22(kTrained) > kP22One / 2);
    CHECK(map.count(kTrained) == 100);
    std::size_t untouched = 0;
    for (std::size_t context = 0; context < kContexts; ++context) {
        if (map.p22(context) == kP22One / 2 && map.count(context) == 0) ++untouched;
    }
    CHECK(untouched == kContexts - 1);
}

}  // namespace

void run_probability_map_tests() {
    test_state_map_starts_from_the_counts();
    test_constant_bits_stay_in_range();
    test_settles_on_the_frequency();
    test_contexts_are_independent();
}

}  // namespace cpress::test
