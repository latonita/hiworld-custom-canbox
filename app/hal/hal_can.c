/* ============================================================================
 * app/hal/hal_can.c -- bxCAN peripheral register driver (HAL layer).
 *
 * LAYER CONTRACT: pure C11, MMIO only. No RAM state, no globals.h, no C++.
 * See hal_can.h for the struct-layout rationale and the bit-timing derivation.
 *
 * Extracted verbatim from the retired app/port/hw_can.c, which was itself ported from
 * reference/decompile/app/can.c (Ghidra). Everything in the stock image below 0x08007000 is a
 * StdPeriph function; this file is exactly that half. The ring buffers, the
 * bring-up/pin/NVIC policy and CAN_RX0_IRQHandler stayed behind in the
 * services layer because they own RAM.
 *
 * On the board these registers drive: PB8 = CAN_RX, PB9 = CAN_TX (both AF5),
 * PB12 = transceiver standby. Pin muxing is NOT done here -- it is board
 * policy and belongs to the service that brings CAN up.
 * ==========================================================================*/

#include <stddef.h>

#include "hal_can.h"
#include "hal_rcc.h"   /* RCC_APB1_CAN only; no RAM state, no code dependency */

/* ---------------------------------------------------------------------------
 * Filter control block.
 *
 * HISTORICAL NOTE, kept because it bit us once: an earlier revision of
 * the retired app/port/hw_can.c declared a local CAN_FilterCtrl_Module at 0x40006600 on
 * the grounds that n32l406_hw.h's CAN_Module put FMC/FM1/FS1/FFA1/FA1 0x50
 * bytes early (the generator had allegedly dropped sTxMailBox[3] +
 * sFIFOMailBox[2]). That is no longer true: the committed CAN_Module carries
 * both mailbox arrays and lands FMC at +0x200 = 0x40006600, which is exactly
 * the CAN filter block at 0x40006600 and exactly how the stock
 * can_filter_init @0x08003aa0 indexes it. The static assertions below pin that
 * down at compile time so the local-override hack can never silently be
 * needed again.
 * -------------------------------------------------------------------------*/
_Static_assert(offsetof(CAN_Module, sTxMailBox)   == 0x180U, "CAN TX mailboxes moved");
_Static_assert(offsetof(CAN_Module, sFIFOMailBox) == 0x1B0U, "CAN FIFO mailboxes moved");
_Static_assert(offsetof(CAN_Module, FMC)          == 0x200U, "CAN filter block moved");
_Static_assert(offsetof(CAN_Module, FA1)          == 0x21CU, "CAN filter block moved");
_Static_assert(offsetof(CAN_Module, sFilterRegister) == 0x240U, "CAN filter regs moved");

/* The three config structs are the ST StdPeriph layouts and the stock code
 * addresses them by raw byte offset, so size and field placement are ABI.
 * Pin them down too -- a reorder here would be a silent brick. */
_Static_assert(sizeof(can_init_t) == 12U,            "CAN_InitTypeDef layout changed");
_Static_assert(offsetof(can_init_t, txfp) == 11U,    "CAN_InitTypeDef layout changed");
_Static_assert(sizeof(can_filter_init_t) == 14U,     "CAN_FilterInitTypeDef layout changed");
_Static_assert(offsetof(can_filter_init_t, activation) == 13U,
                                                     "CAN_FilterInitTypeDef layout changed");
_Static_assert(sizeof(can_tx_msg_t) == 20U,          "CanTxMsg layout changed");
_Static_assert(offsetof(can_tx_msg_t, data) == 11U,  "CanTxMsg layout changed");
_Static_assert(sizeof(can_rx_msg_t) == 20U,          "CanRxMsg layout changed");
_Static_assert(offsetof(can_rx_msg_t, fmi) == 19U,   "CanRxMsg layout changed");

