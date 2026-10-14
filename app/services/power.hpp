/* ============================================================================
 * power.hpp -- the fault path and the give-up path.
 *
 * Two ways this firmware stops doing its job, both deliberate:
 *
 *   fatalErrorReset()    something went wrong. Record a code, force a watchdog
 *                        reset, land back in the IAP bootloader.
 *   shutdownDeepSleep()  phase 1 found no CAN bus after ~18.4 s. Put the
 *                        transceiver and every peripheral to sleep and WFI.
 *
 * Ported from reference/decompile/app/sys.c (0x08002E64), reference/decompile/app/misc.c (0x080032F6) and
 * reference/decompile/app/can.c (0x08002E18, misnamed there).
 * ==========================================================================*/

#ifndef POWER_HPP
#define POWER_HPP

#include <cstdint>

/* ---------------------------------------------------------------------------
 * fatal_error_reset -- 0x08002E64
 *
 * THE NAME AND LINKAGE ARE AN ABI. app/startup/crt0.S defines 54 default trap
 * handlers, each of which is literally
 *
 *      movs r0, #<code>
 *      b    fatal_error_reset
 *
 * and app/startup/startup.c calls it if main_loop ever returns. A C++-mangled
 * name links cleanly and silently leaves those branches unresolved-to-the-wrong
 * thing, so it must stay `extern "C"`, must take a single uint8_t, and must
 * never return.
 *
 * Codes are the stock ones: 100..164 assigned per interrupt veneer slot, so the
 * byte at ram::app().fatalErrorCode tells you which vector fired. Codes 217,
 * 229, 232, 237 and 247 are NOT stock -- crt0.S uses them for the five slots
 * that should have a real handler, so seeing one means a handler name did not
 * match. See docs/ARCHITECTURE.md section 5.
 * ------------------------------------------------------------------------- */
extern "C" [[noreturn]] void fatal_error_reset(std::uint8_t code);

namespace svc {
namespace power {

/* ---------------------------------------------------------------------------
 * The fault codes, in one place.
 *
 * Whatever lands in ram::app().fatalErrorCode is the ONLY thing a reset leaves
 * behind, and on this box it is the only post-mortem available at all: flash is
 * RDP-locked, there is no printf, and SRAM is readable over SWD. So every
 * deliberate reset should name itself.
 *
 * Three ranges:
 *   100..164  STOCK. Assigned per interrupt veneer slot in app/startup/crt0.S,
 *             so the code tells you which vector fired.
 *   217..247  crt0's defaults for the five slots that SHOULD carry a real
 *             handler. Seeing one means a handler name did not match and the
 *             weak trap is still in place -- see docs/ARCHITECTURE.md section 5.
 *   192..194  DELIBERATE DIVERGENCE FROM STOCK, defined below.
 *
 * On that divergence: at these three sites the stock image calls
 * wwdg_init(); wwdg_force_reset() directly and records NOTHING, so every one of
 * them is indistinguishable from a brownout after the fact. Routing them
 * through fatalErrorReset() adds a single store to a byte that no code in the
 * image ever reads. It cannot change behaviour on CAN, on the UART or on any
 * pin; it only makes the reset identifiable. That is judged worth the
 * divergence. See docs/misra-cpp.md section 6.
 * ------------------------------------------------------------------------- */
inline constexpr std::uint8_t kFatalExti2        = 0x7AU;  /* 122, stock */
inline constexpr std::uint8_t kFatalExti9_5      = 0x85U;  /* 133, stock */
inline constexpr std::uint8_t kFatalBusDown      = 0xC0U;  /* 192, ours  */
inline constexpr std::uint8_t kFatalLinkWedged   = 0xC1U;  /* 193, ours  */
inline constexpr std::uint8_t kFatalHostCommand  = 0xC2U;  /* 194, ours  */

/* C++ spelling of the same function, for callers inside the application. */
[[noreturn]] inline void fatalErrorReset(std::uint8_t code) noexcept
{
    fatal_error_reset(code);
}

/* --- 0x08002E18 ---
 * RCC_APB1PCLKEN &= ~WWDGEN. Gating the clock does not logically disarm an
 * armed watchdog, but it does freeze its counter -- which is why the
 * wwdg_force_reset() at the end of the sleep path writes to a dead peripheral
 * and never fires. The reset that actually happens comes from the EXTI9_5 wake
 * handler.
 *
 * The stock decompile calls this one "can_clock_disable", which is wrong: 0x800
 * is the WWDG bit, not CAN's (0x2000000). */
void disableWatchdogClock() noexcept;

/* --- 0x080032F6 ---
 * The phase-1 give-up path, reached on the first 2048 ms tick after the 8th
 * failed autobaud attempt, i.e. ~18.4 s from boot (the 8th retry is at 16.38 s)
 * or when the CAN link state machine loses ACC.
 *
 * ORDER IS LOAD-BEARING. CAN must acknowledge sleep first, and PB8 is only
 * re-armed as the EXTI wake input once the CAN peripheral has released it.
 *
 * If CAN refuses to sleep this does NOTHING AT ALL and the caller just keeps
 * spinning -- the watchdog is still clocked at that point, so a wedge here
 * still ends in a reset rather than a hang. That is the stock behaviour and it
 * is the safe one.
 *
 * Does not return once it has actually slept: the wake path forces a reset. */
void shutdownDeepSleep() noexcept;

}  // namespace power
}  // namespace svc

#endif /* POWER_HPP */
