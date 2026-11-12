// TableAllocator and TableArray tests: blocks of every size class are
// aligned and freed cleanly, with large pages and without, tables are
// zeroed, and a model on refused large pages predicts exactly as one on
// granted ones. Run under ASan, they also prove that nothing leaks and that
// each block is freed the way it was allocated.
#include "check.hpp"

#include "model/history_table.hpp"
#include "model/mix9.hpp"
#include "model/table_allocator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

namespace cpress::test {
namespace {

using model::kCacheLineBytes;
using model::kLargePageBytes;
using model::Mix9;
using model::TableAllocator;

// Refuses large pages while it lives, so a test can take the path a
// platform without them takes. It owns a global setting, so it can be
// neither copied nor moved.
class LargePagesRefused {
public:
    LargePagesRefused() noexcept {
        model::detail::refuse_large_pages(true);
    }
    ~LargePagesRefused() {
        model::detail::refuse_large_pages(false);
    }
    LargePagesRefused(const LargePagesRefused&) = delete;
    LargePagesRefused& operator=(const LargePagesRefused&) = delete;
    LargePagesRefused(LargePagesRefused&&) = delete;
    LargePagesRefused& operator=(LargePagesRefused&&) = delete;
};

// Sizes on both sides of kLargePageBytes, and none at all.
constexpr std::array<std::size_t, 7> kBlockBytes{0,
                                                 1,
                                                 kCacheLineBytes + 1,
                                                 kLargePageBytes - 1,
                                                 kLargePageBytes,
                                                 kLargePageBytes + 1,
                                                 (3 * kLargePageBytes) + kCacheLineBytes};

// A block allocated under `refused_at_allocate`, freed under
// `refused_at_free`.
void check_block(std::size_t bytes, bool refused_at_allocate, bool refused_at_free) {
    TableAllocator<std::uint8_t> allocator;
    model::detail::refuse_large_pages(refused_at_allocate);
    std::uint8_t* block = allocator.allocate(bytes);
    CHECK(block != nullptr);
    CHECK(is_aligned(block, kCacheLineBytes));
    if (bytes > 0) {  // both ends are writable
        block[0] = 1;
        block[bytes - 1] = 2;
    }
    model::detail::refuse_large_pages(refused_at_free);
    allocator.deallocate(block, bytes);
    model::detail::refuse_large_pages(false);
}

// Every size allocates an aligned block and frees it cleanly, with large
// pages granted or refused, and refused at one end of its life only: a
// block's size alone decides how it is freed.
void test_blocks_are_aligned_and_freed() {
    for (const std::size_t bytes : kBlockBytes) {
        for (const bool refused_at_allocate : {false, true}) {
            for (const bool refused_at_free : {false, true}) {
                check_block(bytes, refused_at_allocate, refused_at_free);
            }
        }
    }
}

// A count whose size in bytes does not fit in a std::size_t is refused
// before anything is allocated.
void test_overflowing_count_is_bad_alloc() {
    TableAllocator<std::uint32_t> allocator;
    const std::size_t too_many = (std::numeric_limits<std::size_t>::max() / 4) + 1;
    bool threw = false;
    try {
        static_cast<void>(allocator.allocate(too_many));
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
}

// A table is zeroed and aligned, below and above kLargePageBytes, with
// large pages and without.
void test_tables_are_zeroed() {
    constexpr std::size_t kSmall = kCacheLineBytes;
    constexpr std::size_t kLarge = (kLargePageBytes / sizeof(std::uint32_t)) + 1;
    for (const bool refused : {false, true}) {
        model::detail::refuse_large_pages(refused);
        for (const std::size_t count : {kSmall, kLarge}) {
            const model::TableArray<std::uint32_t> table(count);
            CHECK(table.size() == count);
            CHECK(is_aligned(table.data(), kCacheLineBytes));
            CHECK(std::ranges::all_of(table, [](std::uint32_t value) { return value == 0; }));
        }
    }
    model::detail::refuse_large_pages(false);
}

// The smallest level at which every history table above order 2 is a block
// of kLargePageBytes or more.
constexpr int kLargePageLevel = 17;
static_assert(model::HistoryTable::memory_bytes(kLargePageLevel) >= kLargePageBytes);
static_assert(model::HistoryTable::memory_bytes(kLargePageLevel - 1) < kLargePageBytes);

Mix9 make_model_without_large_pages(int level) {
    const LargePagesRefused refused;
    return Mix9(level);
}

// Large pages change where memory is, not what it holds: a model built on
// refused large pages predicts exactly as one built where they are granted,
// at every bit, over text and over random bytes.
void test_refused_large_pages_change_no_prediction() {
    for (const cpress::Bytes& data : {sample_text(4096), random_bytes(4096, 31)}) {
        Mix9 granted(kLargePageLevel);
        Mix9 refused = make_model_without_large_pages(kLargePageLevel);
        CHECK(predictions(granted, data) == predictions(refused, data));
    }
}

}  // namespace

void run_table_allocator_tests() {
    test_blocks_are_aligned_and_freed();
    test_overflowing_count_is_bad_alloc();
    test_tables_are_zeroed();
    test_refused_large_pages_change_no_prediction();
}

}  // namespace cpress::test
