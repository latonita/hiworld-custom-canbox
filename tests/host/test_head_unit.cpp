/* ============================================================================
 * test_head_unit.cpp -- svc::hu, the USART2 "AA 55" head-unit link.
 *
 * Written from the BEHAVIOURAL SPEC in reference/decompile/app/uart.c (the Ghidra decompile)
 * and docs/notes/hiworld-frame-format.md, not from app/services/head_unit.cpp.
 * A test that merely restates the implementation proves nothing; these restate
 * the protocol and the stock state machine independently:
 *
 *   - the frame shape AA 55 | LEN | CMD | payload[LEN] | CKS is spelled out
 *     here byte by byte, and the checksum is computed as the DOCUMENTED full
 *     sum including the preamble -- never as the firmware's (LEN + CMD + sum
 *     - 1) shortcut, which is itself one of the things under test;
 *   - the RX ring is filled the way the ISR fills it, so the parser is
 *     exercised through its real input;
 *   - the scheduler is driven through service(), i.e. at its real 6-pass
 *     cadence, rather than by poking internals.
 *
 * This is the only evidence of equivalence available without a car on a bench.
 * ==========================================================================*/

#include "test_framework.hpp"

#include "app_config.hpp"
#include "fake_hal.hpp"
#include "head_unit.hpp"
#include "ram_map.hpp"

namespace {

/* fake_hal.cpp records a USART pointer as a small stable tag; USART2 is 0x20. */
constexpr std::uint32_t kTagUsart2 = 0x20U;

/* ---------------------------------------------------------------------------
 * The checksum, as docs/notes/hiworld-frame-format.md defines it: the sum of
 * EVERY byte on the wire, preamble included, mod 256. Deliberately not the
 * firmware's algebraic shortcut.
 * ------------------------------------------------------------------------- */
std::uint8_t cksFullSum(std::uint8_t len, std::uint8_t cmd,
                        const std::uint8_t* payload) noexcept
{
    unsigned s = 0xAAU + 0x55U + static_cast<unsigned>(len) + static_cast<unsigned>(cmd);
    for (std::uint8_t i = 0U; i < len; ++i) {
        s += static_cast<unsigned>(payload[i]);
    }
    return static_cast<std::uint8_t>(s & 0xFFU);
}

/* The form the firmware uses: (LEN + CMD + sum(payload) - 1) & 0xFF. */
std::uint8_t cksFirmwareForm(std::uint8_t len, std::uint8_t cmd,
                             const std::uint8_t* payload) noexcept
{
    unsigned s = static_cast<unsigned>(len) + static_cast<unsigned>(cmd);
    for (std::uint8_t i = 0U; i < len; ++i) {
        s += static_cast<unsigned>(payload[i]);
    }
    return static_cast<std::uint8_t>((s - 1U) & 0xFFU);
}

/* Lay out a complete wire frame. `cksOverride` lets a test corrupt it. */
std::size_t buildWireFrame(std::uint8_t* out, std::uint8_t len, std::uint8_t cmd,
                           const std::uint8_t* payload, int cksOverride = -1) noexcept
{
    std::size_t n = 0U;
    out[n++] = 0xAAU;
    out[n++] = 0x55U;
    out[n++] = len;
    out[n++] = cmd;
    for (std::uint8_t i = 0U; i < len; ++i) {
        out[n++] = payload[i];
    }
    out[n++] = (cksOverride < 0) ? cksFullSum(len, cmd, payload)
                                 : static_cast<std::uint8_t>(cksOverride);
    return n;
}

/* Push bytes into the RX ring exactly as svc::hu::onInterrupt() does: store at
 * the head, then advance the head modulo 200. */
void pushRx(const std::uint8_t* b, std::size_t n) noexcept
{
    ram::AppRam& r = ram::app();
    for (std::size_t i = 0U; i < n; ++i) {
        const std::uint8_t head = r.uartFrame[svc::hu::kFbRxHead];
        r.uartFrame[svc::hu::kFbRxRing + head] = b[i];
        r.uartFrame[svc::hu::kFbRxHead] =
            static_cast<std::uint8_t>((static_cast<std::uint32_t>(head) + 1U)
                                      % svc::hu::kRxRingSize);
    }
}

/* One frame per service() pass is what the stock parser guarantees. */
void serviceOnce() noexcept { svc::hu::service(); }

/* The scheduler only emits on every 6th pass (6 x 8 ms = 48 ms). */
void serviceOneTxCycle() noexcept
{
    for (std::uint8_t i = 0U; i < svc::hu::kTicksPerFrame; ++i) {
        svc::hu::service();
    }
}

/* Read the frame the scheduler left in the working buffer. */
std::uint8_t fbByte(std::size_t i) noexcept { return ram::app().uartFrame[i]; }

}  // namespace

