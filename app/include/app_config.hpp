/* ============================================================================
 * app_config.hpp -- the numbers that describe THIS box, in one place.
 *
 * Clock tree, tick cadences, protocol timings and board identity. Nothing here
 * programs a register; these are the facts the layers above agree on. A value
 * belongs in this file if changing it would change the behaviour of more than
 * one module, or if a reader would otherwise have to go find it in a comment.
 *
 * Every constant carries its evidence. This is a reverse-engineered rebuild:
 * a number without a source is a guess, and a guess here bricks a boot.
 * ==========================================================================*/

#ifndef APP_CONFIG_HPP
#define APP_CONFIG_HPP

#include <cstdint>

namespace cfg {

/* ===========================================================================
 * Clock tree -- set up by the BOOTLOADER, not by this app.
 *
 * HSE 16 MHz -> PLL -> SYSCLK 64 MHz. The app only sets the bus prescalers
 * (PPRE1 = PPRE2 = /4). Confirmed three independent ways:
 *   - the CAN bit-timing arithmetic (BRP = 2000 / baud_kbps at 8 tq/bit),
 *   - the TIM2 reload that yields a 1 ms tick,
 *   - the single initialised word in the stock scatter table
 *     (*0x200010DC = 64000000, i.e. CMSIS SystemCoreClock).
 * ========================================================================= */
inline constexpr std::uint32_t kSysclkHz = 64000000U;
inline constexpr std::uint32_t kPclk1Hz  = 16000000U;  /* APB1 = SYSCLK / 4  */
inline constexpr std::uint32_t kPclk2Hz  = 16000000U;  /* APB2 = SYSCLK / 4  */

/* TIM2 is clocked at 2 x PCLK1: the x2 multiplier applies whenever the APB1
 * prescaler is not /1, which it is not here. */
inline constexpr std::uint32_t kTim2ClkHz = 2U * kPclk1Hz;  /* 32 MHz */

/* ===========================================================================
 * The 1 ms time base -- TIM2
 *
 * 32 MHz / (PSC+1) / AR = 32e6 / 8 / 4000 = 1000 Hz.
 * ========================================================================= */
inline constexpr std::uint16_t kTim2Prescaler = 7U;     /* PSC, so divide by 8 */
inline constexpr std::uint16_t kTim2Period    = 4000U;  /* AR                  */
inline constexpr std::uint32_t kTickHz        = 1000U;

/* The divider chain the TIM2 ISR runs on top of the 1 ms tick. */
inline constexpr std::uint8_t kDiv10ms  = 10U;  /* 1 ms  -> 10 ms hooks  */
inline constexpr std::uint8_t kDiv100ms = 10U;  /* 10 ms -> 100 ms hooks */
inline constexpr std::uint8_t kDiv1s    = 10U;  /* 100 ms -> 1 s         */

/* ===========================================================================
 * Watchdog
 *
 * WWDG armed with a ~65.5 ms timeout. EVERY path through either main loop
 * must stay well under that; both loops kick it at the top.
 * ========================================================================= */
inline constexpr std::uint32_t kWwdgTimeoutMs = 65U;

/* ===========================================================================
 * Phase 1 -- CAN autobaud
 *
 * The box does not know its bus speed at boot. It waits for three CAN frames;
 * every 2048 ms with none it toggles the stored profile byte, rewrites the
 * parameter page and re-initialises CAN at the other bit rate. After 8 failed
 * attempts it takes the shutdown path -- on the NEXT 2048 ms tick, so the box
 * goes quiet at ~18.4 s from boot, not ~16.4 s. (The 8th retry itself lands at
 * 16.38 s; the give-up branch is only reached when the following tick finds the
 * counter already at 8.) Expect 18.4 s on a bench.
 *
 * On a bench with no bus attached this is expected behaviour, not a fault.
 * ========================================================================= */
inline constexpr std::uint32_t kAutobaudFramesNeeded = 3U;
inline constexpr std::uint8_t  kAutobaudMaxTries     = 8U;
inline constexpr std::uint32_t kAutobaudRetryMs      = 2048U;

/* The two bit rates, selected by parameter byte PARAM_PROFILE.
 * Profile 0 is THIS box (Jeep WK family, H1N4JPF20A / JPF2). */
inline constexpr std::uint8_t kProfileJeep83k = 0U;
inline constexpr std::uint8_t kProfileFca125k = 1U;

/* HOW MANY PROFILES THIS FIRMWARE CAN HAVE, and why it is not a free parameter.
 *
 * Three separate places assume exactly two, none of them obviously:
 *
 *   1. svc::can::setBaudByProfile() / applyBitrateB() are not dispatchers, they
 *      are stock NORMALISERS THAT WRITE BACK: an unrecognised profile byte is
 *      rewritten to 0 in the RAM mirror, and the next params::save() commits
 *      that to flash. A third profile would silently erase its own setting.
 *   2. The profile is published to the head unit in ONE BIT
 *      (ram::VehicleState::kFlagProfileSel, bit 7, written as profileActive<<7).
 *      Profile 2 truncates to 0 on the wire -- it would report itself as
 *      profile 0 to the Android side.
 *   3. The phase-1 autobaud is a FLIP-FLOP, not a cycle, and kAutobaudMaxTries
 *      below is a whole number of two-profile sweeps.
 *
 * Raising this is therefore a protocol change, not a configuration change.
 * static_asserts at all three sites reference this constant so the build stops
 * rather than the box corrupting itself. */
inline constexpr std::uint8_t kProfileCount = 2U;
static_assert(kProfileCount == 2U,
              "see the three sites listed above before changing this: the wire "
              "format has one bit for the profile, and the bitrate selectors "
              "write back an unrecognised value instead of rejecting it");
static_assert((kAutobaudMaxTries % kProfileCount) == 0U,
              "the autobaud budget must be a whole number of sweeps, or one "
              "profile gets tried more often than the other and the box "
              "develops a bench-dependent preference");
inline constexpr int kCanBaudJeepKbps = 83;   /* really 83.333; the stock
                                               * bit-timing divisor truncates */
inline constexpr int kCanBaudFcaKbps  = 125;

/* Bus-presence debounce, in decoder passes, reloaded whenever ACC is off. */
inline constexpr std::uint32_t kBusPresentDebounce = 800U;

/* The CAN bus-presence state machine's states (ram::AppRam::canBusStatus).
 * State 3 is entered once by the main loop when the bus answers; it emits the
 * 0x416 "hello" frame and drops to 2. */
inline constexpr std::uint8_t kBusDown     = 0U;
inline constexpr std::uint8_t kBusSettling = 1U;
inline constexpr std::uint8_t kBusRunning  = 2U;
inline constexpr std::uint8_t kBusHello    = 3U;
inline constexpr std::uint8_t kBusLost     = 4U;

/* ===========================================================================
 * Phase 2 -- the steady superloop cadences
 *
 * Driven by the one-shot flags the 1 ms hook raises. The phases are staggered
 * (1, 3, 7, 15, ...) so the 4/8/16/32 ms jobs never land on the same
 * millisecond, which is what keeps the worst-case pass inside the watchdog.
 * ========================================================================= */
inline constexpr std::uint32_t kFastTaskMs = 4U;     /* status, CAN TX drain */
inline constexpr std::uint32_t kMedTaskMs  = 8U;     /* decode, UART, GPIO   */
inline constexpr std::uint32_t kSlowTaskMs = 1024U;  /* parameter page save  */

/* ===========================================================================
 * Head-unit link -- USART2, PA2 = TX, PA3 = RX
 *
 * Framing (docs/notes/hiworld-frame-format.md):
 *     AA 55 | LEN | CMD | payload[LEN] | CKS       wire length = LEN + 5
 * LEN counts the payload only. CMD is at offset 3, AFTER the length byte --
 * that is what distinguishes Hiworld from the sibling "Raise" protocol.
 * CKS = (sum of every byte including the preamble) & 0xFF, computed by the
 * firmware in the identical form (LEN + CMD + sum(payload) - 1) & 0xFF
 * because 0xAA + 0x55 = 0xFF == -1 (mod 256).
 * There is NO escaping: a payload byte may legitimately be 0xAA or 0x55, and
 * resynchronisation relies on the LEN/checksum check failing.
 * ========================================================================= */
inline constexpr std::uint32_t kUartBaud      = 38400U;
inline constexpr std::uint8_t  kUartPreamble0 = 0xAAU;
inline constexpr std::uint8_t  kUartPreamble1 = 0x55U;
inline constexpr std::uint8_t  kUartMaxPayload = 0x37U;  /* LEN >= 0x38 rejected */
inline constexpr std::uint8_t  kUartFrameOverhead = 5U;  /* AA 55 LEN CMD .. CKS */

/* ===========================================================================
 * Box identity
 *
 * The 17-byte version string the app copies into RAM at init. The flashing
 * tool reads it back over SWD (SRAM is RDP-exempt) to prove the new image
 * booted -- see tools/iap_upload.py --verify and app/REBUILD.md section 6.
 * ========================================================================= */
inline constexpr char kVersionString[] = "H1N4JPF20A-220524";
inline constexpr std::uint8_t kVersionLength = 17U;
static_assert(sizeof(kVersionString) == kVersionLength + 1U,
              "the version string is exactly 17 bytes on the wire");

}  // namespace cfg

#endif /* APP_CONFIG_HPP */
