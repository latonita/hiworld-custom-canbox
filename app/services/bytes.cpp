/* ============================================================================
 * bytes.cpp -- see bytes.hpp. Ported from reference/decompile/app/util.c (Ghidra output).
 * ==========================================================================*/

#include "bytes.hpp"

namespace svc {
namespace bytes {

void copy(volatile std::uint8_t* dst, const volatile std::uint8_t* src,
          std::uint8_t len) noexcept
{
    for (std::uint8_t i = 0U; i < len; ++i) {
        dst[i] = src[i];
    }
}

bool differ(const volatile std::uint8_t* a, const volatile std::uint8_t* b,
            std::uint8_t len) noexcept
{
    for (std::uint8_t i = 0U; i < len; ++i) {
        if (a[i] != b[i]) {
            return true;
        }
    }
    return false;
}

std::uint8_t wrapIndex(int base, int delta, int hi, int lo, bool clamp) noexcept
{
    const int v = base + delta;

    if (v < lo) {
        return static_cast<std::uint8_t>(clamp ? lo : hi);
    }
    if (v > hi) {
        return static_cast<std::uint8_t>(clamp ? hi : lo);
    }
    return static_cast<std::uint8_t>(v);
}

}  // namespace bytes
}  // namespace svc
