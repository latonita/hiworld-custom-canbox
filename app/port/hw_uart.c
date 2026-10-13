/* ============================================================================
 * hw_uart.c -- USART2 driver + the head-unit "AA 55" framing/protocol.
 *
 * Ported from app/src/uart.c and the usart2_init part of app/src/misc.c
 * (Ghidra output). Original flash addresses are quoted on every function.
 *
 * THIS IS A NATIONS N32L406, NOT AN STM32. Register names/offsets come from
 * app/include/n32l406_hw.h (generated from the vendor SDK):
 *     USART_Module = { STS +0x00, DAT +0x04, BRCF +0x08,
 *                      CTRL1 +0x0C, CTRL2 +0x10, CTRL3 +0x14, GTP +0x18 }
 * all 16-bit with 16-bit reserved padding between them. Pin muxing is per-pin
 * AF numbers in GPIO AFL/AFH (PA2/PA3 = AF4), NOT an F1-style AFIO remap.
 *
 * Wiring and timing (see docs/hiworld-firmware-analysis.md section 4):
 *     PA2 = USART2_TX -> head unit,  PA3 = USART2_RX <- head unit
 *     38400 8N1, no flow control, RXNE interrupt on, TXE interrupt on demand
 *     NVIC IRQ 38 (USART2_IRQn), pre-emption priority 2, sub 0
 *
 * The TX side is interrupt-driven out of a single shared frame buffer: the
 * scheduler fills g_uart_frame_buf[0..LEN+4], sets [0x3C]=len, [0x3D]=0,
 * [0x3E]=1 and enables TXE; the ISR drains it and clears [0x3E] at the end.
 * There is no TX queue -- a new frame can only be built while [0x3E] == 0.
 *
 * Steady-state cadence: uart_tx_sched() is called from the 8 ms slot of the
 * main scheduler and only acts on every 6th pass, i.e. ONE FRAME PER 48 ms
 * while any payload is changing. When a full round-robin sweep finds nothing
 * changed it falls back to a keep-alive: g_uart_idle_delay = 15 passes of
 * 48 ms = ~720 ms between frames. The first transmission therefore happens
 * ~48 ms into phase 2, on the 6th scheduler pass.
 * ==========================================================================*/

#include "port.h"
#include "hw_uart.h"

/* ============================================================================
 * GPIO dependency -- owned by another agent (app/port/hw_gpio.c + hw_gpio.h).
 *
 * The stock usart2_init calls gpio_init_pin(GPIOA, <pin mask>, 2); mode 2 is
 * the vendor SDK's alternate-function push-pull, and gpio_init_pin itself
 * picks AF4 for GPIOA pin masks 4 (PA2) and 8 (PA3) from its internal
 * port/pin -> AF table (app/src/gpio.c @0x08002494). It also enables the
 * GPIO/AFIO APB2 clocks, so there is nothing for this file to do beyond the
 * two calls.
 *
 * TEMPORARY SCAFFOLDING: the weak no-op below keeps this translation unit
 * linkable on its own; the strong definition in hw_gpio.c overrides it. Delete
 * it once the GPIO port is settled.
 * ==========================================================================*/
#include "hw_gpio.h"

__attribute__((weak)) void gpio_init_pin(GPIO_Module *port, uint32_t pin_mask,
                                         uint32_t mode)
{
    (void)port; (void)pin_mask; (void)mode;
}

#define GPIO_PIN_PA2        0x0004U   /* USART2_TX */
#define GPIO_PIN_PA3        0x0008U   /* USART2_RX */

/* ---------------------------------------------------------------------------
 * USART interrupt/flag ids, encoded exactly as the vendor SDK does:
 *     bits [15:8] = bit number in STS
 *     bits  [7:5] = control-register selector (1 = CTRL1, 2 = CTRL2, else CTRL3)
 *     bits  [4:0] = enable-bit number in that control register
 * ------------------------------------------------------------------------- */
#define USART_INT_RXDNE     0x0525U   /* STS.RXDNE bit 5  <- CTRL1.RXDNEIEN bit 5 */
#define USART_INT_TXDE      0x0727U   /* STS.TXDE  bit 7  <- CTRL1.TXDEIEN  bit 7 */

