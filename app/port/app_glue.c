/* ============================================================================
 * app_glue.c -- application-logic glue, ported from app/src/can.c,
 * app/src/gpio.c, app/src/uart.c and app/src/tim.c (Ghidra output).
 *
 * These seven functions fall between the subsystem drivers: they contain no
 * register programming of their own (bar the two one-line PB12 helpers) and do
 * nothing but sequence the ported drivers and shuffle the fixed-address RAM
 * block. They are all on the live path.
 *
 *   0x080032d2  clock_tim2_pwr_init          step 3 of the boot sequence
 *   0x08007430  gpio_can_xcvr_standby_pb12   PB12 low  (transceiver awake)
 *   0x08007440  gpio_can_xcvr_standby_on_pb12 PB12 high (transceiver standby)
 *   0x080084be  gpio_static_init_b           shutdown-path GPIO teardown
 *   0x080084f6  gpio_write_status_lines      8 ms status fan-out
 *   0x0800755c  can_autobaud_state_machine   8 ms CAN task
 *   0x08008110  can_periodic_tx_scheduler    periodic CAN TX table
 *   0x08007da8  uart_D2_cluster_text_to_can  0xD2 cluster text -> CAN
 *
 * Every write below lands in the stock RAM block 0x20001000..0x20003028 via
 * the macros in app/include/globals.h; this file owns no state of its own.
 * ==========================================================================*/

#include <stdint.h>

#include "port.h"
#include "app_glue.h"
#include "can_decode.h"
#include "core.h"
#include "hw_can.h"
#include "hw_gpio.h"
#include "hw_tim.h"

/* ===========================================================================
 * TEMPORARY SCAFFOLDING -- weak no-op stubs for the util subsystem.
 *
 * Both symbols are owned by app/src/util.c, which has no port yet (main.c
 * already calls softtimer_scan_active_hi through a util.h that does not exist
 * at the time of writing). They are declared here so this file compiles and
 * links standalone; the real strong definitions override them automatically.
 * REMOVE these the moment util.c lands.
 * ========================================================================= */

/* Zero the software-timer slot selected by g_swc_keydata_ref. */
extern void softtimer_clear_current(void);                       /* 0x08007ce0 */
__attribute__((weak)) void softtimer_clear_current(void) { }

/* Scan slots 0x17..0x3F for the first still-running timer and record its index
 * in g_swc_keydata_ref. */
extern void softtimer_scan_active_hi(void);                      /* 0x08007d12 */
__attribute__((weak)) void softtimer_scan_active_hi(void) { }

/* ===========================================================================
 * 0x080032d2 -- clock_tim2_pwr_init (tim2_sched_nvic_init in the decompile)
 *
 * Step 3 of the boot sequence, run from main_loop with interrupts masked:
 *
 *      rcc_set_apb1_prescaler(0x500)   -- PPRE1 = 0b101 = /4
 *      rcc_set_apb2_prescaler(0x500)   -- PPRE2 = 0b101 = /4
 *      tim2_init()                     -- 1 ms timebase + update interrupt
 *      sys_nop_stub()                  -- an empty function, dropped here
 *      rcc_apb1_periph_clock_cmd(0x10000000, ENABLE)   -- PWREN
 *
 * The first two calls are already reconstructed together as
 * rcc_set_bus_prescalers_div4() (hw_rcc.c) and the third as tim2_init()
 * (hw_tim.c); this is only the wrapper that keeps the stock ORDER. Order
 * matters: TIM2's prescaler arithmetic assumes PCLK1 is already divided, so
 * the prescalers must be in place before tim2_init() computes the reload.
 *
 * PWREN is enabled here and not in the shutdown path, even though the PWR
 * block is only ever touched by sys_shutdown_deep_sleep().
 * ==========================================================================*/