/* ===========================================================================
 * 1. The checksum itself.
 * ========================================================================= */

/* A worked example, computed by hand from the spec and from nothing else:
 *     AA 55 02 AD 12 34 ??
 *     0xAA + 0x55 + 0x02 + 0xAD + 0x12 + 0x34 = 0x1F4  ->  CKS = 0xF4
 * and the firmware's form (2 + 0xAD + 0x46 - 1) & 0xFF = 0xF4 as well. */
TEST(head_unit_checksum_hand_computed_example)
{
    fake::reset();

    const std::uint8_t payload[2] = { 0x12U, 0x34U };

    CHECK_EQ(cksFullSum(2U, 0xADU, payload), 0xF4U);
    CHECK_EQ(cksFirmwareForm(2U, 0xADU, payload), 0xF4U);
}

/* The two forms are algebraically identical because 0xAA + 0x55 = 0xFF == -1
 * (mod 256). Proven over a spread of lengths, commands and payloads, including
 * ones that wrap the byte sum several times. */
TEST(head_unit_checksum_firmware_form_equals_full_sum)
{
    fake::reset();

    std::uint8_t payload[0x37];
    for (std::uint8_t len = 0U; len <= cfg::kUartMaxPayload; ++len) {
        for (std::uint8_t i = 0U; i < len; ++i) {
            payload[i] = static_cast<std::uint8_t>(i * 37U + len);
        }
        for (unsigned cmd = 0U; cmd < 256U; cmd += 17U) {
            const std::uint8_t c = static_cast<std::uint8_t>(cmd);
            CHECK_EQ(cksFullSum(len, c, payload), cksFirmwareForm(len, c, payload));
        }
    }
}

/* ===========================================================================
 * 2. The RX parser and dispatcher.
 * ========================================================================= */

/* A complete valid frame reaches the right RX descriptor slot and raises that
 * slot's "new data" flag. Table entry 1: CMD 0xAD, LEN 2 -> hostCmd. */
TEST(head_unit_parses_valid_frame_into_periodic_slot)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t payload[2] = { 0x12U, 0x34U };
    std::uint8_t wire[16];
    const std::size_t n = buildWireFrame(wire, 2U, 0xADU, payload);
    CHECK_EQ(n, 2U + 5U);              /* wire length = LEN + 5 */

    pushRx(wire, n);
    serviceOnce();

    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[0], 0x12U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[1], 0x34U);
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 1U);
    /* the other slot must not have been touched */
    CHECK_EQ(ram::app().hostCmd.clusterNewData, 0U);
}

/* Table entry 0: CMD 0xD2, LEN 13, the instrument-cluster text. */
TEST(head_unit_parses_valid_frame_into_cluster_slot)
{
    fake::reset();
    svc::hu::resetState();

    std::uint8_t payload[13];
    for (std::uint8_t i = 0U; i < 13U; ++i) {
        payload[i] = static_cast<std::uint8_t>(0x41U + i);
    }
    std::uint8_t wire[32];
    const std::size_t n = buildWireFrame(wire, 13U, 0xD2U, payload);

    pushRx(wire, n);
    serviceOnce();

    CHECK_BYTES(ram::app().hostCmd.cluster, payload, 13U);
    CHECK_EQ(ram::app().hostCmd.clusterNewData, 1U);
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 0U);
}

