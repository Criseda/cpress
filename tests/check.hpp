// Minimal test harness shared by every test group: CHECK, the counters, and
// the declarations of the groups that tests/main.cpp runs.
#pragma once

#include "cpress/cpress.hpp"

#include <iostream>
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

// Test groups, run in this order by main(). `files` are extra command-line
// arguments: paths to round-trip through every codec.
void run_roundtrip_tests(std::span<const char* const> files);

}  // namespace cpress::test
