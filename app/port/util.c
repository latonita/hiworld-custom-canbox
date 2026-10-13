/* ============================================================================
 * util.c -- the firmware's shared leaf helpers plus the software-timer pool.
 *
 * Ported from app/src/util.c (Ghidra output). That file is mostly a bit-banged
 * I2C driver (0x08002558..0x080027A6) and a busy-delay family
 * (0x08003342..0x08003372); the decompile marks all of it _DEAD and nothing in
 * the image calls any of it, so none of it is reproduced here. What IS live is
 * everything below: four software-timer primitives and four leaf helpers.
 *
 * This translation unit touches NO hardware at all -- it is pure RAM state, so
 * there is no n32l406_hw.h dependency beyond what port.h pulls in.
 *
 * SOFTWARE-TIMER POOL (see util.h for the full derivation):
 *     base 0x200018F8 (g_softtimer_slots), 64 slots, 4 bytes each, signed,
 *     aged every 10 ms, armed with 150 counts = 1.5 s.
 *
 * The pool is the SWC (steering-wheel-control) key hold tracker: the CAN
 * decoder arms the slot for the key code it just saw, main_loop's 4 ms pass
 * scans for a slot still running and publishes its index as the "key currently
 * held" value the 0x416 reply frame carries, and the 10 ms hook ages the whole
 * pool so a key that stops repeating expires 1.5 s later. Before this file
 * existed softtimer_tick_all was a weak no-op in core.c, which made every
 * armed slot run forever -- i.e. a pressed SWC key never released.
 * ==========================================================================*/

#include "port.h"
#include "util.h"

/* ============================================================================
 * Software-timer pool
 * ==========================================================================*/

/* --- 0x08007CD2 softtimer_arm_at_index ---
 * Arm the slot selected by the freshly decoded SWC key code with the full
 * 1.5 s reload. Takes no arguments in the stock image: the index is passed
 * through the global at 0x2000117F, which the CAN decoder sets immediately
 * before the call. can_decode.c declares a weak stub with exactly this
 * signature; this definition overrides it.
 *
 * No range check here, matching stock: g_swc_keycode is a uint8_t and the pool
 * is only 64 slots, so a key code above 0x3F would write past the pool into
 * g_uart_frame_buf. The decoder never produces one (all table-driven codes are
 * <= 0x3F), but the hazard is the stock behaviour and is reproduced, not
 * silently patched -- changing it would diverge from the reference image. */
void softtimer_arm_at_index(void)
{
    g_softtimer_pool[g_swc_keycode] = SOFTTIMER_RELOAD;
}

/* --- 0x08007CE0 softtimer_clear_current ---
 * Release the slot the scan last reported (g_swc_keydata_ref @0x20001182).
 * Called from the CAN TX path right before re-scanning, so that the key just
 * acknowledged does not win the next scan again. */
void softtimer_clear_current(void)
{
    g_softtimer_pool[g_swc_keydata_ref] = 0;
}

/* --- 0x08007CEE softtimer_tick_all ---
 * Age every slot by one count, floored at zero. Runs from the TIM2 10 ms hook
 * (sys_misc_timers_tick @0x080083E2), which is the single consumer; nothing
 * else decrements the pool. */
void softtimer_tick_all(void)
{
    uint32_t i;

    for (i = 0; i < SOFTTIMER_SLOT_COUNT; i++) {
        if (g_softtimer_pool[i] != 0)
            g_softtimer_pool[i]--;
    }
}

/* --- 0x08007D12 softtimer_scan_active_hi ---
 * Publish the index of the first still-running slot into g_swc_keydata_ref.
 *
 * The stock control flow looks like a scan of the "upper range" only, but it is
 * not: it is a FULL circular sweep of all 64 slots that merely STARTS at 0x17.
 * Walk the decompile -- idx runs 0x17..0x3F, then is reset to 0 and runs
 * 0x00..0x16; at 0x16 the loop gives up and stores 0x16 regardless. So 0x16 is
 * both the last slot examined and the "nothing held" sentinel, and starting at
 * 0x17 just gives the high key codes priority over the low ones.
 *
 * Called from main_loop's 4 ms branch (g_tick_flag_fast) and from the 0x416
 * reply path, immediately after softtimer_clear_current. */
void softtimer_scan_active_hi(void)
{
    uint8_t i = 0x17U;

    for (;;) {
        if (g_softtimer_pool[i] != 0) {
            g_swc_keydata_ref = i;
            return;
        }
        if (i == 0x3FU) {
            i = 0U;                       /* wrap into the low half */
        } else if (i == SOFTTIMER_IDLE_INDEX) {
            g_swc_keydata_ref = SOFTTIMER_IDLE_INDEX;   /* swept, nothing held */
            return;
        } else {
            i++;
        }
    }
}

/* ============================================================================
 * Leaf helpers
 * ==========================================================================*/

/* --- 0x08008574 util_bytes_differ ---
 * 1 as soon as any of the first len bytes differs, 0 if all match. Note the
 * polarity: this is NOT memcmp's "0 means equal" convention inverted by
 * accident -- callers use it directly as "something changed". */
int util_bytes_differ(const volatile uint8_t *a, const volatile uint8_t *b,
                      uint8_t len)
{
    uint8_t i;

    for (i = 0; i < len; i++) {
        if (a[i] != b[i])
            return 1;
    }
    return 0;
}

/* --- 0x08008598 memcpy_bytes ---
 * Byte-at-a-time copy. Both sides are volatile because every real call site
 * moves data in or out of the fixed-address stock RAM block, where the
 * compiler must not coalesce or reorder the accesses. */
void memcpy_bytes(volatile uint8_t *dst, const volatile uint8_t *src,
                  uint8_t len)
{
    uint8_t i;

    for (i = 0; i < len; i++)
        dst[i] = src[i];
}

/* --- 0x080085C4 util_wrap_index ---
 * base + delta constrained to [lo, hi]. wrap_mode selects what happens on
 * overflow, and the sense is the opposite of what the name suggests:
 *     wrap_mode == 0 -> WRAP   (below lo -> hi, above hi -> lo)
 *     wrap_mode != 0 -> CLAMP  (below lo -> lo, above hi -> hi)
 * Two live uses: the soft RTC rolls hours/minutes with (hi, lo, 0), and the
 * UART TX scheduler advances its slot index with (idx, 1, 5, 0, 0).
 *
 * The arithmetic is done in int (delta is -1 in the RTC case) and only the
 * result is narrowed, exactly as the stock code does. */
uint8_t util_wrap_index(int base, int delta, int hi, int lo, int wrap_mode)
{
    int v = base + delta;

    if (v < lo)
        return (uint8_t)((wrap_mode == 0) ? hi : lo);
    if (v > hi)
        return (uint8_t)((wrap_mode == 0) ? lo : hi);
    return (uint8_t)v;
}

/* --- 0x0800413C util_bit_test ---
 * (value & mask) != 0. Trivial, but it is a real out-of-line function in the
 * image and the CAN error/status decoder calls it ~20 times in a row. */
int util_bit_test(uint32_t value, uint32_t mask)
{
    return (value & mask) != 0U;
}
