// Mixer tests: it learns which input to trust, its weights stay within their
// limit under the worst inputs, and its weight sets are independent. A
// decaying rate falls as it should, and at its end learns exactly as a
// plain mixer.
#include "check.hpp"

#include "model/logistic.hpp"
#include "model/mixer.hpp"
#include "model/predictor.hpp"
#include "model/table_allocator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>

namespace cpress::test {
namespace {

using model::kStretchMax;
using model::kWeightInitial;
using model::kWeightLimit;
using model::Mixer;

constexpr std::uint32_t kHalf = model::kProbOne / 2;

void test_learns_which_input_to_trust() {
    // Input 0 always argues for the bit that comes, input 1 always against.
    constexpr std::int32_t kConfidence = model::kStretchOne;
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(11);
    Mixer<2> mixer(1);
    for (int i = 0; i < 1000; ++i) {
        const int bit = static_cast<int>(rng() & 1);
        const std::int32_t right = bit != 0 ? kConfidence : -kConfidence;
        (void)mixer.mix({right, -right});
        mixer.update(bit);
    }
    CHECK(mixer.weights(0)[0] > 0);
    CHECK(mixer.weights(0)[1] < 0);
    CHECK(mixer.mix({kConfidence, -kConfidence}) > kHalf);
    CHECK(mixer.mix({-kConfidence, kConfidence}) < kHalf);
}

// The smallest and largest weight in a set.
template <std::size_t Inputs, bool DecayingRate>
std::pair<std::int32_t, std::int32_t> weight_range(const Mixer<Inputs, DecayingRate>& mixer,
                                                   std::size_t set) {
    const auto weights = mixer.weights(set);
    const auto [low, high] = std::ranges::minmax_element(weights);
    return {*low, *high};
}

// The case UBSan must pass: weights at their limit, inputs at theirs.
//
// Strong inputs alone cannot push a weight far. Once the weights are large
// enough for the mix to be confident, the error is 1 and the step rounds to
// zero. Weak inputs can: six inputs of kWeakInput still only add up to
// 6 * 8.0 * 8 = 384 at the weight limit, a p12 well short of certainty, so
// while they keep agreeing with the bit the error stays large and every
// weight climbs until the clamp stops it. That is the first half of the
// test. The second half then feeds the largest inputs there are, with the
// bit always the opposite of what the mix predicts: the largest dot product
// and the largest steps the arithmetic will ever see. A decaying rate makes
// its largest steps at the start, so the test runs on fresh sets too.
template <bool DecayingRate>
void test_weights_stay_within_the_limit() {
    constexpr std::int32_t kWeakInput = 8;
    constexpr int kBits = 1'000'000;
    using TestMixer = Mixer<6, DecayingRate>;  // six inputs

    for (const int direction : {1, -1}) {
        TestMixer mixer(1);
        typename TestMixer::Stretched weak{};
        weak.fill(direction * kWeakInput);
        for (int i = 0; i < kBits; ++i) {
            (void)mixer.mix(weak);
            mixer.update(1);
        }
        // Agreeing with a 1 drives the weights of positive inputs up, and
        // those of negative inputs down, all the way.
        const auto [low, high] = weight_range(mixer, 0);
        CHECK(low == high);
        CHECK(high == direction * kWeightLimit);

        // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
        std::mt19937_64 rng(12);
        bool within = true;
        bool valid = true;
        for (int i = 0; i < kBits; ++i) {
            typename TestMixer::Stretched extreme{};
            for (std::int32_t& st : extreme) {
                st = (rng() & 1) != 0 ? kStretchMax : -kStretchMax;
            }
            // The first few rounds keep every input on one side, so the dot
            // product is as large as it can be while the weights are still
            // at the limit.
            if (i < 4) extreme.fill(i % 2 == 0 ? kStretchMax : -kStretchMax);

            const std::uint32_t p12 = mixer.mix(extreme);
            valid = valid && p12 >= model::kProbMin && p12 <= model::kProbMax;
            mixer.update(p12 >= kHalf ? 0 : 1);
            const auto [lowest, highest] = weight_range(mixer, 0);
            within = within && lowest >= -kWeightLimit && highest <= kWeightLimit;
        }
        CHECK(valid);
        CHECK(within);
    }
}

void test_weight_sets_are_independent() {
    constexpr std::size_t kSets = 4;
    constexpr std::size_t kTrained = 2;
    Mixer<2> mixer(kSets);
    mixer.select(kTrained);
    for (int i = 0; i < 100; ++i) {
        (void)mixer.mix({model::kStretchOne, -model::kStretchOne});
        mixer.update(1);
    }
    for (std::size_t set = 0; set < kSets; ++set) {
        const auto [low, high] = weight_range(mixer, set);
        // Training moved the two weights of its set apart, and no others.
        CHECK((low < kWeightInitial) == (set == kTrained));
        CHECK((high > kWeightInitial) == (set == kTrained));
    }
    // Selecting an untrained set brings back the untrained mix: two equal
    // weights on opposite inputs cancel.
    mixer.select(0);
    CHECK(mixer.mix({model::kStretchOne, -model::kStretchOne}) == kHalf);
}

// A decaying rate starts at kRateInitial, falls by one with each update of
// its own set, and stops at kRateFinal. Inputs of 0 move no weight, so they
// age a set without teaching it anything.
void test_rate_decays_per_set() {
    constexpr std::size_t kSets = 2;
    Mixer<2, true> mixer(kSets);
    CHECK(mixer.rate(0) == model::kRateInitial);
    CHECK(mixer.rate(1) == model::kRateInitial);
    constexpr int kToFinal = model::kRateInitial - model::kRateFinal;
    bool falls_by_one = true;
    for (int i = 1; i <= kToFinal + 10; ++i) {
        (void)mixer.mix({0, 0});
        mixer.update(1);
        const auto expected =
            static_cast<std::uint16_t>(std::max(model::kRateInitial - i, int{model::kRateFinal}));
        falls_by_one = falls_by_one && mixer.rate(0) == expected;
    }
    CHECK(falls_by_one);
    CHECK(mixer.rate(1) == model::kRateInitial);  // the other set was never updated
    const auto [low, high] = weight_range(mixer, 0);
    CHECK(low == kWeightInitial);
    CHECK(high == kWeightInitial);
}

// The first update of a fresh set takes a step kRateInitial / kRateFinal
// times a plain mixer's, rounded once.
void test_fresh_set_learns_fast() {
    constexpr std::int32_t kInput = model::kStretchOne;
    Mixer<1> plain(1);
    Mixer<1, true> decaying(1);
    const std::uint32_t p12 = plain.mix({kInput});
    CHECK(decaying.mix({kInput}) == p12);
    plain.update(1);
    decaying.update(1);
    const std::int64_t product = std::int64_t{kInput} * (model::kProbOne - p12);
    const std::int64_t plain_step = (product + model::kLearningRounding) >> model::kLearningShift;
    constexpr int kShift = model::kLearningShift + model::kRateBits;
    const std::int64_t fast_step =
        ((product * model::kRateInitial) + (std::int64_t{1} << (kShift - 1))) >> kShift;
    CHECK(plain.weights(0)[0] == kWeightInitial + plain_step);
    CHECK(decaying.weights(0)[0] == kWeightInitial + fast_step);
    CHECK(fast_step > 13 * plain_step);
}

// Once its rate has decayed, a set learns exactly as a plain mixer does:
// the same weights after the same bits, whatever the inputs.
void test_decayed_rate_is_the_plain_rate() {
    constexpr std::size_t kInputs = 3;
    Mixer<kInputs> plain(1);
    Mixer<kInputs, true> decaying(1);
    for (int i = 0; i < model::kRateInitial - model::kRateFinal; ++i) {
        (void)decaying.mix({0, 0, 0});
        decaying.update(0);
    }
    CHECK(decaying.rate(0) == model::kRateFinal);
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(13);
    bool same = true;
    for (int i = 0; i < 100'000; ++i) {
        Mixer<kInputs>::Stretched st{};
        for (std::int32_t& input : st) {
            input = static_cast<std::int32_t>(rng() % ((2 * kStretchMax) + 1)) - kStretchMax;
        }
        same = same && plain.mix(st) == decaying.mix(st);
        const int bit = static_cast<int>(rng() & 1);
        plain.update(bit);
        decaying.update(bit);
    }
    CHECK(same);
    CHECK(std::ranges::equal(plain.weights(0), decaying.weights(0)));
}

// A set and its rate are one cache line, and a plain mixer's sets are as
// they were before rates existed.
static_assert(Mixer<12, true>::memory_bytes(1) == model::kCacheLineBytes);
static_assert(Mixer<10>::memory_bytes(3) == std::size_t{3} * 10 * sizeof(std::int32_t));

}  // namespace

void run_mixer_tests() {
    test_learns_which_input_to_trust();
    test_weights_stay_within_the_limit<false>();
    test_weights_stay_within_the_limit<true>();
    test_weight_sets_are_independent();
    test_rate_decays_per_set();
    test_fresh_set_learns_fast();
    test_decayed_rate_is_the_plain_rate();
}

}  // namespace cpress::test
