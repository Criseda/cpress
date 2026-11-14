// ITCH messages built in the test. There is no ITCH file in the repository,
// and a real one is gigabytes, so the tests make their own: a well-formed
// stream from a seed, with orders that are added, replaced, cancelled,
// executed and deleted, a few stocks, and timestamps that only grow.
#pragma once

#include "cpress/cpress.hpp"

#include <cstddef>
#include <cstdint>
#include <random>

namespace cpress::test {

struct ItchSampleOptions {
    std::size_t messages = 0;
    std::uint64_t seed = 0;
    // Every message about an existing order names a random reference in
    // place of the order's own, so that no order is ever found.
    bool random_references = false;
};

// A stream of framed messages: each with its 2-byte length prefix, as in a
// day file. The same bytes on every platform.
[[nodiscard]] cpress::Bytes itch_sample(const ItchSampleOptions& options);

// One framed message of `type`, an ITCH type, with every field random.
[[nodiscard]] cpress::Bytes random_itch_message(char type, std::mt19937_64& rng);

// How many framed messages `stream` holds, if it is nothing but messages.
[[nodiscard]] std::size_t count_itch_messages(cpress::ByteView stream);

}  // namespace cpress::test
