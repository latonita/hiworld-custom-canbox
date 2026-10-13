/* ============================================================================
 * core.c -- tick scheduler, soft real-time clock, shutdown / deep-sleep path.
 *
 * Ported from app/src/tim.c (0x08007a30), app/src/sys.c (0x08007898,
 * 0x080078da, 0x080078ee, 0x080083e2, 0x0800853c), app/src/misc.c (0x08007858,
 * 0x080032f6) and app/src/can.c (0x08002e18, misnamed there).
 *
 * TIMING CHAIN (docs/hiworld-startup-sequence.md section 3):
 *     TIM2 = 2 x PCLK1 = 32 MHz, PSC = 7, AR = 4000  ->  1000 Hz
 *     TIM2_IRQHandler (hw_tim.c) per update event:
 *         tim2_sched_rate_flags()                              every   1 ms
 *         if (++g_tick_10ms_div  > 9) { = 0; sys_misc_timers_tick();
 *                                            sys_cycle_counter_tick();
 *                                            g_tick_100ms_div++; }   10 ms
 *         if (g_tick_100ms_div > 9)   { = 0; sys_soft_clock_tick();
 *                                            can_autobaud_timers_tick();
 *                                            g_tick_1s_div++; }     100 ms
 *         if (g_tick_1s_div    > 9)   { = 0; }                        1 s
 *     The divider chain itself lives in hw_tim.c; this file owns the hooks.
 * ==========================================================================*/

#include "port.h"
#include "core.h"

/* ---------------------------------------------------------------------------
 * Width corrections over globals.h.
 *
 * globals.h is explicit that its scalars are byte-wide placeholders "unless the
 * real width is known". Two of the words this file owns are genuinely 32-bit in
 * the stock image, and the difference is not cosmetic:
 *
 *   0x20001108 g_sched_tick_counter -- the free-running millisecond counter.
 *       The decompile loads it with LDR and docs section 3 calls it a 32-bit
 *       counter. If it were truncated to 8 bits the (c & 0x3ff) and (c & 0x7ff)
 *       tests could never match, so the 1024 ms param-save flag and the 2048 ms
 *       autobaud-retry flag would never fire and phase 1 would hang forever.
 *
 *   0x20001100 soft-clock flags word -- bit 0 "time changed", bit 1 AM/PM.
 *       The decompile reads/writes it as a word with masks 0xfffffffe /
 *       0xfffffffd. Only bits 0 and 1 are ever touched, and the read-modify-
 *       write preserves 0x20001101 (the 12-hour hour) either way, so byte and
 *       word access are behaviourally identical here -- the word form is kept
 *       to stay faithful to the original instruction stream.
 *
 * Addresses still come from the globals.h macros so there is one map, not two.
 * ------------------------------------------------------------------------- */
#define SCHED_TICK_COUNTER  (*(volatile uint32_t *)&g_sched_tick_counter)

/* Soft clock block @0x200010fc. [0..3] are the h/m/s/tenths the stock
 * sys_soft_clock_init zeroes; [4] and [5] are the derived 12-hour view. */
#define SOFT_CLOCK_HOUR24   (g_soft_clock[0])                            /* 0x200010fc */
#define SOFT_CLOCK_MIN      (g_soft_clock[1])                            /* 0x200010fd */
#define SOFT_CLOCK_SEC      (g_soft_clock[2])                            /* 0x200010fe */
#define SOFT_CLOCK_TENTHS   (g_soft_clock[3])                            /* 0x200010ff */
#define SOFT_CLOCK_FLAGS    (*(volatile uint32_t *)(g_soft_clock + 4))   /* 0x20001100 */
#define SOFT_CLOCK_HOUR12   (g_soft_clock[5])                            /* 0x20001101 */

#define SOFT_CLOCK_FLAG_CHANGED  0x1U   /* set on every minute rollover  */
#define SOFT_CLOCK_FLAG_PM       0x2U   /* 24-hour hour >= 12            */

