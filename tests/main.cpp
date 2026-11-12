// cpress test runner. No framework: a failed CHECK is counted and reported,
// and the process exits non-zero if any failed.
//
// Extra arguments are treated as files to round-trip through every codec:
//   cpress-tests corpus/enwik8
#include "check.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <span>

int main(int argc, char** argv) {
    using namespace cpress::test;

    // argc can be 0: the standard does not promise a program name.
    const std::span<const char* const> arguments(argv, static_cast<std::size_t>(argc));
    const auto files = arguments.subspan(std::min<std::size_t>(arguments.size(), 1));
    run_coder_tests();
    run_order0_tests();
    run_logistic_tests();
    run_counter_tests();
    run_context_table_tests();
    run_mixer_tests();
    run_mix6_tests();
    run_state_table_tests();
    run_probability_map_tests();
    run_table_allocator_tests();
    run_history_table_tests();
    run_match_model_tests();
    run_apm_tests();
    run_mix9_tests();
    run_golden_tests();
    run_roundtrip_tests(files);

    std::cout << g_checks << " checks, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