#define USART_CTRL1_UE      0x2000U   /* USART enable */

/* CTRL1/2/3 field masks used by the stock USART_Init @0x0800707E. */
#define USART_CTRL1_INIT_MASK   0xE9F3U  /* clears TE|RE, PS, PCE, M        */
#define USART_CTRL2_INIT_MASK   0xCFFFU  /* clears STOP[1:0]                */
#define USART_CTRL3_INIT_MASK   0xFCFFU  /* clears CTSEN, RTSEN             */
#define USART_MODE_TX_RX        0x000CU  /* CTRL1.TE | CTRL1.RE             */

/* ---------------------------------------------------------------------------
 * Fixed stock RAM. Everything here lives at a hard address inside the stock
 * block 0x20001000..0x20003028; app/include/globals.h names most of it.
 * ------------------------------------------------------------------------- */
#define FB              g_uart_frame_buf    /* 0x200019F8, see hw_uart.h  */
#define RXF             g_uart_rx_frame     /* 0x20001B01 reassembled frame */
#define ACKQ            g_uart_ack_queue    /* 0x20001B3D, 10 entries      */
#define ACKQ_HEAD       0x0AU               /* producer index (never advanced
                                             * by anything in this build)   */
#define ACKQ_TAIL       0x0BU               /* consumer index               */
#define ACKQ_LEN        10U

/* g_uart_slot_state is declared as a scalar in globals.h but is really a
 * 6-byte array at 0x200011AB..0x200011B0, one state per TX table slot:
 *   0 = idle, 1 = send-now request, 2/3 = frame already built this sweep. */
#define SLOT_STATE      (&g_uart_slot_state)

#define UART_TX_SLOTS   6U
#define UART_TICKS_PER_FRAME  6U    /* 6 x 8 ms = 48 ms                     */
#define UART_IDLE_DELAY       15U   /* 15 x 48 ms = ~720 ms keep-alive      */

/* ---------------------------------------------------------------------------
 * The message tables, verbatim from flash 0x08009F2C (TX) and 0x08009F74 (RX),
 * reached through the literal pools at 0x08008E30 / 0x08008E38 / 0x08009004.
 * Record shape: { uint8 cmd; uint8 len; uint8 pad[2]; void *buf; void *shadow; }
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t           cmd;
    uint8_t           len;
    uint8_t           pad[2];
    volatile uint8_t *buf;      /* live payload source / RX destination */
    volatile uint8_t *shadow;   /* last-sent copy, for change detection */
} uart_msg_desc;

/* Buffers that globals.h does not name individually (all inside the stock
 * block; addresses taken straight out of the flash table). */
#define BUF_73      ((volatile uint8_t *)0x20001B7DU)  /* never written: always 0 */
#define BUF_12      ((volatile uint8_t *)0x20001B5CU)  /* g_vstate_mid - 4 */
#define BUF_32      ((volatile uint8_t *)0x20001B66U)

/* --- flash 0x08009F2C --- box -> head unit, emitted round-robin in order. */
static const uart_msg_desc uart_tx_table[UART_TX_SLOTS] = {
    { 0x72, 14, {0, 0}, g_status_block,       (volatile uint8_t *)0x20001B98U },
    { 0xA6,  8, {0, 0}, &g_amp_level,         (volatile uint8_t *)0x20001BC4U },
    { 0x73,  8, {0, 0}, BUF_73,               (volatile uint8_t *)0x20001BCCU },
    { 0xF0, 17, {0, 0}, g_version_str,        (volatile uint8_t *)0x20001BD5U },
    { 0x12, 10, {0, 0}, BUF_12,               (volatile uint8_t *)0x20001BABU },
    { 0x32, 14, {0, 0}, BUF_32,               (volatile uint8_t *)0x20001BB5U },
};

/* --- flash 0x08009F74 --- head unit -> box. buf == shadow in these records;
 * on a change the byte immediately AFTER the payload is set to 1 as a
 * "new data" flag for the consumer. 0xFC and 0xE0 are handled outside the
 * table, in uart_rx_dispatch. */
