// Residuals: each kind and its inverse return the value, for every width,
// at the edges; and a stepped residual's remainder is always in [0, step).
#include "check.hpp"

#include "itch/residual.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace cpress::test {
namespace {

using itch::Stepped;

constexpr std::array<std::uint32_t, 2> kSteps{4, 100};  // references and prices

// The values of a `width`-byte field worth trying: the smallest, the
// largest, and the two around half the range, where a difference changes
// sign.
std::array<std::uint64_t, 7> edge_values(int width) {
    const std::uint64_t largest = itch::detail::field_mask(width);
    const std::uint64_t half = (largest >> 1) + 1;
    return {0, 1, 2, half - 1, half, largest - 1, largest};
}

// Every kind gives the value back, and a residual that fits the field.
std::size_t failures_for(std::uint64_t value, std::uint64_t expected, int width) {
    const std::uint64_t mask = itch::detail::field_mask(width);
    std::size_t failures = 0;

    const std::uint64_t wrapping = itch::wrapping_residual(value, expected, width);
    if (wrapping > mask || itch::wrapping_value(wrapping, expected, width) != value) ++failures;

    const std::uint64_t signed_u = itch::signed_residual(value, expected, width);
    if (signed_u > mask || itch::signed_value(signed_u, expected, width) != value) ++failures;

    for (const std::uint32_t step : kSteps) {
        const Stepped stepped = itch::stepped_residual(value, expected, width, step);
        if (stepped.quotient > mask || stepped.remainder >= step) ++failures;
        if (itch::stepped_value(stepped, expected, width, step) != value) ++failures;
    }
    return failures;
}

void test_inverses_at_the_edges() {
    std::size_t failures = 0;
    for (int width = 1; width <= 8; ++width) {
        for (const std::uint64_t value : edge_values(width)) {
            for (const std::uint64_t expected : edge_values(width)) {
                failures += failures_for(value, expected, width);
            }
        }
    }
    CHECK(failures == 0);
}

void test_inverses_on_random_values() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(0x2E51D);
    std::size_t failures = 0;
    for (int i = 0; i < 100'000; ++i) {
        const int width = 1 + static_cast<int>(rng() % 8);
        const std::uint64_t mask = itch::detail::field_mask(width);
        // Half the time the two are close, as a good expectation is.
        const std::uint64_t expected = rng() & mask;
        const std::uint64_t value =
            (rng() % 2 == 0 ? rng() : expected + (rng() % 2'001) - 1'000) & mask;
        failures += failures_for(value, expected, width);
    }
    CHECK(failures == 0);
}

// A decoder can be handed any quotient and remainder. The value that comes
// back is some value of the field, and nothing overflows on the way (the
// sanitizer build would say).
void test_stepped_value_accepts_anything() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(0x57E9);
    std::size_t outside = 0;
    for (int i = 0; i < 100'000; ++i) {
        const int width = 1 + static_cast<int>(rng() % 8);
        const Stepped any{.quotient = rng(), .remainder = static_cast<std::uint32_t>(rng())};
        const std::uint64_t value = itch::stepped_value(any, rng(), width, kSteps[rng() % 2]);
        if (value > itch::detail::field_mask(width)) ++outside;
    }
    CHECK(outside == 0);
}

void test_small_differences_are_small_numbers() {
    constexpr int kWidth = 4;
    constexpr std::uint64_t kExpected = 1'000'000;

    CHECK(itch::wrapping_residual(kExpected, kExpected, kWidth) == 0);
    CHECK(itch::wrapping_residual(kExpected + 5, kExpected, kWidth) == 5);

    // zigzag: 0, -1, 1, -2, 2 become 0, 1, 2, 3, 4.
    CHECK(itch::signed_residual(kExpected, kExpected, kWidth) == 0);
    CHECK(itch::signed_residual(kExpected - 1, kExpected, kWidth) == 1);
    CHECK(itch::signed_residual(kExpected + 1, kExpected, kWidth) == 2);
    CHECK(itch::signed_residual(kExpected - 2, kExpected, kWidth) == 3);
    CHECK(itch::signed_residual(kExpected + 2, kExpected, kWidth) == 4);
    // And wrap: the largest value is one below zero.
    CHECK(itch::signed_residual(0xFFFFFFFF, 0, kWidth) == 1);
}

// floor, not truncation: a negative difference has a remainder in
// [0, step), and a difference of -1 is quotient -1 and remainder step - 1.
void test_stepped_rounds_down() {
    constexpr int kWidth = 8;
    constexpr std::uint64_t kExpected = 5'000;
    for (const std::uint32_t step : kSteps) {
        CHECK((itch::stepped_residual(kExpected, kExpected, kWidth, step) == Stepped{0, 0}));
        // One step up is quotient 1, which zigzags to 2.
        CHECK((itch::stepped_residual(kExpected + step, kExpected, kWidth, step) == Stepped{2, 0}));
        CHECK((itch::stepped_residual(kExpected + step + 3, kExpected, kWidth, step) ==
               Stepped{2, 3}));
        // One below is quotient -1, which zigzags to 1.
        CHECK((itch::stepped_residual(kExpected - 1, kExpected, kWidth, step) ==
               Stepped{1, step - 1}));
        CHECK((itch::stepped_residual(kExpected - step, kExpected, kWidth, step) == Stepped{1, 0}));
        CHECK((itch::stepped_residual(kExpected - step - 1, kExpected, kWidth, step) ==
               Stepped{3, step - 1}));
    }
}

}  // namespace

void run_residual_tests() {
    test_inverses_at_the_edges();
    test_inverses_on_random_values();
    test_stepped_value_accepts_anything();
    test_small_differences_are_small_numbers();
    test_stepped_rounds_down();
}

}  // namespace cpress::test