/* g_blink_halfsec @0x20001104 bit assignments (see core.h). */
#define BLINK_STATE         0x01U
#define BLINK_PREV          0x02U
#define BLINK_CHANGED       0x04U

/* PWR / SCB bits used by the deep-sleep entry. */
#define PWR_CTRL1_LPMS_MASK 0x00000007U
#define PWR_CTRL1_LPMS_STOP 0x00000002U
#define PWR_CTRL3_WUF_MASK  0x00003000U   /* bits 13:12, the stock field */
#define PWR_CTRL3_WUF_VAL   0x00001000U
#define SCB_SCR_SLEEPDEEP   0x00000004U

/* ---- ADC block base -----------------------------------------------------
 * N32L40x puts the ADC on the AHB at 0x40020800, NOT at the STM32F1 address.
 * The decompile reaches it as (DAT_08003394 - 0x4c). */
#define ADC_BASE            0x40020800U

/* ===========================================================================
 * TEMPORARY SCAFFOLDING -- weak no-op stubs for other agents' subsystems.
 *
 * Every symbol below is owned by another subsystem and will be replaced by its
 * real (strong) definition as that port lands. They exist only so the image
 * links today. Each carries the stock flash address of the function it stands
 * in for. REMOVE THESE as the owning files appear.
 * ========================================================================= */

/* util subsystem -- ages the 64-entry software-timer array at 0x200018f8. */
extern void softtimer_tick_all(void);                            /* 0x08007cee */
__attribute__((weak)) void softtimer_tick_all(void) { }

/* can subsystem -- requests CAN sleep mode and reports whether the peripheral
 * acknowledged it (CAN_MCR: clear INRQ, set SLEEP; then (CAN_MSR & 3) == SLAK).
 * The stub returns 0 = "did not acknowledge", which makes the whole shutdown
 * path inert -- exactly what the stock code does on a refusal. */
/* 0x08002d1a -- the decompile calls this "can_rx0_msg_pending", which is wrong:
 * it calls CAN_Sleep (0x0800400e) and returns whether the controller accepted
 * sleep. hw_can.c exports it under its real name. */
#include "hw_can.h"
#define can_rx0_msg_pending  can_enter_sleep_mode

/* gpio subsystem -- PB12 HIGH, i.e. CAN transceiver into standby. */
extern void gpio_can_xcvr_standby_on_pb12(void);                 /* 0x08007440 */
__attribute__((weak)) void gpio_can_xcvr_standby_on_pb12(void) { }

/* gpio subsystem -- PC15 / PB6 / PB4 driven low, PB3 and PB2 back to inputs. */
extern void gpio_static_init_b(void);                            /* 0x080084be */
__attribute__((weak)) void gpio_static_init_b(void) { }

/* adc subsystem -- three CTRL2 bit-fields, all cleared on the way down. */
#include "hw_adc.h"   /* real definitions; these are inert on HW (ADC unclocked) */

/* uart subsystem -- USART2 disabled, TXE interrupt off. */
extern void uart_disable_usart2(void);                           /* 0x08003088 */
__attribute__((weak)) void uart_disable_usart2(void) { }

/* gpio/exti subsystem -- re-purposes PB8 from CAN_RX to EXTI line 8 and enables
 * NVIC IRQ 23 (EXTI9_5).
 *
 * *** THIS ONE IS NOT OPTIONAL. *** It is the ONLY wake source configured
 * before the WFI below. With the no-op stub in place the box enters deep sleep
 * and never comes back out -- it has to be power-cycled. Do not ship an image
 * that sleeps while this is still a stub. */
/* 0x08003222 -- the PB8 -> EXTI line 8 wake source armed before the WFI.
 * hw_gpio.c reconstructs this under the name exti_pb8_wake_config; the
 * decompile's "update_path_exti_setup" is the same function. This is NOT
 * optional: it is the only wake source armed before deep sleep, so a no-op
 * stub here means the box sleeps and never wakes (power-cycle only). */
