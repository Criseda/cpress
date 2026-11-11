// Minimal test harness shared by every test group: CHECK, the counters, a
// helper for test data, and the declarations of the groups that
// tests/main.cpp runs.
#pragma once

#include "cpress/cpress.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <span>
#include <utility>

namespace cpress::test {

// Counts every CHECK and every failed one. Inline so all groups share them.
inline int g_checks = 0;
inline int g_failures = 0;

// A failed CHECK is counted and reported; the test keeps running so one run
// shows every failure.
#define CHECK(condition)                                                                          \
    do {                                                                                          \
        ++::cpress::test::g_checks;                                                               \
        if (!(condition)) {                                                                       \
            ++::cpress::test::g_failures;                                                         \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " << #condition << "\n"; \
        }                                                                                         \
    } while (false)

// True if `f` throws cpress::Error.
template <typename F>
bool throws_error(F&& f) {
    try {
        std::forward<F>(f)();
    } catch (const Error&) {
        return true;
    }
    return false;
}

// `count` bytes from a generator seeded with `seed`: the same bytes on every
// platform, so a test that uses them reproduces.
inline cpress::Bytes random_bytes(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    cpress::Bytes data(count);
    for (std::uint8_t& byte : data) {
        byte = static_cast<std::uint8_t>(rng() >> 56);
    }
    return data;
}

// Test groups, run in this order by main(). `files` are extra command-line
// arguments: paths to round-trip through every codec.
void run_coder_tests();
void run_order0_tests();
void run_logistic_tests();
void run_counter_tests();
void run_context_table_tests();
void run_mixer_tests();
void run_mix6_tests();
void run_state_table_tests();
void run_probability_map_tests();
void run_history_table_tests();
void run_match_model_tests();
void run_apm_tests();
void run_mix9_tests();
void run_golden_tests();
void run_roundtrip_tests(std::span<const char* const> files);

}  // namespace cpress::test
