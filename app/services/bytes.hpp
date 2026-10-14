/* ============================================================================
 * bytes.hpp -- the four leaf helpers the rest of the firmware is built from.
 *
 * Ported from reference/decompile/app/util.c (Ghidra output). Every one is
 * a real out-of-line function in the stock image, not an idiom the compiler
 * invented, so they are reproduced as functions rather than inlined away --
 * the CAN error decoder alone calls bitTest about twenty times in a row.
 *
 * Pure: no state, no hardware. Fully exercised by tests/host/test_bytes.cpp.
 * ==========================================================================*/

#ifndef BYTES_HPP
#define BYTES_HPP

#include <cstdint>

namespace svc {
namespace bytes {

/* --- 0x08008598 memcpy_bytes ---
 * Byte-at-a-time copy. Both sides are volatile because every real call site
 * moves data in or out of the fixed-address RAM block, where the compiler must
 * not coalesce or reorder the accesses.
 *
 * `len` is 8-bit on purpose. The stock loop counter is masked with 0xFF on
 * every increment, so a length of 256 or more would spin forever in the
 * original; the narrow type makes that unrepresentable instead of latent. */
void copy(volatile std::uint8_t* dst, const volatile std::uint8_t* src,
          std::uint8_t len) noexcept;

/* --- 0x08008574 util_bytes_differ ---
 * true as soon as any of the first `len` bytes differs. Note the polarity: this
 * is NOT memcmp's convention. Callers use the result directly as "something
 * changed", which is what drives the UART round-robin's send decision. */
[[nodiscard]] bool differ(const volatile std::uint8_t* a,
                          const volatile std::uint8_t* b,
                          std::uint8_t len) noexcept;

/* --- 0x080085C4 util_wrap_index ---
 * base + delta constrained to [lo, hi].
 *
 * `clamp` replaces the stock `wrap_mode` argument, whose sense is the opposite
 * of what its name suggests:
 *     stock wrap_mode == 0  ->  WRAP   (below lo -> hi, above hi -> lo)  -> clamp = false
 *     stock wrap_mode != 0  ->  CLAMP  (below lo -> lo, above hi -> hi)  -> clamp = true
 * Both live call sites pass the wrapping form: the soft clock rolls hours and
 * minutes, and the UART TX scheduler advances its slot index.
 *
 * The arithmetic is done in int because delta is -1 in the clock case; only the
 * result is narrowed, exactly as the stock code does. */
[[nodiscard]] std::uint8_t wrapIndex(int base, int delta, int hi, int lo,
                                     bool clamp) noexcept;

/* --- 0x0800413C util_bit_test --- */
[[nodiscard]] constexpr bool bitTest(std::uint32_t value, std::uint32_t mask) noexcept
{
    return (value & mask) != 0U;
}

/* Set or clear `mask` in `target`. The stock code writes this as
 * x = (x & ~m) | (v << n) everywhere, which ARM emits as a single BFI; this is
 * the same operation with the shift accounted for by the caller's mask. */
inline void bitAssign(volatile std::uint8_t& target, std::uint8_t mask,
                      bool value) noexcept
{
    const std::uint8_t cur = target;
    target = value ? static_cast<std::uint8_t>(cur | mask)
                   : static_cast<std::uint8_t>(cur & static_cast<std::uint8_t>(~mask));
}

}  // namespace bytes
}  // namespace svc

#endif /* BYTES_HPP */