/* The dispatcher only copies when the payload actually CHANGED: an identical
 * frame arriving again must not re-raise the flag the consumer just cleared. */
TEST(head_unit_unchanged_payload_does_not_reraise_new_data)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t payload[2] = { 0x07U, 0x09U };
    std::uint8_t wire[16];
    const std::size_t n = buildWireFrame(wire, 2U, 0xADU, payload);

    pushRx(wire, n);
    serviceOnce();
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 1U);

    ram::app().hostCmd.periodicNewData = 0U;   /* the consumer clears it */

    pushRx(wire, n);
    serviceOnce();
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 0U);
}

/* A bad checksum is rejected, and the parser resynchronises on the NEXT valid
 * frame -- there is no escaping and no timeout, so this is the only recovery
 * mechanism the protocol has. */
TEST(head_unit_bad_checksum_rejected_then_resyncs)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t bad[2] = { 0x11U, 0x22U };
    std::uint8_t wireBad[16];
    const std::size_t nBad = buildWireFrame(
        wireBad, 2U, 0xADU,
        bad, static_cast<int>(cksFullSum(2U, 0xADU, bad) ^ 0xFFU));

    pushRx(wireBad, nBad);
    serviceOnce();

    CHECK_EQ(ram::app().hostCmd.periodicNewData, 0U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[0], 0U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[1], 0U);

    const std::uint8_t good[2] = { 0x33U, 0x44U };
    std::uint8_t wireGood[16];
    const std::size_t nGood = buildWireFrame(wireGood, 2U, 0xADU, good);

    pushRx(wireGood, nGood);
    serviceOnce();

    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[0], 0x33U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[1], 0x44U);
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 1U);
}

/* LEN >= 0x38 is rejected at the length byte; the machine drops back to state
 * 0 and keeps scanning, so a valid frame immediately behind it still parses. */
TEST(head_unit_rejects_oversized_length)
{
    fake::reset();
    svc::hu::resetState();

    /* LEN = 0x38 is the first rejected value. The bytes behind it are plain
     * garbage from the parser's point of view and contain no 0xAA. */
    const std::uint8_t oversized[] = { 0xAAU, 0x55U, 0x38U, 0xD2U,
                                       0x01U, 0x02U, 0x03U };
    pushRx(oversized, sizeof oversized);
    serviceOnce();

    CHECK_EQ(ram::app().hostCmd.clusterNewData, 0U);

    const std::uint8_t payload[2] = { 0x55U, 0x66U };
    std::uint8_t wire[16];
    const std::size_t n = buildWireFrame(wire, 2U, 0xADU, payload);
    pushRx(wire, n);
    serviceOnce();

    CHECK_EQ(ram::app().hostCmd.periodicNewData, 1U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[0], 0x55U);
}

/* The largest accepted length, 0x37, must still be accepted -- the bound is
 * `>= 0x38`, not `> 0x38`. 0x37 is longer than any real message, so this uses
 * a command the table ignores and only checks that the parser consumed the
 * whole frame and came back ready for the next one. */
TEST(head_unit_accepts_maximum_length)
{
    fake::reset();
    svc::hu::resetState();

    std::uint8_t payload[0x37];
    for (std::uint8_t i = 0U; i < cfg::kUartMaxPayload; ++i) {
        payload[i] = static_cast<std::uint8_t>(i);
    }
    std::uint8_t wire[0x37 + 5];
    const std::size_t n = buildWireFrame(wire, cfg::kUartMaxPayload, 0x55U, payload);
    pushRx(wire, n);
    serviceOnce();

    /* the tail has walked the entire frame, so the ring is drained */
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbRxTail],
             ram::app().uartFrame[svc::hu::kFbRxHead]);
}