/* ---- MCTRL / MSTS / TSTS / RFFn bits used below ------------------------- */
#define CAN_MCTRL_INRQ      0x00000001U
#define CAN_MCTRL_SLEEP     0x00000002U
#define CAN_MCTRL_TXFP      0x00000004U
#define CAN_MCTRL_RFLM      0x00000008U
#define CAN_MCTRL_NART      0x00000010U
#define CAN_MCTRL_AWUM      0x00000020U
#define CAN_MCTRL_ABOM      0x00000040U
#define CAN_MCTRL_TTCM      0x00000080U

#define CAN_MSTS_INAK       0x00000001U
#define CAN_MSTS_SLAK       0x00000002U

#define CAN_TSTS_TME0       0x04000000U
#define CAN_TSTS_TME1       0x08000000U
#define CAN_TSTS_TME2       0x10000000U

#define CAN_TMI_TXRQ        0x00000001U
#define CAN_RFF_RFOM        0x00000020U

#define CAN_FMC_FINIT       0x00000001U

#define CAN_INIT_TIMEOUT    0xFFFFU

/* --- 0x08003974 (decompiled as "can_rcc_clock_enable") ---
 * It is really CAN_DeInit: a reset pulse on the APB1 CAN bit. Called by the
 * bring-up AFTER the CAN clock has been gated on, so every re-baud starts from
 * a known register state. */
void can_deinit(CAN_Module *canx)
{
    (void)canx;   /* stock takes the base but the reset bit is fixed */
    RCC->APB1PRST |= RCC_APB1_CAN;
    RCC->APB1PRST &= ~RCC_APB1_CAN;
}

/* --- 0x08003ba2 --- CAN_StructInit. */
void can_struct_init_defaults(can_init_t *init)
{
    init->ttcm = 0;
    init->abom = 0;
    init->awum = 0;
    init->nart = 0;
    init->rflm = 0;
    init->txfp = 0;
    init->mode = CAN_MODE_NORMAL;
    init->sjw  = 0;
    init->bs1  = 3;
    init->bs2  = 2;
    init->prescaler = 1;
}

/* --- 0x0800398c --- CAN_Init. Returns 1 on success, 0 if the controller would
 * not enter or leave initialisation mode within 0xFFFF polls. */
int can_init_regs(CAN_Module *canx, const can_init_t *init)
{
    uint32_t wait;

    /* leave sleep, request initialisation mode */
    canx->MCTRL &= ~CAN_MCTRL_SLEEP;
    canx->MCTRL |= CAN_MCTRL_INRQ;
    for (wait = 0; (canx->MSTS & CAN_MSTS_INAK) == 0 && wait != CAN_INIT_TIMEOUT; wait++) {
    }
    if ((canx->MSTS & CAN_MSTS_INAK) == 0)
        return 0;

    if (init->ttcm == 1) canx->MCTRL |= CAN_MCTRL_TTCM; else canx->MCTRL &= ~CAN_MCTRL_TTCM;
    if (init->abom == 1) canx->MCTRL |= CAN_MCTRL_ABOM; else canx->MCTRL &= ~CAN_MCTRL_ABOM;
    if (init->awum == 1) canx->MCTRL |= CAN_MCTRL_AWUM; else canx->MCTRL &= ~CAN_MCTRL_AWUM;
    if (init->nart == 1) canx->MCTRL |= CAN_MCTRL_NART; else canx->MCTRL &= ~CAN_MCTRL_NART;
    if (init->rflm == 1) canx->MCTRL |= CAN_MCTRL_RFLM; else canx->MCTRL &= ~CAN_MCTRL_RFLM;
    if (init->txfp == 1) canx->MCTRL |= CAN_MCTRL_TXFP; else canx->MCTRL &= ~CAN_MCTRL_TXFP;

    /* BTIM: mode[31:30] | SJW[25:24] | TS2[22:20] | TS1[19:16] | BRP[9:0]
     * -> 8 tq/bit, sample point 62.5 %, SJW 1 tq; see the derivation in
     *    hal_can.h for why BRP = 2000/baud_kbps lands both 125k and 83 1/3k
     *    exactly at f_APB1 = 16 MHz. */
    canx->BTIM = ((uint32_t)init->mode << 30)
               | ((uint32_t)init->sjw  << 24)
               | ((uint32_t)init->bs2  << 20)
               | ((uint32_t)init->bs1  << 16)
               | ((uint32_t)init->prescaler - 1U);

    /* back to normal mode */
    canx->MCTRL &= ~CAN_MCTRL_INRQ;
    for (wait = 0; (canx->MSTS & CAN_MSTS_INAK) != 0 && wait != CAN_INIT_TIMEOUT; wait++) {
    }
    return (canx->MSTS & CAN_MSTS_INAK) == 0;
}