void clock_tim2_pwr_init(void)
{
    rcc_set_bus_prescalers_div4();              /* 0x080032d2 first half  */
    tim2_init();                                /* 0x080027f8             */
    rcc_apb1_periph_clock(RCC_APB1_PWR, 1);     /* 0x08005cb0, PWREN      */
}

/* ===========================================================================
 * PB12 -- CAN transceiver standby
 *
 * The transceiver (an NSI/TJA-class part) idles in standby with STB high; the
 * CAN bring-up pulls it low to put the transceiver in normal mode, and the
 * phase-1 give-up path puts it back high before the MCU sleeps so the quiescent
 * current drops.
 *
 * NOTE the stock decompile drops the third argument of the 0x08007440 call
 * (`gpio_write_pin(REG_GPIOB,0x1000)`); the instruction stream passes 1, hence
 * "standby ON = PB12 high".
 *
 * hw_can.c open-codes the 0x08007430 behaviour inline inside its bring-up
 * (gpio_write_pin(GPIOB, 0x1000, 0)) rather than calling this, so these two are
 * the named entry points for the shutdown path and for anything else that needs
 * the line. core.c carries a weak no-op gpio_can_xcvr_standby_on_pb12 that the
 * strong definition below replaces at link time.
 * ==========================================================================*/

/* --- 0x08007430 --- transceiver awake (normal mode) */
void gpio_can_xcvr_standby_pb12(void)
{
    gpio_write_pin(GPIOB, GPIO_PIN_12, 0);
}

/* --- 0x08007440 --- transceiver into standby */
void gpio_can_xcvr_standby_on_pb12(void)
{
    gpio_write_pin(GPIOB, GPIO_PIN_12, 1);
}

/* ===========================================================================
 * 0x080084be -- gpio_static_init_b
 *
 * The teardown counterpart of gpio_status_lines_init (0x08008458, hw_gpio.c):
 * drive the three remaining status outputs low, then hand PB3 and PB2 back to
 * the input state so nothing is being driven while the box sleeps.
 *
 * Differences from the init block, all reproduced verbatim:
 *   - PB5 (0x20) is NOT touched here, only PC15 / PB6 / PB4;
 *   - the pins are driven low but NOT reconfigured as outputs (they already
 *     are), while PB3 (0x8) and PB2 (0x4) ARE reconfigured, to mode 0 = input;
 *   - g_gpio_initialized is left alone.
 * ==========================================================================*/
void gpio_static_init_b(void)
{
    gpio_write_pin(GPIOC, GPIO_PIN_15, 0);
    gpio_write_pin(GPIOB, GPIO_PIN_6,  0);
    gpio_write_pin(GPIOB, GPIO_PIN_4,  0);

    gpio_init_pin(GPIOB, GPIO_PIN_3, GPIO_MODE_INPUT);
    gpio_init_pin(GPIOB, GPIO_PIN_2, GPIO_MODE_INPUT);
}

/* ===========================================================================
 * 0x080084f6 -- gpio_write_status_lines
 *
 * Called from main_loop on every 8 ms tick. Copies the low nibble of the
 * decoded-vehicle status byte g_status_block[0] (0x20001b49) out to the four
 * hardwired lines to the head unit:
 *
 *      bit 0 -> PB5        bit 1 -> PC15  (illumination)
 *      bit 2 -> PB6 (reverse gear)        bit 3 -> PB4
 *
 * The stock code extracts each bit with a mask-then-shift pair
 * ((b & 7) >> 2, b & 1, (b & 0xf) >> 3, (b & 3) >> 1), which is exactly
 * "bit n" for n = 2, 0, 3, 1. Written that way below so the intent is visible;
 * the generated code is identical. The call ORDER is kept as in the image
 * (PB6, PB5, PB4, PC15) in case anything downstream is edge-sensitive.
 * ==========================================================================*/
