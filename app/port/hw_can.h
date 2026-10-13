/* ============================================================================
 * hw_can.h -- bxCAN controller driver + the two 50-entry CAN ring buffers.
 *
 * Ported from app/src/can.c (Ghidra output). The stock image is built on the
 * ST StdPeriph CAN driver, so the three config structs below are byte-for-byte
 * the StdPeriph ones (CAN_InitTypeDef / CAN_FilterInitTypeDef / CanTx|RxMsg) --
 * the stock code addresses them by raw offset, so the layouts are load-bearing
 * and must not be reordered.
 *
 * Addresses in comments are the ORIGINAL flash addresses.
 * ==========================================================================*/

#ifndef HW_CAN_H
#define HW_CAN_H

#include <stdint.h>
#include "n32l406_hw.h"

/* ---- mode values for the BTIM[31:30] field (StdPeriph CAN_Mode) ---------- */
#define CAN_MODE_NORMAL          0x00U
#define CAN_MODE_LOOPBACK        0x01U
#define CAN_MODE_SILENT          0x02U
#define CAN_MODE_SILENT_LOOPBACK 0x03U

/* ---- IDE values the stock code puts in CanTxMsg.ide / reads from .ide ---- */
#define CAN_ID_STANDARD          0x00U
#define CAN_ID_EXTENDED          0x04U

/* ---- CAN_IT_* bits in INTE (only FMP0 is ever enabled) ------------------- */
#define CAN_IT_FMP0              0x00000002U

/* ---- can_transmit() return: StdPeriph CAN_TxStatus_NoMailBox ------------- */
#define CAN_TX_NO_MAILBOX        4U

/* ---- g_can_err_flags bits (0x20001138) ---------------------------------- */
#define CAN_ERR_RX_RING_FULL     0x01U   /* RX ISR had < 2 free slots       */
#define CAN_ERR_TX_NO_MAILBOX    0x02U   /* TX pump found all 3 MBs busy    */
#define CAN_ERR_TX_RING_FULL     0x04U   /* can_tx_enqueue() dropped a frame*/

/* ---- ring geometry (both rings: 50 entries x 16 bytes) ------------------- */
#define CAN_RING_ENTRIES         50U
#define CAN_RING_STRIDE          16U

/* Ring entry layout (identical for RX and TX):
 *   [0..1] u16  ID bits 31..16   [2..3] u16  ID bits 15..0
 *   [4]    u8   IDE flag (0 = standard, non-zero = extended)
 *   [5]    u8   DLC
 *   [6..13] u8  data[0..7]       [14..15] pad                              */

/* ---- StdPeriph-compatible config structs -------------------------------- */

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

/* ---- HAL layer (verbatim StdPeriph behaviour) --------------------------- */
void    can_deinit(CAN_Module *canx);                                /* 0x08003974 */
void    can_struct_init_defaults(can_init_t *init);                  /* 0x08003ba2 */
int     can_init_regs(CAN_Module *canx, const can_init_t *init);     /* 0x0800398c */
void    can_filter_init(const can_filter_init_t *filt);              /* 0x08003aa0 */
void    can_it_config(CAN_Module *canx, uint32_t it, int enable);    /* 0x0800407e */
uint8_t can_transmit(CAN_Module *canx, can_tx_msg_t *msg);           /* 0x08003c4e */
void    can_receive_fifo(CAN_Module *canx, int fifo, can_rx_msg_t *msg); /* 0x08003e2e */
int     can_sleep(CAN_Module *canx);                                 /* 0x0800400e */
int     can_wakeup(CAN_Module *canx);                                /* 0x0800402c */

/* ---- bring-up ----------------------------------------------------------- */
void can_init(int baud_kbps, uint8_t mode);                          /* 0x08002a90 */
void can_init_normal_mode(int baud_kbps);                            /* 0x08002b76 */
void can_init_silent_mode(int baud_kbps);                            /* 0x08002b84 */
void can_reinit_at_bitrate_A(int baud_kbps);                         /* 0x08007482 */
void can_reinit_at_bitrate_B(int baud_kbps);                         /* 0x0800744e */
void can_set_baud_by_profile(void);                                  /* 0x08007b2c */
void can_apply_bitrate_B(void);                                      /* 0x08007b4c */

/* ---- rings -------------------------------------------------------------- */
int  can_transmit_frame(const volatile uint8_t *entry);              /* 0x08002b98 */
void can_tx_enqueue(uint32_t can_id, uint8_t ide,
                    uint8_t dlc, const uint8_t *data);               /* 0x080074c0 */
void can_tx_ring_drain(void);                                        /* 0x08007698 */
void can_rx0_irq_push_ring(void);                                    /* 0x08002c0a */
void CAN_RX0_IRQHandler(void);                                       /* 0x08002f32 */

/* ---- state / shutdown --------------------------------------------------- */
uint8_t can_bus_active_flag(void);                                   /* 0x080074b6 */
void    can_autobaud_timers_tick(void);                              /* 0x080077e8 */
int     can_enter_sleep_mode(void);                                  /* 0x08002d1a */

#endif /* HW_CAN_H */