#include "hw_gpio.h"
#define update_path_exti_setup  exti_pb8_wake_config

/* ===========================================================================
 * 1 ms scheduler
 * ========================================================================= */

/* --- 0x08007a30 tim2_sched_rate_flags ---
 * Increment the free-running millisecond counter and raise the ten one-shot
 * cadence flags. Note the deliberately staggered phases (1, 3, 7, 15, ...):
 * the 4 / 8 / 16 / 32 ms jobs never land on the same millisecond, which keeps
 * the worst-case main_loop pass short enough for the ~65.5 ms watchdog.
 *
 * The 64 ms flag and slower all trigger on an all-ones mask instead, so from
 * 64 ms down they DO coincide -- that is what the stock image does, not a
 * transcription slip. */
void tim2_sched_rate_flags(void)
{
    uint32_t c;

    SCHED_TICK_COUNTER = SCHED_TICK_COUNTER + 1U;
    c = SCHED_TICK_COUNTER;

    if ((c & 0x003U) == 0x001U)  g_tick_flag_fast     = 1;   /*    4 ms */
    if ((c & 0x007U) == 0x003U)  g_tick_flag_med      = 1;   /*    8 ms */
    if ((c & 0x00FU) == 0x007U)  g_tick_flag_div16    = 1;   /*   16 ms */
    if ((c & 0x01FU) == 0x00FU)  g_tick_flag_div32    = 1;   /*   32 ms */
    if ((c & 0x03FU) == 0x03FU)  g_tick_flag_div64    = 1;   /*   64 ms */
    if ((c & 0x07FU) == 0x07FU)  g_tick_flag_div128   = 1;   /*  128 ms */
    if ((c & 0x0FFU) == 0x0FFU)  g_tick_flag_div256   = 1;   /*  256 ms */
    if ((c & 0x1FFU) == 0x1FFU)  g_tick_flag_div512   = 1;   /*  512 ms */
    if ((c & 0x3FFU) == 0x3FFU)  g_tick_flag_slow     = 1;   /* 1024 ms */
    if ((c & 0x7FFU) == 0x7FFU)  g_autobaud_tick_flag = 1;   /* 2048 ms */
}

/* ===========================================================================
 * 10 ms hooks
 * ========================================================================= */

/* --- 0x080083e2 sys_misc_timers_tick ---
 * Four short countdowns plus the shared software-timer array.
 *
 * Note the two different floors, which are load-bearing: 0x2000118d runs down
 * to 0 (a plain "expired" test), while the three SWC timers stop at 1, so an
 * "== 1" test in the consumer fires on exactly one pass and then latches. */
void sys_misc_timers_tick(void)
{
    if (g_misc_timer_118d != 0)
        g_misc_timer_118d--;

    if (g_swc_resp_mode > 1)
        g_swc_resp_mode--;

    if (g_swc_flag_118b > 1)
        g_swc_flag_118b--;

    if (g_swc_timer_118c > 1)
        g_swc_timer_118c--;

    softtimer_tick_all();
}

/* --- 0x0800853c sys_cycle_counter_tick ---
 * Rolling counter over 1 .. (g_cycle_ctr_max - 1); wraps back to 1, not 0.
 * The comparison is against the post-increment value. */
void sys_cycle_counter_tick(void)
{
    g_cycle_ctr++;
    if (g_cycle_ctr >= g_cycle_ctr_max)
        g_cycle_ctr = 1;
}

/* ===========================================================================
 * Soft real-time clock
 * ========================================================================= */

/* --- 0x08007898 sys_soft_clock_recalc_12h ---
 * Derive the AM/PM bit and the 12-hour hour from the 24-hour hour.
 * hour12 = hour24 % 12, with 0 mapped to 12 (so midnight and noon read 12). */
