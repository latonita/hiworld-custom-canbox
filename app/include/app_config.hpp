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
 * WHICH HEAD-UNIT PROTOCOL THIS IMAGE SPEAKS -- a BUILD-TIME choice.
 *
 * The box can present itself to the Android side as either of two canbox
 * families. This is not a runtime setting and deliberately so: see the note
 * on kProtocolIsRuntimeSelectable below.
 *
 *   kHiworldJpf2 (default) -- what this box IS. Hiworld JPF2, protocol id
 *       3009001 on the Android side, "AA 55" framing. A build with this
 *       selected must stay byte-for-byte equivalent on the wire to the stock
 *       firmware; everything protocol-specific below is compiled out.
 *
 *   kRaiseBenz -- pretend to be a Raise Mercedes-Benz box, protocol ids
 *       1034025/27/28, "0x2E" framing. The Android side must be switched to
 *       the matching protocol id in Canset for this to mean anything. Chosen
 *       because the Benz plugin has by far the richest LIVE GAUGE set of any
 *       protocol in the corpus -- see ../richest-protocol-comparison.md.
 *
 * Select with  python tools/build.py --protocol benz  (default: jeep), which
 * passes -DCANBOX_PROTOCOL_BENZ. Only the version string below tests that
 * macro directly (a string literal cannot survive a ternary); everything
 * else tests cfg::kProtocol / cfg::kIsBenz, so both variants always COMPILE
 * and a typo in one branch cannot hide behind the preprocessor.
 *
 * WHY NOT A RUNTIME SETTING. The parameter page would have to survive an
 * update, and the only way the head unit can reach a box whose dialect it
 * does not speak is the IAP escape hatch below -- i.e. a wrong stored value
 * is recoverable only by reflashing, which is exactly the failure mode a
 * runtime setting is supposed to avoid. Two images, one of which is the
 * untouched stock-equivalent, is the safer shape.
 * ========================================================================= */
enum class Protocol : std::uint8_t {
    kHiworldJpf2 = 0U,
    kRaiseBenz   = 1U,
};

#if defined(CANBOX_PROTOCOL_BENZ)
inline constexpr Protocol kProtocol = Protocol::kRaiseBenz;
#else
inline constexpr Protocol kProtocol = Protocol::kHiworldJpf2;
#endif

inline constexpr bool kIsBenz   = (kProtocol == Protocol::kRaiseBenz);
inline constexpr bool kIsHiworld = !kIsBenz;

/* ===========================================================================
 * Head-unit link -- USART2, PA2 = TX, PA3 = RX
 *
 * BOTH dialects are 38400 8N1 on the same two pins, which is what makes this
 * a build option at all rather than a board change.
 *
 * Hiworld (docs/notes/hiworld-frame-format.md):
 *     AA 55 | LEN | CMD | payload[LEN] | CKS       wire length = LEN + 5
 * LEN counts the payload only. CMD is at offset 3, AFTER the length byte --
 * that is what distinguishes Hiworld from the sibling "Raise" protocol.
 * CKS = (sum of every byte including the preamble) & 0xFF, computed by the
 * firmware in the identical form (LEN + CMD + sum(payload) - 1) & 0xFF
 * because 0xAA + 0x55 = 0xFF == -1 (mod 256).
 *
 * Raise / Benz:
 *     2E | CMD | LEN | payload[LEN] | CKS          wire length = LEN + 4
 * One sync byte, and CMD comes BEFORE LEN -- the opposite order. The checksum
 * is a one's complement, not a sum: CKS = ~(CMD + LEN + sum(payload)) & 0xFF.
 *
 * Neither dialect escapes anything: a payload byte may legitimately be 0xAA,
 * 0x55 or 0x2E, and resynchronisation relies on the LEN bound and the
 * checksum check failing.
 * ========================================================================= */
inline constexpr std::uint32_t kUartBaud = 38400U;

