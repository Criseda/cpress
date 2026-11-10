// Order-0 bit model: predicts each bit from the bits of the current byte seen
// so far, and nothing else. It learns the byte-value histogram of the input.
//
// Memory: 256 x 16 bits = 512 bytes, eight cache lines; it stays in L1.
//
// Context: `context_` starts at 1 and becomes context_ * 2 + bit after each
// bit, returning to 1 after eight bits. It takes values 1 to 255, so a leading
// 1 marks how many bits are known and table_[0] is unused.
//
// Update rule: each entry is P(1) in 16 bits (`p16`) and moves a fixed
// fraction of the way towards the bit just seen, an exponentially weighted
// moving average: p += (target - p) >> kRate.
//
// Technique: the context is that of Matt Mahoney's fpaq0, which keeps a pair
// of bit counts in each one. Keeping a probability instead, and moving it by
// 1/32 of the error, is Ilia Muraviev's fpaq0p.
#pragma once

#include "model/predictor.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cpress::model {

class Order0 {
public:
    // Adaptation shift: each update moves an entry 1/2^kRate of the way to
    // its target. Larger is slower to adapt and more precise at rest.
    static constexpr int kRate = 5;

    // Bounds on a table entry. Entries start at kP16Half and only move by
    // (distance >> kRate), which is zero once the distance is below 2^kRate,
    // so an entry stops short of both ends: it never falls below
    // kEntryMin or rises above kEntryMax: [31, 65505] for a kRate of 5. Both
    // fit in 16 bits; kP16One itself would not.
    static constexpr int kP16Bits = 16;
    static constexpr std::uint32_t kP16One = 1U << kP16Bits;  // probability 1.0, never stored
    static constexpr std::uint32_t kP16Half = kP16One / 2;
    static constexpr std::uint32_t kEntryMin = (1U << kRate) - 1;
    static constexpr std::uint32_t kEntryMax = kP16One - kEntryMin;

    // A prediction is an entry with its low bits dropped. The entry bounds
    // must keep that inside [kProbMin, kProbMax]; a kRate for which they do
    // not (4 lets an entry reach 15, which shifts to 0) fails to compile.
    static constexpr int kP16ToP12Shift = kP16Bits - kProbBits;
    static_assert((kEntryMin >> kP16ToP12Shift) >= kProbMin, "kRate lets a prediction reach 0");
    static_assert((kEntryMax >> kP16ToP12Shift) <= kProbMax, "kRate lets a prediction reach 1");

    // p12 in [kProbMin, kProbMax], with no clamp: see the assertions above.
    [[nodiscard]] std::uint32_t predict() const noexcept {
        return std::uint32_t{table_[context_]} >> kP16ToP12Shift;
    }

    void update(int bit) noexcept {
        assert(bit == 0 || bit == 1);
        // Arithmetic in std::uint32_t: uint16_t operands would promote to
        // int, and every store back to the table is a visible cast.
        std::uint32_t p16 = table_[context_];
        if (bit != 0) {
            p16 += (kP16One - p16) >> kRate;
        } else {
            p16 -= p16 >> kRate;
        }
        assert(p16 >= kEntryMin && p16 <= kEntryMax);
        table_[context_] = static_cast<std::uint16_t>(p16);

        context_ = (context_ * 2) + static_cast<std::uint32_t>(bit);
        if (context_ >= kContexts) context_ = 1;
    }

    // The 16-bit probabilities, indexed by context. For tests.
    [[nodiscard]] std::span<const std::uint16_t> entries() const noexcept {
        return table_;
    }

private:
    static constexpr std::size_t kContexts = 256;

    static constexpr std::array<std::uint16_t, kContexts> make_table() noexcept {
        std::array<std::uint16_t, kContexts> table{};
        table.fill(static_cast<std::uint16_t>(kP16Half));
        return table;
    }

    std::array<std::uint16_t, kContexts> table_ = make_table();
    std::uint32_t context_ = 1;
};

static_assert(Predictor<Order0>);

}  // namespace cpress::model
