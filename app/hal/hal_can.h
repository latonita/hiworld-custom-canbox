/* ============================================================================
 * app/hal/hal_can.h -- bxCAN peripheral register driver (HAL layer).
 *
 * LAYER CONTRACT: pure C11, MMIO only. No RAM state, no globals.h, no C++.
 * Every entry point carries the flash address of the stock function it
 * reproduces so the rebuild can be diffed against
 * reference/2429-H1N4JPF20A-220524.app.bin.
 *
 * Extracted from the retired app/port/hw_can.c. The stock image is built on the ST
 * StdPeriph CAN driver, so everything here is a verbatim StdPeriph function
 * (CAN_DeInit / CAN_StructInit / CAN_Init / CAN_FilterInit / CAN_ITConfig /
 * CAN_Transmit / CAN_Receive / CAN_Sleep / CAN_WakeUp) and the three config
 * structs below are byte-for-byte the StdPeriph ones (CAN_InitTypeDef /
 * CAN_FilterInitTypeDef / CanTxMsg / CanRxMsg) -- the stock code addresses
 * them by raw offset, so the layouts are LOAD-BEARING and must not be
 * reordered.
 *
 * What is deliberately NOT here (it is stateful or board policy, and lives in
 * the services layer): CAN bring-up (pin mux + NVIC + accept-all filter
 * policy), the bitrate/profile wrappers, the two 50-entry RAM ring buffers and
 * their head/tail bytes, CAN_RX0_IRQHandler, the g_can_err_flags bits and the
 * auto-baud hold-off timers. This file touches nothing but registers.
 *
 * ---------------------------------------------------------------------------
 * BIT TIMING -- derivation (the arithmetic behind can_init_regs' BTIM write)
 * ---------------------------------------------------------------------------
 * can_init_regs @0x0800398c programs:
 *     BTIM = (mode << 30) | (SJW << 24) | (BS2 << 20) | (BS1 << 16) | (BRP - 1)
 * with the constants the stock bring-up stores into its CAN_InitTypeDef:
 *     SJW = 0  -> SJW field 0 ->  1 tq
 *     BS1 = 3  -> TS1 field 3 ->  4 tq
 *     BS2 = 2  -> TS2 field 2 ->  3 tq
 *     BRP = 2000 / baud_kbps   (integer division, done by the caller)
 *
 * Nominal bit = SYNC_SEG(1) + TSEG1(BS1+1 = 4) + TSEG2(BS2+1 = 3) = 8 tq,
 * sample point = (1 + 4) / 8 = 62.5 %, resynchronisation jump width = 1 tq.
 *
 *     bitrate = f_APB1 / (BRP * 8)
 *
 * With f_APB1 = PCLK1_HZ = 16 MHz (SYSCLK 64 MHz, APB1 = /4 -- see hal_rcc.h):
 *     125 kbit/s: BRP = 2000/125 = 16 -> 16e6 / (16 * 8) = 125.000 kbit/s
 *                 BTIM = 0x0023000F   (tq = 1.0 us, bit = 8 us)
 *      83 kbit/s: BRP = 2000/83  = 24 (truncated!) -> 16e6/(24*8) = 83.333 kbit/s
 *                 BTIM = 0x00230017   (tq = 1.5 us, bit = 12 us)
 * i.e. the 0x53 (= 83) argument is deliberately sloppy: the truncation is what
 * produces the exact 83 1/3 kbit/s the vehicle bus actually uses. Both rates
 * come out exact ONLY at f_APB1 = 2000 * 8 * 1000 = 16 MHz, which independently
 * confirms the APB1 prescaler.
 *
 * In silent mode mode = 2, so BTIM bit 31 (SILM) is set and bit 30 (LBKM) is
 * clear.
 *
 * Addresses in comments are the ORIGINAL flash addresses.
 * ==========================================================================*/

#ifndef HAL_CAN_H
#define HAL_CAN_H

#include <stdint.h>
#include "n32l406_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- mode values for the BTIM[31:30] field (StdPeriph CAN_Mode) ---------- */
#define CAN_MODE_NORMAL          0x00U
#define CAN_MODE_LOOPBACK        0x01U
#define CAN_MODE_SILENT          0x02U
#define CAN_MODE_SILENT_LOOPBACK 0x03U

/* ---- IDE values the stock code puts in CanTxMsg.ide / reads from .ide ----
 * NOTE these are the raw RMI/TMI bit values (0 / 4), NOT 0 / 1: can_receive_fifo
 * stores `rmi & 4` straight into .ide and can_transmit ORs .ide back into TMI. */
#define CAN_ID_STANDARD          0x00U
#define CAN_ID_EXTENDED          0x04U

/* ---- CAN_IT_* bits in INTE (only FMP0 is ever enabled) ------------------- */
#define CAN_IT_FMP0              0x00000002U

/* ---- can_transmit() return: StdPeriph CAN_TxStatus_NoMailBox ------------- */
#define CAN_TX_NO_MAILBOX        4U

