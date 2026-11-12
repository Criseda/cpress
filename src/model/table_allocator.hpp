// TableAllocator: allocates the model's large tables, asking for large pages
// where the operating system grants them; and TableArray, a fixed-size table
// whose memory comes from it.
//
// Why large pages. A program addresses memory through pages, which the
// processor maps to where the memory really is with a page table, itself in
// memory. It caches a few thousand of those mappings. With 4 KiB pages, a
// model of 1 GiB is 262,144 pages, so a lookup at a random place in a table
// almost always misses that cache, and the processor walks the page table:
// more dependent reads from memory before the slot itself can be read. With
// 2 MiB pages the same model is 512 pages, and the walks mostly hit.
// Measured on enwik8: 14% faster compressing, 12% decompressing (stage 4's
// brief).
//
// Large pages change how an address is translated, never what it holds, so
// they cannot change a prediction. Failing to get them is never an error:
// the model is the same, only slower. Running out of memory is still
// std::bad_alloc.
//
// A block of kLargePageBytes or more may be rounded up to a whole number of
// large pages, where the platform code asks for them; a table still sees
// the size it asked for, and
// memory_bytes() still counts that. Every block, of any size, is aligned to
// kCacheLineBytes at least, so a structure of one cache line laid out at the
// start of the table never straddles two.
//
// Where large pages come from is platform code, in src/table_memory.cpp.
// Nothing else in src/model/ knows the platform.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

namespace cpress::model {

// The size of a large page on x86-64 and on ARM64 with 4 KiB base pages, and
// the smallest block worth one.
inline constexpr std::size_t kLargePageBytes = std::size_t{2} << 20;

inline constexpr std::size_t kCacheLineBytes = 64;

namespace detail {

// Returns a block of at least `bytes`, aligned to kCacheLineBytes, on large
// pages where the platform grants them if `bytes` is kLargePageBytes or
// more. Its contents are unspecified. Throws std::bad_alloc if there is no
// memory.
[[nodiscard]] void* allocate_table(std::size_t bytes);

// Frees a block from allocate_table(`bytes`), with the same `bytes`.
void deallocate_table(void* block, std::size_t bytes) noexcept;

// For tests: while `refused` is true, allocate_table() asks for no large
// pages, as if the platform refused them all, and frees what it returns as it
// frees any other block. Blocks allocated either way may be freed either way.
void refuse_large_pages(bool refused) noexcept;

}  // namespace detail

// For the benchmark: the bytes of every block this process has been granted
// on large pages so far, counted where the platform says so when it grants
// them. Only Windows does; Linux backs a block with large pages page by
// page, later, and counts them only in /proc/vmstat, so there this is 0.
[[nodiscard]] std::uint64_t large_page_bytes_granted() noexcept;

// Blocks for `count` elements of T, in the shape of a standard allocator's
// allocate() and deallocate(). It has no state.
template <typename T>
class TableAllocator {
public:
    static_assert(alignof(T) <= kCacheLineBytes);

    // Throws std::bad_alloc if there is no memory, or if `count` elements
    // would not fit in a std::size_t of bytes.
    [[nodiscard]] T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_alloc();
        return static_cast<T*>(detail::allocate_table(count * sizeof(T)));
    }

    void deallocate(T* pointer, std::size_t count) noexcept {
        detail::deallocate_table(pointer, count * sizeof(T));
    }
};

// A model table: a fixed number of elements, value-initialised, in memory
// from TableAllocator.
//
// Why not a std::vector with that allocator: a table never grows, and
// libstdc++ value-initialises a vector with any allocator but std::allocator
// one call per element. Optimised, that loop becomes one fill; unoptimised,
// as in the sanitizer build, it made building a level-10 Mix9 take 32 ms
// instead of 7, and the mutation test, which builds thousands, twice as
// long. Here the elements are value-initialised in one call, as std::vector
// with std::allocator does it.
//
// A table is neither copied nor moved: a model's tables stay where they were
// built, so no table is ever left empty by a move with its old size.
template <typename T>
class TableArray {
public:
    static_assert(std::is_trivially_default_constructible_v<T> &&
                      std::is_trivially_destructible_v<T>,
                  "a table of plain values: zeroed once, never destroyed one by one");

    // Allocates `size` elements, all zero. Throws std::bad_alloc if there is
    // no memory.
    explicit TableArray(std::size_t size)
        : elements_(TableAllocator<T>().allocate(size), Free{.size = size}) {
        std::uninitialized_value_construct_n(elements_.get(), size);
    }

    TableArray(const TableArray&) = delete;
    TableArray& operator=(const TableArray&) = delete;
    TableArray(TableArray&&) = delete;
    TableArray& operator=(TableArray&&) = delete;
    ~TableArray() = default;

    [[nodiscard]] T& operator[](std::size_t i) noexcept {
        assert(i < size());
        return elements_[i];
    }
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept {
        assert(i < size());
        return elements_[i];
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return elements_.get_deleter().size;
    }
    [[nodiscard]] T* data() noexcept {
        return elements_.get();
    }
    [[nodiscard]] const T* data() const noexcept {
        return elements_.get();
    }
    [[nodiscard]] T* begin() noexcept {
        return data();
    }
    [[nodiscard]] T* end() noexcept {
        return data() + size();
    }
    [[nodiscard]] const T* begin() const noexcept {
        return data();
    }
    [[nodiscard]] const T* end() const noexcept {
        return data() + size();
    }

private:
    // Returns the block to TableAllocator, which needs its size.
    struct Free {
        std::size_t size;
        void operator()(T* elements) const noexcept {
            TableAllocator<T>().deallocate(elements, size);
        }
    };

    std::unique_ptr<T[], Free> elements_;  // NOLINT(*-avoid-c-arrays): an owned block of `size`
};

}  // namespace cpress::model
