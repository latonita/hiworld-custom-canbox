/* ============================================================================
 * soft_timer.cpp -- see soft_timer.hpp.
 *
 * Ported from reference/decompile/app/util.c (0x08007CD2, 0x08007CE0, 0x08007CEE, 0x08007D12)
 * and reference/decompile/app/sys.c (0x080083E2), Ghidra output, and
 * the retired app/port/core.c.
 *
 * This translation unit touches NO hardware at all -- it is pure RAM state, so
 * it includes no HAL header.
 * ==========================================================================*/

#include "soft_timer.hpp"

#include <cstddef>
#include <cstdint>

#include "ram_map.hpp"

namespace svc {
namespace timers {

/* --- 0x08007CD2 softtimer_arm_at_index --- */
void armForKeycode() noexcept
{
    /* No range check: stock behaviour, see the HAZARD note in the header. The
     * assertion below records what the pool can actually hold, so a future
     * change to the slot count cannot quietly widen the hazard. */
    static_assert(ram::kSoftTimerSlots == 64U,
                  "the unchecked index is only safe because the decoder's key "
                  "codes are all <= 0x3F");

    ram::app().softTimers[ram::app().swc.keycode] = ram::kSoftTimerReload;
}

/* --- 0x08007CE0 softtimer_clear_current --- */
void clearCurrent() noexcept
{
    ram::app().softTimers[ram::app().swc.keydataRef] = 0;
}

/* --- 0x08007CEE softtimer_tick_all ---
 * Age every slot by one, floored at zero. The floor is a test-then-decrement,
 * not a saturating subtract: a slot already at 0 is left alone, so the pool can
 * never go negative even though the elements are signed. */
void tickAll() noexcept
{
    for (std::size_t i = 0U; i < ram::kSoftTimerSlots; ++i) {
        if (ram::app().softTimers[i] != 0) {
            --ram::app().softTimers[i];
        }
    }
}

/* --- 0x08007D12 softtimer_scan_active_hi ---
 * A full circular sweep of all 64 slots that starts at 0x17: 0x17..0x3F, wrap
 * to 0x00, then 0x00..0x16, where it gives up and publishes the sentinel.
 * Index 0x16 is therefore both the last slot examined and "nothing held". */
void scanActive() noexcept
{
    std::uint8_t i = ram::kSoftTimerScanStart;   /* 0x17 */

    for (;;) {
        if (ram::app().softTimers[i] != 0) {
            ram::app().swc.keydataRef = i;
            return;
        }
        if (i == static_cast<std::uint8_t>(ram::kSoftTimerSlots - 1U)) {
            i = 0U;                               /* 0x3F -> wrap into the low half */
        } else if (i == ram::kSoftTimerIdle) {
            ram::app().swc.keydataRef = ram::kSoftTimerIdle;  /* swept, nothing held */
            return;
        } else {
            ++i;
        }
    }
}

/* --- 0x080083E2 sys_misc_timers_tick ---
 * The 10 ms hook. Four short countdowns with TWO different floors, both
 * load-bearing:
 *   miscTimer   -> 0, so the consumer's "expired" test is a plain == 0;
 *   swcRespMode
 *   swcFlag     -> 1, so the consumer's "== 1" test fires on exactly one pass
 *   swcTimer       and then latches until something reloads the timer.
 * Then the whole software-timer pool is aged. */
void tickShortTimers() noexcept
{
    if (ram::app().miscTimers.miscTimer != 0U) {
        --ram::app().miscTimers.miscTimer;
    }
    if (ram::app().miscTimers.swcRespMode > 1U) {
        --ram::app().miscTimers.swcRespMode;
    }
    if (ram::app().miscTimers.swcFlag > 1U) {
        --ram::app().miscTimers.swcFlag;
    }
    if (ram::app().miscTimers.swcTimer > 1U) {
        --ram::app().miscTimers.swcTimer;
    }

    tickAll();
}

}  // namespace timers
}  // namespace svc
