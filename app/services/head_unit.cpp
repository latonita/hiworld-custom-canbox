/* ============================================================================
 * head_unit.cpp -- see head_unit.hpp.
 *
 * Ported from reference/decompile/app/uart.c and the usart2_init part of reference/decompile/app/misc.c
 * (Ghidra output). The explanatory comments are carried
 * over deliberately: in this repo they ARE the reverse-engineering record.
 *
 * THIS IS A NATIONS N32L406, NOT AN STM32 -- but none of that is visible here
 * any more. Every register poke has moved down into app/hal/hal_usart.c; this
 * file owns the board policy (which port, which pins, which clock, which NVIC
 * line, which baud rate) and everything above it.
 * ==========================================================================*/

#include "head_unit.hpp"

#include "app_config.hpp"
#include "bytes.hpp"
#include "power.hpp"
#include "ram_map.hpp"

extern "C" {
#include "hal_gpio.h"
#include "hal_nvic.h"
#include "hal_rcc.h"
#include "hal_usart.h"
#include "hal_wwdg.h"
}

namespace svc {
namespace hu {
namespace {

/* ===========================================================================
 * Board policy -- the facts about THIS box that the HAL deliberately does not
 * know. See app/hal/hal_usart.h, "SCOPE".
 * ========================================================================= */
constexpr std::uint32_t kPinPa2       = 0x0004U;  /* USART2_TX -> head unit  */
constexpr std::uint32_t kPinPa3       = 0x0008U;  /* USART2_RX <- head unit  */
constexpr std::uint8_t  kUsart2Irqn   = 38U;      /* stock literal 0x26      */
constexpr std::uint8_t  kUsart2Preempt = 2U;
constexpr std::uint8_t  kUsart2Sub     = 0U;
constexpr std::uint32_t kPriorityGroup = 0x300U;  /* NVIC_PriorityGroup_4    */

/* ===========================================================================
 * Shorthand for the fixed RAM this module owns. All of it is named in
 * ram_map.hpp; nothing here may use a raw address.
 * ========================================================================= */
inline volatile std::uint8_t* fb() noexcept { return ram::app().uartFrame; }
inline volatile std::uint8_t* rxf() noexcept { return ram::app().uartRxFrame; }
inline volatile std::uint8_t* ackq() noexcept { return ram::app().uartAckQueue; }
inline volatile std::uint8_t* slotState() noexcept { return ram::app().uartSlotState; }

/* ===========================================================================
 * The message tables, from flash 0x08009F2C (TX) and 0x08009F74 (RX), reached
 * through the literal pools at 0x08008E30 / 0x08008E38 / 0x08009004. The stock
 * record shape is { u8 cmd; u8 len; u8 pad[2]; void *buf; void *shadow; }.
 *
 * WHY FUNCTION POINTERS AND NOT DATA POINTERS.
 * Historical, and no longer forced. When this was written the buffers lived
 * inside a ram::app() produced by a reinterpret_cast to a fixed SRAM address,
 * which is NOT a constant expression -- so a table of `volatile uint8_t *`
 * would have needed a runtime initialiser in .init_array, which this image
 * never runs and which app/linker/app.ld fails the build over. A table of
 * POINTERS TO FUNCTIONS is a constant expression, so the table is `constexpr`,
 * lands in .rodata, and the accessors below fold to one constant address each
 * at -Os.
 *
 * The state is an ordinary .bss object now, so `&ram::g_appRam.uartShadows.msg72`
 * IS an address constant and a plain data-pointer table would work. The
 * indirection is kept because it costs nothing after inlining and because the
 * accessors are where the old bare addresses became names; collapsing it is a
 * tidy-up for someone with a bench, not a correctness fix.
 *
 * The accessors are also the place where the old bare addresses became NAMES:
 * what hw_uart.c spelled 0x20001B98 is ram::app().uartShadows.msg72.
 * ========================================================================= */
using BufFn = volatile std::uint8_t* (*)() noexcept;

struct TxDesc {
    std::uint8_t cmd;
    std::uint8_t len;
    BufFn        buf;     /* live payload source                            */
    BufFn        shadow;  /* last-sent copy, for change detection           */
};

struct RxDesc {
    std::uint8_t cmd;
    std::uint8_t len;
    BufFn        buf;     /* destination for the payload                    */
    BufFn        flag;    /* the "new data" byte, which in the stock layout
                           * is simply buf[len] -- here it has a name       */
};

/* ---- live buffers: windows into ram::VehicleState, plus two standalone --- */
volatile std::uint8_t* buf72() noexcept
{
    return ram::app().vehicle.window(ram::VehicleState::kWin72Off);
}
volatile std::uint8_t* bufA6() noexcept
{
    return ram::app().vehicle.window(ram::VehicleState::kWinA6Off);
}
volatile std::uint8_t* buf12() noexcept
{
    return ram::app().vehicle.window(ram::VehicleState::kWin12Off);
}
volatile std::uint8_t* buf32() noexcept
{
    return ram::app().vehicle.window(ram::VehicleState::kWin32Off);
}
/* 0x20001B7D. Never written by anything in this image: message 0x73 is eight
 * zero bytes for ever, and is still sent because the head unit expects it. */
volatile std::uint8_t* buf73() noexcept { return ram::app().buf73; }
/* 0x20001B86 -- also the liveness probe tools/iap_upload.py --verify reads
 * back over SWD to prove the flashed image booted. */
volatile std::uint8_t* bufF0() noexcept { return ram::g_versionString; }

/* ---- shadows: ram::AppRam::uartShadows, the old bare literals ------------ */
volatile std::uint8_t* shadow72() noexcept { return ram::app().uartShadows.msg72; }
volatile std::uint8_t* shadowA6() noexcept { return ram::app().uartShadows.msgA6; }
volatile std::uint8_t* shadow73() noexcept { return ram::app().uartShadows.msg73; }
volatile std::uint8_t* shadowF0() noexcept { return ram::app().uartShadows.msgF0; }
volatile std::uint8_t* shadow12() noexcept { return ram::app().uartShadows.msg12; }
volatile std::uint8_t* shadow32() noexcept { return ram::app().uartShadows.msg32; }

/* --- flash 0x08009F2C --- box -> head unit, emitted round-robin in order. */
constexpr TxDesc kTxTable[kTxSlots] = {
    { 0x72U, 14U, &buf72, &shadow72 },  /* flags, SWC key, illum, steering   */
    { 0xA6U,  8U, &bufA6, &shadowA6 },  /* amplifier level + tone alias      */
    { 0x73U,  8U, &buf73, &shadow73 },  /* always zero                       */
    { 0xF0U, 17U, &bufF0, &shadowF0 },  /* the version string                */
    { 0x12U, 10U, &buf12, &shadow12 },  /* doors, volume, tone               */
    { 0x32U, 14U, &buf32, &shadow32 },  /* gear, rpm                         */
};

/* ===========================================================================
 * The couplings this table depends on, made into compile errors.
 *
 * Adding a TX slot touches four things that do NOT follow from kTxSlots, and
 * every one of them fails SILENTLY:
 *
 *   - ram::AppRam::uartSlotState is a hand-sized array. If kTxSlots outgrows
 *     it, resetState() and txSched() index one past the end -- which is
 *     uartTxSlotIdx, the very next byte. The scheduler would corrupt its own
 *     slot index on every sweep, and the tests would corrupt the same byte and
 *     keep passing.
 *   - a row's `len` is copied into that row's shadow with no bounds check, so a
 *     len larger than the shadow writes into the NEXT message's shadow and
 *     poisons its change detection. The symptom shows up on a different
 *     message entirely.
 *   - the same len is copied out of the live buffer, so an over-long len reads
 *     past the end of ram::VehicleState's window.
 *   - the built frame is len + 5 bytes and has to fit under kFbTxLen.
 *
 * Note the two orderings deliberately differ: kTxTable is in TRANSMIT order
 * (72, A6, 73, F0, 12, 32) while ram::UartShadows is in the stock RAM order
 * (72, 12, 32, A6, 73, F0). Pairing a row with its shadow is therefore a
 * lookup, not a glance -- which is exactly the condition under which a length
 * typo survives review. These asserts do the pairing once, here.
 * ========================================================================= */
static_assert(sizeof(ram::AppRam::uartSlotState) == kTxSlots,
              "ram::AppRam::uartSlotState must have one byte per TX slot; it is "
              "immediately followed by uartTxSlotIdx, so a short array means the "
              "scheduler overwrites its own index");

/* Row -> shadow, in table order. */
static_assert(kTxTable[0].len == sizeof(ram::UartShadows::msg72), "0x72 shadow");
static_assert(kTxTable[1].len == sizeof(ram::UartShadows::msgA6), "0xA6 shadow");
static_assert(kTxTable[2].len == sizeof(ram::UartShadows::msg73), "0x73 shadow");
static_assert(kTxTable[3].len == sizeof(ram::UartShadows::msgF0), "0xF0 shadow");
static_assert(kTxTable[4].len == sizeof(ram::UartShadows::msg12), "0x12 shadow");
static_assert(kTxTable[5].len == sizeof(ram::UartShadows::msg32), "0x32 shadow");

/* Row -> live buffer. The four VehicleState rows must match their window
 * lengths; the other two are standalone objects. */
static_assert(kTxTable[0].len == ram::VehicleState::kWin72Len, "0x72 window");
static_assert(kTxTable[1].len == ram::VehicleState::kWinA6Len, "0xA6 window");
static_assert(kTxTable[2].len == sizeof(ram::AppRam::buf73),   "0x73 buffer");
static_assert(kTxTable[3].len == cfg::kVersionLength,          "0xF0 buffer");
static_assert(kTxTable[4].len == ram::VehicleState::kWin12Len, "0x12 window");
static_assert(kTxTable[5].len == ram::VehicleState::kWin32Len, "0x32 window");

/* Every row's wire frame must fit the outbound area below kFbTxLen. Written as
 * a fold so a seventh row is covered without touching this. */
constexpr bool txFramesFit() noexcept
{
    for (std::uint8_t i = 0U; i < kTxSlots; ++i) {
        if ((static_cast<std::size_t>(kTxTable[i].len) + cfg::kUartFrameOverhead)
            > kFbTxLen) {
            return false;
        }
    }
    return true;
}
static_assert(txFramesFit(),
              "a TX payload plus the 5-byte AA 55 LEN CMD CKS overhead runs past "
              "the outbound frame area and into the tx length/cursor bytes");

/* ---- inbound destinations ------------------------------------------------
 * In the stock table buf == shadow for these records and the "new data" byte
 * is reached as buf[len]; ram_map.hpp static_asserts that adjacency, so the
 * flag can be addressed by name instead of by arithmetic. The old code reached
 * the 0xAD payload twice under two different names (g_cmd_block[0x0E] and
 * g_periodic_tx_cfg); they are the same two bytes and now have one name. */
volatile std::uint8_t* bufD2() noexcept { return ram::app().hostCmd.cluster; }
volatile std::uint8_t* flagD2() noexcept { return &ram::app().hostCmd.clusterNewData; }
volatile std::uint8_t* bufAD() noexcept { return ram::app().hostCmd.periodicTxCfg; }
volatile std::uint8_t* flagAD() noexcept { return &ram::app().hostCmd.periodicNewData; }

/* --- flash 0x08009F74 --- head unit -> box. 0xFC and 0xE0 are handled
 * outside the table, in rxDispatch(). */
constexpr RxDesc kRxTable[kRxSlots] = {
    { 0xD2U, 13U, &bufD2, &flagD2 },  /* instrument-cluster text            */
    { 0xADU,  2U, &bufAD, &flagAD },  /* periodic-TX / amplifier config     */
};

static_assert(kRxTable[0].len == sizeof(ram::HeadUnitCommands::cluster),
              "the 0xD2 payload must match ram::HeadUnitCommands::cluster; it is "
              "immediately followed by clusterNewData, so an over-long len would "
              "raise the new-data flag as a side effect of the copy");
static_assert(kRxTable[1].len == sizeof(ram::HeadUnitCommands::periodicTxCfg),
              "the 0xAD payload must match ram::HeadUnitCommands::periodicTxCfg");

/* ---- the slot rotation ---------------------------------------------------
 * util_wrap_index(idx, +1, hi = 5, lo = 0, wrap_mode = 0). The stock
 * wrap_mode's sense is the opposite of its name: 0 means WRAP, which is
 * svc::bytes::wrapIndex's `clamp == false`. See bytes.hpp. */
inline std::uint8_t nextSlot(std::uint8_t idx) noexcept
{
    return svc::bytes::wrapIndex(static_cast<int>(idx), 1,
                                 static_cast<int>(kTxSlots) - 1, 0, false);
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
 *
 * gpio_init_pin(GPIOA, mask, GPIO_MODE_AF_PP) picks AF4 for PA2/PA3 out of its
 * own port/pin table (hal_gpio.c, stock 0x08002494) and enables the GPIO/AFIO
 * clocks, so there is nothing else to do for the muxing. PA3 (RX) is
 * configured BEFORE PA2 (TX), exactly as the stock code does.
 *
 * usart_configure() takes PCLK1 explicitly because choosing PCLK1 over PCLK2
 * is a board decision, not a driver one: USART2 is on APB1. With
 * PCLK1 = 16 MHz and baud = 38400 that reproduces the stock BRCF = 0x01A1:
 *     d = 25 * 16000000 / (4 * 38400) = 2604
 *     mantissa = 26, fraction = (4 * 16 + 50) / 100 = 1  ->  0x01A1
 * = 26.0625 against the ideal 26.0417, i.e. +0.08% error.
 * ==========================================================================*/
void usart2Init(std::uint32_t baud) noexcept
{
    rcc_apb1_periph_clock(RCC_APB1_USART2, 1);

    nvic_set_priority_grouping(kPriorityGroup);
    nvic_init(kUsart2Irqn, kUsart2Preempt, kUsart2Sub, 1);

    gpio_init_pin(GPIOA, kPinPa3, GPIO_MODE_AF_PP);
    gpio_init_pin(GPIOA, kPinPa2, GPIO_MODE_AF_PP);

    usart_configure(USART2, cfg::kPclk1Hz, baud);
    usart_it_config(USART2, USART_INT_RXDNE, 1);  /* RX drives everything   */
    usart_it_config(USART2, USART_INT_TXDE, 0);   /* armed only per frame   */
    usart_cmd_enable(USART2, 1);
}

/* ============================================================================
 * 0x08008A96 uart_rx_dispatch -- act on one validated inbound frame.
 *
 * 1. table-driven: 0xD2 (cluster text, 13 B) and 0xAD (periodic-TX config,
 *    2 B). The payload is only copied when it actually CHANGED, and then the
 *    "new data" flag is raised for the consumer, which clears it.
 * 2. 0xFC vendor handshake -> echo the frame back with CMD changed to 0xFD and
 *    the checksum incremented by 1. That is correct precisely because CKS is a
 *    plain sum and 0xFD - 0xFC = 1. Fixed 14-byte reply, so the echo is only
 *    well-formed for LEN == 9, which is the only length the head unit sends.
 * 3. 0xE0 -> force a watchdog reset, which drops back into the IAP bootloader.
 * Anything else, 0xFF ACKs included, is silently ignored: this box cannot be
 * polled, only listened to.
 * ==========================================================================*/
void rxDispatch() noexcept
{
    volatile std::uint8_t* const f = fb();
    volatile std::uint8_t* const rf = rxf();

    for (std::uint8_t i = 0U; i < kRxSlots; ++i) {
        const RxDesc& e = kRxTable[i];

        if (rf[3] == e.cmd && rf[2] == e.len &&
            svc::bytes::differ(e.buf(), &rf[4], e.len)) {
            svc::bytes::copy(e.buf(), &rf[4], e.len);
            *e.flag() = 1U;
        }
    }

    /* The 0xFC payload must be 3F F3 F1 01 ?? ?? 3A A3 ?? -- i.e. LEN >= 9 --
     * and we must not be mid-transmission, since there is no TX queue. */
    if (rf[3] == 0xFCU && rf[4] == 0x3FU && rf[5] == 0xF3U && rf[6] == 0xF1U &&
        rf[7] == 0x01U && rf[10] == 0x3AU && rf[11] == 0xA3U &&
        f[kFbTxBusy] == 0U) {
        f[0] = cfg::kUartPreamble0;
        f[1] = cfg::kUartPreamble1;
        svc::bytes::copy(&f[2], &rf[2], 0x0BU);   /* LEN, CMD, payload[0..8] */
        f[3]  = 0xFDU;
        f[13] = static_cast<std::uint8_t>(rf[13] + 1U);  /* CMD bumped by 1  */
        f[kFbTxPos]  = 0U;
        f[kFbTxLen]  = 0x0EU;                     /* 14 bytes on the wire    */
        f[kFbTxBusy] = 1U;
        usart_tx_start(USART2);
    }

    if (rf[3] == 0xE0U) {
        /* The head unit asked us to reboot. Stock calls wwdg_force_reset()
         * bare; this records the reason first. See svc::power's code table. */
        svc::power::fatalErrorReset(svc::power::kFatalHostCommand);
    }
}

/* ============================================================================
 * 0x08008BB2 uart_rx_parser_AA55 -- byte-at-a-time state machine over the ring.
 *
 *   state 0 : wait for 0xAA
 *   state 1 : expect 0x55, else back to 0
 *   state 2 : LEN; rejected if LEN >= 0x38 (56). Needs LEN+2 further bytes
 *             buffered, or we leave the tail put and retry on the next call.
 *   state>=3: CMD + payload into RXF[3 .. LEN+3], accumulating the running sum;
 *             then RXF[LEN+4] is the checksum and must equal (sum - 1) & 0xFF.
 *             The running sum is SEEDED WITH LEN, so sum - 1 is exactly the
 *             documented (LEN + CMD + sum(payload) - 1) & 0xFF.
 *
 * Tail-commit discipline, reproduced exactly: the tail index at FB[0x108] is
 * only written when the machine is back in state 0. A half-received frame
 * therefore leaves the tail parked on its 0xAA and is re-parsed from scratch
 * on the next call -- which is also why there is no timeout anywhere.
 *
 * One frame per call: as soon as a frame completes (checksum good or bad) the
 * function commits the tail and returns.
 *
 * The reassembled frame lives in ram::AppRam::uartRxFrame (0x20001B01);
 * RXF[0] and RXF[1] are never written, because the preamble is implicit.
 *
 * NOTE there is no escaping anywhere in this protocol: a payload byte of 0xAA
 * or 0x55 is perfectly legal and is consumed by the state >= 3 branch like any
 * other. Resynchronisation after a corrupt frame depends only on the LEN bound
 * and the checksum failing.
 * ==========================================================================*/
void rxParse() noexcept
{
    volatile std::uint8_t* const f = fb();
    volatile std::uint8_t* const rf = rxf();

    const std::uint8_t head = f[kFbRxHead];   /* read once, as the stock does */
    std::uint32_t tail  = f[kFbRxTail];
    std::uint32_t state = 0U;
    std::uint32_t sum   = 0U;  /* running checksum                           */
    std::uint32_t need  = 0U;  /* index of the checksum byte = LEN + 4        */

    for (;;) {
        if (head == tail) {
            return;                       /* ring drained; tail left as-is   */
        }

        const std::uint8_t b = f[kFbRxRing + tail];

        if (state == 0U) {
            state = (b == cfg::kUartPreamble0) ? 1U : 0U;
        } else if (state == 1U) {
            /* 5A A5 never gets here: the only accepted second byte is 0x55,
             * so the sibling dialect simply falls back to state 0. */
            state = (b == cfg::kUartPreamble1) ? 2U : 0U;
        } else if (state == 2U) {
            if (b < (cfg::kUartMaxPayload + 1U)) {
                /* bytes still buffered, counting this LEN byte. The stock
                 * `& 0xFF` is a no-op on a 0..199 result; kept for fidelity. */
                const std::uint32_t avail = static_cast<std::uint32_t>(
                    ((static_cast<int>(head) - static_cast<int>(tail)
                      + static_cast<int>(kRxRingSize))
                     % static_cast<int>(kRxRingSize)) & 0xFF);

                if (avail < static_cast<std::uint32_t>(b) + 2U) {
                    return;               /* incomplete: retry next call     */
                }
                rf[2] = b;                /* LEN                             */
                state = 3U;
                need  = (static_cast<std::uint32_t>(b) + 4U) & 0xFFU;
                sum   = b;
            } else {
                state = 0U;               /* oversized: resynchronise        */
            }
        } else {
            rf[state] = b;
            if (state < need) {
                sum   = (sum + static_cast<std::uint32_t>(rf[state])) & 0xFFU;
                state = (state + 1U) & 0xFFU;
            } else {
                sum = (sum - 1U) & 0xFFU;
                if (static_cast<std::uint32_t>(rf[state]) == sum) {
                    rxDispatch();
                }
                /* frame finished either way: commit the tail and leave */
                tail = (tail + 1U) % kRxRingSize;
                f[kFbRxTail] = static_cast<std::uint8_t>(tail);
                return;
            }
        }

        tail = (tail + 1U) % kRxRingSize;
        if (state == 0U) {
            f[kFbRxTail] = static_cast<std::uint8_t>(tail);
        }
    }
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
 *
 * The stock code accumulates straight into FB[k] after zeroing it; the local
 * accumulator here is identical in effect, since the summed range FB[2..k-1]
 * never includes FB[k] and nothing else observes the buffer mid-loop.
 * ==========================================================================*/
void buildFrame(std::uint8_t slot) noexcept
{
    const TxDesc& e = kTxTable[slot];
    volatile std::uint8_t* const f = fb();

    const std::uint8_t len = e.len;
    const std::uint8_t k   = static_cast<std::uint8_t>(len + 4U);

    svc::bytes::copy(&f[4], e.buf(), len);

    f[0] = cfg::kUartPreamble0;
    f[1] = cfg::kUartPreamble1;
    f[2] = len;
    f[3] = e.cmd;

    std::uint8_t cks = 0U;
    for (std::uint8_t j = 2U; j < k; ++j) {
        cks = static_cast<std::uint8_t>(cks + f[j]);
    }
    /* -1 because the preamble AA + 55 = 0xFF == -1 (mod 256) is not summed. */
    f[k] = static_cast<std::uint8_t>(cks - 1U);

    f[kFbTxPos]  = 0U;
    f[kFbTxLen]  = static_cast<std::uint8_t>(len + cfg::kUartFrameOverhead);
    f[kFbTxBusy] = 1U;
    usart_tx_start(USART2);
}

/* ============================================================================
 * 0x08008D9C uart_tx_round_robin -- find the next TX table entry whose live
 * payload differs from its shadow copy, starting at the current slot index and
 * wrapping. On a hit the shadow is refreshed and the index is LEFT POINTING AT
 * that slot (the caller builds the frame). If a full sweep finds nothing, the
 * index is parked at kTxSlots = "nothing changed", which is the caller's cue
 * to run the ~720 ms keep-alive instead.
 *
 * The index is assumed to be in range on entry; the scheduler clamps it just
 * above, exactly as the stock code does.
 * ==========================================================================*/
void roundRobin() noexcept
{
    ram::AppRam& r = ram::app();
    const std::uint8_t start = r.uartTxSlotIdx;

    do {
        const TxDesc& e = kTxTable[r.uartTxSlotIdx];

        if (svc::bytes::differ(e.shadow(), e.buf(), e.len)) {
            svc::bytes::copy(e.shadow(), e.buf(), e.len);
            return;
        }
        r.uartTxSlotIdx = nextSlot(r.uartTxSlotIdx);
    } while (r.uartTxSlotIdx != start);

    r.uartTxSlotIdx = kTxSlots;   /* sentinel: nothing to send */
}

/* ============================================================================
 * 0x08008E62 uart_tx_sched -- the transmit scheduler.
 *
 * Called from the 8 ms slot of the main scheduler. Nothing happens while a
 * frame is still being clocked out (FB[0x3E]). Otherwise:
 *
 *   - drain at most one entry of the 10-deep ACK queue, skipping the reserved
 *     codes 0xFE/0xFF. The tick is rolled BACK when an ACK is actually emitted
 *     so the ACK does not consume this slot's data frame. Dead in this build:
 *     the producer index is never advanced by anything.
 *   - every 6th pass (6 x 8 ms = 48 ms) emit one data frame:
 *       slot_state == 1      -> explicit send request: refresh shadow, send
 *       slot_state == 2 or 3 -> already sent this sweep: step to the next slot
 *       slot_state == 0      -> round-robin for a changed payload; if one was
 *                               found (index < 6) send it
 *       nothing changed      -> after kIdleDelay (15) idle passes, i.e.
 *                               ~720 ms, send the next table entry anyway as a
 *                               keep-alive, using the SEPARATE aux index.
 *
 * Note the "already sent" branch increments the index and then re-tests the
 * INCREMENTED value against the bound -- that is what the stock code does, and
 * it is why slot 0's state is cleared twice on a wrap.
 * ==========================================================================*/
void txSched() noexcept
{
    ram::AppRam& r = ram::app();
    volatile std::uint8_t* const f = fb();
    volatile std::uint8_t* const q = ackq();
    volatile std::uint8_t* const st = slotState();

    r.uartTxTick = static_cast<std::uint8_t>(r.uartTxTick + 1U);

    if (f[kFbTxBusy] != 0U) {
        return;
    }

    if (q[kAckQueueTail] != q[kAckQueueHead]) {
        const std::uint8_t qt   = q[kAckQueueTail];
        const std::uint8_t code = q[qt];

        if (code != 0xFEU && code != 0xFFU) {
            txAck(0xFFU, code);
            r.uartTxTick = static_cast<std::uint8_t>(r.uartTxTick - 1U);
        }
        q[kAckQueueTail] =
            static_cast<std::uint8_t>((static_cast<std::size_t>(qt) + 1U)
                                      % kAckQueueEntries);
    }

    if (r.uartTxTick <= static_cast<std::uint8_t>(kTicksPerFrame - 1U)) {
        return;
    }
    r.uartTxTick = 0U;

    if (r.uartTxSlotIdx > static_cast<std::uint8_t>(kTxSlots - 1U)) {
        r.uartTxSlotIdx = 0U;
        st[r.uartTxSlotIdx] = kSlotIdle;
    }

    if (st[r.uartTxSlotIdx] == 3U || st[r.uartTxSlotIdx] == 2U) {
        st[r.uartTxSlotIdx] = kSlotIdle;
        r.uartTxSlotIdx = static_cast<std::uint8_t>(r.uartTxSlotIdx + 1U);
        if (r.uartTxSlotIdx > static_cast<std::uint8_t>(kTxSlots - 1U)) {
            r.uartTxSlotIdx = 0U;
            st[r.uartTxSlotIdx] = kSlotIdle;
        }
    }

    if (st[r.uartTxSlotIdx] == kSlotRequest) {
        const TxDesc& e = kTxTable[r.uartTxSlotIdx];

        svc::bytes::copy(e.shadow(), e.buf(), e.len);
        buildFrame(r.uartTxSlotIdx);
        st[r.uartTxSlotIdx] = kSlotSent;
    } else {
        if (st[r.uartTxSlotIdx] == kSlotIdle) {
            roundRobin();
        }

        if (r.uartTxSlotIdx < kTxSlots) {
            buildFrame(r.uartTxSlotIdx);
            st[r.uartTxSlotIdx] = kSlotSent;
        } else if (r.uartIdleDelay == 0U) {
            r.uartIdleDelay   = kIdleDelay;
            r.uartAuxSlotIdx  = nextSlot(r.uartAuxSlotIdx);
            buildFrame(r.uartAuxSlotIdx);
        } else {
            r.uartIdleDelay = static_cast<std::uint8_t>(r.uartIdleDelay - 1U);
        }
    }
}

}  // namespace

/* ============================================================================
 * 0x08008A18 uart_reset_state -- drop any frame in flight, empty both rings.
 *
 * Note uartTxSlotIdx is seeded to kTxSlots, i.e. one PAST the last table slot:
 * that is the "nothing to send" sentinel txSched() tests for, so the very
 * first scheduler pass runs a full round-robin sweep.
 * ==========================================================================*/
void resetState() noexcept
{
    volatile std::uint8_t* const f = fb();
    volatile std::uint8_t* const q = ackq();

    f[kFbTxLen]  = 0U;
    f[kFbTxPos]  = 0U;
    f[kFbTxBusy] = 0U;
    f[kFbRxHead] = f[kFbRxTail];   /* RX ring empty  */

    q[kAckQueueHead] = q[kAckQueueTail];   /* ACK queue empty */

    ram::app().uartTxSlotIdx = kTxSlots;
    for (std::uint8_t i = 0U; i < kTxSlots; ++i) {
        slotState()[i] = kSlotIdle;
    }
}

/* ============================================================================
 * 0x08008A54 uart_init_38400_and_version
 *
 * The version string MUST end up as 17 bytes at SRAM 0x20001B86: it is both
 * the payload of TX slot 3 (CMD 0xF0) and the liveness probe the flashing tool
 * reads back over SWD to prove the app booted (tools/iap_upload.py --verify,
 * app/REBUILD.md section 6). It is NOT NUL-terminated on the wire, which is
 * why the copy is bounded by cfg::kVersionLength and not by sizeof.
 *
 * The stock code calls memcpy_bytes from a flash literal @0x08008E18 to the
 * pointer literal @0x08008E2C; the explicit loop here is byte-for-byte the same
 * operation, without needing a cast to get at either end.
 * ==========================================================================*/
void init() noexcept
{
    resetState();
    usart2Init(cfg::kUartBaud);     /* stock literal 0x9600 == 38400 */

    for (std::uint8_t i = 0U; i < cfg::kVersionLength; ++i) {
        ram::g_versionString[i] = static_cast<std::uint8_t>(cfg::kVersionString[i]);
    }
}

/* ============================================================================
 * 0x08008CCE uart_tx_ack_frame -- the fixed 6-byte, one-payload-byte frame
 *     AA 55 01 <cmd> <arg> <cks>,   cks = cmd + arg  (== sum of all - 1)
 * Only ever called as txAck(0xFF, X), the box -> head-unit ACK. In this build
 * the ACK queue's producer index is never advanced by anything, so this path
 * is dead on the wire -- kept because it is stock behaviour.
 * ==========================================================================*/
void txAck(std::uint8_t cmd, std::uint8_t arg) noexcept
{
    volatile std::uint8_t* const f = fb();

    f[0] = cfg::kUartPreamble0;
    f[1] = cfg::kUartPreamble1;
    f[2] = 1U;                       /* LEN: one payload byte */
    f[3] = cmd;
    f[4] = arg;
    f[5] = static_cast<std::uint8_t>(cmd + arg);

    f[kFbTxLen]  = 6U;
    f[kFbTxPos]  = 0U;
    f[kFbTxBusy] = 1U;
    usart_tx_start(USART2);
}

/* ============================================================================
 * 0x0800286E usart_aux_rxtx_service -- the USART2 interrupt BODY.
 *
 * The `extern "C" void USART2_IRQHandler(void)` the bootloader's veneer (slot
 * 47, IRQ 38, stock handler @0x08003016 -> 0x0800286E) branches to is defined
 * in app/logic/interrupts.cpp and does nothing but call this. It must keep
 * that exact spelling, or crt0.S's weak trap stub stays in place and the box
 * resets with a trap code instead of talking to the head unit.
 *
 * RX: one byte into the 200-byte ring. The ring is FULL when advancing head
 * would hit tail, in which case the byte is DROPPED and the overflow flag is
 * latched -- the stock code never clears it again.
 * TX: one byte out of the frame buffer per TXE until the cursor reaches the
 * length, then disarm TXE and release the busy flag.
 * ==========================================================================*/
void onInterrupt() noexcept
{
    volatile std::uint8_t* const f = fb();

    if (usart_get_it_status(USART2, USART_INT_RXDNE) != 0) {
        const std::uint8_t  b    = usart_receive_data(USART2);
        const std::uint8_t  head = f[kFbRxHead];
        const std::uint32_t next =
            (static_cast<std::uint32_t>(head) + 1U) % kRxRingSize;

        if (static_cast<std::uint32_t>(f[kFbRxTail]) == next) {
            ram::app().uartRxOverflow = 1U;
        } else {
            f[kFbRxRing + head] = b;
            f[kFbRxHead] = static_cast<std::uint8_t>(next);
        }
    }

    if (usart_get_it_status(USART2, USART_INT_TXDE) != 0) {
        const std::uint8_t pos = f[kFbTxPos];

        if (pos < f[kFbTxLen]) {
            usart_send_data(USART2, f[pos]);
            f[kFbTxPos] = static_cast<std::uint8_t>(pos + 1U);
        } else {
            usart_it_config(USART2, USART_INT_TXDE, 0);
            f[kFbTxBusy] = 0U;
        }
    }
}

/* --- 0x08003088 uart_disable_usart2 --- clear CTRL1.UE, then disarm TXE. */
void disable() noexcept
{
    usart_disable(USART2);
}

/* --- 0x08008FE2 uart_rx_tx_service --- the single entry point the main loop
 * calls, from its 8 ms slot. Parse first, then schedule: a 0xFC handshake
 * received this pass therefore claims the transmitter before the round-robin
 * can, which is what makes the handshake reply prompt. */
void service() noexcept
{
    rxParse();
    txSched();
}

}  // namespace hu
}  // namespace svc
