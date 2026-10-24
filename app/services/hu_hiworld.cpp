/* ============================================================================
 * hu_hiworld.cpp -- the stock "AA 55" head-unit dialect.
 *
 *      AA 55 | LEN | CMD | payload[LEN] | CKS          wire length = LEN + 5
 *
 * Ported from reference/decompile/app/uart.c (Ghidra output); see head_unit.hpp
 * for the framing rules and head_unit.cpp for the scheduler that drives this.
 * This is the stock-equivalent half of svc::hu: a --protocol jeep image puts
 * exactly these bytes on the wire.
 *
 * This translation unit is compiled in EVERY build, benz included -- the
 * dialect banner at the top of head_unit.cpp says why.
 * ==========================================================================*/

#include "hu_dialect.hpp"

#include "app_config.hpp"
#include "bytes.hpp"
#include "head_unit.hpp"
#include "ram_map.hpp"

extern "C" {
#include "hal_usart.h"
}

namespace svc {
namespace hu {
namespace hiworld {
namespace {

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

/* ---- the custom 0xB0-0xB8 telemetry block -------------------------------
 * NOT stock. Standalone objects in ram::AppRam::customFrames, deliberately NOT
 * windows onto ram::VehicleState: that block's relative offsets ARE the stock
 * Hiworld protocol and cannot absorb a field the stock protocol does not have.
 * Filled by logic::customPublish() from the 8 ms task. See
 * docs/CUSTOM-TX-SPEC.md. */
volatile std::uint8_t* bufB0() noexcept { return ram::app().customFrames.b0; }
volatile std::uint8_t* bufB2() noexcept { return ram::app().customFrames.b2; }
volatile std::uint8_t* bufB3() noexcept { return ram::app().customFrames.b3; }
volatile std::uint8_t* bufB4() noexcept { return ram::app().customFrames.b4; }
volatile std::uint8_t* bufB5() noexcept { return ram::app().customFrames.b5; }
volatile std::uint8_t* bufB7() noexcept { return ram::app().customFrames.b7; }
volatile std::uint8_t* bufB8() noexcept { return ram::app().customFrames.b8; }
volatile std::uint8_t* bufBB() noexcept { return ram::app().customFrames.bB; }
volatile std::uint8_t* bufBD() noexcept { return ram::app().customFrames.bD; }
volatile std::uint8_t* bufBC() noexcept { return ram::app().customFrames.bC; }

/* ---- shadows: ram::AppRam::uartShadows, the old bare literals ------------ */
volatile std::uint8_t* shadow72() noexcept { return ram::app().uartShadows.msg72; }
volatile std::uint8_t* shadowA6() noexcept { return ram::app().uartShadows.msgA6; }
volatile std::uint8_t* shadow73() noexcept { return ram::app().uartShadows.msg73; }
volatile std::uint8_t* shadowF0() noexcept { return ram::app().uartShadows.msgF0; }
volatile std::uint8_t* shadow12() noexcept { return ram::app().uartShadows.msg12; }
volatile std::uint8_t* shadow32() noexcept { return ram::app().uartShadows.msg32; }
volatile std::uint8_t* shadowB0() noexcept { return ram::app().uartShadows.msgB0; }
volatile std::uint8_t* shadowB2() noexcept { return ram::app().uartShadows.msgB2; }
volatile std::uint8_t* shadowB3() noexcept { return ram::app().uartShadows.msgB3; }
volatile std::uint8_t* shadowB4() noexcept { return ram::app().uartShadows.msgB4; }
volatile std::uint8_t* shadowB5() noexcept { return ram::app().uartShadows.msgB5; }
volatile std::uint8_t* shadowB7() noexcept { return ram::app().uartShadows.msgB7; }
volatile std::uint8_t* shadowB8() noexcept { return ram::app().uartShadows.msgB8; }
volatile std::uint8_t* shadowBB() noexcept { return ram::app().uartShadows.msgBB; }
volatile std::uint8_t* shadowBD() noexcept { return ram::app().uartShadows.msgBD; }
volatile std::uint8_t* shadowBC() noexcept { return ram::app().uartShadows.msgBC; }

/* The non-payload bytes THIS dialect puts on the wire: AA 55 LEN CMD .. CKS.
 * Spelled out rather than read from cfg::kUartFrameOverhead, which follows the
 * ACTIVE protocol -- the check below has to hold for this table in every
 * build, not only in a jeep one. */
constexpr std::size_t kWireOverhead = 5U;

}  // namespace

/* --- flash 0x08009F2C --- box -> head unit, emitted round-robin in order.
 *
 * The first SIX rows are the stock table, in the stock transmit order; a
 * reader diffing this against Ghidra should find them unchanged. The last six
 * are the custom telemetry block and have no stock counterpart at all --
 * docs/CUSTOM-TX-SPEC.md section 2 says which commands in 0xB0-0xBF are built
 * and, as importantly, which are deliberately NOT (0xB1 VERSION and 0xB6 BODY
 * are pure duplicates of stock 0xF0 and 0x12; 0xB7 TPMS and 0xB9 KEY have no
 * source on this car). Those four stay RESERVED: do not fill the gap in the
 * numbering just because it looks untidy -- and the same goes for the gaps at
 * 0xBA and 0xBE (and 0xBF), which ../canbus-to-canbox-signal-map.md section 9
 * has already allocated to LIGHTING, BODY2 and a spare. 0xBB and 0xBD are not
 * free choices either; they are that allocation. 0xBC was reserved for GEAR
 * there and struck out (gear is already in stock 0x32), which freed the number
 * for FLOW, the last row -- appended, so the rows above keep their indices. */
extern constexpr TxDesc kTxTable[kSlots] = {
    { 0x72U, 14U, &buf72, &shadow72 },  /* flags, SWC key, illum, steering   */
    { 0xA6U,  8U, &bufA6, &shadowA6 },  /* amplifier level + tone alias      */
    { 0x73U,  8U, &buf73, &shadow73 },  /* always zero                       */
    { 0xF0U, 17U, &bufF0, &shadowF0 },  /* the version string                */
    { 0x12U, 10U, &buf12, &shadow12 },  /* doors, volume, tone               */
    { 0x32U, 14U, &buf32, &shadow32 },  /* gear, rpm                         */
    /* ---- custom telemetry, jeep builds only (docs/CUSTOM-TX-SPEC.md) ---- */
    { 0xB0U,  2U, &bufB0, &shadowB0 },  /* heartbeat + validity mask         */
    { 0xB2U,  1U, &bufB2, &shadowB2 },  /* ACC, engine, reverse, park brake  */
    { 0xB3U,  6U, &bufB3, &shadowB3 },  /* speed, rpm, load, throttle        */
    { 0xB4U,  5U, &bufB4, &shadowB4 },  /* coolant, intake, fuel %, volts    */
    { 0xB5U,  2U, &bufB5, &shadowB5 },  /* outside air                       */
    { 0xB7U,  5U, &bufB7, &shadowB7 },  /* ONE tyre, rotating through corners */
    { 0xB8U,  4U, &bufB8, &shadowB8 },  /* fuel litres, range                */
    { 0xBBU,  3U, &bufBB, &shadowBB },  /* odometer, 0.1 km                  */
    { 0xBDU,  3U, &bufBD, &shadowBD },  /* compass sector + heading          */
    { 0xBCU,  2U, &bufBC, &shadowBC },  /* fuel flow, 0.1 L/h (appended last) */
};

/* ===========================================================================
 * The couplings this table depends on, made into compile errors. The shared
 * slot-count coupling is asserted in head_unit.cpp; the row-shaped ones are
 * here, because an `extern` table is not usable in a constant expression from
 * any other translation unit.
 *
 * Note the two orderings deliberately differ: kTxTable is in TRANSMIT order
 * (72, A6, 73, F0, 12, 32) while ram::UartShadows is in the stock RAM order
 * (72, 12, 32, A6, 73, F0). Pairing a row with its shadow is therefore a
 * lookup, not a glance -- which is exactly the condition under which a length
 * typo survives review. These asserts do the pairing once, here.
 * ========================================================================= */

/* Row -> shadow, in table order. This file is compiled in EVERY build, so the
 * Hiworld geometry is proved by a --protocol benz build too -- a mistyped
 * length cannot wait for someone to build the other variant. */
static_assert(kTxTable[0].len == sizeof(ram::UartShadows::msg72), "0x72 shadow");
static_assert(kTxTable[1].len == sizeof(ram::UartShadows::msgA6), "0xA6 shadow");
static_assert(kTxTable[2].len == sizeof(ram::UartShadows::msg73), "0x73 shadow");
static_assert(kTxTable[3].len == sizeof(ram::UartShadows::msgF0), "0xF0 shadow");
static_assert(kTxTable[4].len == sizeof(ram::UartShadows::msg12), "0x12 shadow");
static_assert(kTxTable[5].len == sizeof(ram::UartShadows::msg32), "0x32 shadow");
static_assert(kTxTable[6].len == sizeof(ram::UartShadows::msgB0), "0xB0 shadow");
static_assert(kTxTable[7].len == sizeof(ram::UartShadows::msgB2), "0xB2 shadow");
static_assert(kTxTable[8].len == sizeof(ram::UartShadows::msgB3), "0xB3 shadow");
static_assert(kTxTable[9].len == sizeof(ram::UartShadows::msgB4), "0xB4 shadow");
static_assert(kTxTable[10].len == sizeof(ram::UartShadows::msgB5), "0xB5 shadow");
static_assert(kTxTable[11].len == sizeof(ram::UartShadows::msgB7), "0xB7 shadow");
static_assert(kTxTable[12].len == sizeof(ram::UartShadows::msgB8), "0xB8 shadow");
static_assert(kTxTable[13].len == sizeof(ram::UartShadows::msgBB), "0xBB shadow");
static_assert(kTxTable[14].len == sizeof(ram::UartShadows::msgBD), "0xBD shadow");
static_assert(kTxTable[15].len == sizeof(ram::UartShadows::msgBC), "0xBC shadow");

/* Row -> live buffer. The four VehicleState rows must match their window
 * lengths; the other two are standalone objects. */
static_assert(kTxTable[0].len == ram::VehicleState::kWin72Len, "0x72 window");
static_assert(kTxTable[1].len == ram::VehicleState::kWinA6Len, "0xA6 window");
static_assert(kTxTable[2].len == sizeof(ram::AppRam::buf73),   "0x73 buffer");
static_assert(kTxTable[3].len == cfg::kVersionLength,          "0xF0 buffer");
static_assert(kTxTable[4].len == ram::VehicleState::kWin12Len, "0x12 window");
static_assert(kTxTable[5].len == ram::VehicleState::kWin32Len, "0x32 window");

/* ...and the six custom rows against their standalone payload buffers. These
 * are NOT VehicleState windows -- see the note on the buffer accessors. */
static_assert(kTxTable[6].len == sizeof(ram::CustomFrames::b0), "0xB0 buffer");
static_assert(kTxTable[7].len == sizeof(ram::CustomFrames::b2), "0xB2 buffer");
static_assert(kTxTable[8].len == sizeof(ram::CustomFrames::b3), "0xB3 buffer");
static_assert(kTxTable[9].len == sizeof(ram::CustomFrames::b4), "0xB4 buffer");
static_assert(kTxTable[10].len == sizeof(ram::CustomFrames::b5), "0xB5 buffer");
static_assert(kTxTable[11].len == sizeof(ram::CustomFrames::b7), "0xB7 buffer");
static_assert(kTxTable[12].len == sizeof(ram::CustomFrames::b8), "0xB8 buffer");
static_assert(kTxTable[13].len == sizeof(ram::CustomFrames::bB), "0xBB buffer");
static_assert(kTxTable[14].len == sizeof(ram::CustomFrames::bD), "0xBD buffer");
static_assert(kTxTable[15].len == sizeof(ram::CustomFrames::bC), "0xBC buffer");

/* The reserved commands must never appear in the table. 0xE0 is the head
 * unit's own reboot trigger, 0xFE/0xFF are ACK/NAK and 0xD2 is cluster text;
 * emitting any of them from the telemetry block would be read as a command.
 * Written as a fold so a thirteenth row is covered without touching this. */
constexpr bool noReservedCommands() noexcept
{
    for (std::uint8_t i = 0U; i < kSlots; ++i) {
        const std::uint8_t c = kTxTable[i].cmd;
        if (c == 0xE0U || c == 0xFEU || c == 0xFFU || c == 0xD2U) {
            return false;
        }
    }
    return true;
}
static_assert(noReservedCommands(),
              "a TX row claims a command the head unit reads as something else: "
              "0xE0 reboot, 0xFE/0xFF ACK/NAK, 0xD2 cluster text");

namespace {

/* Every row's wire frame must fit the outbound area below kFbTxLen. Written as
 * a fold so a seventh row is covered without touching this. */
constexpr bool txFramesFit() noexcept
{
    for (std::uint8_t i = 0U; i < kSlots; ++i) {
        if ((static_cast<std::size_t>(kTxTable[i].len) + kWireOverhead)
            > kFbTxLen) {
            return false;
        }
    }
    return true;
}
static_assert(txFramesFit(),
              "a TX payload plus the 5-byte AA 55 LEN CMD CKS overhead runs past "
              "the outbound frame area and into the tx length/cursor bytes");

}  // namespace

/* ============================================================================
 * 0x08008D04 uart_build_frame_AA55 -- assemble one message into the frame
 * buffer and hand it to the ISR.
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
 *
 * Split from buildFrame() so the Raise dialect can share the slot machinery:
 * the stock function took a slot index, this takes the three things a frame
 * actually needs. A jeep build emits the identical bytes it always did.
 * ==========================================================================*/
void emit(std::uint8_t cmd, const volatile std::uint8_t* payload,
          std::uint8_t len) noexcept
{
    volatile std::uint8_t* const f = fb();
    const std::uint8_t k = static_cast<std::uint8_t>(len + 4U);

    svc::bytes::copy(&f[4], payload, len);

    f[0] = cfg::kUartPreamble0;
    f[1] = cfg::kUartPreamble1;
    f[2] = len;
    f[3] = cmd;

    std::uint8_t cks = 0U;
    for (std::uint8_t j = 2U; j < k; ++j) {
        cks = static_cast<std::uint8_t>(cks + f[j]);
    }
    /* -1 because the preamble AA + 55 = 0xFF == -1 (mod 256) is not summed. */
    f[k] = static_cast<std::uint8_t>(cks - 1U);

    f[kFbTxPos]  = 0U;
    f[kFbTxLen]  = static_cast<std::uint8_t>(len + 5U);
    f[kFbTxBusy] = 1U;
    usart_tx_start(USART2);
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
                    detail::rxDispatch();
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

}  // namespace hiworld
}  // namespace hu
}  // namespace svc
