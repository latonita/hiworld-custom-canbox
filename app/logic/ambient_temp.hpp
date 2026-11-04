/* 0x014 b4:b5 -- outside air temperature, shared by BOTH protocol dialects.
 * Verified on the car: u16 big-endian, degC = raw * 0.01 - 40; 0xFFFF means
 * unavailable. One decode so the dialects cannot disagree. */
#ifndef APP_LOGIC_AMBIENT_TEMP_HPP
#define APP_LOGIC_AMBIENT_TEMP_HPP

#include <cstdint>

namespace logic {

inline constexpr std::int32_t  kAmbientOffsetCenti = 4000;    /* -40.00 degC */
inline constexpr std::int32_t  kAmbientCentiPerDeci = 10;
inline constexpr std::uint16_t kAmbientRawNA       = 0xFFFFU; /* waking / not ready */
inline constexpr std::uint8_t  kAmbientMinDlc      = 6U;      /* b5 must exist */

/* The raw u16 from d[4]:d[5]. */
inline std::uint16_t ambientRaw(const volatile std::uint8_t* d) noexcept
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(d[4]) << 8) | d[5]);
}

/* True when the frame carries a usable reading (DLC >= 6 and not 0xFFFF);
 * otherwise the caller skips it and keeps its previous value. */
constexpr bool ambientUsable(std::uint16_t raw, std::uint8_t dlc) noexcept
{
    return dlc >= kAmbientMinDlc && raw != kAmbientRawNA;
}

/* raw -> signed decidegrees, nearest tenth, an exact half AWAY from zero so
 * negatives round symmetrically (1955 -> 196, -5 -> -1, -1955 -> -196). */
constexpr std::int16_t ambientDeciC(std::uint16_t raw) noexcept
{
    const std::int32_t centi = static_cast<std::int32_t>(raw) - kAmbientOffsetCenti;
    const std::int32_t mag   = (centi < 0) ? -centi : centi;
    const std::int32_t deci  = (mag + kAmbientCentiPerDeci / 2) / kAmbientCentiPerDeci;
    return static_cast<std::int16_t>((centi < 0) ? -deci : deci);
}

}  /* namespace logic */

#endif
