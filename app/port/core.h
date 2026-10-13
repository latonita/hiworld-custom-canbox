/* ============================================================================
 * core.h -- tick scheduler, soft real-time clock, shutdown / deep-sleep path.
 *
 * Ported from app/src/tim.c, app/src/sys.c and app/src/misc.c (Ghidra output).
 * Addresses in the comments are the ORIGINAL flash addresses of the stock
 * functions this subsystem reproduces.
 *
 * Three things live here:
 *
 *   1. The 1 ms scheduler hook. The TIM2 ISR (hw_tim.c) calls it on every
 *      update event; it bumps a free-running 32-bit millisecond counter and
 *      raises ten one-shot cadence flags at 4/8/16/32/64/128/256/512/1024/
 *      2048 ms. main_loop polls three of them (4 ms, 8 ms, 1024 ms) plus the
 *      2048 ms autobaud-retry flag and clears each one itself.
 *
 *   2. The soft real-time clock at 0x200010fc (h / m / s / tenths + a flags
 *      word and a 12-hour hour). Advanced once per 100 ms from the same ISR;
 *      it also owns the 500 ms blink/heartbeat bit g_blink_halfsec.
 *
 *   3. The give-up path: when phase 1 finds no CAN bus after ~16.4 s the box
 *      puts CAN to sleep, shuts every peripheral down and executes WFI with
 *      SLEEPDEEP. See docs/hiworld-startup-sequence.md section 8 / 9.
 * ==========================================================================*/

#ifndef CORE_H
#define CORE_H

#include <stdint.h>

/* ---- 1 ms scheduler ------------------------------------------------------
 * Called from TIM2_IRQHandler once per millisecond, before the 10 ms / 100 ms
 * divider chain. Sets the cadence flags listed in the table below; it never
 * clears them -- the consumer in main_loop does that.
 *
 *   flag                  RAM          set when                    period
 *   g_tick_flag_fast      0x2000110c   (c & 0x003) == 0x001          4 ms
 *   g_tick_flag_med       0x2000110d   (c & 0x007) == 0x003          8 ms
 *   g_tick_flag_div16     0x2000110e   (c & 0x00f) == 0x007         16 ms
 *   g_tick_flag_div32     0x2000110f   (c & 0x01f) == 0x00f         32 ms
 *   g_tick_flag_div64     0x20001110   (c & 0x03f) == 0x03f         64 ms
 *   g_tick_flag_div128    0x20001111   (c & 0x07f) == 0x07f        128 ms
 *   g_tick_flag_div256    0x20001112   (c & 0x0ff) == 0x0ff        256 ms
 *   g_tick_flag_div512    0x20001113   (c & 0x1ff) == 0x1ff        512 ms
 *   g_tick_flag_slow      0x20001114   (c & 0x3ff) == 0x3ff       1024 ms
 *   g_autobaud_tick_flag  0x20001115   (c & 0x7ff) == 0x7ff       2048 ms
 *
 * c is the post-increment value of the free-running counter at 0x20001108
 * (32-bit -- see the note in core.c; the byte-wide macro in globals.h is a
 * placeholder and would break every flag slower than 256 ms). */
void tim2_sched_rate_flags(void);                             /* 0x08007a30 */

/* ---- 10 ms hooks (TIM2 ISR, divider 0x20001000) ------------------------- */

/* Ages the short software timers 0x2000118a..0x2000118d and then the 64-entry
 * software-timer array at 0x200018f8 (via softtimer_tick_all, util subsystem). */
void sys_misc_timers_tick(void);                              /* 0x080083e2 */

/* Rolling 1..g_cycle_ctr_max counter at 0x200011b7 (UART TX slot rotation). */
void sys_cycle_counter_tick(void);                            /* 0x0800853c */

/* ---- 100 ms hook (TIM2 ISR, divider 0x20001001) ------------------------- */

/* Soft RTC advance: 10 -> second, 60 -> minute, 60 -> hour, 24 -> day. Also
 * toggles the 500 ms blink bit and raises the "time changed" flag bit. */
void sys_soft_clock_tick(void);                               /* 0x080078ee */

/* NOTE: the other 100 ms hook, can_autobaud_timers_tick @0x080077e8, is CAN
 * state (0x2000113a, 0x200011a0) and belongs to the CAN agent. hw_tim.h
 * already declares it and hw_tim.c carries the weak stub; it is deliberately
 * NOT defined here. */

/* ---- soft real-time clock ----------------------------------------------- */

/* Zero h/m/s/tenths at 0x200010fc..0x200010ff, then recompute the 12-hour
 * view. Called once from main_loop init (step 5 of the startup sequence). */
void sys_soft_clock_init(void);                               /* 0x080078da */

/* Recompute the AM/PM bit (bit 1 of the flags word at 0x20001100) and the
 * 12-hour hour at 0x20001101 from the 24-hour hour. */
void sys_soft_clock_recalc_12h(void);                         /* 0x08007898 */

/* ---- blink / heartbeat --------------------------------------------------
 * g_blink_halfsec @0x20001104, consumed on the 4 ms flag by main_loop:
 *   bit 0  heartbeat, toggled every 500 ms by sys_soft_clock_tick
 *   bit 1  last value of bit 0 seen by the edge detector
 *   bit 2  one-shot "bit 0 changed" pulse, set by the edge detector */
void status_bit_change_detect(void);                          /* 0x08007858 */

/* ---- shutdown / deep sleep ---------------------------------------------- */

/* Turn the WWDG clock off (RCC_APB1PCLKEN &= ~WWDGEN). Only used by the
 * shutdown path -- note the stock decompile misnames this can_clock_disable
 * in app/src/can.c, but 0x800 is the WWDG bit, not CAN's (0x2000000). */
void wwdg_clock_disable(void);                                /* 0x08002e18 */

/* Phase-1 give-up: CAN sleep, transceiver to standby, status GPIOs low, ADC
 * and USART2 off, WWDG unclocked, PB8 re-armed as EXTI line 8, then WFI with
 * SLEEPDEEP + PWR deep-standby. Returns WITHOUT doing anything at all if the
 * CAN peripheral does not acknowledge sleep -- the caller just keeps spinning.
 * Does not return once it has slept: the wake path forces a watchdog reset. */
void sys_shutdown_deep_sleep(void);                           /* 0x080032f6 */

/* Compatibility alias under the Ghidra name used in app/src/misc.c. Weak, so
 * a stronger definition elsewhere wins. */

#endif /* CORE_H */
