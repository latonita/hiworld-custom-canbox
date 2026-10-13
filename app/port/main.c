/* ============================================================================
 * main.c -- main_loop @0x080088d8, the app's entry point.
 *
 * Reconstructed from app/src/sys.c (Ghidra output) and cross-checked against
 * docs/hiworld-startup-sequence.md §1. Called from _start_c, which the
 * bootloader reaches via `bl 0x08002202`.
 *
 * Two phases:
 *
 *   PHASE 1 (autobaud).  Interrupts are unmasked and the loop spins until three
 *   CAN frames have landed in the RX ring. Every 2048 ms it toggles the stored
 *   profile byte, rewrites the param page, and re-inits CAN at the other baud
 *   (83.333 kbit/s <-> 125 kbit/s), up to 8 attempts. After the 8th failure it
 *   takes the shutdown path: CAN sleep -> peripherals off -> WFI. On a bench
 *   with no CAN bus that is ~16.4 s after boot, and the box goes quiet -- this
 *   is expected, not a fault.
 *
 *   PHASE 2 (steady superloop).  Latches the winning profile and services three
 *   scheduler tick flags raised by the 1 ms TIM2 ISR:
 *       fast (4 ms)  : status-change detect, CAN TX drain, soft-timer scan
 *       med  (8 ms)  : CAN decode state machine, UART RX/TX, GPIO status lines
 *       slow (1024 ms): param-page save if dirty
 *
 * The watchdog is kicked at the top of BOTH loops. It is armed with a ~65.5 ms
 * timeout, so every path through either loop must stay well under that.
 *
 * Runs on the bootloader's vector table (VTOR = 0x08000000, MSP = 0x20000600);
 * this function never returns.
 * ==========================================================================*/

#include "port.h"
#include "app_glue.h"
#include "can_decode.h"
#include "core.h"
#include "hw_can.h"
#include "hw_gpio.h"
#include "hw_tim.h"
#include "hw_uart.h"
#include "param.h"
#include "util.h"

/* Param block byte PARAM_PROFILE (=1, from param.h) is the CAN profile selector:
 * 0 = 83.333 kbit/s (this box, Jeep WK / JPF2), 1 = 125 kbit/s (FCA variant). */
#define PROFILE_BYTE    (g_param_struct[PARAM_PROFILE])

void main_loop(void)
{
    /* ---- init block: interrupts masked throughout -------------------- */
    irq_disable();

    param_page_load();              /* 0x08002dbc  32 bytes <- flash page   */
    clock_tim2_pwr_init();          /* 0x080032d2  PPRE1/2 = /4, 1 ms tick  */
    gpio_status_lines_init();       /* 0x08008458  PB5/PC15/PB6/PB4, PB2 in */
    sys_soft_clock_init();          /* 0x080078da  zero the soft RTC        */
    uart_init_38400_and_version();  /* 0x08008a54  USART2 + version string  */
    param_validate_defaults();      /* 0x08007b6c  magic 0xAA, clamp, prime */
    can_set_baud_by_profile();      /* 0x08007b2c  first CAN bring-up       */
    wwdg_init();                    /* 0x08002dec  arm the watchdog         */

    g_autobaud_try_count = 0;

    irq_enable();                   /* the clock starts here */

    /* ---- PHASE 1: wait for the bus, retrying the baud rate ----------- */
    while (wwdg_kick(), g_can_rx_ring_head < 3) {
        if (g_autobaud_tick_flag == 1) {          /* every 2048 ms */
            g_status_block[0] = (g_status_block[0] & 0xBF) | 0x40;

            if (g_autobaud_try_count < 8) {
                /* toggle the profile: 1 -> 0, anything else -> 1 */
                PROFILE_BYTE = (PROFILE_BYTE != 1);
                param_page_save();

                irq_disable();
                can_set_baud_by_profile();
                irq_enable();

                g_autobaud_try_count++;
                g_autobaud_tick_flag = 0;
            } else {
                /* Give up. Note the stock code deliberately does NOT clear
                 * g_autobaud_tick_flag here, so this runs on every pass until
                 * the WFI inside takes effect. */
                sys_shutdown_deep_sleep();        /* 0x080032f6 */
            }
        }
    }

    /* ---- the bus answered: latch the winning profile ----------------- */
    g_profile_active = PROFILE_BYTE;
    param_validate_defaults();
    can_apply_bitrate_B();          /* 0x08007b4c  re-init at the winner */
    g_can_bus_status = 3;
    g_bus_present_debounce = 800;

    /* ---- PHASE 2: steady superloop ----------------------------------- */
    for (;;) {
        wwdg_kick();

        /* edge-detect on the status block's change counter */
        if (g_status_block[0x11] != g_status_block[0x10]) {
            g_status_block[0x11] = g_status_block[0x10];
            g_status_block[0x0F]++;
        }

        if (g_tick_flag_fast) {                   /* 4 ms */
            g_tick_flag_fast = 0;
            status_bit_change_detect();           /* 0x08007858 */
            can_tx_ring_drain();                  /* 0x08007698 */
            softtimer_scan_active_hi();           /* 0x08007d12 */
        }

        if (g_tick_flag_med) {                    /* 8 ms */
            g_tick_flag_med = 0;
            can_autobaud_state_machine();         /* 0x0800755c */
            uart_rx_tx_service();                 /* 0x08008fe2 */
            gpio_write_status_lines();            /* 0x080084f6 */
        }

        if (g_tick_flag_slow) {                   /* 1024 ms */
            g_tick_flag_slow = 0;
            param_page_save();                    /* 0x08002d46 */
        }
    }
}