/* There is NO escaping: a payload byte may legitimately be 0xAA or 0x55 and
 * must not be mistaken for a preamble. */
TEST(head_unit_payload_may_contain_preamble_bytes)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t payload[2] = { 0xAAU, 0x55U };
    std::uint8_t wire[16];
    const std::size_t n = buildWireFrame(wire, 2U, 0xADU, payload);

    pushRx(wire, n);
    serviceOnce();

    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[0], 0xAAU);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[1], 0x55U);
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 1U);
}

/* The sibling 5A A5 dialect exists in the wild (selector 38401) but THIS box
 * speaks 38403 / AA 55 only, and must ignore it. */
TEST(head_unit_rejects_5A_A5_dialect)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t payload[2] = { 0x12U, 0x34U };
    std::uint8_t wire[16];
    const std::size_t n = buildWireFrame(wire, 2U, 0xADU, payload);
    wire[0] = 0x5AU;
    wire[1] = 0xA5U;

    pushRx(wire, n);
    serviceOnce();

    CHECK_EQ(ram::app().hostCmd.periodicNewData, 0U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[0], 0U);
}

/* A frame split across two service() passes must be re-parsed from scratch,
 * because the tail is only committed in state 0. */
TEST(head_unit_partial_frame_is_retried_next_pass)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t payload[2] = { 0x7EU, 0x7FU };
    std::uint8_t wire[16];
    const std::size_t n = buildWireFrame(wire, 2U, 0xADU, payload);

    pushRx(wire, n - 2U);          /* everything but the last payload byte + CKS */
    serviceOnce();
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 0U);
    /* the tail is parked on the 0xAA, not consumed */
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbRxTail], 0U);

    pushRx(&wire[n - 2U], 2U);
    serviceOnce();
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 1U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[1], 0x7FU);
}

/* The 0xFC vendor handshake is echoed back as 0xFD with the checksum bumped by
 * one -- which is only correct because CKS is a plain sum. Fixed 14 bytes. */
TEST(head_unit_fc_handshake_echoes_fd)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t payload[9] = { 0x3FU, 0xF3U, 0xF1U, 0x01U, 0x00U,
                                      0x00U, 0x3AU, 0xA3U, 0x00U };
    std::uint8_t wire[16];
    const std::size_t n = buildWireFrame(wire, 9U, 0xFCU, payload);

    pushRx(wire, n);
    serviceOnce();

    CHECK_EQ(fbByte(0), 0xAAU);
    CHECK_EQ(fbByte(1), 0x55U);
    CHECK_EQ(fbByte(2), 0x09U);
    CHECK_EQ(fbByte(3), 0xFDU);
    CHECK_BYTES(&ram::app().uartFrame[4], payload, 9U);
    /* the reply's checksum must itself be a valid full-sum checksum */
    CHECK_EQ(fbByte(13), cksFullSum(9U, 0xFDU, payload));
    CHECK_EQ(fbByte(13), static_cast<std::uint8_t>(wire[n - 1U] + 1U));

    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxLen], 0x0EU);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxPos], 0U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxBusy], 1U);
    CHECK_EQ(fake::countOf("usart_tx_start"), 1U);
}

/* ===========================================================================
 * 3. The transmit scheduler and the round-robin.
 * ========================================================================= */

/* Nothing is emitted before the 6th pass. */
TEST(head_unit_scheduler_emits_only_every_sixth_pass)
{
    fake::reset();
    svc::hu::resetState();

    ram::app().vehicle.at(ram::VehicleState::kWin72Off) = 0x01U;

    for (std::uint8_t i = 0U; i < 5U; ++i) {
        svc::hu::service();
        CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxBusy], 0U);
        CHECK_EQ(fake::countOf("usart_tx_start"), 0U);
    }

    svc::hu::service();
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxBusy], 1U);
    CHECK_EQ(fake::countOf("usart_tx_start"), 1U);
}

