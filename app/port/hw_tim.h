/* ============================================================================
 * hw_tim.h -- TIM2 time base and the 1 ms scheduler tick.
 *
 * Ported from app/src/tim.c (Ghidra output). Addresses in the comments are the
 * ORIGINAL flash addresses of the stock functions this reproduces.
 *
 * Only the TIM2 time-base path is reconstructed here: the stock image uses the
 * vendor TIM driver's output-compare / PWM / encoder / slave-mode entry points
 * nowhere, so they are deliberately not ported (they stay in app/src/tim.c as
 * the spec, in case a later feature needs them).
 * ==========================================================================*/

#ifndef HW_TIM_H
#define HW_TIM_H

#include <stdint.h>
#include "n32l406_hw.h"

/* ---- TIM register bits used by this subsystem --------------------------- */
#define TIM_CTRL1_CEN        0x0001U   /* counter enable                     */
#define TIM_CTRL1_DIR_CMS    0x0070U   /* DIR + CMS ("CounterMode") field    */
#define TIM_CTRL1_CKD        0x0300U   /* clock division ("ClkDiv") field    */
#define TIM_EVTGEN_UG        0x0001U   /* update generation                  */
#define TIM_INT_UPDATE       0x0001U   /* DINTEN.UIE / STS.UIF               */

/* Time-base init block. Layout copied byte-for-byte from the stock vendor
 * struct as the decompile of 0x08006140 indexes it, so the field offsets are
 * the SDK's TIM_TimeBaseInitType offsets, not a redesign.
 *
 * The trailing uint8_t fields are vendor extension flags whose meaning is NOT
 * identified: 0x08006140 copies them into CTRL1 bit 11, CTRL1 bit 15 and (for
 * TIM2/TIM4 only) CTRL2 bit 8. The stock tim2_init never initialises them (it
 * fills only the first four u16s and leaves the rest of the stack frame
 * undefined), so this port zeroes them, which leaves those bits at their reset
 * value. See the note in hw_tim.c. */
typedef struct {
    uint16_t Prescaler;    /* +0x00 -> PSC                                   */
    uint16_t CntMode;      /* +0x02 -> CTRL1[6:4]  (pre-shifted by caller)    */
    uint16_t Period;       /* +0x04 -> AR                                    */
    uint16_t ClkDiv;       /* +0x06 -> CTRL1[9:8]  (pre-shifted by caller)    */
    uint8_t  RepetCnt;     /* +0x08 -> REPCNT, advanced timers (TIM1) only    */
    uint8_t  Ctrl1Bit11;   /* +0x09 -> CTRL1 bit 11, purpose unidentified     */
    uint8_t  Reserved[3];  /* +0x0A -> CTRL1 bits 12..14, TIM9 only (unused)  */
    uint8_t  Ctrl1Bit15;   /* +0x0D -> CTRL1 bit 15, purpose unidentified     */
    uint8_t  Ctrl2Bit8;    /* +0x0E -> CTRL2 bit 8,  purpose unidentified     */
} tim_timebase_init_t;

/* ---- vendor TIM driver subset (app/port/hw_tim.c) ----------------------- */
void tim_timebase_init(TIM_Module *tim,
                       const tim_timebase_init_t *init);        /* 0x08006140 */
void tim_prescaler_config(TIM_Module *tim, uint16_t prescaler,
                          uint16_t reload_mode);                /* 0x08006a54 */
void tim_it_config(TIM_Module *tim, uint16_t it, int enable);   /* 0x0800693c */
void tim_cmd(TIM_Module *tim, int enable);                      /* 0x08006908 */
int  tim_get_it_status(TIM_Module *tim, uint16_t it);           /* 0x08006fb4 */
void tim_clear_it_pending(TIM_Module *tim, uint16_t it);        /* 0x08006fd6 */

/* ---- the app's TIM2 timebase -------------------------------------------- */
void tim2_init(void);                                           /* 0x080027f8 */

/* TIM2 interrupt. The name must match the weak symbol in app/startup/crt0.S
 * exactly (veneer slot 37, IRQ 28); any other spelling silently does nothing. */
void TIM2_IRQHandler(void);                                     /* 0x08002f72 */

/* ---- hooks the tick drives, owned by OTHER subsystems -------------------
 * Not implemented here. hw_tim.c carries a weak no-op stub for each so the
 * image links before they land; the real definition overrides the stub. */

/* 1 ms: free-running tick counter + the 10 cadence flags main_loop consumes.
 * Body is in app/src/tim.c but it is explicitly NOT part of this port. */
void tim2_sched_rate_flags(void);                               /* 0x08007a30 */

/* 10 ms pair -- sys subsystem (app/src/sys.c). */
void sys_misc_timers_tick(void);                                /* 0x080083e2 */
void sys_cycle_counter_tick(void);                              /* 0x0800853c */

/* 100 ms pair -- sys subsystem (app/src/sys.c) + can (app/src/can.c). */
void sys_soft_clock_tick(void);                                 /* 0x080078ee */
void can_autobaud_timers_tick(void);                            /* 0x080077e8 */

#endif /* HW_TIM_H */