void sys_soft_clock_recalc_12h(void)
{
    if (SOFT_CLOCK_HOUR24 < 12U)
        SOFT_CLOCK_FLAGS = SOFT_CLOCK_FLAGS & ~SOFT_CLOCK_FLAG_PM;
    else
        SOFT_CLOCK_FLAGS = (SOFT_CLOCK_FLAGS & ~SOFT_CLOCK_FLAG_PM) | SOFT_CLOCK_FLAG_PM;

    SOFT_CLOCK_HOUR12 = (uint8_t)(SOFT_CLOCK_HOUR24 % 12U);
    if (SOFT_CLOCK_HOUR12 == 0U)
        SOFT_CLOCK_HOUR12 = 12U;
}

/* --- 0x080078da sys_soft_clock_init ---
 * Zero 0x200010fc..0x200010ff and recompute the 12-hour view. Called once from
 * main_loop init; it does NOT clear the flags word at 0x20001100 (the stock
 * code leaves it to the scatter-load zero fill). */
void sys_soft_clock_init(void)
{
    SOFT_CLOCK_HOUR24 = 0;
    SOFT_CLOCK_MIN    = 0;
    SOFT_CLOCK_SEC    = 0;
    SOFT_CLOCK_TENTHS = 0;
    sys_soft_clock_recalc_12h();
}

/* --- 0x080078ee sys_soft_clock_tick ---
 * One call per 100 ms. This is the firmware's only time base of record, which
 * is the independent confirmation that the TIM2 ISR really runs at 1 kHz.
 *
 * The blink toggle is evaluated BEFORE the increment, on tenths 0 and 5, i.e.
 * every 500 ms. The "time changed" flag is raised on the minute rollover only;
 * nothing in this file clears it (the UART clock-report consumer does). */
void sys_soft_clock_tick(void)
{
    if ((SOFT_CLOCK_TENTHS % 5U) == 0U)
        g_blink_halfsec ^= BLINK_STATE;

    SOFT_CLOCK_TENTHS++;
    if (SOFT_CLOCK_TENTHS > 9U) {
        SOFT_CLOCK_TENTHS = 0;
        SOFT_CLOCK_SEC++;
        if (SOFT_CLOCK_SEC > 59U) {
            SOFT_CLOCK_SEC = 0;
            SOFT_CLOCK_MIN++;
            SOFT_CLOCK_FLAGS = (SOFT_CLOCK_FLAGS & ~SOFT_CLOCK_FLAG_CHANGED)
                             | SOFT_CLOCK_FLAG_CHANGED;
            if (SOFT_CLOCK_MIN > 59U) {
                SOFT_CLOCK_MIN = 0;
                SOFT_CLOCK_HOUR24++;
                if (SOFT_CLOCK_HOUR24 == 24U)
                    SOFT_CLOCK_HOUR24 = 0;
                sys_soft_clock_recalc_12h();
            }
        }
    }
}

/* ===========================================================================
 * Blink / heartbeat edge detector
 * ========================================================================= */

/* --- 0x08007858 status_bit_change_detect ---
 * Run from main_loop on the 4 ms flag. Compares the 500 ms heartbeat bit
 * against the shadow copy taken last time it changed: equal -> clear the
 * one-shot "changed" bit, different -> set it and re-shadow. The consumer sees
 * BLINK_CHANGED set for the 4 ms passes between the toggle and the next call. */
void status_bit_change_detect(void)
{
    uint8_t v = g_blink_halfsec;

    if ((v & BLINK_STATE) == ((v & (BLINK_STATE | BLINK_PREV)) >> 1)) {
        g_blink_halfsec = (uint8_t)(v & ~BLINK_CHANGED);
    } else {
        v = (uint8_t)((v & ~BLINK_CHANGED) | BLINK_CHANGED);
        g_blink_halfsec = v;
        g_blink_halfsec = (uint8_t)((v & ~BLINK_PREV)
                                    | (uint8_t)((v & BLINK_STATE) << 1));
    }
}

