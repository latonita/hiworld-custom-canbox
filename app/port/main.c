/* ============================================================================
 * main.c -- the app's entry point.
 *
 * *** NOT YET PORTED -- THIS IMAGE IS NOT A FUNCTIONAL REPLACEMENT. ***
 *
 * The real main_loop is at 0x080088d8 in the stock image and is documented in
 * docs/hiworld-startup-sequence.md §1 steps 5-23. Reconstructing it needs the
 * subsystems below to land first; until then this file only exists so the
 * image links, and so the preamble/entry/packaging path can be validated
 * end to end against the hardware constraints.
 *
 * Stock main_loop, in order (the porting checklist):
 *    1  CPSID i                                       mask interrupts for init
 *    2  param_page_load_0801F800   0x08002dbc         32 config bytes <- flash
 *    3  clock + TIM2 + PWR         0x080032d2         PPRE1/2 = /4, 1 ms tick
 *    4  status GPIOs               0x08008458         PB5/PC15/PB6/PB4 out, PB2 in
 *    5  soft RTC zero              0x080078da
 *    6  uart_init_38400_and_version 0x08008a54        USART2 38400 8N1 PA2/PA3
 *    7  param validate/defaults    0x08007b6c         magic 0xAA, CAN 0x416 payload
 *    8  first CAN bring-up         0x08007b2c         83.333k or 125k by profile
 *    9  wwdg_init                  0x08002dec         <-- ported (hw_wwdg.c)
 *   10  CPSIE i                                       clock starts here
 *   11  phase 1: spin until 3 CAN frames arrive; every 2048 ms toggle profile,
 *       rewrite the param page, re-init CAN at the other baud (max 8 tries)
 *   12  no bus after ~16.4 s -> CAN sleep -> WFI deep sleep   0x080032f6
 *   13  phase 2: tick-flag driven scheduler (CAN decode, CAN TX pump, UART TX)
 *
 * Subsystem port status (app/port/):
 *      rcc    DONE   hw_rcc.c
 *      wwdg   DONE   hw_wwdg.c
 *      sys    partial  sys.c (fatal path only)
 *      gpio / tim / uart / can / can_rx_decode / flash / core   TODO
 * ==========================================================================*/

#include "port.h"

void main_loop(void)
{
    /* Deliberately explicit: an unported image must not pretend to run. Trap
     * with a code outside the stock 100..164 range so it is unmistakable if
     * this ever reaches a bench. fatal_error_reset resets via the watchdog,
     * which hands control back to the IAP bootloader -- so the box stays
     * flashable rather than sitting in a dead loop. */
    fatal_error_reset(250);
}