#define UART_RX_SLOTS   2U
static const uart_msg_desc uart_rx_table[UART_RX_SLOTS] = {
    { 0xD2, 13, {0, 0}, g_cmd_block,          g_cmd_block        },
    { 0xAD,  2, {0, 0}, &g_periodic_tx_cfg,   &g_periodic_tx_cfg },
};

/* ---------------------------------------------------------------------------
 * Small helpers. The stock image shares these with the rest of the firmware
 * (util.c @0x08008574 / 0x08008598 / 0x080085C4); they are reproduced here as
 * file-local statics so this translation unit links on its own and does not
 * race the util port. Swap them for the shared ones when util.c lands.
 * ------------------------------------------------------------------------- */

/* --- 0x08008598 memcpy_bytes --- */
static void uart_memcpy(volatile uint8_t *dst, const volatile uint8_t *src,
                        uint8_t len)
{
    uint8_t i;
    for (i = 0; i < len; i++)
        dst[i] = src[i];
}

/* --- 0x08008574 util_bytes_differ --- 1 if any of len bytes differs. */
static int uart_bytes_differ(const volatile uint8_t *a,
                             const volatile uint8_t *b, uint8_t len)
{
    uint8_t i;
    for (i = 0; i < len; i++) {
        if (a[i] != b[i])
            return 1;
    }
    return 0;
}

/* --- 0x080085C4 util_wrap_index(idx, +1, hi = 5, lo = 0, mode = 0) --- */
static uint8_t uart_next_slot(uint8_t idx)
{
    return (uint8_t)((idx + 1U > (UART_TX_SLOTS - 1U)) ? 0U : (idx + 1U));
}

/* ---------------------------------------------------------------------------
 * USART register helpers, ported from the vendor-SDK copies in app/src/uart.c.
 * Specialised to USART2 where the stock code was generic; the generic
 * USART1/USART3 paths (usart_init_uart1_uart3_DEAD @0x080029B2) have no call
 * sites in this image and are deliberately not ported.
 * ------------------------------------------------------------------------- */

/* --- 0x08007320 USART_GetIntStatus --- both the enable bit and the status
 * flag must be set. */
static int usart_get_it_status(USART_Module *usart, uint32_t it)
{
    uint16_t ctrl;

    switch ((it & 0xFFU) >> 5) {
    case 1:  ctrl = usart->CTRL1; break;
    case 2:  ctrl = usart->CTRL2; break;
    default: ctrl = usart->CTRL3; break;
    }
    if ((ctrl & (uint16_t)(1U << (it & 0x1FU))) == 0)
        return 0;
    return (usart->STS & (uint16_t)(1U << ((it >> 8) & 0xFFU))) != 0;
}

/* --- 0x08007184 USART_ConfigInt --- set/clear the enable bit only. */
static void usart_it_config(USART_Module *usart, uint32_t it, int enable)
{
    volatile uint16_t *reg;
    uint16_t bit = (uint16_t)(1U << (it & 0x1FU));

    switch ((it & 0xFFU) >> 5) {
    case 1:  reg = &usart->CTRL1; break;
    case 2:  reg = &usart->CTRL2; break;
    default: reg = &usart->CTRL3; break;
    }
    if (enable)
        *reg = (uint16_t)(*reg | bit);
    else
        *reg = (uint16_t)(*reg & (uint16_t)~bit);
}

/* --- 0x0800716C USART_Enable --- CTRL1.UE. */
static void usart_cmd_enable(USART_Module *usart, int enable)
{
    if (enable)
        usart->CTRL1 = (uint16_t)(usart->CTRL1 | USART_CTRL1_UE);
    else
        usart->CTRL1 = (uint16_t)(usart->CTRL1 & (uint16_t)~USART_CTRL1_UE);
}

/* --- 0x08007246 / 0x0800724E --- the data register is 9 bits wide. */
static void usart_send_data(USART_Module *usart, uint8_t data)
{
    usart->DAT = (uint16_t)(data & 0x1FFU);
}

static uint8_t usart_receive_data(USART_Module *usart)
{
    return (uint8_t)(usart->DAT & 0x1FFU);
}

