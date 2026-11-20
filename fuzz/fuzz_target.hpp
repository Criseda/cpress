// The decoder's fuzz target, declared in the form libFuzzer calls
// (https://llvm.org/docs/LibFuzzer.html). Built with libFuzzer it is
// cpress-fuzz; built with fuzz/replay.cpp it is cpress-fuzz-replay, which
// runs it on files with any compiler.
#pragma once

#include <cstddef>
#include <cstdint>

namespace cpress::fuzz {

// What the target returns for an input it decoded or that was rejected.
inline constexpr int kTested = 0;

// What it returns for an input it skipped without decoding (see
// decompress_target.cpp). libFuzzer leaves such an input out of its corpus.
inline constexpr int kSkipped = -1;

}  // namespace cpress::fuzz

// Decodes the archive `data[0, size)`. Returns kTested or kSkipped, and
// aborts if the decoder breaks the rule for untrusted input. The pointer and
// size are libFuzzer's interface; the target views them as a span at once.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
