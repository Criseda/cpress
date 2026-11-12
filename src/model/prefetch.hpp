// prefetch(): asks the processor to start loading a cache line that will be
// read soon, so that the read finds it in the cache instead of waiting on
// memory.
//
// A prefetch is a hint and nothing else. It reads nothing the program sees
// and writes nothing; the processor may ignore it. Fetching a line that is
// never used costs a little memory traffic, never a wrong value, so a
// prefetch cannot change what a model predicts.
//
// This is the one preprocessor conditional in model code: each compiler
// spells the hint its own way, and a compiler with neither spelling gets a
// function that does nothing, which is correct, only slower.
#pragma once

#if defined(__GNUC__) || defined(__clang__)

namespace cpress::model {

// Into every level of the cache, for reading.
//
// GCC counts __builtin_prefetch as having no effect at all, so a function
// that only prefetches, such as Mix9::prefetch_after(), looks to it like a
// function that does nothing; where it is not inlined, GCC 13 deleted every
// call to it, and with them every prefetch. The empty volatile asm is an
// effect the compiler must keep, which keeps the calls. It emits no
// instruction.
inline void prefetch(const void* address) noexcept {
    __builtin_prefetch(address);
    asm volatile("");
}

}  // namespace cpress::model

#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))

#include <xmmintrin.h>

namespace cpress::model {

// Into every level of the cache (_MM_HINT_T0), for reading.
inline void prefetch(const void* address) noexcept {
    _mm_prefetch(static_cast<const char*>(address), _MM_HINT_T0);
}

}  // namespace cpress::model

#else

namespace cpress::model {

inline void prefetch(const void* /*address*/) noexcept {}

}  // namespace cpress::model

#endif