void gpio_write_status_lines(void)
{
    uint8_t s = g_status_block[0];

    gpio_write_pin(GPIOB, GPIO_PIN_6,  (s >> 2) & 1);   /* reverse       */
    gpio_write_pin(GPIOB, GPIO_PIN_5,   s       & 1);
    gpio_write_pin(GPIOB, GPIO_PIN_4,  (s >> 3) & 1);
    gpio_write_pin(GPIOC, GPIO_PIN_15, (s >> 1) & 1);   /* illumination  */
}

/* ===========================================================================
 * 0x08007da8 -- uart_D2_cluster_text_to_can
 *
 * CAN-TX side of the head unit's 0xD2 "cluster text" message. g_cmd_block
 * (0x20001be7) holds the parsed command: byte 0 is the message class and
 * bytes 1..8 are ASCII digits. The result is folded into three outbound frames
 * that the periodic scheduler then transmits:
 *
 *      g_tx_frame_190 (0x20001164)  byte 0 low 5 bits = message class code,
 *                                   byte 1 low 5 bits = sub-code
 *      g_tx_frame_18c (0x2000116b)  byte 1 low nibble / byte 2 = the 16-bit
 *                                   numeric value, high nibble preserved
 *      g_tx_frame_9f  (0x200011a1)  byte 0 = tx_frame_190[0] & 0x1f,
 *                                   bytes 1..2 = the same numeric value
 *
 * The top 3 bits of g_tx_frame_190[0]/[1] are owned by another writer and are
 * always preserved (& 0xE0).
 *
 * UNCERTAINTY, flagged rather than hidden:
 *   1. Ghidra shows this as taking an `undefined2 param_1`, but the only call
 *      site (can_periodic_tx_scheduler @0x08008110) passes nothing. param_1 is
 *      an UNINITIALISED register that the 0x06/0x07/0x0D/0x0E/0x15/0x16 branch
 *      reads when the digit test fails. Reconstructed as a local initialised to
 *      0, which is the only defined behaviour available; the stock image here
 *      emits whatever was last in that register.
 *   2. Classes 1..3 parse the units digit from g_cmd_block[8] while classes
 *      4..5 parse it from g_cmd_block[7], and both skip the digit in between.
 *      That asymmetry is in the image; it is reproduced, not corrected.
 * ==========================================================================*/

/* Shared tail of the 0x00..0x05 branches: fold `value` into 0x18c and 0x9f. */
static void d2_store_value(uint16_t value)
{
    uint8_t hi = (uint8_t)(value >> 8);
    uint8_t lo = (uint8_t)value;

    g_tx_frame_18c[1] = (uint8_t)((g_tx_frame_18c[1] & 0xF0u) | (hi & 0x0Fu));
    g_tx_frame_18c[2] = lo;

    g_tx_frame_9f[0] = (uint8_t)(g_tx_frame_190[0] & 0x1Fu);
    g_tx_frame_9f[1] = (uint8_t)(hi | 0x20u);
    g_tx_frame_9f[2] = lo;
}

/* g_tx_frame_190[0] = (old & 0xE0) | code  -- the stock form is "+ code", and
 * every code used is < 0x20, so OR and ADD are identical here. */
static void d2_set_class_code(uint8_t code)
{
    g_tx_frame_190[0] = (uint8_t)((g_tx_frame_190[0] & 0xE0u) + code);
}

static uint8_t d2_digit(uint8_t c)
{
    return (uint8_t)(c - 0x30u);
}