/* Hiworld framing. These keep their names because the stock-equivalent code
 * path uses them unchanged. */
inline constexpr std::uint8_t kUartPreamble0 = 0xAAU;
inline constexpr std::uint8_t kUartPreamble1 = 0x55U;

/* Raise/Benz framing. */
inline constexpr std::uint8_t kRaiseSync = 0x2EU;

/* Per-dialect geometry, folded at compile time. `kUartFrameOverhead` is the
 * number of non-payload bytes on the wire: AA 55 LEN CMD .. CKS = 5 for
 * Hiworld, 2E CMD LEN .. CKS = 4 for Raise. */
inline constexpr std::uint8_t kUartFrameOverhead = kIsBenz ? 4U : 5U;

/* LEN >= 0x38 is rejected outright. Stock bound; kept for both dialects
 * because it is the RX frame buffer's size, not a protocol rule. */
inline constexpr std::uint8_t kUartMaxPayload = 0x37U;

/* How many messages the box transmits. FOURTEEN for Hiworld -- the six stock
 * slots (0x72 0xA6 0x73 0xF0 0x12 0x32) plus the eight of the custom telemetry
 * block (0xB0 0xB2 0xB3 0xB4 0xB5 0xB8 0xBB 0xBD, docs/CUSTOM-TX-SPEC.md);
 * five for Benz (0x08 0x07 and three pages of 0x41). The round-robin, the
 * slot-state array and the shadow block are all sized from this.
 *
 * THE BUDGET IS NOT FREE. One frame leaves every 48 ms, so N slots mean
 * N x 48 ms worst case between two sends of the same signal and an idle
 * keep-alive of N x 720 ms. At 14: 672 ms and 10.1 s, still inside the
 * consumer's 15 s staleness check with six slots of headroom. Do not add slots
 * without redoing that arithmetic -- 20 slots is 14.4 s and 21 crosses. */
inline constexpr std::uint8_t kTxSlotCount = kIsBenz ? 5U : 14U;

/* A Raise LEN is read by the head unit into a SIGNED byte and counted down to
 * zero, so LEN = 0 (and anything >= 0x80) makes its parser consume bytes
 * until its own 120-byte frame cap trips -- swallowing whatever follows.
 * Every Benz frame this firmware builds must therefore have 1 <= LEN <= 117. */
inline constexpr std::uint8_t kRaiseMinPayload = 1U;
inline constexpr std::uint8_t kRaiseMaxPayload = 117U;

/* ===========================================================================
 * The IAP escape hatch -- why a Benz build still answers a Hiworld frame.
 *
 * The head unit's "Update box" button does NOT hand the image to the running
 * app. It asks the app to reboot, and the 8 KB bootloader does the transfer:
 * BoxUpgradeActivity writes AA 55 02 E0 00 00 E1 once a second, the app's RX
 * dispatcher sees CMD 0xE0 and forces a watchdog reset into the loader
 * (docs/hiworld-iap-update-sequence.md section 1).
 *
 * A box that spoke ONLY the Raise dialect would not recognise that frame and
 * could never be updated from the radio again -- only on the bench, over the
 * FTDI/nRST rig. So the Hiworld reset frame is matched UNCONDITIONALLY, in
 * every build, by a minimal side-branch of the RX parser. It costs a handful
 * of bytes and it is the difference between a recoverable box and one that
 * needs the lid off.
 *
 * This is a LITERAL match on the whole frame, not a general Hiworld parser:
 * the seven bytes below and nothing else. 0xAA cannot begin a Raise frame
 * (which syncs on 0x2E), so the two state machines cannot be confused.
 * ========================================================================= */
inline constexpr std::uint8_t kIapResetFrame[] = {
    0xAAU, 0x55U, 0x02U, 0xE0U, 0x00U, 0x00U, 0xE1U
};
inline constexpr std::uint8_t kIapResetFrameLen =
    static_cast<std::uint8_t>(sizeof(kIapResetFrame));
