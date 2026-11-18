// Minimal test harness shared by every test group: CHECK, the counters,
// helpers for test data and for driving models, and the declarations of the
// groups that tests/main.cpp runs.
#pragma once

#include "cpress/cpress.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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

// `count` bytes of text made of a few words in a seeded order: it repeats
// with variations, as real text does, so that a model's contexts, words and
// matches all come into play.
inline cpress::Bytes sample_text(std::size_t count) {
    constexpr std::array<std::string_view, 8> kWords{"the ", "model ", "predicts ", "each ",
                                                     "bit ", "of ",    "text, ",    "Again. "};
    std::mt19937_64 rng(count);
    cpress::Bytes text;
    while (text.size() < count) {
        const std::string_view word = kWords.at(rng() % kWords.size());
        text.insert(text.end(), word.begin(), word.end());
    }
    text.resize(count);
    return text;
}

// The prediction at every bit of `data`, coded through `model` most
// significant bit first, as the bitwise driver codes it. `before_byte(i)` is
// called before byte i, where the driver gives its hints.
template <typename Model, typename BeforeByte>
std::vector<std::uint32_t> predictions(Model& model, cpress::ByteView data,
                                       BeforeByte before_byte) {
    std::vector<std::uint32_t> p12s;
    p12s.reserve(data.size() * 8);
    for (std::size_t i = 0; i < data.size(); ++i) {
        before_byte(i);
        for (int shift = 7; shift >= 0; --shift) {
            p12s.push_back(model.predict());
            model.update((data[i] >> shift) & 1);
        }
    }
    return p12s;
}

template <typename Model>
std::vector<std::uint32_t> predictions(Model& model, cpress::ByteView data) {
    return predictions(model, data, [](std::size_t /*byte*/) {});
}

// True if `address` is a multiple of `alignment`.
inline bool is_aligned(const void* address, std::size_t alignment) {
    return std::bit_cast<std::uintptr_t>(address) % alignment == 0;
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
void run_table_allocator_tests();
void run_history_table_tests();
void run_match_model_tests();
void run_apm_tests();
void run_mix9_tests();
void run_golden_tests();
void run_itch_schema_tests();
void run_residual_tests();
void run_field_coder_tests();
void run_order_table_tests();
void run_message_model_tests();
void run_stream_tests();
void run_message_file_tests();
void run_word_transform_tests();
void run_roundtrip_tests(std::span<const char* const> files);

}  // namespace cpress::test