/* A slot whose live buffer differs from its shadow is sent, and the shadow is
 * then refreshed so it is not sent again. Slot 0 = CMD 0x72, LEN 14, live
 * buffer = the 0x72 window of ram::VehicleState. */
TEST(head_unit_round_robin_sends_changed_slot_and_updates_shadow)
{
    fake::reset();
    svc::hu::resetState();

    ram::app().vehicle.at(ram::VehicleState::kWin72Off) = 0x01U;
    serviceOneTxCycle();

    CHECK_EQ(fbByte(0), 0xAAU);
    CHECK_EQ(fbByte(1), 0x55U);
    CHECK_EQ(fbByte(2), 14U);
    CHECK_EQ(fbByte(3), 0x72U);
    CHECK_EQ(fbByte(4), 0x01U);
    /* wire length = LEN + 5 */
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxLen], 14U + 5U);
    CHECK_EQ(ram::app().uartTxSlotIdx, 0U);

    /* the shadow now mirrors what was sent */
    CHECK_BYTES(ram::app().uartShadows.msg72,
                ram::app().vehicle.window(ram::VehicleState::kWin72Off), 14U);
}

/* The outbound checksum must be the documented full sum including the
 * preamble, at wire offset LEN + 4. */
TEST(head_unit_built_frame_checksum_matches_spec)
{
    fake::reset();
    svc::hu::resetState();

    ram::app().vehicle.at(ram::VehicleState::kWin72Off)     = 0x5AU;
    ram::app().vehicle.at(ram::VehicleState::kWin72Off + 7) = 0xA5U;
    serviceOneTxCycle();

    std::uint8_t payload[14];
    for (std::uint8_t i = 0U; i < 14U; ++i) {
        payload[i] = ram::app().vehicle.at(ram::VehicleState::kWin72Off + i);
    }
    CHECK_EQ(fbByte(14U + 4U), cksFullSum(14U, 0x72U, payload));
}

/* A slot whose shadow already matches is skipped; the sweep carries on to the
 * next one that changed. Slot 0 = 0x72 (unchanged), slot 1 = 0xA6, LEN 8,
 * live buffer = the 0xA6 window. */
TEST(head_unit_round_robin_skips_unchanged_slot)
{
    fake::reset();
    svc::hu::resetState();

    ram::app().vehicle.at(ram::VehicleState::kWinA6Off) = 0x5AU;
    serviceOneTxCycle();

    CHECK_EQ(fbByte(3), 0xA6U);
    CHECK_EQ(fbByte(2), 8U);
    CHECK_EQ(fbByte(4), 0x5AU);
    CHECK_EQ(ram::app().uartTxSlotIdx, 1U);
    CHECK_EQ(ram::app().uartShadows.msgA6[0], 0x5AU);
    /* slot 0 was examined and left alone */
    CHECK_EQ(ram::app().uartShadows.msg72[0], 0U);
}

/* Once a slot has been sent its shadow matches, so the next sweep moves on
 * rather than repeating it. */
TEST(head_unit_sent_slot_is_not_repeated)
{
    fake::reset();
    svc::hu::resetState();

    ram::app().vehicle.at(ram::VehicleState::kWin72Off) = 0x01U;
    serviceOneTxCycle();
    CHECK_EQ(fbByte(3), 0x72U);

    /* release the transmitter, as the ISR does when the frame is clocked out */
    ram::app().uartFrame[svc::hu::kFbTxBusy] = 0U;
    ram::app().vehicle.at(ram::VehicleState::kWinA6Off) = 0x77U;

    serviceOneTxCycle();
    CHECK_EQ(fbByte(3), 0xA6U);
}

/* When a full sweep finds nothing changed the index parks on the kTxSlots
 * sentinel and the ~720 ms keep-alive takes over, walking a SEPARATE aux
 * index: the first keep-alive frame is therefore table slot 1 (CMD 0xA6). */
