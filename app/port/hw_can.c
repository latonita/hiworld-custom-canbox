/* ============================================================================
 * hw_can.c -- bxCAN controller driver, ported from app/src/can.c (Ghidra).
 *
 * This is the vehicle-side interface: PB8 = CAN_RX, PB9 = CAN_TX (both AF5),
 * PB12 = transceiver standby. The stock firmware is built on the ST StdPeriph
 * CAN driver, so everything below 0x08007000 is a verbatim StdPeriph function
 * and everything above it is Hiworld's own ring-buffer plumbing.
 *
 * Data flow:
 *     bus -> FIFO0 -> CAN_RX0_IRQHandler (IRQ 54) -> g_can_rx_ring_buf
 *                                                 -> can_rx_decode (other agent)
 *     can_tx_enqueue -> g_can_tx_ring_buf -> can_tx_ring_drain -> TX mailboxes
 * Both rings are 50 entries x 16 bytes, head/tail are plain bytes in the stock
 * RAM block. The RX ring head is written ONLY from the ISR and the TX ring tail
 * is advanced ONLY from the main loop, so no critical sections are needed (and
 * the stock code has none).
 *
 * ---------------------------------------------------------------------------
 * BIT TIMING -- derivation
 * ---------------------------------------------------------------------------
 * can_init programs, via can_init_regs @0x0800398c:
 *     BTIM = (mode << 30) | (SJW << 24) | (BS2 << 20) | (BS1 << 16) | (BRP - 1)
 * with the constants the stock can_init stores into its CAN_InitTypeDef:
 *     SJW = 0  -> SJW field 0 ->  1 tq
 *     BS1 = 3  -> TS1 field 3 ->  4 tq
 *     BS2 = 2  -> TS2 field 2 ->  3 tq
 *     BRP = 2000 / baud_kbps   (integer division, in can_init)
 *
 * Nominal bit = SYNC_SEG(1) + TSEG1(BS1+1 = 4) + TSEG2(BS2+1 = 3) = 8 tq,
 * sample point = (1 + 4) / 8 = 62.5 %, resynchronisation jump width = 1 tq.
 *
 *     bitrate = f_APB1 / (BRP * 8)
 *
 * With f_APB1 = PCLK1_HZ = 16 MHz (SYSCLK 64 MHz, APB1 = /4 -- see port.h):
 *     125 kbit/s: BRP = 2000/125 = 16 -> 16e6 / (16 * 8) = 125.000 kbit/s
 *                 BTIM = 0x0023000F   (tq = 1.0 us, bit = 8 us)
 *      83 kbit/s: BRP = 2000/83  = 24 (truncated!) -> 16e6/(24*8) = 83.333 kbit/s
 *                 BTIM = 0x00230017   (tq = 1.5 us, bit = 12 us)
 * i.e. the 0x53 (= 83) argument is deliberately sloppy: the truncation is what
 * produces the exact 83 1/3 kbit/s the vehicle bus actually uses. Both rates
 * come out exact ONLY at f_APB1 = 2000 * 8 * 1000 = 16 MHz, which independently
 * confirms the APB1 prescaler.
 *
 * In silent mode (can_init_silent_mode) mode = 2, so BTIM bit 31 (SILM) is set
 * and bit 30 (LBKM) is clear.
 * ---------------------------------------------------------------------------
 *
 * IRQ NUMBER: can_init passes IRQn = 0x36 = 54. On the N32L406 CAN_RX0_IRQn
 * really IS 54 (it is NOT the STM32F1 value 20). Do not "fix" it --
 * see docs/notes/vector-table-slot-map.md.
 * ==========================================================================*/

#include "port.h"
#include "hw_can.h"

/* Pin configuration comes from hw_gpio.c. NOTE: gpio_init_pin takes THREE
 * arguments (port, pin_mask, mode) -- the AF number is chosen internally from a
 * hard-coded (port, pin) table, but the MODE is a real parameter. The modes
 * below are read out of the reference image, not guessed:
 *     0x08002ac6  movs r2,#2 -> PB8  mode 2 (AF_PP)
 *     0x08002ad0  movs r2,#2 -> PB9  mode 2 (AF_PP)
 *     0x0800745e  movs r2,#1 -> PB12 mode 1 (OUTPUT_PP)
 * An earlier revision of this file declared a two-argument form; that linked
 * silently against the real three-argument definition and left the mode as
 * whatever was in r2. */
