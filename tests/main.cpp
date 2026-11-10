// cpress test runner. No framework: a failed CHECK is counted and reported,
// and the process exits non-zero if any failed.
//
// Extra arguments are treated as files to round-trip through every codec:
//   cpress-tests corpus/enwik8
#include "check.hpp"

#include <iostream>
#include <span>

int main(int argc, char** argv) {
    using namespace cpress::test;

    const std::span<const char* const> files(argv + 1, static_cast<std::size_t>(argc - 1));
    run_roundtrip_tests(files);

    std::cout << g_checks << " checks, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
