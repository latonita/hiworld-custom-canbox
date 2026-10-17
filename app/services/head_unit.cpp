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
#include "hu_dialect.hpp"
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
 * The rest of the fixed RAM this module owns. fb() and rxf() live in
 * hu_dialect.hpp because both dialect TUs need them; these two are the
 * scheduler's alone. All of it is named in ram_map.hpp; nothing here may use
 * a raw address.
 * ========================================================================= */
inline volatile std::uint8_t* ackq() noexcept { return ram::app().uartAckQueue; }
inline volatile std::uint8_t* slotState() noexcept { return ram::app().uartSlotState; }

/* ===========================================================================
 * THE TWO DIALECTS, AND WHY THEY ARE NOT BEHIND #if.
 *
 * svc::hu speaks one of two head-unit dialects (cfg::kProtocol). Each one owns
 * a translation unit of its own -- hu_hiworld.cpp for the stock "AA 55" and
 * hu_raise_benz.cpp for the Raise/Mercedes "2E" -- and hu_dialect.hpp is the
 * contract between them and the scheduler in THIS file: TxDesc, the two frame
 * accessors both parsers need, detail::rxDispatch(), and per dialect a
 * kTxTable, an emit() and an rxParse().
 *
 * BOTH TUs are compiled in EVERY build: tools/build.py and tools/test.py walk
 * app/services/ rather than listing files, and the three selection points
 * below are `if constexpr (cfg::kIsBenz)`, never #if. So the inactive dialect
 * is still parsed, type-checked, warned about and static_asserted -- including
 * the row-geometry asserts that now sit beside each table, which is why both
 * protocols' geometry is still proved by either build -- and only then
 * dropped, by --gc-sections, at link.
 *
 * The alternative -- #if around each dialect -- would mean the inactive one is
 * never even parsed, so a typo in it survives until someone builds the other
 * variant.
 * ========================================================================= */

struct RxDesc {
    std::uint8_t cmd;
    std::uint8_t len;
    BufFn        buf;     /* destination for the payload                    */
    BufFn        flag;    /* the "new data" byte, which in the stock layout
                           * is simply buf[len] -- here it has a name       */
};

/* The active table. A pointer rather than a reference because the two arrays
 * have different extents and so different types; --gc-sections drops
 * whichever .rodata table is not reached. */
constexpr const TxDesc* kTxTable =
    cfg::kIsBenz ? &raisebenz::kTxTable[0] : &hiworld::kTxTable[0];

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
 * Only the first of those is asserted here, because it is the one coupling the
 * two dialects share. The three row-shaped ones are asserted beside the table
 * they are about, in hu_hiworld.cpp and hu_raise_benz.cpp: an `extern` array
 * is not usable in a constant expression, so from here only the ADDRESS of the
 * active table is a compile-time thing, not its contents.
 * ========================================================================= */
static_assert(sizeof(ram::AppRam::uartSlotState) == kTxSlots,
              "ram::AppRam::uartSlotState must have one byte per TX slot; it is "
              "immediately followed by uartTxSlotIdx, so a short array means the "
              "scheduler overwrites its own index");

/* ...and the active dialect must agree with that shared count, or the
 * scheduler walks off the end of the table it was handed. */
static_assert(kTxSlots == (cfg::kIsBenz ? raisebenz::kSlots : hiworld::kSlots),
              "cfg::kTxSlotCount and the active dialect's kTxTable disagree on "
              "how many TX slots there are");

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

}  // namespace

/* Deliberately NOT in the anonymous namespace: both dialect translation
 * units call it, through the declaration in hu_dialect.hpp. */
namespace detail {

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

}  // namespace detail

namespace {

/* The one place the dialect is chosen on receive. */
void rxParse() noexcept
{
    if constexpr (cfg::kIsBenz) {
        raisebenz::rxParse();
    } else {
        hiworld::rxParse();
    }
}

/* The one place the dialect is chosen on transmit. `if constexpr` rather than
 * a function pointer so the unused emitter is dropped entirely by
 * --gc-sections and neither build pays for the other. */
void emitFrame(std::uint8_t cmd, const volatile std::uint8_t* payload,
               std::uint8_t len) noexcept
{
    if constexpr (cfg::kIsBenz) {
        raisebenz::emit(cmd, payload, len);
    } else {
        hiworld::emit(cmd, payload, len);
    }
}

void buildFrame(std::uint8_t slot) noexcept
{
    const TxDesc& e = kTxTable[slot];
    emitFrame(e.cmd, e.buf(), e.len);
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
    /* One payload byte. Going through emitFrame() rather than open-coding the
     * six bytes keeps the ACK in whichever dialect the image speaks; for a
     * jeep build the result is the stock AA 55 01 cmd arg (cmd+arg), because
     * LEN = 1 and the "-1" and the "+LEN" cancel. */
    const std::uint8_t payload = arg;
    emitFrame(cmd, &payload, 1U);
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
