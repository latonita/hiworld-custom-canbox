/* ============================================================================
 * app_glue.h -- the application-logic glue between the ported subsystems.
 *
 * Seven stock functions sit on the boundary between can.c, gpio.c, uart.c and
 * tim.c and belong to none of them cleanly; they are reconstructed in
 * app_glue.c. Everything here is on the live path -- main_loop calls four of
 * these directly, and the CAN bring-up and the shutdown path call the rest.
 *
 * Addresses in the comments are the ORIGINAL flash addresses of the stock
 * functions this file reproduces.
 * ==========================================================================*/

#ifndef APP_GLUE_H
#define APP_GLUE_H

#include <stdint.h>

/* ---- step-3 bring-up (tim.c) --------------------------------------------
 * Bus prescalers to /4, TIM2 timebase + 1 ms interrupt, then the PWR clock.
 * Thin wrapper: the two halves live in hw_rcc.c and hw_tim.c. Called once from
 * main_loop's init block, with interrupts masked. */
void clock_tim2_pwr_init(void);                /* 0x080032d2 */

/* ---- CAN transceiver standby line, PB12 (can.c) -------------------------
 * PB12 LOW  = transceiver awake (normal mode), driven during CAN bring-up.
 * PB12 HIGH = transceiver in standby, driven by the phase-1 give-up path.
 * The "_on_" spelling is the one core.c already references (it carries a weak
 * no-op stub that this file's strong definition replaces). */
void gpio_can_xcvr_standby_pb12(void);         /* 0x08007430  PB12 = 0 */
void gpio_can_xcvr_standby_on_pb12(void);      /* 0x08007440  PB12 = 1 */

/* ---- GPIO fan-out (gpio.c) ----------------------------------------------
 * Per 8 ms tick: copy four bits of the decoded-CAN status byte g_status_block[0]
 * out to the hardwired lines.
 *      bit 0 -> PB5      bit 1 -> PC15 (illumination)
 *      bit 2 -> PB6 (reverse)   bit 3 -> PB4 */
void gpio_write_status_lines(void);            /* 0x080084f6 */

/* Second static GPIO init block: status outputs forced low, PB3 and PB2 back
 * to inputs. Used only by the shutdown path (core.c calls it; it carries a
 * weak no-op stub that this file's strong definition replaces). */
void gpio_static_init_b(void);                 /* 0x080084be */

/* ---- CAN application logic (can.c) --------------------------------------
 * The phase-2 8 ms task: runs the CAN decoder, flushes the SWC key replies,
 * runs the periodic TX table, and drives the five-state bus-presence machine
 * in g_can_bus_status. In state 3 (entered once by main_loop when the bus
 * answers) it emits the 0x416 "hello" frame FE 16 3F FF FF FF FF FF and drops
 * to state 2. */
void can_autobaud_state_machine(void);         /* 0x0800755c */

/* Free-running periodic CAN TX table, stepped once per call (so once per 8 ms)
 * through 151 phases -> each frame goes out roughly every 400 ms (0x3d0/0x3d9,
 * 0xf0/0x2e9, 0x326/0x290, 0x190/0x291, 0x18c/0x293, 0x9f). Also applies the
 * head unit's pending tone/volume command to the amplifier frame. */
void can_periodic_tx_scheduler(void);          /* 0x08008110 */

/* ---- cluster text (uart.c) ----------------------------------------------
 * CAN-TX side of the head unit's 0xD2 cluster-text message: turns the ASCII
 * command block at g_cmd_block into the three outbound frames
 * g_tx_frame_190 / g_tx_frame_18c / g_tx_frame_9f. Driven from
 * can_periodic_tx_scheduler when g_cmd_block[0x0D] goes to 1. */
void uart_D2_cluster_text_to_can(void);        /* 0x08007da8 */

#endif /* APP_GLUE_H */