#include "hw_gpio.h"

/* ---------------------------------------------------------------------------
 * Filter control block.
 *
 * NOTE: the CAN_Module struct in app/include/n32l406_hw.h puts FMC/FM1/FS1/
 * FFA1/FA1 at +0x1B0.. -- that is WRONG. The generator dropped the vendor
 * header's sTxMailBox[3] + sFIFOMailBox[2] members (0x50 bytes) and replaced
 * them with 0x30 bytes of padding, so those five members land 0x50 bytes early,
 * on top of the FIFO mailboxes. The real block is at CAN_BASE + 0x200, which is
 * exactly REG_CAN_FILTER (0x40006600) in app/include/registers.h and exactly
 * how the stock can_filter_init @0x08003aa0 indexes it. app/include is not ours
 * to edit, so the correct layout is declared locally and used instead.
 * (CAN->MCTRL..BTIM at +0x00..+0x1C and the CAN_TxMailBox / CAN_FIFOMailBox /
 * CAN_FilterReg macros are all correct and are used as-is.)
 * -------------------------------------------------------------------------*/
typedef struct {
    __IO uint32_t FMC;          /* +0x200  filter master, bit0 = FINIT      */
    __IO uint32_t FM1;          /* +0x204  filter mode                      */
    __IO uint32_t RESERVED0;    /* +0x208                                   */
    __IO uint32_t FS1;          /* +0x20C  filter scale                     */
    __IO uint32_t RESERVED1;    /* +0x210                                   */
    __IO uint32_t FFA1;         /* +0x214  FIFO assignment                  */
    __IO uint32_t RESERVED2;    /* +0x218                                   */
    __IO uint32_t FA1;          /* +0x21C  filter activation                */
} CAN_FilterCtrl_Module;

#define CAN_FILTER_CTRL  ((CAN_FilterCtrl_Module *)0x40006600U)

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

#define CAN_INIT_TIMEOUT    0xFFFFU

/* ---------------------------------------------------------------------------
 * TX ring head / tail.
 *
 * globals.h still carries the pre-correction names for these two words: the
 * full-decode pass established that 0x20001120 / 0x20001124 are the CAN TX ring
 * producer / consumer indices, not SWC variables (see
 * docs/notes/ghidra-merged-app-bootloader.md). globals.h is not ours to edit,
 * so alias them here. The RX pair already has the right names.
 * -------------------------------------------------------------------------*/
#define g_can_tx_ring_head   g_swc_var_120   /* 0x20001120 */
#define g_can_tx_ring_tail   g_swc_var_124   /* 0x20001124 */

/* 16-bit "no free mailbox" counter inside the TX scratch block @0x200018e4 + 2. */
#define g_can_tx_busy_ctr    (*(volatile uint16_t *)(0x200018e4U + 2U))

/* Ring entry accessor (both rings are 4-byte aligned, stride 16). */
#define RING_ENTRY(buf, idx) ((volatile uint8_t *)((buf) + (uint32_t)(idx) * CAN_RING_STRIDE))

/* ===========================================================================
 * StdPeriph HAL layer
 * ======================================================================== */

/* --- 0x08003974 (decompiled as "can_rcc_clock_enable") ---
 * It is really CAN_DeInit: a reset pulse on the APB1 CAN bit. Called by
 * can_init AFTER the CAN clock has been gated on, so every re-baud starts from
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

    /* BTIM: mode[31:30] | SJW[25:24] | TS2[22:20] | TS1[19:16] | BRP[9:0] */
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
 * profile decoders. */
