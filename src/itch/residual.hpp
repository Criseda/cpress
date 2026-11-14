// Residuals: what is coded for a field in place of its value. A field is
// rarely coded as it is. The model first says what it expects the field to
// be, and the number coded is how far the field is from that: zero when the
// model was right, small when it was close.
//
// Three kinds, each a pair of functions that are inverses for every value,
// every expectation and every width of 1 to 8 bytes:
//
//   wrapping   u = (value - expected) mod 2^(8 * width)
//   signed     d = that, read as a two's complement integer of `width` bytes;
//              u = zigzag(d), which sends 0, -1, 1, -2, ... to 0, 1, 2, 3, ...
//   stepped    d as for signed; q = floor(d / step) and r = d - q * step,
//              in [0, step). Coded: zigzag(q), then r.
//
// Wrapping is for a value that only grows, or that is expected to be exact.
// Signed is for one that moves either way. Stepped is for one that moves in
// steps: order reference numbers by 4, prices by a cent. Its remainder is
// almost always 0 and costs almost nothing.
//
// Nothing here assumes the expectation is good. A wrong one gives a large
// number, never a wrong value. All arithmetic is unsigned and wraps, so no
// input, from a file or from a crafted archive, can overflow.
#pragma once

#include <cassert>
#include <cstdint>

namespace cpress::itch {

namespace detail {

// The values of a `width`-byte field: all ones in its low 8 * width bits.
[[nodiscard]] constexpr std::uint64_t field_mask(int width) noexcept {
    assert(width >= 1 && width <= 8);
    return width == 8 ? ~std::uint64_t{0} : (std::uint64_t{1} << (8 * width)) - 1;
}

// `d`, a two's complement integer of `width` bytes, in 64 bits: the bits
// above the field are copies of its sign bit.
[[nodiscard]] constexpr std::uint64_t sign_extend(std::uint64_t d, int width) noexcept {
    const std::uint64_t mask = field_mask(width);
    const std::uint64_t sign = (mask >> 1) + 1;  // the field's top bit
    return (d & sign) != 0 ? d | ~mask : d & mask;
}

// 0, -1, 1, -2, 2, ... to 0, 1, 2, 3, 4, ...; `d` is two's complement in 64
// bits. And its inverse.
[[nodiscard]] constexpr std::uint64_t zigzag(std::uint64_t d) noexcept {
    return (d << 1) ^ (std::uint64_t{0} - (d >> 63));
}
[[nodiscard]] constexpr std::uint64_t unzigzag(std::uint64_t u) noexcept {
    return (u >> 1) ^ (std::uint64_t{0} - (u & 1));
}

}  // namespace detail

[[nodiscard]] constexpr std::uint64_t wrapping_residual(std::uint64_t value, std::uint64_t expected,
                                                        int width) noexcept {
    return (value - expected) & detail::field_mask(width);
}

[[nodiscard]] constexpr std::uint64_t wrapping_value(std::uint64_t u, std::uint64_t expected,
                                                     int width) noexcept {
    return (expected + u) & detail::field_mask(width);
}

// The result fits the field: a difference of `width` bytes is in
// [-2^(8w-1), 2^(8w-1)), and zigzag sends that range onto [0, 2^(8w)).
[[nodiscard]] constexpr std::uint64_t signed_residual(std::uint64_t value, std::uint64_t expected,
                                                      int width) noexcept {
    const std::uint64_t d = detail::sign_extend(value - expected, width);
    return detail::zigzag(d) & detail::field_mask(width);
}

[[nodiscard]] constexpr std::uint64_t signed_value(std::uint64_t u, std::uint64_t expected,
                                                   int width) noexcept {
    return (expected + detail::unzigzag(u)) & detail::field_mask(width);
}

// A stepped residual: the quotient, zigzagged, and the remainder.
struct Stepped {
    std::uint64_t quotient;   // zigzag(floor(d / step)); fits the field
    std::uint32_t remainder;  // d - floor(d / step) * step, in [0, step)

    [[nodiscard]] constexpr bool operator==(const Stepped&) const noexcept = default;
};

[[nodiscard]] constexpr Stepped stepped_residual(std::uint64_t value, std::uint64_t expected,
                                                 int width, std::uint32_t step) noexcept {
    assert(step >= 2);
    const auto d = static_cast<std::int64_t>(detail::sign_extend(value - expected, width));
    // C++ division rounds towards zero; floor is one less when that left a
    // negative remainder. Neither line can overflow: |d / step| is at most
    // 2^62, because step is at least 2.
    const std::int64_t divisor = step;
    std::int64_t quotient = d / divisor;
    std::int64_t remainder = d % divisor;
    if (remainder < 0) {
        remainder += divisor;
        quotient -= 1;
    }
    return {.quotient =
                detail::zigzag(static_cast<std::uint64_t>(quotient)) & detail::field_mask(width),
            .remainder = static_cast<std::uint32_t>(remainder)};
}

// Accepts any quotient and remainder, as a decoder must: the product wraps.
[[nodiscard]] constexpr std::uint64_t stepped_value(const Stepped& residual, std::uint64_t expected,
                                                    int width, std::uint32_t step) noexcept {
    const std::uint64_t d = (detail::unzigzag(residual.quotient) * step) + residual.remainder;
    return (expected + d) & detail::field_mask(width);
}

}  // namespace cpress::itch