/* --- 0x0800707E USART_Init, reduced to what usart2_init actually asks for ---
 * 8 data bits, 1 stop bit, no parity, no hardware flow control, TX+RX on.
 *
 * Baud divisor, bit-identical to the stock arithmetic:
 *     d        = (25 * PCLK) / (4 * baud)      i.e. 100 x USARTDIV
 *     mantissa = d / 100
 *     fraction = ((d % 100) * 16 + 50) / 100   (rounded)
 *     BRCF     = (mantissa << 4) | (fraction & 0x0F)
 *
 * The stock code picks PCLK2 for USART1 and PCLK1 for everyone else; USART2
 * is on APB1, so PCLK1_HZ = 16 MHz (port.h).
 *     d = 25 * 16000000 / (4 * 38400) = 2604
 *     mantissa = 26, fraction = (4 * 16 + 50) / 100 = 1  ->  BRCF = 0x01A1
 * = 26.0625, against the ideal 16e6 / (16 * 38400) = 26.0417: +0.08% error. */
static void usart_configure(USART_Module *usart, uint32_t baud)
{
    uint32_t d = (25U * PCLK1_HZ) / (4U * baud);
    uint32_t mantissa = d / 100U;
    uint32_t fraction = ((d % 100U) * 16U + 50U) / 100U;

    usart->CTRL2 = (uint16_t)(usart->CTRL2 & USART_CTRL2_INIT_MASK);
    usart->CTRL1 = (uint16_t)((usart->CTRL1 & USART_CTRL1_INIT_MASK)
                              | USART_MODE_TX_RX);
    usart->CTRL3 = (uint16_t)(usart->CTRL3 & USART_CTRL3_INIT_MASK);
    usart->BRCF  = (uint16_t)((mantissa << 4) | (fraction & 0x0FU));
}

/* ============================================================================
 * 0x0800291E usart2_init  (reached through the thunk usart2_init_wrapper
 * @0x0800306C, which is not worth keeping).
 *
 * The stock caller passes 0x9600. That is NOT a register value and not a
 * "pseudo-baud selector": 0x9600 == 38400 decimal -- Ghidra simply rendered
 * the immediate in hex. So the argument is a plain baud rate in bit/s and the
 * link runs at 38400 8N1, which matches the captures and the Android plugin's
 * 0x9603 / "38403" protocol token.
 * ==========================================================================*/
void usart2_init(uint32_t baud)
{
    rcc_apb1_periph_clock(RCC_APB1_USART2, 1);

    nvic_set_priority_grouping(0x300U);
    /* IRQn 0x26 = 38 = USART2_IRQn, pre-emption 2, sub 0, enable. */
    nvic_init(38U, 2U, 0U, 1);

    /* PA3 = RX first, then PA2 = TX, exactly as the stock code does.
     * gpio_init_pin selects AF4 for both from its own port/pin table. */
    gpio_init_pin(GPIOA, GPIO_PIN_PA3, GPIO_MODE_AF_PP);
    gpio_init_pin(GPIOA, GPIO_PIN_PA2, GPIO_MODE_AF_PP);

    usart_configure(USART2, baud);
    usart_it_config(USART2, USART_INT_RXDNE, 1);   /* RX drives everything */
    usart_it_config(USART2, USART_INT_TXDE, 0);    /* armed only per frame */
    usart_cmd_enable(USART2, 1);
}

/* --- 0x08003078 --- arm the TXE interrupt; the ISR takes it from here. */
void uart_tx_start(void)
{
    usart_it_config(USART2, USART_INT_TXDE, 1);
}

/* --- 0x08003088 --- shut the port down (used by the sleep path). */
void uart_disable_usart2(void)
{
    usart_cmd_enable(USART2, 0);
    usart_it_config(USART2, USART_INT_TXDE, 0);
}

/* ============================================================================
 * 0x0800286E usart_aux_rxtx_service -- the USART2 interrupt handler.
 *
 * MUST be spelled USART2_IRQHandler: app/startup/crt0.S declares that symbol
 * weak (veneer slot 47, IRQ 38, stock handler @0x08003016 -> 0x0800286E) and
 * any other spelling silently leaves the trap stub in place.
 *
 * RX: one byte into the 200-byte ring at FB[0x3F]. The ring is full when
 * advancing head would hit tail, in which case the byte is DROPPED and the
 * overflow flag is latched (the stock code never clears it again).
 * TX: one byte out of the frame buffer per TXE until the cursor reaches the
 * length, then disarm TXE and release the busy flag.
 * ==========================================================================*/
