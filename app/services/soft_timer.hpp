/* ============================================================================
 * soft_timer.hpp -- the 64-slot software-timer pool and the four short
 * countdowns, namespace svc::timers.
 *
 * Ported from reference/decompile/app/util.c and reference/decompile/app/sys.c (Ghidra output) via
 * the retired app/port/util.c and the retired app/port/core.c. Every entry point carries the ORIGINAL
 * flash address of the stock function it reproduces.
 *
 * ---------------------------------------------------------------------------
 * WHAT THE POOL IS FOR
 *
 * ram::AppRam::softTimers (0x200018F8, 64 signed 32-bit slots) is the SWC
 * (steering-wheel-control) key-hold tracker:
 *
 *   - the CAN decoder arms the slot for the key code it just saw
 *     (armForKeycode, reload 150 counts),
 *   - the TIM2 10 ms hook ages the whole pool by one count (tickAll), so a key
 *     that stops repeating expires 1.5 s later,
 *   - main_loop's 4 ms pass scans for a slot still running (scanActive) and
 *     publishes its index as the "key currently held" value the 0x416 reply
 *     frame carries.
 *
 * The geometry is derived, not guessed (see ram_map.hpp): tickAll walks
 * idx < 0x40 and scanActive treats 0x3F as the last index, and 64 * 4 = 256
 * bytes butts exactly against the UART frame buffer at 0x200019F8 --
 * independent confirmation of both the slot count and the element width.
 * One count == 10 ms because the only caller of tickAll is the 10 ms hook.
 * ==========================================================================*/

#ifndef SOFT_TIMER_HPP
#define SOFT_TIMER_HPP

namespace svc {
namespace timers {

/* --- 0x08007CD2 softtimer_arm_at_index ---
 * Arm the slot selected by the freshly decoded SWC key code
 * (ram::AppRam::swc.keycode) with the full 1.5 s reload. Takes no arguments in
 * the stock image either: the index travels through the global at 0x2000117F,
 * which the CAN decoder sets immediately before the call.
 *
 * HAZARD, reproduced deliberately: there is no range check. keycode is a byte
 * and the pool is 64 slots, so a key code above 0x3F would write past the pool
 * into the UART frame buffer. The decoder never produces one (every
 * table-driven code is <= 0x3F), but this is the stock behaviour and silently
 * patching it would diverge from the reference image. */
void armForKeycode() noexcept;

/* --- 0x08007CE0 softtimer_clear_current ---
 * Release the slot the last scan reported (ram::AppRam::swc.keydataRef).
 * Called from the CAN TX path right before re-scanning, so the key just
 * acknowledged cannot win the next scan again. */
void clearCurrent() noexcept;

/* --- 0x08007CEE softtimer_tick_all ---
 * Age every slot by one count, floored at zero. Runs from the TIM2 10 ms hook
 * (tickShortTimers), which is the single consumer; nothing else decrements the
 * pool. */
void tickAll() noexcept;

/* --- 0x08007D12 softtimer_scan_active_hi ---
 * Publish the index of the first still-running slot into swc.keydataRef.
 *
 * The stock control flow looks like a scan of the "upper range" only, but it is
 * not: it is a FULL circular sweep of all 64 slots that merely STARTS at 0x17.
 * Walk the decompile -- idx runs 0x17..0x3F, is then reset to 0 and runs
 * 0x00..0x16; at 0x16 the loop gives up and stores 0x16 regardless. So 0x16
 * (ram::kSoftTimerIdle) is both the last slot examined and the "nothing held"
 * sentinel, and starting at 0x17 simply gives high key codes priority over low
 * ones. */
void scanActive() noexcept;

/* --- 0x080083E2 sys_misc_timers_tick ---
 * The TIM2 10 ms hook: ages the four short countdowns in
 * ram::AppRam::miscTimers and then the whole 64-slot pool.
 *
 * Note the two different floors, which are load-bearing: miscTimer runs down
 * to 0 (a plain "expired" test), while the three SWC timers stop at 1, so an
 * "== 1" test in the consumer fires on exactly one pass and then latches. */
void tickShortTimers() noexcept;

}  // namespace timers
}  // namespace svc

#endif /* SOFT_TIMER_HPP */