TEST(head_unit_idle_sweep_falls_back_to_keepalive)
{
    fake::reset();
    svc::hu::resetState();

    serviceOneTxCycle();

    CHECK_EQ(ram::app().uartTxSlotIdx, svc::hu::kTxSlots);
    CHECK_EQ(ram::app().uartIdleDelay, svc::hu::kIdleDelay);
    CHECK_EQ(ram::app().uartAuxSlotIdx, 1U);
    CHECK_EQ(fbByte(3), 0xA6U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxBusy], 1U);
}

/* ...and the keep-alive only fires once every kIdleDelay sweeps. */
TEST(head_unit_keepalive_waits_fifteen_idle_sweeps)
{
    fake::reset();
    svc::hu::resetState();

    serviceOneTxCycle();                       /* first keep-alive */
    CHECK_EQ(fake::countOf("usart_tx_start"), 1U);

    for (std::uint8_t sweep = 0U; sweep < svc::hu::kIdleDelay; ++sweep) {
        ram::app().uartFrame[svc::hu::kFbTxBusy] = 0U;
        serviceOneTxCycle();
        CHECK_EQ(fake::countOf("usart_tx_start"), 1U);   /* still counting down */
    }

    ram::app().uartFrame[svc::hu::kFbTxBusy] = 0U;
    serviceOneTxCycle();
    CHECK_EQ(fake::countOf("usart_tx_start"), 2U);
    CHECK_EQ(ram::app().uartAuxSlotIdx, 2U);             /* aux index advanced */
}

/* Nothing at all happens while a frame is still in flight. */
TEST(head_unit_scheduler_waits_while_tx_busy)
{
    fake::reset();
    svc::hu::resetState();

    ram::app().uartFrame[svc::hu::kFbTxBusy] = 1U;
    ram::app().vehicle.at(ram::VehicleState::kWin72Off) = 0x01U;

    for (std::uint8_t i = 0U; i < 20U; ++i) {
        svc::hu::service();
    }
    CHECK_EQ(fake::countOf("usart_tx_start"), 0U);
    CHECK_EQ(ram::app().uartShadows.msg72[0], 0U);
}

/* ===========================================================================
 * 4. The ACK frame.
 * ========================================================================= */

/* AA 55 01 <cmd> <arg> <cks>, cks = cmd + arg, which is the general rule with
 * LEN = 1 folded in (1 - 1 = 0). */
TEST(head_unit_tx_ack_frame_shape)
{
    fake::reset();
    svc::hu::resetState();

    svc::hu::txAck(0xFFU, 0x12U);

    const std::uint8_t arg = 0x12U;
    CHECK_EQ(fbByte(0), 0xAAU);
    CHECK_EQ(fbByte(1), 0x55U);
    CHECK_EQ(fbByte(2), 0x01U);
    CHECK_EQ(fbByte(3), 0xFFU);
    CHECK_EQ(fbByte(4), 0x12U);
    CHECK_EQ(fbByte(5), cksFullSum(1U, 0xFFU, &arg));
    CHECK_EQ(fbByte(5), static_cast<std::uint8_t>(0xFFU + 0x12U));

    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxLen], 6U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxPos], 0U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxBusy], 1U);
    CHECK_EQ(fake::countOf("usart_tx_start"), 1U);
}

/* ===========================================================================
 * 5. The interrupt body.
 * ========================================================================= */

/* TXE drains the frame buffer one byte per interrupt, then disarms itself and
 * releases the busy flag. This is the only path that puts bytes on the wire. */
TEST(head_unit_isr_clocks_out_the_whole_frame)
{
    fake::reset();
    svc::hu::resetState();

    svc::hu::txAck(0xFFU, 0x12U);

    fake::returns().usartItStatus = 1;
    fake::returns().usartRxByte   = 0x00U;

    for (std::uint8_t i = 0U; i < 7U; ++i) {
        svc::hu::onInterrupt();
    }

    const std::uint8_t expected[6] = { 0xAAU, 0x55U, 0x01U, 0xFFU, 0x12U, 0x11U };
    CHECK_EQ(fake::usartTxCount(), 6U);
    CHECK_BYTES(fake::usartTxBytes(), expected, 6U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxBusy], 0U);
}