void USART2_IRQHandler(void)
{
    if (usart_get_it_status(USART2, USART_INT_RXDNE)) {
        uint8_t  b    = usart_receive_data(USART2);
        uint8_t  head = FB[UART_FB_RX_HEAD];
        uint32_t next = (uint32_t)(head + 1U) % UART_RX_RING_SIZE;

        if (FB[UART_FB_RX_TAIL] == next) {
            g_uart_aux_rx_overflow_flag = 1;
        } else {
            FB[UART_FB_RX_RING + head] = b;
            FB[UART_FB_RX_HEAD] = (uint8_t)next;
        }
    }

    if (usart_get_it_status(USART2, USART_INT_TXDE)) {
        if (FB[UART_FB_TX_POS] < FB[UART_FB_TX_LEN]) {
            usart_send_data(USART2, FB[FB[UART_FB_TX_POS]]);
            FB[UART_FB_TX_POS]++;
        } else {
            usart_it_config(USART2, USART_INT_TXDE, 0);
            FB[UART_FB_TX_BUSY] = 0;
        }
    }
}

/* ============================================================================
 * 0x08008A18 uart_reset_state -- drop any frame in flight, empty both rings.
 *
 * Note g_uart_tx_slot_idx is seeded to 6, i.e. one PAST the last table slot:
 * that is the "nothing to send" sentinel uart_tx_sched tests for, so the very
 * first scheduler pass runs a full round-robin sweep.
 * ==========================================================================*/
void uart_reset_state(void)
{
    uint8_t i;

    FB[UART_FB_TX_LEN]  = 0;
    FB[UART_FB_TX_POS]  = 0;
    FB[UART_FB_TX_BUSY] = 0;
    FB[UART_FB_RX_HEAD] = FB[UART_FB_RX_TAIL];   /* RX ring empty */

    ACKQ[ACKQ_HEAD] = ACKQ[ACKQ_TAIL];           /* ACK queue empty */

    g_uart_tx_slot_idx = UART_TX_SLOTS;
    for (i = 0; i < UART_TX_SLOTS; i++)
        SLOT_STATE[i] = 0;
}

/* ============================================================================
 * 0x08008A54 uart_init_38400_and_version
 *
 * The version string MUST end up as 17 bytes at SRAM 0x20001B86: it is both
 * the payload of TX slot 3 (CMD 0xF0) and the liveness probe the flashing tool
 * reads back over SWD to prove the app booted. Not NUL-terminated on the wire.
 * Stock source is flash @0x08008E18, destination pointer literal @0x08008E2C.
 * ==========================================================================*/
static const char uart_version_str[17] = { 'H','1','N','4','J','P','F','2','0',
                                           'A','-','2','2','0','5','2','4' };

void uart_init_38400_and_version(void)
{
    uart_reset_state();
    usart2_init(38400U);                 /* stock literal 0x9600 = 38400 */
    uart_memcpy(g_version_str, (const volatile uint8_t *)uart_version_str,
                (uint8_t)sizeof uart_version_str);
}

/* ============================================================================
 * 0x08008A96 uart_rx_dispatch -- act on one validated inbound frame.
 *
 * 1. table-driven: 0xD2 (cluster text, 13 B) and 0xAD (amplifier write, 2 B).
 *    The payload is only copied when it actually changed, and then the byte
 *    right after the payload is raised to 1 as a "new data" flag.
 * 2. 0xFC vendor handshake -> echo the frame back with CMD changed to 0xFD and
 *    the checksum incremented by 1 (correct precisely because CKS is a plain
 *    sum and 0xFD - 0xFC = 1). Fixed 14-byte reply.
 * 3. 0xE0 -> force a watchdog reset, which drops back into the IAP bootloader.
 * Anything else, 0xFF ACKs included, is silently ignored: this box cannot be
 * polled, only listened to.
 * ==========================================================================*/
