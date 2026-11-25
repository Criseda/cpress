// CRC-32: the checksum an archive keeps of its original data, so that
// decompress() can tell when what it decoded is not what was compressed.
//
// How it works. Read the data as one long binary number and divide it by a
// fixed 33-bit number, the polynomial; the 32-bit remainder is the CRC. The
// division is long division in which subtraction is XOR, so nothing ever
// carries. Each step looks at the lowest bit of the remainder: if it is 1,
// the polynomial is XORed in; either way the remainder shifts right by one.
// This is the reflected form, which takes each byte from its lowest bit up,
// so the remainder shifts right rather than left and the polynomial is
// written with its bits reversed.
//
// Eight steps depend only on the byte they consume and the remainder's low
// byte, so they are done once for all 256 values when this compiles, and the
// running code does one table lookup per byte instead of eight steps.
//
// Technique: this is the CRC-32 of zip, gzip, PNG and Ethernet (IEEE 802.3),
// with the byte-at-a-time table of Sarwate, "Computation of cyclic
// redundancy checks via table look-up", Communications of the ACM 31(8),
// 1988.
#include "cpress/cpress.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cpress {
namespace {

// The IEEE 802.3 polynomial, 0x04C11DB7 with its implied top bit, written
// bit-reversed for the reflected form.
constexpr std::uint32_t kPolynomial = 0xEDB88320u;

// The standard starts the remainder at all ones and inverts the result, so
// that zero bytes at the start of the data still change the CRC.
constexpr std::uint32_t kInvertAll = 0xFFFFFFFFu;

constexpr std::size_t kByteValues = 256;
constexpr std::uint32_t kLowByteMask = 0xFFu;

// One step of the division: consumes the remainder's lowest bit.
constexpr std::uint32_t divide_one_bit(std::uint32_t remainder) noexcept {
    const bool low_bit_set = (remainder & 1u) != 0;
    remainder >>= 1;
    if (low_bit_set) remainder ^= kPolynomial;
    return remainder;
}

// kByteSteps[b] is what eight steps make of a remainder whose low byte is b
// and whose other bytes are zero.
constexpr std::array<std::uint32_t, kByteValues> make_byte_steps() noexcept {
    std::array<std::uint32_t, kByteValues> steps{};
    for (std::uint32_t byte = 0; byte < kByteValues; ++byte) {
        std::uint32_t remainder = byte;
        for (int bit = 0; bit < 8; ++bit) {
            remainder = divide_one_bit(remainder);
        }
        steps[byte] = remainder;
    }
    return steps;
}

constexpr auto kByteSteps = make_byte_steps();

}  // namespace

std::uint32_t crc32(ByteView data, std::uint32_t previous) noexcept {
    // `previous` is a finished CRC, so it is inverted. Inverting it again
    // gives back the remainder it was made from, and the division carries
    // on from there.
    std::uint32_t remainder = previous ^ kInvertAll;
    for (const std::uint8_t byte : data) {
        // The next byte joins the remainder's low byte; eight steps of the
        // division then come from the table, and the remainder's upper
        // three bytes move down to meet them.
        const std::uint32_t low_byte = (remainder ^ byte) & kLowByteMask;
        remainder = kByteSteps[low_byte] ^ (remainder >> 8);
    }
    return remainder ^ kInvertAll;
}

}  // namespace cpress