/* RXNE stores one byte at the head and advances it modulo 200. */
TEST(head_unit_isr_stores_rx_byte_in_ring)
{
    fake::reset();
    svc::hu::resetState();

    fake::returns().usartItStatus = 1;
    fake::returns().usartRxByte   = 0xA5U;

    svc::hu::onInterrupt();

    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbRxRing + 0U], 0xA5U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbRxHead], 1U);
    CHECK_EQ(ram::app().uartRxOverflow, 0U);
}

/* A full ring drops the byte and latches the overflow flag; the stock code
 * never clears it again. */
TEST(head_unit_isr_rx_ring_overflow_drops_byte)
{
    fake::reset();
    svc::hu::resetState();

    ram::app().uartFrame[svc::hu::kFbRxHead] = 199U;   /* advancing wraps to 0 */
    ram::app().uartFrame[svc::hu::kFbRxTail] = 0U;

    fake::returns().usartItStatus = 1;
    fake::returns().usartRxByte   = 0x5AU;

    svc::hu::onInterrupt();

    CHECK_EQ(ram::app().uartRxOverflow, 1U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbRxHead], 199U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbRxRing + 199U], 0U);
}

/* ===========================================================================
 * 6. init(), resetState(), disable().
 * ========================================================================= */

/* The flashing tool reads exactly these 17 bytes back from 0x20001B86 over SWD
 * to prove the image booted (tools/iap_upload.py --verify). Getting this wrong
 * makes every successful flash look like a failure. Not NUL-terminated. */
TEST(head_unit_init_publishes_the_version_string)
{
    fake::reset();

    svc::hu::init();

    /* Checked against cfg::kVersionString, not a literal copy of it: the box ID
     * is a build-time choice, but it must land in RAM byte-for-byte whatever it
     * is set to. The length below is the part that is NOT free to change. */
    std::uint8_t expected[17] = {};
    for (std::uint8_t i = 0U; i < 17U; ++i) {
        expected[i] = static_cast<std::uint8_t>(cfg::kVersionString[i]);
    }
    CHECK_BYTES(ram::g_versionString, expected, 17U);
    CHECK_EQ(static_cast<std::size_t>(cfg::kVersionLength), 17U);
    /* Exactly 17 bytes, and not a NUL-terminated 18: the uploader reads back
     * precisely sizeof(g_versionString), and the object is sized to match. */
    CHECK_EQ(sizeof(ram::g_versionString), 17U);
}

/* The version string is also TX slot 3's payload, so it goes out as CMD 0xF0
 * with LEN 17 once the round-robin notices it changed. */
TEST(head_unit_version_string_is_sent_as_cmd_F0)
{
    fake::reset();

    svc::hu::init();
    serviceOneTxCycle();

    /* slots 0..2 are all-zero against all-zero shadows, so the first changed
     * slot is 3 = CMD 0xF0 */
    CHECK_EQ(ram::app().uartTxSlotIdx, 3U);
    CHECK_EQ(fbByte(2), 17U);
    CHECK_EQ(fbByte(3), 0xF0U);
    CHECK_EQ(fbByte(4), static_cast<std::uint8_t>('H'));
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxLen], 17U + 5U);
}

/* USART2 is on APB1, so the services layer -- not the HAL -- must pass PCLK1.
 * With PCLK1 = 16 MHz and 38400 baud the stock divisor BRCF = 0x01A1 falls
 * out of the documented arithmetic; recomputed here from the arguments the
 * driver was actually handed. */