static void uart_rx_dispatch(void)
{
    uint8_t i;

    for (i = 0; i < UART_RX_SLOTS; i++) {
        const uart_msg_desc *e = &uart_rx_table[i];

        if (RXF[3] == e->cmd && RXF[2] == e->len &&
            uart_bytes_differ(e->buf, &RXF[4], e->len)) {
            uart_memcpy(e->buf, &RXF[4], e->len);
            e->buf[e->len] = 1;          /* "new data" flag */
        }
    }

    /* 0xFC payload must be 3F F3 F1 01 ?? ?? 3A A3 ??  (LEN >= 9). */
    if (RXF[3] == 0xFC && RXF[4] == 0x3F && RXF[5] == 0xF3 && RXF[6] == 0xF1 &&
        RXF[7] == 0x01 && RXF[10] == 0x3A && RXF[11] == 0xA3 &&
        FB[UART_FB_TX_BUSY] == 0) {
        FB[0] = UART_PREAMBLE_0;
        FB[1] = UART_PREAMBLE_1;
        uart_memcpy(&FB[2], &RXF[2], 0x0B);  /* LEN, CMD, payload[0..8] */
        FB[3]  = 0xFD;
        FB[13] = (uint8_t)(RXF[13] + 1);     /* checksum, CMD bumped by 1 */
        FB[UART_FB_TX_POS]  = 0;
        FB[UART_FB_TX_LEN]  = 0x0E;          /* 14 bytes on the wire */
        FB[UART_FB_TX_BUSY] = 1;
        uart_tx_start();
    }

    if (RXF[3] == 0xE0)
        wwdg_force_reset();
}

/* ============================================================================
 * 0x08008BB2 uart_rx_parser_AA55 -- byte-at-a-time state machine over the ring.
 *
 *   state 0 : wait for 0xAA
 *   state 1 : expect 0x55, else back to 0
 *   state 2 : LEN; rejected if LEN >= 0x38 (56). Needs LEN+2 further bytes
 *             buffered or we leave the tail put and retry on the next call.
 *   state>=3: CMD + payload into RXF[3..LEN+3] accumulating the running sum,
 *             then RXF[LEN+4] is the checksum and must equal (sum - 1) & 0xFF.
 *             The running sum is seeded with LEN, so sum-1 == the documented
 *             (LEN + CMD + sum(payload) - 1) & 0xFF.
 *
 * Tail-commit discipline, reproduced exactly: the tail index at FB[0x108] is
 * only written when the machine is back in state 0. A half-received frame
 * therefore leaves the tail parked on its 0xAA and is re-parsed from scratch
 * next call -- which is also why there is no timeout anywhere.
 *
 * One frame per call: as soon as a frame completes (checksum good or bad) the
 * function commits the tail and returns.
 *
 * The reassembled frame lives at g_uart_rx_frame (0x20001B01); RXF[0]/RXF[1]
 * are never written because the preamble is implicit.
 * ==========================================================================*/
void uart_rx_parser_AA55(void)
{
    uint8_t  head  = FB[UART_FB_RX_HEAD];
    uint32_t tail  = FB[UART_FB_RX_TAIL];
    uint32_t state = 0;
    uint32_t sum   = 0;    /* running checksum                         */
    uint32_t need  = 0;    /* index of the checksum byte = LEN + 4     */

    for (;;) {
        uint8_t b;

        if (head == tail)
            return;                       /* ring drained; tail left as-is */

        b = FB[UART_FB_RX_RING + tail];

        if (state == 0) {
            state = (b == UART_PREAMBLE_0) ? 1U : 0U;
        } else if (state == 1) {
            state = (b == UART_PREAMBLE_1) ? 2U : 0U;
        } else if (state == 2) {
            if (b < 0x38U) {
                /* bytes still buffered, counting this LEN byte */
                uint32_t avail = (uint32_t)
                    ((((int)head - (int)tail) + (int)UART_RX_RING_SIZE)
                     % (int)UART_RX_RING_SIZE) & 0xFFU;
                if (avail < (uint32_t)b + 2U)
                    return;               /* incomplete: retry next call */
                RXF[2] = b;               /* LEN */
                state  = 3;
                need   = (b + 4U) & 0xFFU;
                sum    = b;
            } else {
                state = 0;                /* oversized: resynchronise */
            }
        } else {
            RXF[state] = b;
            if (state < need) {
                sum   = (sum + RXF[state]) & 0xFFU;
                state = (state + 1U) & 0xFFU;
            } else {
                sum = (sum - 1U) & 0xFFU;
                if (RXF[state] == sum)
                    uart_rx_dispatch();
                /* frame finished either way: commit and leave */
                tail = (tail + 1U) % UART_RX_RING_SIZE;
                FB[UART_FB_RX_TAIL] = (uint8_t)tail;
                return;
            }
        }

        tail = (tail + 1U) % UART_RX_RING_SIZE;
        if (state == 0)
            FB[UART_FB_RX_TAIL] = (uint8_t)tail;
    }
}