/* --- 0x08003aa0 --- CAN_FilterInit. The stock image calls this exactly once,
 * with an all-zero 32-bit id/mask pair on bank 0 -> every identifier matches,
 * everything lands in FIFO0, and all real filtering is done in software by the
 * profile decoders. (That call site is board policy and lives in the services
 * layer; this function just programs whatever bank it is handed.) */
void can_filter_init(const can_filter_init_t *filt)
{
    uint32_t bit = 1U << filt->number;

    CAN->FMC |= CAN_FMC_FINIT;    /* hold the filters in init mode */
    CAN->FA1 &= ~bit;             /* deactivate the bank while we touch it */

    if (filt->scale == 0) {       /* two 16-bit filters */
        CAN->FS1 &= ~bit;
        CAN_FilterReg[filt->number].FR1 =
            ((uint32_t)filt->mask_id_low << 16) | filt->id_low;
        CAN_FilterReg[filt->number].FR2 =
            ((uint32_t)filt->mask_id_high << 16) | filt->id_high;
    }
    if (filt->scale == 1) {       /* one 32-bit filter */
        CAN->FS1 |= bit;
        CAN_FilterReg[filt->number].FR1 =
            ((uint32_t)filt->id_high << 16) | filt->id_low;
        CAN_FilterReg[filt->number].FR2 =
            ((uint32_t)filt->mask_id_high << 16) | filt->mask_id_low;
    }

    if (filt->mode == 0)
        CAN->FM1 &= ~bit;         /* identifier / mask */
    else
        CAN->FM1 |= bit;          /* identifier list */

    if (filt->fifo_assignment == 0)
        CAN->FFA1 &= ~bit;        /* FIFO0 */
    if (filt->fifo_assignment == 1)
        CAN->FFA1 |= bit;         /* FIFO1 */

    if (filt->activation == 1)
        CAN->FA1 |= bit;

    CAN->FMC &= ~CAN_FMC_FINIT;   /* leave filter init mode */
}

/* --- 0x0800407e --- CAN_ITConfig. */
void can_it_config(CAN_Module *canx, uint32_t it, int enable)
{
    if (enable)
        canx->INTE |= it;
    else
        canx->INTE &= ~it;
}

/* --- 0x08003c4e --- CAN_Transmit. Returns the mailbox used (0..2) or
 * CAN_TX_NO_MAILBOX (4) when all three are busy. NOTE: it writes msg->dlc back
 * masked to 4 bits, exactly as the stock code does -- which is why `msg` is
 * not const. */