void uart_D2_cluster_text_to_can(void)
{
    /* See UNCERTAINTY note 1: uninitialised in the stock image. */
    uint16_t text_val = 0;
    uint16_t value;
    uint8_t  cls;

    g_d2_cluster_state = 0;

    cls = g_cmd_block[0];

    switch (cls) {
    case 0x00:
        d2_set_class_code(0x1D);
        break;

    case 0x01:
    case 0x02:
    case 0x03:
        d2_set_class_code(0x01);
        /* 4 digits when cmd[4] == '1', else 3; units digit from cmd[8]. */
        if (g_cmd_block[4] == '1')
            value = (uint16_t)(d2_digit(g_cmd_block[4]) * 1000u +
                               d2_digit(g_cmd_block[5]) * 100u +
                               d2_digit(g_cmd_block[6]) * 10u +
                               d2_digit(g_cmd_block[8]));
        else
            value = (uint16_t)(d2_digit(g_cmd_block[5]) * 100u +
                               d2_digit(g_cmd_block[6]) * 10u +
                               d2_digit(g_cmd_block[8]));
        d2_store_value(value);
        break;

    case 0x04:
    case 0x05:
        d2_set_class_code(0x00);
        /* same parse, but the units digit comes from cmd[7] here. */
        if (g_cmd_block[4] == '1')
            value = (uint16_t)(d2_digit(g_cmd_block[4]) * 1000u +
                               d2_digit(g_cmd_block[5]) * 100u +
                               d2_digit(g_cmd_block[6]) * 10u +
                               d2_digit(g_cmd_block[7]));
        else
            value = (uint16_t)(d2_digit(g_cmd_block[5]) * 100u +
                               d2_digit(g_cmd_block[6]) * 10u +
                               d2_digit(g_cmd_block[7]));
        d2_store_value(value);
        break;

    case 0x06:
    case 0x07:
    case 0x0D:
    case 0x0E:
    case 0x15:
    case 0x16:
        g_tx_frame_190[1] = (uint8_t)((g_tx_frame_190[1] & 0xE0u) + 3u);
        g_tx_frame_18c[1] = (uint8_t)(g_tx_frame_18c[1] & 0xF0u);
        g_tx_frame_18c[2] = 0;

        /* Every one of the six classes ends up writing code 4; class 7 also
         * overwrites the sub-code it just set to 3 with 8. (The stock code
         * spells this out as a nested if-chain over the same six values.) */
        d2_set_class_code(0x04);
        if (cls == 0x07)
            g_tx_frame_190[1] = (uint8_t)((g_tx_frame_190[1] & 0xE0u) + 8u);

        /* 3 ASCII digits at cmd[1..3], only if cmd[1] really is a digit. */
        if (g_cmd_block[1] >= 0x30 && g_cmd_block[1] <= 0x39) {
            text_val = (uint16_t)(d2_digit(g_cmd_block[1]) * 100u +
                                  d2_digit(g_cmd_block[2]) * 10u +
                                  d2_digit(g_cmd_block[3]));
            g_tx_frame_18c[1] = (uint8_t)((g_tx_frame_18c[1] & 0xF0u) |
                                          ((text_val >> 8) & 0x0Fu));
            g_tx_frame_18c[2] = (uint8_t)text_val;
        }

        /* NOTE: unlike d2_store_value(), byte 1 is NOT or-ed with 0x20 here. */
        g_tx_frame_9f[0] = (uint8_t)(g_tx_frame_190[0] & 0x1Fu);
        g_tx_frame_9f[1] = (uint8_t)(text_val >> 8);
        g_tx_frame_9f[2] = (uint8_t)text_val;
        break;

    case 0x0A:
        d2_set_class_code(0x08);
        break;

    case 0x0C:
        d2_set_class_code(0x06);
        g_tx_frame_9f[0] = (uint8_t)(g_tx_frame_190[0] & 0x1Fu);
        break;

    /* 0x08, 0x09, 0x0B, 0x0F..0x14, 0x17: explicit empty cases in the stock
     * jump table -- the message is accepted and deliberately ignored. */
    default:
        break;
    }
}