/* ============================================================================
 * 0x08008CCE uart_tx_ack_frame -- the fixed 6-byte two-payload-byte frame
 *     AA 55 01 <cmd> <arg> <cks>,   cks = cmd + arg  (== sum of all - 1)
 * Only ever called as uart_tx_ack_frame(0xFF, X), the box->head-unit ACK. In
 * this build the ACK queue's producer index is never advanced by anything, so
 * this path is dead on the wire -- kept because it is stock behaviour.
 * ==========================================================================*/
void uart_tx_ack_frame(uint8_t cmd, uint8_t arg)
{
    FB[0] = UART_PREAMBLE_0;
    FB[1] = UART_PREAMBLE_1;
    FB[2] = 1;                            /* LEN: one payload byte */
    FB[3] = cmd;
    FB[4] = arg;
    FB[5] = (uint8_t)(cmd + arg);
    FB[UART_FB_TX_LEN]  = 6;
    FB[UART_FB_TX_POS]  = 0;
    FB[UART_FB_TX_BUSY] = 1;
    uart_tx_start();
}

/* ============================================================================
 * 0x08008D04 uart_build_frame_AA55 -- assemble TX table entry `slot` into the
 * frame buffer and hand it to the ISR.
 *
 *     FB[0..1] = AA 55
 *     FB[2]    = LEN (payload only)
 *     FB[3]    = CMD
 *     FB[4..]  = payload, copied from the descriptor's live buffer
 *     FB[k]    = CKS, k = LEN + 4, = (sum FB[2..k-1]) - 1
 *     wire length = LEN + 5
 * ==========================================================================*/
void uart_build_frame_AA55(uint8_t slot)
{
    const uart_msg_desc *e = &uart_tx_table[slot];
    uint8_t len = e->len;
    uint32_t k  = (uint8_t)(len + 4U);
    uint32_t j;

    uart_memcpy(&FB[4], e->buf, len);

    FB[0] = UART_PREAMBLE_0;
    FB[1] = UART_PREAMBLE_1;
    FB[2] = len;
    FB[3] = e->cmd;

    FB[k] = 0;
    for (j = 2; j < k; j++)
        FB[k] = (uint8_t)(FB[k] + FB[j]);
    FB[k] = (uint8_t)(FB[k] - 1U);

    FB[UART_FB_TX_POS]  = 0;
    FB[UART_FB_TX_LEN]  = (uint8_t)(len + UART_FRAME_OVERHEAD);
    FB[UART_FB_TX_BUSY] = 1;
    uart_tx_start();
}

/* ============================================================================
 * 0x08008D9C uart_tx_round_robin -- find the next TX table entry whose live
 * payload differs from its shadow copy, starting at the current slot index and
 * wrapping. On a hit the shadow is refreshed and the index is LEFT POINTING AT
 * that slot (the caller builds the frame). If a full sweep finds nothing, the
 * index is parked at 6 = "nothing changed", which is the caller's cue to run
 * the ~720 ms keep-alive instead.
 * ==========================================================================*/
void uart_tx_round_robin(void)
{
    uint8_t start = g_uart_tx_slot_idx;

    do {
        const uart_msg_desc *e = &uart_tx_table[g_uart_tx_slot_idx];

        if (uart_bytes_differ(e->shadow, e->buf, e->len)) {
            uart_memcpy(e->shadow, e->buf, e->len);
            return;
        }
        g_uart_tx_slot_idx = uart_next_slot(g_uart_tx_slot_idx);
    } while (g_uart_tx_slot_idx != start);

    g_uart_tx_slot_idx = UART_TX_SLOTS;   /* sentinel: nothing to send */
}

