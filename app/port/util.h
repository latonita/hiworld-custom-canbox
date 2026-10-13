/* ============================================================================
 * util.h -- shared leaf helpers and the software-timer pool.
 *
 * Ported from app/src/util.c (Ghidra output). Only the LIVE functions are
 * reproduced; everything the decompile marks _DEAD (the bit-banged I2C driver
 * at 0x08002558.. and the busy-delay family at 0x08003342..) has no caller in
 * this image and is deliberately absent.
 *
 * Every declaration carries the original flash address of what it reproduces.
 * ==========================================================================*/

#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include "globals.h"

/* ---------------------------------------------------------------------------
 * The software-timer pool.
 *
 * Layout derived from the decompile, not guessed:
 *   base        g_softtimer_slots = 0x200018F8   (globals.h already names it)
 *   element     4 bytes, SIGNED 32-bit -- every access in the stock code is
 *               `*(int *)(p_softtimer_slots + idx * 4)`
 *   slots       64 (0x40) -- softtimer_tick_all @0x08007CEE walks idx < 0x40,
 *               and softtimer_scan_active_hi @0x08007D12 treats 0x3F as the
 *               last index. 64 * 4 = 256 bytes spans 0x200018F8..0x200019F7,
 *               which butts exactly against g_uart_frame_buf @0x200019F8 --
 *               independent confirmation of both the count and the width.
 *   aging       softtimer_tick_all runs from sys_misc_timers_tick @0x080083E2,
 *               i.e. the TIM2 10 ms hook (1 ms ISR / divider at 0x20001000),
 *               so one count == 10 ms.
 *   reload      150 (0x96) == 1.5 s of hold time per armed slot.
 * ------------------------------------------------------------------------- */
#define SOFTTIMER_SLOT_COUNT  0x40U   /* 64 slots                             */
#define SOFTTIMER_SLOT_WIDTH  4U      /* bytes per slot (int32_t)             */
#define SOFTTIMER_RELOAD      150     /* 0x96 counts = 1.5 s at 10 ms/count   */
#define SOFTTIMER_TICK_MS     10U     /* aging period, from the TIM2 10 ms hook*/

/* The pool itself, re-typed from the byte pointer globals.h exposes. */
#define g_softtimer_pool      ((volatile int32_t *)g_softtimer_slots)

/* Index the scan reports when no slot is running; also the first slot the
 * scan does NOT look at first (it starts one past it, at 0x17). */
#define SOFTTIMER_IDLE_INDEX  0x16

void softtimer_arm_at_index(void);     /* 0x08007CD2 */
void softtimer_clear_current(void);    /* 0x08007CE0 */
void softtimer_tick_all(void);         /* 0x08007CEE */
void softtimer_scan_active_hi(void);   /* 0x08007D12 */

/* ---------------------------------------------------------------------------
 * Leaf helpers. The length arguments are uint8_t on purpose: the stock loop
 * counters are masked with 0xFF on every increment (`i = i + 1 & 0xff`), so a
 * length of 256 or more would spin forever in the original. Keeping the type
 * at 8 bits makes that constraint unrepresentable instead of latent.
 * ------------------------------------------------------------------------- */
void memcpy_bytes(volatile uint8_t *dst, const volatile uint8_t *src,
                  uint8_t len);                                 /* 0x08008598 */

int util_bytes_differ(const volatile uint8_t *a, const volatile uint8_t *b,
                      uint8_t len);                             /* 0x08008574 */

uint8_t util_wrap_index(int base, int delta, int hi, int lo,
                        int wrap_mode);                         /* 0x080085C4 */

int util_bit_test(uint32_t value, uint32_t mask);               /* 0x0800413C */

#endif /* UTIL_H */
