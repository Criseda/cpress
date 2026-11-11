# cpress

A lossless compressor built on context mixing, written from scratch in C++20.
It is a portfolio project: the quality of the code matters as much as the
compression ratio.

The code is written for a human reader first. Priorities, in order: correct,
clear, fast. Aim for C++20 a careful engineer would enjoy reading: small
functions, honest names, invariants stated and checked, nothing clever
without a reason. Code that works but is hard to follow is not done.

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

Stages 0 to 2 are done and tagged (`v0.1`, `v0.2`). Stage 3 is next; its
brief, `docs/stage-3.md`, is not written yet.
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
- Reads cleanly from top to bottom, and every comment is true of the code as
  it now stands. Run the review checklist in `docs/style.md`.
- If it claims a size or speed result, the `cpress-bench` numbers are in the
  commit message.
- README and `cpress.hpp` updated if the format or roadmap changed.

## Working style

- A pull request lands as a few commits, ideally one. Do not split work into
  many small commits. The project is young, so large diffs are normal; do not
  spend effort on commit granularity.
- Do not start the next roadmap stage, restructure existing code, or change
  the archive format unless asked.
- When a design choice has a real trade-off, state it and the recommendation
  before writing code.
- If a brief turns out to be wrong, amend it in the same commit as the code
  that departs from it, and say why.
- No AI attribution in commits, pull requests or source files.