/* ===========================================================================
 * Shutdown / deep sleep
 * ========================================================================= */

/* --- 0x08002e18 (named can_clock_disable in the decompile -- it is not) ---
 * RCC_APB1PCLKEN &= ~WWDGEN. Gating the clock does not stop an armed watchdog
 * logically, but it does freeze its counter, which is why the wwdg_force_reset
 * at the end of the sleep path writes to a dead peripheral and never fires.
 * The reset that actually happens comes from the EXTI9_5 wake ISR. */
void wwdg_clock_disable(void)
{
    rcc_apb1_periph_clock(RCC_APB1_WWDG, 0);
}

/* --- 0x0800563c pwr_enter_stop, inlined here as the sleep primitive ---
 * PWR->CTRL3[13:12] = cfg, PWR->CTRL1[2:0] = LPMS(2), SCB->SCR |= SLEEPDEEP,
 * then WFI. The stock code clears SLEEPDEEP again after the wake; kept for
 * fidelity even though this call site never gets that far on a real wake.
 *
 * Static on purpose: the rcc port does not export this, and keeping it file
 * local avoids colliding with it if it ever does. */
static void pwr_enter_stop(int use_wfi, uint32_t cfg)
{
    PWR->CTRL3 = (PWR->CTRL3 & ~PWR_CTRL3_WUF_MASK) | cfg;
    PWR->CTRL1 = (PWR->CTRL1 & ~PWR_CTRL1_LPMS_MASK) | PWR_CTRL1_LPMS_STOP;
    SCB_SCR |= SCB_SCR_SLEEPDEEP;

    if (use_wfi == 1) {
        wait_for_irq();
    } else {
        /* The stock WFE branch really does issue two WFEs -- the first clears
         * a stale event-register bit so the second actually sleeps. */
        __asm volatile ("wfe");
        __asm volatile ("wfe");
    }

    SCB_SCR &= ~SCB_SCR_SLEEPDEEP;
}

/* --- 0x080032f6 (named exti8_bus_wake_setup in the decompile) ---
 * The phase-1 give-up path, reached after 8 failed autobaud attempts (~16.4 s)
 * or when the CAN state machine loses ACC. Order matters: CAN must acknowledge
 * sleep first, and PB8 is only re-armed as the EXTI wake input once the CAN
 * peripheral has released it.
 *
 * If CAN refuses to sleep this does nothing at all and the caller keeps
 * spinning -- the watchdog is still clocked at that point, so a wedge here
 * still ends in a reset rather than a hang. */
void sys_shutdown_deep_sleep(void)
{
    if (can_rx0_msg_pending() != 1)
        return;

    gpio_can_xcvr_standby_on_pb12();   /* PB12 high -> transceiver standby  */
    gpio_static_init_b();              /* status outputs low, PB3/PB2 in    */

    adc_dma_cmd(ADC_BASE, 0);
    adc_enable(ADC_BASE, 0);
    adc_sw_start_regular(ADC_BASE, 0);

    uart_disable_usart2();
    wwdg_clock_disable();
    update_path_exti_setup();          /* PB8 -> EXTI line 8, NVIC IRQ 23   */

    pwr_enter_stop(1, PWR_CTRL3_WUF_VAL);

    /* On wake. The WWDG is unclocked by now, so this write is inert and the
     * real reset comes from the EXTI9_5 handler -> fatal_error_reset(0x85) ->
     * wwdg_init() -> wwdg_force_reset(). Reproduced because the stock image
     * has it and because it is the correct fallback if the clock ever is
     * still running. */
    wwdg_force_reset();
}

/* Compatibility alias under the Ghidra name. Weak so a strong definition
 * elsewhere (should misc.c ever be ported separately) takes precedence. */
/* The decompile's name "exti8_bus_wake_setup" for 0x080032f6 is a misnomer --
 * it is the shutdown/deep-sleep path. Exported above as
 * sys_shutdown_deep_sleep(); the alias is gone to keep one name per function. */
