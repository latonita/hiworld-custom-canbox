/* ============================================================================
 * hu_raise_benz.cpp -- the Raise/Mercedes "2E" head-unit dialect.
 *
 *      2E | CMD | LEN | payload[LEN] | CKS             wire length = LEN + 4
 *
 * NOT a rebuild of anything: the stock firmware never spoke this dialect, so
 * nothing in this file carries a stock flash address. It emulates a Raise
 * Mercedes canbox so the radio draws its richer gauge screen; see
 * docs/raise-benz-emulation.md and the Protocol block in app_config.hpp.
 *
 * This translation unit is compiled in EVERY build, jeep included -- the
 * dialect banner at the top of head_unit.cpp says why.
 * ==========================================================================*/

#include "hu_dialect.hpp"

#include "app_config.hpp"
#include "bytes.hpp"
#include "head_unit.hpp"
#include "power.hpp"
#include "ram_map.hpp"

extern "C" {
#include "hal_usart.h"
}

namespace svc {
namespace hu {
namespace raisebenz {
namespace {

/* ---- Benz live buffers and shadows, ram::AppRam::benzFrames ------------- */
volatile std::uint8_t* bufBenzRpm() noexcept    { return ram::app().benzFrames.rpm08; }
volatile std::uint8_t* bufBenzSpeed() noexcept  { return ram::app().benzFrames.speed07; }
volatile std::uint8_t* bufBenzDoor() noexcept   { return ram::app().benzFrames.door41; }
volatile std::uint8_t* bufBenzTrip() noexcept   { return ram::app().benzFrames.trip41; }
volatile std::uint8_t* bufBenzEngine() noexcept { return ram::app().benzFrames.engine41; }

volatile std::uint8_t* shBenzRpm() noexcept    { return ram::app().benzShadows.rpm08; }
volatile std::uint8_t* shBenzSpeed() noexcept  { return ram::app().benzShadows.speed07; }
volatile std::uint8_t* shBenzDoor() noexcept   { return ram::app().benzShadows.door41; }
volatile std::uint8_t* shBenzTrip() noexcept   { return ram::app().benzShadows.trip41; }
volatile std::uint8_t* shBenzEngine() noexcept { return ram::app().benzShadows.engine41; }

/* The non-payload bytes THIS dialect puts on the wire: 2E CMD LEN .. CKS.
 * Spelled out rather than read from cfg::kUartFrameOverhead, which follows the
 * ACTIVE protocol -- the check below has to hold for this table in every
 * build, not only in a benz one. */
constexpr std::size_t kWireOverhead = 4U;

}  // namespace

/* The Benz set. Three rows share CMD 0x41 and are told apart by LEN and by
 * payload[0], the page selector that logic::benzPrimeFrames() writes --
 * which is why the lengths here are exact and not "at least".
 *
 * The LENGTHS ARE LOAD-BEARING BEYOND THE CHECKSUM: the plugin's own length
 * gate admits a short page-2 or page-3 frame and then reads past it, so an
 * under-long frame does not get ignored, it throws inside the radio's canbus
 * service. 6 and 11 are the safe lengths, not the minimum ones. */
extern constexpr TxDesc kTxTable[kSlots] = {
    { 0x08U,  2U, &bufBenzRpm,    &shBenzRpm    },  /* engine rpm            */
    { 0x07U,  2U, &bufBenzSpeed,  &shBenzSpeed  },  /* road speed, km/h      */
    { 0x41U,  2U, &bufBenzDoor,   &shBenzDoor   },  /* page 1: doors         */
    { 0x41U,  6U, &bufBenzTrip,   &shBenzTrip   },  /* page 2: odo + ambient */
    { 0x41U, 11U, &bufBenzEngine, &shBenzEngine },  /* page 3: engine        */
};

/* ===========================================================================
 * The couplings this table depends on, made into compile errors. The shared
 * slot-count coupling is asserted in head_unit.cpp; the row-shaped ones are
 * here, because an `extern` table is not usable in a constant expression from
 * any other translation unit.
 * ========================================================================= */

/* The Benz rows, against the buffers AND their shadows. The payload sizes
 * are the plugin's safe lengths (see the table), so these asserts are the
 * thing standing between a one-byte typo and an exception inside the
 * radio's canbus service. This file is compiled in EVERY build, so they are
 * checked by a --protocol jeep build too: a mistyped Benz length cannot wait
 * for someone to build the other variant. */
static_assert(kTxTable[0].len == sizeof(ram::BenzFrames::rpm08),    "0x08 buffer");
static_assert(kTxTable[1].len == sizeof(ram::BenzFrames::speed07),  "0x07 buffer");
static_assert(kTxTable[2].len == sizeof(ram::BenzFrames::door41),   "0x41 p1 buffer");
static_assert(kTxTable[3].len == sizeof(ram::BenzFrames::trip41),   "0x41 p2 buffer");
static_assert(kTxTable[4].len == sizeof(ram::BenzFrames::engine41), "0x41 p3 buffer");
static_assert(sizeof(ram::AppRam::benzFrames) == sizeof(ram::AppRam::benzShadows),
              "every Benz payload needs a shadow of exactly its own size, or "
              "change detection writes into the next message's shadow");

/* A Raise LEN of 0 wedges the head unit's parser (it counts down a SIGNED
 * byte), and anything over 117 overruns its 120-byte frame cap. Neither is
 * reachable with the table above, and this is what keeps it that way. */
static_assert([]() constexpr {
                  for (const TxDesc& e : kTxTable) {
                      if (e.len < cfg::kRaiseMinPayload) { return false; }
                      if (e.len > cfg::kRaiseMaxPayload) { return false; }
                  }
                  return true;
              }(),
              "a Benz TX row has a payload length the head unit's parser "
              "cannot survive: LEN must be 1..117");

namespace {

/* Every row's wire frame must fit the outbound area below kFbTxLen. Written as
 * a fold so a sixth row is covered without touching this. */
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
              "a TX payload plus the 4-byte 2E CMD LEN CKS overhead runs past "
              "the outbound frame area and into the tx length/cursor bytes");

/* ============================================================================
 * The IAP escape hatch -- NOT a stock routine.
 *
 * Feed it every byte the Raise parser sees while it is between frames. When
 * the whole of cfg::kIapResetFrame has gone past in order, the head unit is
 * asking us to reboot into the bootloader so it can flash us, and we do
 * exactly what the Hiworld dispatcher does for CMD 0xE0.
 *
 * Without this a Benz build could only ever be reflashed on the bench, over
 * the FTDI/nRST rig -- the radio's Update button would be talking a dialect
 * the box no longer parses. See app_config.hpp for the full reasoning.
 *
 * Standard restart-on-mismatch: a failing byte that is itself the first byte
 * of the pattern starts a new match rather than dropping it. The pattern has
 * no internal repeat of 0xAA so this cannot currently matter, but a matcher
 * that is only correct for one specific pattern is a trap for whoever edits
 * the pattern.
 * ==========================================================================*/
void iapHatchFeed(std::uint8_t b) noexcept
{
    ram::AppRam& r = ram::app();
    std::uint8_t idx = r.uartIapMatch;

    if (b == cfg::kIapResetFrame[idx]) {
        ++idx;
    } else {
        idx = (b == cfg::kIapResetFrame[0]) ? 1U : 0U;
    }

    if (idx >= cfg::kIapResetFrameLen) {
        r.uartIapMatch = 0U;
        /* Let whatever is already in the shift register finish before the
         * part resets, so it cannot arrive at the radio ahead of the
         * bootloader's 'U'. Bounded -- see hal_usart.h. */
        usart_tx_drain(USART2, cfg::kUartTxDrainSpins);
        /* Same exit as the Hiworld 0xE0 path: record the reason, then let the
         * watchdog reset us into the loader. Does not return. */
        svc::power::fatalErrorReset(svc::power::kFatalHostCommand);
    }

    r.uartIapMatch = idx;
}

}  // namespace

/* ---- the Raise/Benz dialect: 2E | CMD | LEN | payload | CKS --------------
 * NOT a stock routine -- there is no address annotation because the original
 * firmware never emitted this shape. One sync byte, CMD before LEN, and the
 * checksum is a one's complement of the body sum rather than a plain sum:
 *
 *     CKS = ~(CMD + LEN + sum(payload)) & 0xFF
 *
 * Worked example from ../richest-raise-benz.md section 1.2, which the host
 * tests reproduce: body 90 02 08 00 sums to 0x9A, ~0x9A & 0xFF = 0x65, so the
 * frame is 2E 90 02 08 00 65.
 * ------------------------------------------------------------------------ */
void emit(std::uint8_t cmd, const volatile std::uint8_t* payload,
          std::uint8_t len) noexcept
{
    volatile std::uint8_t* const f = fb();
    const std::uint8_t k = static_cast<std::uint8_t>(len + 3U);

    svc::bytes::copy(&f[3], payload, len);

    f[0] = cfg::kRaiseSync;
    f[1] = cmd;
    f[2] = len;

    std::uint8_t cks = 0U;
    for (std::uint8_t j = 1U; j < k; ++j) {
        cks = static_cast<std::uint8_t>(cks + f[j]);
    }
    f[k] = static_cast<std::uint8_t>(~cks);

    f[kFbTxPos]  = 0U;
    f[kFbTxLen]  = static_cast<std::uint8_t>(len + 4U);
    f[kFbTxBusy] = 1U;
    usart_tx_start(USART2);
}

/* ============================================================================
 * The Raise/Benz RX parser -- NOT a stock routine.
 *
 *   state 0 : wait for 0x2E. Every other byte is offered to the IAP hatch.
 *   state 1 : CMD
 *   state 2 : LEN; rejected if LEN > cfg::kUartMaxPayload, which is the RX
 *             frame buffer's bound rather than a protocol rule. Needs LEN+2
 *             further bytes buffered (this LEN byte, the payload and the
 *             checksum) or we leave the tail put and retry next call.
 *   state 3 : payload
 *   state 4 : CKS, which must equal ~(CMD + LEN + sum(payload)) & 0xFF.
 *
 * The reassembled frame is written into ram::AppRam::uartRxFrame in the SAME
 * LAYOUT the Hiworld parser uses -- RXF[2] = LEN, RXF[3] = CMD, RXF[4..] =
 * payload -- even though that is not the wire order. That is deliberate: it
 * lets rxDispatch() be shared verbatim between the two dialects instead of
 * being written twice and drifting.
 *
 * Tail-commit discipline is the stock discipline: the stored tail only moves
 * when the machine is back in state 0, so a half-received frame is re-parsed
 * from its sync byte on the next call and no timeout is needed anywhere.
 * ==========================================================================*/
void rxParse() noexcept
{
    volatile std::uint8_t* const f = fb();
    volatile std::uint8_t* const rf = rxf();

    const std::uint8_t head = f[kFbRxHead];
    std::uint32_t tail  = f[kFbRxTail];
    std::uint32_t state = 0U;
    std::uint32_t sum   = 0U;
    std::uint32_t got   = 0U;  /* payload bytes stored so far */

    for (;;) {
        if (head == tail) {
            return;                       /* ring drained; tail left as-is   */
        }

        const std::uint8_t b = f[kFbRxRing + tail];

        if (state == 0U) {
            if (b == cfg::kRaiseSync) {
                state = 1U;
            } else {
                iapHatchFeed(b);
            }
        } else if (state == 1U) {
            rf[3] = b;                    /* CMD                             */
            sum   = b;
            state = 2U;
        } else if (state == 2U) {
            if (b <= cfg::kUartMaxPayload) {
                const std::uint32_t avail = static_cast<std::uint32_t>(
                    (static_cast<int>(head) - static_cast<int>(tail)
                     + static_cast<int>(kRxRingSize))
                    % static_cast<int>(kRxRingSize));

                if (avail < static_cast<std::uint32_t>(b) + 2U) {
                    return;               /* incomplete: retry next call     */
                }
                rf[2] = b;                /* LEN                             */
                sum   = (sum + b) & 0xFFU;
                got   = 0U;
                /* A zero-length payload goes straight to the checksum. */
                state = (b == 0U) ? 4U : 3U;
            } else {
                state = 0U;               /* oversized: resynchronise        */
            }
        } else if (state == 3U) {
            rf[4U + got] = b;
            sum = (sum + b) & 0xFFU;
            ++got;
            if (got >= static_cast<std::uint32_t>(rf[2])) {
                state = 4U;
            }
        } else {
            if (static_cast<std::uint32_t>(b) ==
                (static_cast<std::uint32_t>(~sum) & 0xFFU)) {
                detail::rxDispatch();
            }
            tail = (tail + 1U) % kRxRingSize;
            f[kFbRxTail] = static_cast<std::uint8_t>(tail);
            return;                       /* one frame per call, as stock    */
        }

        tail = (tail + 1U) % kRxRingSize;
        if (state == 0U) {
            f[kFbRxTail] = static_cast<std::uint8_t>(tail);
        }
    }
}

}  // namespace raisebenz
}  // namespace hu
}  // namespace svc