void can_filter_init(const can_filter_init_t *filt)
{
    CAN_FilterCtrl_Module *fc = CAN_FILTER_CTRL;
    uint32_t bit = 1U << filt->number;

    fc->FMC |= 1U;                /* FINIT: hold the filters in init mode */
    fc->FA1 &= ~bit;              /* deactivate the bank while we touch it */

    if (filt->scale == 0) {       /* two 16-bit filters */
        fc->FS1 &= ~bit;
        CAN_FilterReg[filt->number].FR1 =
            ((uint32_t)filt->mask_id_low << 16) | filt->id_low;
        CAN_FilterReg[filt->number].FR2 =
            ((uint32_t)filt->mask_id_high << 16) | filt->id_high;
    }
    if (filt->scale == 1) {       /* one 32-bit filter */
        fc->FS1 |= bit;
        CAN_FilterReg[filt->number].FR1 =
            ((uint32_t)filt->id_high << 16) | filt->id_low;
        CAN_FilterReg[filt->number].FR2 =
            ((uint32_t)filt->mask_id_high << 16) | filt->mask_id_low;
    }

    if (filt->mode == 0)
        fc->FM1 &= ~bit;          /* identifier / mask */
    else
        fc->FM1 |= bit;           /* identifier list */

    if (filt->fifo_assignment == 0)
        fc->FFA1 &= ~bit;         /* FIFO0 */
    if (filt->fifo_assignment == 1)
        fc->FFA1 |= bit;          /* FIFO1 */

    if (filt->activation == 1)
        fc->FA1 |= bit;

    fc->FMC &= ~1U;               /* leave filter init mode */
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
 * masked to 4 bits, exactly as the stock code does. */
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

/* --- 0x08003e2e --- CAN_Receive: pop one message and release the FIFO. */
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

/* ===========================================================================
 * Bring-up
 * ======================================================================== */

/* --- 0x08002a90 --- Full CAN bring-up at `baud_kbps` kbit/s.
 * Order is kept exactly as the stock: NVIC first (global interrupts are still
 * masked at this point), then clocks + pins, then a reset pulse, then the bit
 * timing, then the accept-all filter, then FMPIE0. */
void can_init(int baud_kbps, uint8_t mode)
{
    can_init_t        init;
    can_filter_init_t filt;

    nvic_set_priority_grouping(0x300U);          /* PRIGROUP = 3 */
    /* IRQn 0x36 = 54 = CAN_RX0_IRQn on the N32L406. This is CORRECT. */
    nvic_init(54U, 3U, 0U, 1);

    rcc_apb2_periph_clock(RCC_APB2_AFIO | RCC_APB2_IOPB, 1);   /* stock: 0x09 */
    gpio_init_pin(GPIOB, 0x0100U, GPIO_MODE_AF_PP);   /* PB8 = CAN_RX, AF5 */
    gpio_init_pin(GPIOB, 0x0200U, GPIO_MODE_AF_PP);   /* PB9 = CAN_TX, AF5 */

    rcc_apb1_periph_clock(RCC_APB1_CAN, 1);
    can_deinit(CAN);

    can_struct_init_defaults(&init);
    init.ttcm = 0;                               /* no time-triggered mode   */
    init.abom = 0;                               /* no auto bus-off recovery */
    init.awum = 0;                               /* no auto wake-up          */
    init.nart = 0;                               /* automatic retransmission */
    init.rflm = 0;                               /* FIFO overwrites on full  */
    init.txfp = 1;                               /* FIFO-priority TX order   */
    init.sjw  = 0;                               /* 1 tq */
    init.bs1  = 3;                               /* 4 tq */
    init.bs2  = 2;                               /* 3 tq -> 8 tq/bit, SP 62.5 % */
    init.prescaler = (uint16_t)(2000 / baud_kbps);
    init.mode = mode;
    can_init_regs(CAN, &init);

    filt.number          = 0;
    filt.mode            = 0;                    /* identifier / mask        */
    filt.scale           = 1;                    /* one 32-bit filter        */
    filt.id_high         = 0;
    filt.id_low          = 0;
    filt.mask_id_high    = 0;                    /* mask 0 -> accept all     */
    filt.mask_id_low     = 0;
    filt.fifo_assignment = 0;                    /* FIFO0                    */
    filt.activation      = 1;
    can_filter_init(&filt);

    can_it_config(CAN, CAN_IT_FMP0, 1);
}

/* --- 0x08002b76 --- */
void can_init_normal_mode(int baud_kbps)
{
    can_init(baud_kbps, CAN_MODE_NORMAL);
}

/* --- 0x08002b84 --- Present in the image but never called by live code. */
void can_init_silent_mode(int baud_kbps)
{
    can_init(baud_kbps, CAN_MODE_SILENT);
}

/* Shared body of the two byte-identical bitrate wrappers below.
 * PB12 is the transceiver standby line: it is configured, then driven low
 * (= transceiver active). The stock calls gpio_init_pin twice in a row on it;
 * that redundant-but-harmless duplicate is reproduced. */
static void can_reinit_at_bitrate(int baud_kbps)
{
    can_init_normal_mode(baud_kbps);
    gpio_init_pin(GPIOB, 0x1000U, GPIO_MODE_OUTPUT_PP);  /* PB12 xcvr standby */
    gpio_init_pin(GPIOB, 0x1000U, GPIO_MODE_OUTPUT_PP);  /* stock does it twice */
    gpio_write_pin(GPIOB, 0x1000U, 0);           /* 0x08007430: standby off */
    g_can_reinit_bitrate    = (uint8_t)baud_kbps;
    g_can_bus_status        = 1;
    g_profile0_activity_ctr = 200;
}

/* --- 0x08007482 --- */
void can_reinit_at_bitrate_A(int baud_kbps)
{
    can_reinit_at_bitrate(baud_kbps);
}

/* --- 0x0800744e --- Byte-identical twin of _A in the stock image. */
void can_reinit_at_bitrate_B(int baud_kbps)
{
    can_reinit_at_bitrate(baud_kbps);
}

/* --- 0x08007b2c --- Baud is locked to the decode profile:
 * profile 1 -> 0x7D = 125 kbit/s, profile 0 -> 0x53 = 83.333 kbit/s. Any other
 * stored value is forced back to profile 0. */
void can_set_baud_by_profile(void)
{
    if (g_param_profile_sel == 1) {
        can_reinit_at_bitrate_A(0x7D);
    } else {
        g_param_profile_sel = 0;
        can_reinit_at_bitrate_A(0x53);
    }
}

/* --- 0x08007b4c --- Same, through the _B wrapper. */
void can_apply_bitrate_B(void)
{
    if (g_param_profile_sel == 1) {
        can_reinit_at_bitrate_B(0x7D);
    } else {
        g_param_profile_sel = 0;
        can_reinit_at_bitrate_B(0x53);
    }
}

/* ===========================================================================
 * Ring buffers
 * ======================================================================== */

/* --- 0x08002b98 --- Hand one ring entry to a free TX mailbox.
 * Returns non-zero when no mailbox was free (the caller then stops draining).
 * Entry: [0..1] ID hi, [2..3] ID lo, [4] IDE, [5] DLC, [6..13] data. */
int can_transmit_frame(const volatile uint8_t *entry)
{
    can_tx_msg_t msg;
    uint32_t i;
    uint32_t id;

    id = ((uint32_t)*(const volatile uint16_t *)(entry) << 16)
       |  (uint32_t)*(const volatile uint16_t *)(entry + 2);

    msg.std_id = 0;
    msg.ext_id = 0;
    msg.rtr    = 0;
    msg.dlc    = entry[5];

    if (entry[4] == 0) {
        msg.std_id = id;
        msg.ide    = CAN_ID_STANDARD;
    } else {
        msg.ide    = CAN_ID_EXTENDED;
        msg.ext_id = id;
    }

    /* The stock loop only fills data[0..dlc-1] and leaves the rest of the
     * stack frame uninitialised; bytes past DLC are never transmitted, but
     * zeroing them keeps the mailbox write deterministic. */
    for (i = 0; i < 8U; i++)
        msg.data[i] = (i < msg.dlc) ? entry[6 + i] : (uint8_t)0U;

    return can_transmit(CAN, &msg) == CAN_TX_NO_MAILBOX;
}

/* --- 0x080074c0 --- Producer side of the TX ring. Drops the frame and sets
 * CAN_ERR_TX_RING_FULL when the ring is full (one slot is always left unused,
 * which is how head == tail stays unambiguous as "empty"). */
void can_tx_enqueue(uint32_t can_id, uint8_t ide, uint8_t dlc, const uint8_t *data)
{
    volatile uint8_t *e;
    uint32_t i;
    uint8_t next = (uint8_t)((g_can_tx_ring_head + 1U) % CAN_RING_ENTRIES);

    if (next == g_can_tx_ring_tail) {
        g_can_err_flags |= CAN_ERR_TX_RING_FULL;
        return;
    }

    e = RING_ENTRY(g_can_tx_ring_buf, g_can_tx_ring_head);
    *(volatile uint16_t *)(e)     = (uint16_t)(can_id >> 16);
    *(volatile uint16_t *)(e + 2) = (uint16_t)can_id;
    e[4] = ide;
    e[5] = dlc;
    for (i = 0; i < dlc && i < 8U; i++)
        e[6 + i] = data[i];

    g_can_tx_ring_head = next;
}

/* --- 0x08007698 --- TX pump, run from the 4 ms scheduler slot. Drains at most
 * 10 entries per pass; bails out the moment all three mailboxes are busy. */
void can_tx_ring_drain(void)
{
    uint8_t drained;

    for (drained = 0; drained < 10U; drained++) {
        if (g_can_tx_ring_tail == g_can_tx_ring_head)
            return;

        if (can_transmit_frame(RING_ENTRY(g_can_tx_ring_buf, g_can_tx_ring_tail))) {
            g_can_err_flags = CAN_ERR_TX_NO_MAILBOX;   /* stock assigns, not ORs */
            if (g_can_tx_busy_ctr < 100U)
                g_can_tx_busy_ctr++;
            else
                g_can_tx_busy_ctr = 0;
            return;
        }

        g_can_tx_ring_tail = (uint8_t)((g_can_tx_ring_tail + 1U) % CAN_RING_ENTRIES);
    }
}

/* --- 0x08002c0a --- RX FIFO0 -> RX ring. One message per interrupt.
 * Needs at least 2 free slots; otherwise the frame is still popped (to release
 * the FIFO and clear the pending interrupt) but discarded with an error flag. */
void can_rx0_irq_push_ring(void)
{
    can_rx_msg_t msg;
    volatile uint8_t *e;
    uint32_t free_slots;
    uint32_t i;
    uint32_t id;

    if (g_can_rx_ring_head < g_can_rx_ring_tail)
        free_slots = (uint32_t)g_can_rx_ring_tail - g_can_rx_ring_head;
    else
        free_slots = ((uint32_t)g_can_rx_ring_tail + CAN_RING_ENTRIES) - g_can_rx_ring_head;

    if (free_slots < 2U) {
        can_receive_fifo(CAN, 0, &msg);          /* pop and drop */
        g_can_err_flags |= CAN_ERR_RX_RING_FULL;
        return;
    }

    can_receive_fifo(CAN, 0, &msg);
    id = (msg.ide == CAN_ID_STANDARD) ? msg.std_id : msg.ext_id;

    e = RING_ENTRY(g_can_rx_ring_buf, g_can_rx_ring_head);
    *(volatile uint16_t *)(e + 2) = (uint16_t)id;
    *(volatile uint16_t *)(e)     = (uint16_t)(id >> 16);
    e[5] = msg.dlc;
    for (i = 0; i < msg.dlc && i < 8U; i++)
        e[6 + i] = msg.data[i];

    g_can_rx_ring_head = (uint8_t)((g_can_rx_ring_head + 1U) % CAN_RING_ENTRIES);
}

/* --- 0x08002f32 --- veneer slot 29 / IRQ 54. Overrides the weak default in
 * app/startup/crt0.S. The FIFO release inside can_receive_fifo is what clears
 * the FMP0 condition, so there is nothing else to acknowledge. */
void CAN_RX0_IRQHandler(void)
{
    can_rx0_irq_push_ring();
}

/* ===========================================================================
 * State / shutdown
 * ======================================================================== */

/* --- 0x080074b6 --- bit 0 of the decoded status block = "vehicle bus alive". */
uint8_t can_bus_active_flag(void)
{
    return (uint8_t)(g_status_block[0] & 1U);
}

/* --- 0x080077e8 --- decrement of the two CAN hold-off counters. */
void can_autobaud_timers_tick(void)
{
    if (g_profile0_activity_ctr != 0)
        g_profile0_activity_ctr--;
    if (g_canerr_hold_timer != 0)
        g_canerr_hold_timer--;
}

/* --- 0x08002d1a --- Put the controller into sleep mode; returns 1 on success.
 * The decompile named this "can_rx0_msg_pending", which is wrong: it is the
 * entry gate of the shutdown sequence (exti8_bus_wake_setup @0x080032f6 only
 * powers the board down once the CAN core has acknowledged sleep). */
int can_enter_sleep_mode(void)
{
    return can_sleep(CAN) == 1;
}

/* 0x08002e18 is NOT a CAN function despite the decompile's name: its body is
 * rcc_apb1_periph_clock(RCC_APB1_WWDG, 0), i.e. it gates the WATCHDOG clock
 * before STOP mode. core.c owns it as wwdg_clock_disable(). Removed from here
 * so one stock function has exactly one reconstruction. */