/* ============================================================================
 * 0x08008E62 uart_tx_sched -- the transmit scheduler.
 *
 * Called from the 8 ms slot of the main scheduler. Nothing happens while a
 * frame is still being clocked out (FB[0x3E]). Otherwise:
 *
 *   - drain at most one entry of the 10-deep ACK queue at 0x20001B3D, skipping
 *     the reserved codes 0xFE/0xFF. The tick is rolled back when an ACK is
 *     actually emitted so the ACK does not consume this slot's data frame.
 *     Dead in this build: the producer index is never advanced.
 *   - every 6th pass (6 x 8 ms = 48 ms) emit one data frame:
 *       slot_state == 1      -> explicit send request: refresh shadow, send
 *       slot_state == 2 or 3 -> already sent this sweep: step to the next slot
 *       slot_state == 0      -> round-robin for a changed payload; if one was
 *                               found (index < 6) send it
 *       nothing changed      -> after g_uart_idle_delay (15) idle passes, i.e.
 *                               ~720 ms, send the next table entry anyway as a
 *                               keep-alive, using the separate aux index.
 *
 * Consequence (confirmed on the wire): the box free-runs. It never waits to be
 * polled, and the capture order 73 F0 12 32 72 A6 is just this table walk.
 * ==========================================================================*/
void uart_tx_sched(void)
{
    g_uart_tx_tick++;

    if (FB[UART_FB_TX_BUSY] != 0)
        return;

    if (ACKQ[ACKQ_TAIL] != ACKQ[ACKQ_HEAD]) {
        uint8_t code = ACKQ[ACKQ[ACKQ_TAIL]];

        if (code != 0xFE && code != 0xFF) {
            uart_tx_ack_frame(0xFF, code);
            g_uart_tx_tick--;
        }
        ACKQ[ACKQ_TAIL] = (uint8_t)((ACKQ[ACKQ_TAIL] + 1U) % ACKQ_LEN);
    }

    if (g_uart_tx_tick <= (UART_TICKS_PER_FRAME - 1U))
        return;
    g_uart_tx_tick = 0;

    if (g_uart_tx_slot_idx > (UART_TX_SLOTS - 1U)) {
        g_uart_tx_slot_idx = 0;
        SLOT_STATE[g_uart_tx_slot_idx] = 0;
    }

    if (SLOT_STATE[g_uart_tx_slot_idx] == 3 ||
        SLOT_STATE[g_uart_tx_slot_idx] == 2) {
        SLOT_STATE[g_uart_tx_slot_idx] = 0;
        g_uart_tx_slot_idx++;
        if (g_uart_tx_slot_idx > (UART_TX_SLOTS - 1U)) {
            g_uart_tx_slot_idx = 0;
            SLOT_STATE[g_uart_tx_slot_idx] = 0;
        }
    }

    if (SLOT_STATE[g_uart_tx_slot_idx] == 1) {
        const uart_msg_desc *e = &uart_tx_table[g_uart_tx_slot_idx];

        uart_memcpy(e->shadow, e->buf, e->len);
        uart_build_frame_AA55(g_uart_tx_slot_idx);
        SLOT_STATE[g_uart_tx_slot_idx] = 3;
    } else {
        if (SLOT_STATE[g_uart_tx_slot_idx] == 0)
            uart_tx_round_robin();

        if (g_uart_tx_slot_idx < UART_TX_SLOTS) {
            uart_build_frame_AA55(g_uart_tx_slot_idx);
            SLOT_STATE[g_uart_tx_slot_idx] = 3;
        } else if (g_uart_idle_delay == 0) {
            g_uart_idle_delay   = UART_IDLE_DELAY;
            g_uart_aux_slot_idx = uart_next_slot(g_uart_aux_slot_idx);
            uart_build_frame_AA55(g_uart_aux_slot_idx);
        } else {
            g_uart_idle_delay--;
        }
    }
}

/* --- 0x08008FE2 --- the single entry point the main loop calls. */
void uart_rx_tx_service(void)
{
    uart_rx_parser_AA55();
    uart_tx_sched();
}