/* ---- StdPeriph-compatible config structs --------------------------------
 * These are the ST StdPeriph layouts. The stock code reads them by raw byte
 * offset, so field order and padding are part of the ABI: DO NOT REORDER. */

/* CAN_InitTypeDef, 12 bytes. can_init_regs() reads it by offset. */
typedef struct {
    uint16_t prescaler;   /* +0  BRP + 1                                    */
    uint8_t  mode;        /* +2  CAN_MODE_* -> BTIM[31:30]                  */
    uint8_t  sjw;         /* +3  BTIM[25:24], tq = sjw + 1                  */
    uint8_t  bs1;         /* +4  BTIM[19:16], tq = bs1 + 1                  */
    uint8_t  bs2;         /* +5  BTIM[22:20], tq = bs2 + 1                  */
    uint8_t  ttcm;        /* +6  MCTRL bit 7                                */
    uint8_t  abom;        /* +7  MCTRL bit 6                                */
    uint8_t  awum;        /* +8  MCTRL bit 5                                */
    uint8_t  nart;        /* +9  MCTRL bit 4                                */
    uint8_t  rflm;        /* +10 MCTRL bit 3                                */
    uint8_t  txfp;        /* +11 MCTRL bit 2                                */
} can_init_t;

/* CAN_FilterInitTypeDef, 14 bytes. */
typedef struct {
    uint16_t id_high;         /* +0  */
    uint16_t id_low;          /* +2  */
    uint16_t mask_id_high;    /* +4  */
    uint16_t mask_id_low;     /* +6  */
    uint16_t fifo_assignment; /* +8  0 = FIFO0, 1 = FIFO1                   */
    uint8_t  number;          /* +10 filter bank 0..13                      */
    uint8_t  mode;            /* +11 0 = id/mask, 1 = id list               */
    uint8_t  scale;           /* +12 0 = 2 x 16 bit, 1 = 1 x 32 bit         */
    uint8_t  activation;      /* +13 1 = enable the bank                    */
} can_filter_init_t;

/* CanTxMsg / CanRxMsg, 20 bytes. */
typedef struct {
    uint32_t std_id;      /* +0  */
    uint32_t ext_id;      /* +4  */
    uint8_t  ide;         /* +8  CAN_ID_STANDARD / CAN_ID_EXTENDED          */
    uint8_t  rtr;         /* +9  */
    uint8_t  dlc;         /* +10 */
    uint8_t  data[8];     /* +11 */
} can_tx_msg_t;

typedef struct {
    uint32_t std_id;      /* +0  */
    uint32_t ext_id;      /* +4  */
    uint8_t  ide;         /* +8  */
    uint8_t  rtr;         /* +9  */
    uint8_t  dlc;         /* +10 */
    uint8_t  data[8];     /* +11 */
    uint8_t  fmi;         /* +19 matched filter index                       */
} can_rx_msg_t;

/* ---- API (verbatim StdPeriph behaviour) ---------------------------------- */

/* CAN_DeInit: reset pulse on the APB1 CAN bit. Call it AFTER the CAN clock has
 * been gated on, so every re-baud starts from a known register state. */
void    can_deinit(CAN_Module *canx);                                /* 0x08003974 */

/* CAN_StructInit: StdPeriph defaults (normal mode, 1/4/3 tq, prescaler 1). */
void    can_struct_init_defaults(can_init_t *init);                  /* 0x08003ba2 */

/* CAN_Init: apply MCTRL option bits + BTIM. Returns 1 on success, 0 if the
 * controller would not enter or leave initialisation mode in 0xFFFF polls. */
int     can_init_regs(CAN_Module *canx, const can_init_t *init);     /* 0x0800398c */

/* CAN_FilterInit: program one acceptance-filter bank. Takes no base pointer --
 * the stock function hard-codes the single CAN instance's filter block. */
void    can_filter_init(const can_filter_init_t *filt);              /* 0x08003aa0 */

/* CAN_ITConfig: set/clear `it` (CAN_IT_*) in INTE. */
void    can_it_config(CAN_Module *canx, uint32_t it, int enable);    /* 0x0800407e */

/* CAN_Transmit: returns the mailbox used (0..2) or CAN_TX_NO_MAILBOX (4).
 * `msg` is non-const on purpose -- the stock driver writes msg->dlc back
 * masked to 4 bits. Do not add const. */
uint8_t can_transmit(CAN_Module *canx, can_tx_msg_t *msg);           /* 0x08003c4e */

/* CAN_Receive: pop one message from FIFO `fifo` (0 or 1) and release it. */
void    can_receive_fifo(CAN_Module *canx, int fifo, can_rx_msg_t *msg); /* 0x08003e2e */

/* CAN_Sleep / CAN_WakeUp. Both return 1 on success. */
int     can_sleep(CAN_Module *canx);                                 /* 0x0800400e */
int     can_wakeup(CAN_Module *canx);                                /* 0x0800402c */

#ifdef __cplusplus
}
#endif

#endif /* HAL_CAN_H */