TEST(head_unit_init_configures_usart2_with_pclk1_and_38400)
{
    fake::reset();

    svc::hu::init();

    const fake::Call* c = fake::lastCall("usart_configure");
    CHECK(c != nullptr);
    if (c != nullptr) {
        CHECK_EQ(c->a0, kTagUsart2);
        CHECK_EQ(c->a1, 16000000U);      /* PCLK1, not PCLK2 by accident */
        CHECK_EQ(c->a2, 38400U);         /* stock literal 0x9600          */

        const std::uint32_t d        = (25U * c->a1) / (4U * c->a2);
        const std::uint32_t mantissa = d / 100U;
        const std::uint32_t fraction = ((d % 100U) * 16U + 50U) / 100U;
        CHECK_EQ((mantissa << 4) | (fraction & 0x0FU), 0x01A1U);
    }
}

/* RXNE on, TXE off, the port enabled, NVIC IRQ 38 at pre-emption 2 / sub 0,
 * and PA3 configured before PA2 -- all exactly as the stock bring-up does. */
TEST(head_unit_init_brings_up_the_port_in_stock_order)
{
    fake::reset();

    svc::hu::init();

    const fake::Call* nv = fake::lastCall("nvic_init");
    CHECK(nv != nullptr);
    if (nv != nullptr) {
        CHECK_EQ(nv->a0, 38U);           /* USART2_IRQn */
        /* fake_hal packs (preempt << 8) | sub: pre-emption 2, sub 0. */
        CHECK_EQ(nv->a1, (2U << 8) | 0U);
        CHECK_EQ(nv->a2, 1U);            /* enabled */
    }

    const fake::Call* rx = fake::nthCall("usart_it_config", 0);
    const fake::Call* tx = fake::nthCall("usart_it_config", 1);
    CHECK(rx != nullptr);
    CHECK(tx != nullptr);
    if (rx != nullptr && tx != nullptr) {
        CHECK_EQ(rx->a1, 0x0525U);       /* USART_INT_RXDNE */
        CHECK_EQ(rx->a2, 1U);
        CHECK_EQ(tx->a1, 0x0727U);       /* USART_INT_TXDE  */
        CHECK_EQ(tx->a2, 0U);
    }

    const fake::Call* pin0 = fake::nthCall("gpio_init_pin", 0);
    const fake::Call* pin1 = fake::nthCall("gpio_init_pin", 1);
    CHECK(pin0 != nullptr);
    CHECK(pin1 != nullptr);
    if (pin0 != nullptr && pin1 != nullptr) {
        CHECK_EQ(pin0->a1, 0x0008U);     /* PA3 = RX, first  */
        CHECK_EQ(pin1->a1, 0x0004U);     /* PA2 = TX, second */
    }

    CHECK_EQ(fake::countOf("usart_cmd_enable"), 1U);
}

/* The slot index is seeded one PAST the last table slot: that sentinel is what
 * makes the very first scheduler pass run a full round-robin sweep. */
TEST(head_unit_reset_state_parks_on_the_sentinel)
{
    fake::reset();

    ram::app().uartFrame[svc::hu::kFbTxLen]  = 0x12U;
    ram::app().uartFrame[svc::hu::kFbTxPos]  = 0x34U;
    ram::app().uartFrame[svc::hu::kFbTxBusy] = 1U;
    ram::app().uartFrame[svc::hu::kFbRxHead] = 40U;
    ram::app().uartFrame[svc::hu::kFbRxTail] = 7U;
    ram::app().uartSlotState[2] = 3U;

    svc::hu::resetState();

    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxLen], 0U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxPos], 0U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxBusy], 0U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbRxHead], 7U);   /* ring emptied */
    CHECK_EQ(ram::app().uartTxSlotIdx, svc::hu::kTxSlots);
    for (std::uint8_t i = 0U; i < svc::hu::kTxSlots; ++i) {
        CHECK_EQ(ram::app().uartSlotState[i], 0U);
    }
}

/* The sleep path shuts the port down through the HAL. */
TEST(head_unit_disable_shuts_down_usart2)
{
    fake::reset();

    svc::hu::disable();

    const fake::Call* c = fake::lastCall("usart_disable");
    CHECK(c != nullptr);
    if (c != nullptr) {
        CHECK_EQ(c->a0, kTagUsart2);
    }
}