/* ===========================================================================
 * 0x08008110 -- can_periodic_tx_scheduler
 *
 * Reached once per 8 ms from can_autobaud_state_machine. Three parts:
 *
 *   1. ACC edge detect. g_status_block[0x12] mirrors bit 0 (ignition/ACC) of
 *      g_status_block[0]; on a change, g_tx_frame_9f[0] is primed with 1 (ACC
 *      on) or 7 (ACC off).
 *
 *   2. The hold timer g_can_tx_hold_timer. While ACC is on it is pinned at 400;
 *      when ACC drops it counts down one per call, i.e. 400 * 8 ms = 3.2 s of
 *      continued transmission after ACC goes away. Everything below only runs
 *      while that timer is non-zero.
 *
 *   3. The TX table. g_periodic_tx_phase free-runs 0..0x96 (151 phases x 8 ms
 *      = 1208 ms per lap) and fires one frame on each odd phase 1..0x0B, so
 *      each frame repeats about every 1.2 s -- except that it is really the
 *      scheduler's 8 ms cadence x the phase spacing that gives the "roughly
 *      400 ms" figure in the notes for the frames that the decoder also kicks.
 *      Profile 0 (83.333k) and profile 1 (125k) use different CAN IDs for the
 *      same five payload buffers; profile 1 has no 0x9f equivalent.
 *      Phase 0x0D is an empty case in the stock jump table (no frame).
 *
 *   4. The pending head-unit commands: g_cmd_block[0x0D] triggers the cluster
 *      text, g_cmd_block[0x10] triggers a tone/volume change (selector in
 *      [0x0E], value in [0x0F]) which is clamped, mirrored into the amp frame,
 *      the status block and the param page, and transmitted immediately.
 * ==========================================================================*/
void can_periodic_tx_scheduler(void)
{
    uint8_t acc;
    uint8_t sel;
    uint8_t val;

    acc = (uint8_t)(g_status_block[0] & 1u);

    /* --- 1. ACC edge detect ------------------------------------------- */
    if ((g_status_block[0x12] & 1u) != acc) {
        g_tx_frame_9f[0] = acc ? 1u : 7u;
        g_status_block[0x12] = acc;
    }

    /* --- 2. post-ACC hold timer --------------------------------------- */
    if (acc == 0) {
        if (g_can_tx_hold_timer != 0)
            g_can_tx_hold_timer--;
    } else {
        g_can_tx_hold_timer = 400;
    }

    if (g_can_tx_hold_timer == 0)
        return;

    /* --- 3. phase table ------------------------------------------------ */
    g_status_block[0] = (uint8_t)((g_status_block[0] & 0x7Fu) |
                                  (uint8_t)(g_profile_active << 7));

    if (g_periodic_tx_phase < 0x96u)
        g_periodic_tx_phase++;
    else
        g_periodic_tx_phase = 0;

    if (g_profile_active == 0) {
        switch (g_periodic_tx_phase) {
        case 0x01: can_tx_enqueue(0x3D0, 0, 7, (const uint8_t *)g_tx_frame_amp); break;
        case 0x03: can_tx_enqueue(0x0F0, 0, 4, (const uint8_t *)g_tx_frame_f0);  break;
        case 0x05: can_tx_enqueue(0x326, 0, 8, (const uint8_t *)g_tx_frame_326); break;
        case 0x07: can_tx_enqueue(0x190, 0, 7, (const uint8_t *)g_tx_frame_190); break;
        case 0x09: can_tx_enqueue(0x18C, 0, 8, (const uint8_t *)g_tx_frame_18c); break;
        case 0x0B: can_tx_enqueue(0x09F, 0, 8, (const uint8_t *)g_tx_frame_9f);  break;
        default:   break;   /* includes the empty stock case 0x0D */
        }
    } else if (g_profile_active == 1) {
        switch (g_periodic_tx_phase) {
        case 0x01: can_tx_enqueue(0x3D9, 0, 7, (const uint8_t *)g_tx_frame_amp); break;
        case 0x03: can_tx_enqueue(0x2E9, 0, 4, (const uint8_t *)g_tx_frame_f0);  break;
        case 0x05: can_tx_enqueue(0x290, 0, 8, (const uint8_t *)g_tx_frame_326); break;
        case 0x07: can_tx_enqueue(0x291, 0, 7, (const uint8_t *)g_tx_frame_190); break;
        case 0x09: can_tx_enqueue(0x293, 0, 8, (const uint8_t *)g_tx_frame_18c); break;
        default:   break;   /* 0x0B and 0x0D are empty cases in the image */
        }
    }

    /* --- 4a. pending cluster text -------------------------------------- */
    if (g_cmd_block[0x0D] == 1) {
        g_cmd_block[0x0D] = 0;
        uart_D2_cluster_text_to_can();
    }

    /* --- 4b. pending tone / volume change ------------------------------ */
    if (g_cmd_block[0x10] != 1)
        return;

    g_cmd_block[0x10] = 0;
    sel = g_cmd_block[0x0E];

    if (sel == 1) {
        /* volume: 0..0x26 */
        if (g_cmd_block[0x0F] > 0x26u)
            g_cmd_block[0x0F] = 0x26u;
    } else {
        /* tone (balance/fader/bass/mid/treble): 1..0x13 */
        if (g_cmd_block[0x0F] < 0x14u) {
            if (g_cmd_block[0x0F] == 0)
                g_cmd_block[0x0F] = 1;
        } else {
            g_cmd_block[0x0F] = 0x13u;
        }
    }

    val = g_cmd_block[0x0F];

    /* selector 1..6 -> amp frame byte 0..5, status block 0x2C..0x31, param
     * bytes 0x10..0x15 (PARAM_VOLUME then the five PARAM_TONE_* bytes). */
    if (sel >= 1u && sel <= 6u) {
        g_tx_frame_amp[sel - 1u]      = val;
        g_status_block[0x2Bu + sel]   = val;
        g_param_struct[0x0Fu + sel]   = val;
    }

    if (g_profile_active == 1)
        can_tx_enqueue(0x3D9, 0, 7, (const uint8_t *)g_tx_frame_amp);
    else
        can_tx_enqueue(0x3D0, 0, 7, (const uint8_t *)g_tx_frame_amp);
}

