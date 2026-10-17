/* ============================================================================
 * app/hal/hal_usart.h -- USART register driver (HAL layer).
 *
 * LAYER CONTRACT: pure C11, MMIO only. No RAM state, no globals.h, no C++.
 * Every entry point carries the flash address of the stock function it
 * reproduces so the rebuild can be diffed against
 * reference/2429-H1N4JPF20A-220524.app.bin.
 *
 * THIS IS A NATIONS N32L406, NOT AN STM32. Register names/offsets come from
 * app/include/n32l406_hw.h (generated from the vendor SDK):
 *     USART_Module = { STS +0x00, DAT +0x04, BRCF +0x08,
 *                      CTRL1 +0x0C, CTRL2 +0x10, CTRL3 +0x14, GTP +0x18 }
 * all 16-bit with 16-bit reserved padding between them. Pin muxing is per-pin
 * AF numbers in GPIO AFL/AFH (PA2/PA3 = AF4), NOT an F1-style AFIO remap --
 * and it is not this layer's business either way.
 *
 * SCOPE. This unit is the register layer only: it knows how to poke a
 * USART_Module and nothing else. Everything the stock image hard-wired to
 * USART2 has been generalised to take a `USART_Module *usart`, so the board
 * policy -- which port, which pins, which clock, which NVIC line, which baud
 * rate -- lives one layer up, in the services layer:
 *
 *     NOT HERE: usart2_init      (0x0800291E: PA2/PA3 AF mux, APB1 clock gate,
 *                                 NVIC IRQ 38 prio 2/0, 38400 8N1)
 *     NOT HERE: USART2_IRQHandler(0x0800286E: the RX ring + TX cursor ISR)
 *     NOT HERE: the whole AA 55 framing/protocol above it.
 *
 * The services layer binds USART2 (0x40004400) to these calls, e.g.
 *     usart_tx_start(USART2);  usart_disable(USART2);
 *
 * Wiring and timing this driver is used with, for the record
 * (see docs/hiworld-firmware-analysis.md section 4):
 *     PA2 = USART2_TX -> head unit,  PA3 = USART2_RX <- head unit
 *     38400 8N1, no flow control, RXNE interrupt on, TXE interrupt on demand
 *     NVIC IRQ 38 (USART2_IRQn), pre-emption priority 2, sub 0
 * ==========================================================================*/

#ifndef HAL_USART_H
#define HAL_USART_H

#include <stdint.h>

#include "n32l406_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * USART interrupt/flag ids, encoded exactly as the vendor SDK does:
 *     bits [15:8] = bit number in STS
 *     bits  [7:5] = control-register selector (1 = CTRL1, 2 = CTRL2, else CTRL3)
 *     bits  [4:0] = enable-bit number in that control register
 *
 * Only the two the stock app actually arms are reproduced; the rest of the
 * vendor table has no call site in this image.
 * ------------------------------------------------------------------------- */
#define USART_INT_RXDNE         0x0525U   /* STS.RXDNE bit 5 <- CTRL1.RXDNEIEN b5 */
#define USART_INT_TXDE          0x0727U   /* STS.TXDE  bit 7 <- CTRL1.TXDEIEN  b7 */

/* ---- CTRL1 bits --------------------------------------------------------- */
#define USART_CTRL1_RE          0x0004U   /* receiver enable                 */
#define USART_CTRL1_TE          0x0008U   /* transmitter enable              */
#define USART_CTRL1_UE          0x2000U   /* USART enable                    */

/* CTRL1/2/3 field masks used by the stock USART_Init @0x0800707E. The stock
 * driver clears fields with ABSOLUTE masks rather than ~field, so they are
 * reproduced verbatim and the controller sees exactly the stock writes. */
#define USART_CTRL1_INIT_MASK   0xE9F3U   /* clears TE|RE, PS, PCE, M        */
#define USART_CTRL2_INIT_MASK   0xCFFFU   /* clears STOP[1:0]                */
#define USART_CTRL3_INIT_MASK   0xFCFFU   /* clears CTSEN, RTSEN             */
#define USART_MODE_TX_RX        (USART_CTRL1_TE | USART_CTRL1_RE)  /* 0x000C */

/* The data register is 9 bits wide on this part (0x08007246 / 0x0800724E). */
#define USART_DAT_MASK          0x01FFU

/* STS.TXC -- transmission complete: the shift register has finished, not just
 * the data register (that is TXDE, bit 7). Only the drain below reads it. */
#define USART_STS_TXC           0x0040U   /* bit 6 */

/* ---- API ----------------------------------------------------------------
 * `it` is one of the USART_INT_* ids above. `enable` is a plain 0/1 flag. */

/* Both the enable bit and the status flag must be set -> non-zero. */
int     usart_get_it_status(USART_Module *usart, uint32_t it);   /* 0x08007320 */

/* Set/clear the enable bit only; never touches STS. */
void    usart_it_config(USART_Module *usart, uint32_t it, int enable);
                                                                 /* 0x08007184 */
/* CTRL1.UE. */
void    usart_cmd_enable(USART_Module *usart, int enable);       /* 0x0800716C */

void    usart_send_data(USART_Module *usart, uint8_t data);      /* 0x08007246 */
uint8_t usart_receive_data(USART_Module *usart);                 /* 0x0800724E */

/* 8 data bits, 1 stop bit, no parity, no hardware flow control, TX+RX on --
 * i.e. USART_Init @0x0800707E reduced to what the stock usart2_init asks for.
 *
 * `pclk_hz` is the APB clock feeding THIS usart and must be supplied by the
 * caller: the HAL has no business knowing the clock tree. The stock code took
 * PCLK2 for USART1 and PCLK1 for everyone else; USART2 is on APB1, so the
 * services layer passes PCLK1 = 16 MHz and baud = 38400. */
void    usart_configure(USART_Module *usart, uint32_t pclk_hz, uint32_t baud);

/* --- 0x08003078 (stock uart_tx_start, USART2-specific) --- arm the TXE
 * interrupt; the ISR takes it from here. */
void    usart_tx_start(USART_Module *usart);

/* Spin until the transmitter has clocked the last stop bit out of the shift
 * register (STS.TXC), or until `spins` iterations have elapsed, whichever
 * comes first.
 *
 * NOT a stock routine -- the original firmware never waited for anything on
 * this port. It exists for the IAP escape hatch: the head unit's reboot
 * command is answered with an immediate watchdog reset, and a byte still in
 * flight at that instant arrives at the radio AHEAD of the bootloader's 'U'
 * greeting. The radio compares the first byte of an untrimmed 64-byte read
 * against 'U', so that stray byte makes the handshake miss and the 1 Hz
 * retry can land inside the next window and loop.
 *
 * BOUNDED on purpose: this runs on a path that is about to reset the part,
 * with the window watchdog armed at ~65 ms. One byte at 38400 8N1 takes
 * ~260 us, so the bound only has to cover a byte and a half; it must never
 * be able to hold the CPU long enough for the watchdog to fire first and
 * turn a tidy reboot into an unexplained one. */
void    usart_tx_drain(USART_Module *usart, uint32_t spins);

/* --- 0x08003088 (stock uart_disable_usart2) --- shut the port down: clear
 * CTRL1.UE, then disarm TXE. Used by the sleep path. */
void    usart_disable(USART_Module *usart);

#ifdef __cplusplus
}
#endif

#endif /* HAL_USART_H */
