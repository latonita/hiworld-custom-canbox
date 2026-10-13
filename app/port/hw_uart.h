/* ============================================================================
 * hw_uart.h -- USART2 driver + head-unit "AA 55" framing/protocol.
 *
 * Ported from app/src/uart.c + the usart2_init half of app/src/misc.c (Ghidra
 * output). Every declaration carries the original flash address of what it
 * reproduces.
 *
 * The box talks to the Android head unit over USART2 (PA2 = TX, PA3 = RX) at
 * 38400 8N1. Framing, verified against captures, the Android plugin and the
 * decompile (docs/notes/hiworld-frame-format.md, docs/hiworld-firmware-
 * analysis.md section 4):
 *
 *      AA 55 | LEN | CMD | payload[LEN] | CKS          wire length = LEN + 5
 *
 *   - LEN counts the PAYLOAD ONLY (no preamble, no CMD, no checksum).
 *   - CMD is at offset 3, i.e. AFTER the length byte (this is what separates
 *     Hiworld from the sibling "Raise" protocol).
 *   - CKS = (sum of all bytes incl. the AA 55 preamble) & 0xFF, which the
 *     firmware computes in the algebraically identical form
 *          CKS = (LEN + CMD + sum(payload) - 1) & 0xFF
 *     because AA + 55 = 0xFF == -1 (mod 256).
 *   - There is NO escaping/stuffing anywhere: a payload byte may legitimately
 *     be 0xAA or 0x55. Resynchronisation relies purely on the LEN/checksum
 *     check failing and the parser restarting at state 0.
 *   - Only the AA 55 dialect is accepted; 5A A5 is not.
 * ==========================================================================*/

#ifndef HW_UART_H
#define HW_UART_H

#include <stdint.h>

/* ---- protocol constants ------------------------------------------------- */
#define UART_PREAMBLE_0     0xAAU
#define UART_PREAMBLE_1     0x55U
#define UART_MAX_PAYLOAD    0x37U   /* parser rejects LEN >= 0x38 (56) */
#define UART_FRAME_OVERHEAD 5U      /* AA 55 LEN CMD ... CKS */

/* ---- layout of the shared working buffer g_uart_frame_buf @0x200019F8 ----
 * [0x00..0x3B] outbound frame being clocked out by the ISR
 * [0x3C]       tx length (total wire bytes)
 * [0x3D]       tx cursor
 * [0x3E]       tx busy flag (1 = a frame is in flight)
 * [0x3F..0x106] 200-byte RX ring filled by USART2_IRQHandler
 * [0x107]      RX ring head (written by the ISR)
 * [0x108]      RX ring tail (written by the parser)                       */
#define UART_FB_TX_LEN      0x3CU
#define UART_FB_TX_POS      0x3DU
#define UART_FB_TX_BUSY     0x3EU
#define UART_FB_RX_RING     0x3FU
#define UART_FB_RX_HEAD     0x107U
#define UART_FB_RX_TAIL     0x108U
#define UART_RX_RING_SIZE   200U

/* ---- driver ------------------------------------------------------------- */
void usart2_init(uint32_t baud);                 /* 0x0800291E            */
void uart_tx_start(void);                        /* 0x08003078 TXEIE = 1  */
void uart_disable_usart2(void);                  /* 0x08003088            */
void USART2_IRQHandler(void);                    /* 0x0800286E, IRQ 38    */

/* ---- protocol layer ----------------------------------------------------- */
void uart_reset_state(void);                     /* 0x08008A18            */
void uart_init_38400_and_version(void);          /* 0x08008A54            */
void uart_rx_parser_AA55(void);                  /* 0x08008BB2            */
void uart_tx_ack_frame(uint8_t cmd, uint8_t arg);/* 0x08008CCE            */
void uart_build_frame_AA55(uint8_t slot);        /* 0x08008D04            */
void uart_tx_round_robin(void);                  /* 0x08008D9C            */
void uart_tx_sched(void);                        /* 0x08008E62            */
void uart_rx_tx_service(void);                   /* 0x08008FE2            */

#endif /* HW_UART_H */