uint8_t can_transmit(CAN_Module *canx, can_tx_msg_t *msg)
{
    uint32_t mb = 0;

    if ((canx->TSTS & CAN_TSTS_TME0) != CAN_TSTS_TME0) {
        if ((canx->TSTS & CAN_TSTS_TME1) == CAN_TSTS_TME1)
            mb = 1;
        else if ((canx->TSTS & CAN_TSTS_TME2) == CAN_TSTS_TME2)
            mb = 2;
        else
            mb = CAN_TX_NO_MAILBOX;
    }

    if (mb == CAN_TX_NO_MAILBOX)
        return CAN_TX_NO_MAILBOX;

    CAN_TxMailBox[mb].TMI &= CAN_TMI_TXRQ;
    if (msg->ide == CAN_ID_STANDARD)
        CAN_TxMailBox[mb].TMI |= (msg->std_id << 21) | msg->rtr;
    else
        CAN_TxMailBox[mb].TMI |= (uint32_t)msg->ide | (msg->ext_id << 3) | msg->rtr;

    msg->dlc &= 0x0FU;
    CAN_TxMailBox[mb].TMDT &= ~0x0000000FU;
    CAN_TxMailBox[mb].TMDT |= msg->dlc;

    CAN_TxMailBox[mb].TMDL = ((uint32_t)msg->data[3] << 24)
                           | ((uint32_t)msg->data[2] << 16)
                           | ((uint32_t)msg->data[1] << 8)
                           |  (uint32_t)msg->data[0];
    CAN_TxMailBox[mb].TMDH = ((uint32_t)msg->data[7] << 24)
                           | ((uint32_t)msg->data[6] << 16)
                           | ((uint32_t)msg->data[5] << 8)
                           |  (uint32_t)msg->data[4];

    CAN_TxMailBox[mb].TMI |= CAN_TMI_TXRQ;
    return (uint8_t)mb;
}

/* --- 0x08003e2e --- CAN_Receive: pop one message and release the FIFO.
 * The FIFO release (RFOM) here is also what clears the FMP0 interrupt
 * condition, so the RX0 ISR in the services layer has nothing else to ack. */
void can_receive_fifo(CAN_Module *canx, int fifo, can_rx_msg_t *msg)
{
    uint32_t rmi  = CAN_FIFOMailBox[fifo].RMI;
    uint32_t rmdt = CAN_FIFOMailBox[fifo].RMDT;
    uint32_t rmdl = CAN_FIFOMailBox[fifo].RMDL;
    uint32_t rmdh = CAN_FIFOMailBox[fifo].RMDH;

    msg->ide = (uint8_t)(rmi & 0x04U);        /* stores 0 or 4, not 0 or 1 */
    if (msg->ide == CAN_ID_STANDARD)
        msg->std_id = rmi >> 21;
    else
        msg->ext_id = rmi >> 3;
    msg->rtr = (uint8_t)(rmi & 0x02U);

    msg->dlc = (uint8_t)(rmdt & 0x0FU);
    msg->fmi = (uint8_t)(rmdt >> 8);

    msg->data[0] = (uint8_t)rmdl;
    msg->data[1] = (uint8_t)(rmdl >> 8);
    msg->data[2] = (uint8_t)(rmdl >> 16);
    msg->data[3] = (uint8_t)(rmdl >> 24);
    msg->data[4] = (uint8_t)rmdh;
    msg->data[5] = (uint8_t)(rmdh >> 8);
    msg->data[6] = (uint8_t)(rmdh >> 16);
    msg->data[7] = (uint8_t)(rmdh >> 24);

    if (fifo == 0)
        canx->RFF0 |= CAN_RFF_RFOM;
    else
        canx->RFF1 |= CAN_RFF_RFOM;
}

/* --- 0x0800400e --- CAN_Sleep. Clears INRQ, sets SLEEP, then checks
 * MSTS[1:0] == SLAK once -- the stock function really has no polling loop.
 * Returns 1 on success. */
int can_sleep(CAN_Module *canx)
{
    canx->MCTRL = (canx->MCTRL & ~CAN_MCTRL_INRQ) | CAN_MCTRL_SLEEP;
    return (canx->MSTS & (CAN_MSTS_INAK | CAN_MSTS_SLAK)) == CAN_MSTS_SLAK;
}

/* --- 0x0800402c --- CAN_WakeUp. Only reachable from a dead wrapper in the
 * stock image (0x08002d2c); kept as the counterpart of can_sleep. */
int can_wakeup(CAN_Module *canx)
{
    uint32_t wait;

    canx->MCTRL &= ~CAN_MCTRL_SLEEP;
    for (wait = CAN_INIT_TIMEOUT;
         (canx->MSTS & CAN_MSTS_SLAK) == CAN_MSTS_SLAK && wait != 0; wait--) {
    }
    return (canx->MSTS & CAN_MSTS_SLAK) != CAN_MSTS_SLAK;
}