/* ===========================================================================
 * 0x0800755c -- can_autobaud_state_machine
 *
 * main_loop's 8 ms task, and the only thing that drives the CAN side once
 * phase 1 is over. Despite the name it is not really an autobaud machine any
 * more at this point (that is main_loop's phase 1); it is the bus-presence
 * supervisor plus the SWC reply pump.
 *
 * Sequence:
 *   1. can_profile_dispatch_gate() -- drain the RX ring through the active car
 *      profile decoder (can_decode.c).
 *   2. Two SWC key reply paths, both sending the 8-byte g_swc_tx_buf as CAN ID
 *      0x416:
 *        - g_swc_resp_mode == 1: immediate reply, then arm g_swc_flag_118b with
 *          0x28 (40 ticks = 320 ms, aged down by sys_misc_timers_tick).
 *        - g_swc_flag_118b == 1 (the timer reached 1): the repeat/release
 *          reply. Clears the current soft-timer slot, rescans for the next
 *          active key, and sends with byte 1 = the new key index.
 *   3. can_periodic_tx_scheduler().
 *   4. The CAN-error hold: bit 2 of g_status_block[0] (re)arms a 5-tick hold,
 *      and once it has expired bit 5 of g_status_block[0] is cleared. The
 *      asymmetry between the bit tested (2) and the bit cleared (5) is in the
 *      image and is reproduced as-is.
 *   5. The bus-presence state machine in g_can_bus_status:
 *
 *        0  probing     -> bus active: go to 1, arm 10 ticks
 *                       -> bus dead:   shut down, then force a watchdog reset
 *        1  settling    -> active: go to 2 and set g_swc_tx_buf[2] = 0x3F
 *        4  (re)settling   -> timer expired: back to 0
 *        2  running     -> bus lost: go to 4, g_swc_tx_buf[2] = 0xBF, 100 ticks
 *                       -> timer expired while still up: force a reset
 *        3  hello       -> emit 0x416 FE 16 3F FF FF FF FF FF, go to 2
 *        else           -> 1
 *
 *      State 3 is entered exactly once, by main_loop, the moment phase 1 ends.
 *      g_profile0_activity_ctr is the shared countdown, aged by
 *      can_autobaud_timers_tick (100 ms, hw_can.c).
 *
 * The two "force a reset" exits are wwdg_init() followed by wwdg_force_reset()
 * -- the stock decompile names them reinit_can_filters_and_state and
 * wwdg_force_reset_thunk, but 0x08002dec is the watchdog init (see hw_wwdg.c)
 * and the pair is simply "re-arm the WWDG, then spin until it bites".
 * ==========================================================================*/