static_assert(kIapResetFrameLen == 7U,
              "the reset frame is AA 55 | LEN=02 | CMD=E0 | 00 00 | CKS=E1");
/* The checksum byte must agree with the Hiworld rule, or this table is wrong
 * and the box silently stops being updatable from the radio. */
static_assert(kIapResetFrame[6] ==
                  static_cast<std::uint8_t>(kIapResetFrame[2] + kIapResetFrame[3] +
                                            kIapResetFrame[4] + kIapResetFrame[5] - 1U),
              "CKS = (LEN + CMD + sum(payload) - 1) & 0xFF");

/* How long the reset path may spin waiting for the transmitter to finish.
 *
 * The bootloader greets the radio with 'U' about 120 ms after reset and gives
 * up at about 788 ms, so the handshake window is ~0.67 s -- and the radio
 * tests the FIRST byte of an untrimmed 64-byte read against 'U'. A byte of
 * ours still in the shift register when the watchdog fires lands ahead of
 * that greeting and costs the whole attempt.
 *
 * One byte at 38400 8N1 is ~260 us = ~16.6 k cycles at 64 MHz. The loop is
 * three or four cycles, so 50 000 iterations is roughly 2.5-3 ms: about ten
 * byte-times of margin, and still an order of magnitude inside the ~65 ms
 * window watchdog, which is the bound that actually matters -- overshoot it
 * and a deliberate reboot becomes an unexplained watchdog reset. */
inline constexpr std::uint32_t kUartTxDrainSpins = 50000U;

/* ===========================================================================
 * Box identity
 *
 * The 17-byte version string the app copies into RAM at init. The flashing
 * tool reads it back over SWD (SRAM is RDP-exempt) to prove the new image
 * booted -- see tools/iap_upload.py --verify and app/REBUILD.md section 6.
 *
 * The string also names the PROTOCOL this image was built for, so a flashed
 * box can be identified without guessing: JPF2 = the stock Hiworld dialect,
 * BNZ1 = the Raise/Benz emulation. Both are 17 bytes, because the length is
 * the wire format of message 0xF0 and is asserted below.
 *
 * This is the ONE place that tests the build macro directly rather than
 * cfg::kIsBenz: a string literal cannot be chosen by a ternary without
 * decaying to a pointer and losing the sizeof() that proves the length.
 * ========================================================================= */
 /* Stock was "H1N4JPF20A-220524". The shape is H1N4 | JPF2 | 0 | A, then a
  * DDMMYY date:
  *   H1N4  the BSP / hardware family
  *   JPF2  the car profile -- the SAME token the Android side knows as
  *         protocol id 3009001. This identifies the protocol, not the build.
  *   0     the profile's sub-revision
  *   A     vendor image; this rebuild uses L
  *
  * SO ONLY THE DATE MOVES BETWEEN OUR BUILDS. Bumping "JPF20" to "JPF21" would
  * not read as a revision bump -- it would change the profile token itself,
  * because the digit belongs to JPF2's sub-revision and not to us. The custom
  * 0xB0-0xB8 telemetry block is wire-visible but it does not change the
  * profile, so the protocol identity stays put and the date tells the two
  * images apart: -171026 speaks the six stock slots only, -211026 adds the
  * telemetry block. */
 //                                      "H1N4JPF20A-220524"
#if defined(CANBOX_PROTOCOL_BENZ)
inline constexpr char kVersionString[] = "H1N4BNZ10L-171026";
#else
inline constexpr char kVersionString[] = "H1N4JPF20L-211026";
#endif
inline constexpr std::uint8_t kVersionLength = 17U;
static_assert(sizeof(kVersionString) == kVersionLength + 1U,
              "the version string is exactly 17 bytes on the wire");

}  // namespace cfg

#endif /* APP_CONFIG_HPP */
