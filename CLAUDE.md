# cpress

A lossless compressor built on context mixing, written from scratch in C++20.
It is a portfolio project: the quality of the code matters as much as the
compression ratio.

Read `docs/style.md` before writing or reviewing any code, and the brief for
the current stage before implementing anything. Both are binding.

| Document | Contents |
|---|---|
| `README.md` | Build, usage, archive format, roadmap summary |
| `docs/idea.md` | Why the project exists; goals and non-goals |
| `docs/roadmap.md` | Every stage, with exit criteria |
| `docs/stage-N.md` | The brief for stage N |
| `docs/style.md` | Code style and review checklist |

## Where things stand

Stage 0 is done. Stage 1 is next; its brief is `docs/stage-1.md`.
Update this line and the README roadmap when a stage is tagged.
Stage targets are sizes on `corpus/enwik8` (100 MB); see `corpus/README.md`.

## Commands

```
cmake -S . -B build -DCPRESS_WERROR=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build/Release/cpress-bench corpus/enwik8        # build/cpress-bench off Windows
```

- `-DCPRESS_SANITIZE=ON` adds ASan and UBSan (GCC and Clang only).
- On Windows, clang-format and clang-tidy ship with Visual Studio under
  `VC/Tools/Llvm/x64/bin/`.
- MSBuild fails if the build path exceeds 260 characters; keep paths short.

## Hard rules

1. Compressor and decompressor share one predictor. Never duplicate model
   logic between the two directions.
2. No floating point in model or coder code. Decoding must be bit-identical
   across compilers and platforms.
3. The per-bit loop does not allocate, throw, or make virtual calls.
4. Everything read from an archive is untrusted input.
5. Once a stage is tagged, its codec ids and their output are permanent.
   Changed predictions after that mean a new codec id.
6. Standard library only. No new dependencies.

## Definition of done for a change

- Builds with no warnings on MSVC and GCC or Clang.
- `ctest` passes; new components have their own tests.
- Formatted with clang-format.
- If it claims a size or speed result, the `cpress-bench` numbers are in the
  commit message.
- README and `cpress.hpp` updated if the format or roadmap changed.

## Working style

- Keep changes small and reviewable: one idea per commit.
- Do not start the next roadmap stage, restructure existing code, or change
  the archive format unless asked.
- When a design choice has a real trade-off, state it and the recommendation
  before writing code.
- No AI attribution in commits, pull requests or source files.