void can_autobaud_state_machine(void)
{
    /* 1. run the decoder */
    can_profile_dispatch_gate();                   /* 0x08007d44 */

    /* 2. SWC replies */
    if (g_swc_resp_mode == 1) {
        g_swc_resp_mode = 0;
        can_tx_enqueue(0x416, 0, 8, (const uint8_t *)g_swc_tx_buf);
        g_swc_flag_118b = 0x28;                    /* 40 ticks */
    }

    if (g_swc_flag_118b == 1) {
        g_swc_flag_118b = 0;
        softtimer_clear_current();                 /* 0x08007ce0 */
        softtimer_scan_active_hi();                /* 0x08007d12 */
        g_swc_tx_buf[1] = g_swc_keydata_ref;
        can_tx_enqueue(0x416, 0, 8, (const uint8_t *)g_swc_tx_buf);
    }

    /* 3. periodic TX table */
    can_periodic_tx_scheduler();                   /* 0x08008110 */

    /* 4. CAN-error hold (test bit 2, clear bit 5 -- as in the image) */
    if (g_status_block[0] & 0x04u)
        g_canerr_hold_timer = 5;
    else if (g_canerr_hold_timer == 0)
        g_status_block[0] = (uint8_t)(g_status_block[0] & 0xDFu);

    /* 5. bus-presence state machine */
    switch (g_can_bus_status) {
    case 0:
        if (can_bus_active_flag() == 0) {
            sys_shutdown_deep_sleep();             /* 0x080032f6 */
            wwdg_init();                           /* 0x08002dec */
            wwdg_force_reset();                    /* 0x08002e26, noreturn */
        } else {
            g_can_bus_status = 1;
            g_profile0_activity_ctr = 10;
        }
        break;

    case 1:
    case 4:
        if (can_bus_active_flag() == 1) {
            g_can_bus_status = 2;
            g_swc_tx_buf[2] = 0x3F;
        } else if (g_profile0_activity_ctr == 0) {
            g_can_bus_status = 0;
        }
        break;

    case 2:
        if (can_bus_active_flag() == 0) {
            g_can_bus_status = 4;
            g_swc_tx_buf[2] = 0xBF;
            g_profile0_activity_ctr = 100;
        } else if (g_profile0_activity_ctr == 0) {
            wwdg_init();                           /* 0x08002dec */
            wwdg_force_reset();                    /* 0x08002e26, noreturn */
        }
        break;

    case 3:
        /* the one-shot "hello": 0x416 / DLC 8 / FE 16 3F FF FF FF FF FF */
        g_swc_tx_buf[0] = 0xFE;
        g_swc_tx_buf[1] = 0x16;
        g_swc_tx_buf[2] = 0x3F;
        g_swc_tx_buf[3] = 0xFF;
        g_swc_tx_buf[4] = 0xFF;
        g_swc_tx_buf[5] = 0xFF;
        g_swc_tx_buf[6] = 0xFF;
        g_swc_tx_buf[7] = 0xFF;
        can_tx_enqueue(0x416, 0, 8, (const uint8_t *)g_swc_tx_buf);
        g_can_bus_status = 2;
        break;

    default:
        g_can_bus_status = 1;
        break;
    }
}
