/* ============================================================================
 * test_head_unit_raise.cpp -- svc::hu in its RAISE / MERCEDES-BENZ dialect.
 *
 * Built and run ONLY in the benz configuration (tools/test.py --protocol benz,
 * which is half of the default run). The jeep suite lives in
 * test_head_unit.cpp and the two are mutually exclusive, because the dialect
 * is the compile-time constant cfg::kIsBenz.
 *
 * Written from the BEHAVIOURAL SPEC -- ../richest-raise-benz.md section 1 and
 * the Protocol block in app/include/app_config.hpp -- and not from
 * app/services/head_unit.cpp. The frame shape and the checksum are spelled
 * out here independently:
 *
 *      2E | CMD | LEN | payload[LEN] | CKS          wire length = LEN + 4
 *      CKS = ~(CMD + LEN + sum(payload)) & 0xFF
 *
 * Note what is deliberately NOT different from the jeep suite: the parser
 * writes the reassembled frame into ram::AppRam::uartRxFrame in the Hiworld
 * layout (LEN at [2], CMD at [3], payload from [4]) regardless of wire order,
 * so rxDispatch() is shared. The RX slot expectations below are therefore the
 * same ones the jeep suite asserts, which is the point.
 *
 * The IAP escape hatch is the other half of this file and the reason the
 * firmware is still recoverable: see the note in app_config.hpp.
 * ==========================================================================*/

#include "test_framework.hpp"

#include "app_config.hpp"
#include "fake_hal.hpp"
#include "head_unit.hpp"
#include "ram_map.hpp"

namespace {

/* The checksum as the spec states it: one's complement of the body sum, where
 * the body is CMD, LEN and the payload -- the sync byte is NOT included. */
std::uint8_t cksRaise(std::uint8_t cmd, std::uint8_t len,
                      const std::uint8_t* payload) noexcept
{
    unsigned s = static_cast<unsigned>(cmd) + static_cast<unsigned>(len);
    for (std::uint8_t i = 0U; i < len; ++i) {
        s += static_cast<unsigned>(payload[i]);
    }
    return static_cast<std::uint8_t>(~s & 0xFFU);
}

std::size_t buildRaiseFrame(std::uint8_t* out, std::uint8_t cmd, std::uint8_t len,
                            const std::uint8_t* payload, int cksOverride = -1) noexcept
{
    std::size_t n = 0U;
    out[n++] = 0x2EU;
    out[n++] = cmd;
    out[n++] = len;
    for (std::uint8_t i = 0U; i < len; ++i) {
        out[n++] = payload[i];
    }
    out[n++] = (cksOverride < 0) ? cksRaise(cmd, len, payload)
                                 : static_cast<std::uint8_t>(cksOverride);
    return n;
}

/* Fill the RX ring the way svc::hu::onInterrupt() does. */
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

void serviceOnce() noexcept { svc::hu::service(); }

std::uint8_t fbByte(std::size_t i) noexcept { return ram::app().uartFrame[i]; }

}  // namespace

/* ===========================================================================
 * 1. The build is actually configured for this dialect.
 *
 * If tools/test.py ever selects this file without the define, every other
 * case below would fail in a confusing way. Fail clearly instead.
 * ========================================================================= */
TEST(raise_build_is_configured_for_the_benz_dialect)
{
    CHECK(cfg::kIsBenz);
    CHECK_EQ(static_cast<unsigned>(cfg::kUartFrameOverhead), 4U);
    CHECK_EQ(static_cast<unsigned>(cfg::kRaiseSync), 0x2EU);
    /* Same wire speed as the stock dialect -- that is what makes the two
     * interchangeable on one board. */
    CHECK_EQ(cfg::kUartBaud, 38400U);
}

/* ===========================================================================
 * 2. The checksum.
 * ========================================================================= */

/* The worked example from ../richest-raise-benz.md section 1.2: the RPM poll
 * 0x90 with payload 08 00. Body 90 02 08 00 sums to 0x9A, so CKS = 0x65 and
 * the whole frame is 2E 90 02 08 00 65. Computed from the spec, by hand. */
TEST(raise_checksum_hand_computed_example)
{
    const std::uint8_t payload[2] = { 0x08U, 0x00U };
    CHECK_EQ(cksRaise(0x90U, 2U, payload), 0x65U);

    std::uint8_t wire[8];
    const std::size_t n = buildRaiseFrame(wire, 0x90U, 2U, payload);
    CHECK_EQ(n, 6U);
    const std::uint8_t want[6] = { 0x2EU, 0x90U, 0x02U, 0x08U, 0x00U, 0x65U };
    CHECK_BYTES(wire, want, 6U);
}

/* A zero-length payload still has a checksum over CMD and LEN. */
TEST(raise_checksum_of_empty_payload)
{
    CHECK_EQ(cksRaise(0x41U, 0U, nullptr),
             static_cast<std::uint8_t>(~(0x41U + 0x00U) & 0xFFU));
}

/* ===========================================================================
 * 3. Transmit.
 * ========================================================================= */

/* txAck() is the one fixed-shape frame the box can be asked for. In this
 * dialect it must come out as 2E | CMD | 01 | arg | ~(CMD + 1 + arg). */
TEST(raise_txack_emits_a_five_byte_frame)
{
    fake::reset();
    svc::hu::resetState();

    svc::hu::txAck(0xFFU, 0x12U);

    const std::uint8_t arg = 0x12U;
    CHECK_EQ(fbByte(0), 0x2EU);
    CHECK_EQ(fbByte(1), 0xFFU);
    CHECK_EQ(fbByte(2), 0x01U);
    CHECK_EQ(fbByte(3), 0x12U);
    CHECK_EQ(fbByte(4), cksRaise(0xFFU, 1U, &arg));

    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxLen], 1U + 4U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxPos], 0U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbTxBusy], 1U);
}

/* ===========================================================================
 * 4. Receive -- the shared RX slots, reached through the 2E parser.
 * ========================================================================= */

/* RX table entry 1: CMD 0xAD, LEN 2, the periodic-TX config. */
TEST(raise_parses_valid_frame_into_periodic_slot)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t payload[2] = { 0x12U, 0x34U };
    std::uint8_t wire[8];
    const std::size_t n = buildRaiseFrame(wire, 0xADU, 2U, payload);
    CHECK_EQ(n, 2U + 4U);              /* wire length = LEN + 4 */

    pushRx(wire, n);
    serviceOnce();

    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[0], 0x12U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[1], 0x34U);
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 1U);
    CHECK_EQ(ram::app().hostCmd.clusterNewData, 0U);
}

/* RX table entry 0: CMD 0xD2, LEN 13, the cluster text. */
TEST(raise_parses_valid_frame_into_cluster_slot)
{
    fake::reset();
    svc::hu::resetState();

    std::uint8_t payload[13];
    for (std::uint8_t i = 0U; i < 13U; ++i) {
        payload[i] = static_cast<std::uint8_t>(0x30U + i);
    }
    std::uint8_t wire[24];
    const std::size_t n = buildRaiseFrame(wire, 0xD2U, 13U, payload);

    pushRx(wire, n);
    serviceOnce();

    CHECK_EQ(ram::app().hostCmd.clusterNewData, 1U);
    for (std::uint8_t i = 0U; i < 13U; ++i) {
        CHECK_EQ(ram::app().hostCmd.cluster[i], payload[i]);
    }
}

/* A corrupt checksum must be dropped silently, and must not leave the parser
 * wedged: the next good frame still arrives. */
TEST(raise_rejects_bad_checksum_then_recovers)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t bad[2] = { 0x77U, 0x88U };
    std::uint8_t wire[8];
    std::size_t n = buildRaiseFrame(wire, 0xADU, 2U, bad,
                                    static_cast<int>(cksRaise(0xADU, 2U, bad) ^ 0xFFU));
    pushRx(wire, n);
    serviceOnce();
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 0U);

    const std::uint8_t good[2] = { 0x12U, 0x34U };
    n = buildRaiseFrame(wire, 0xADU, 2U, good);
    pushRx(wire, n);
    serviceOnce();
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 1U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[0], 0x12U);
}

/* LEN above the RX frame buffer's bound is refused, and the parser resyncs on
 * the next sync byte rather than swallowing the following frame. */
TEST(raise_rejects_oversized_length_then_recovers)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t oversized[] = { 0x2EU, 0xD2U,
                                       static_cast<std::uint8_t>(cfg::kUartMaxPayload + 1U),
                                       0x00U, 0x00U };
    pushRx(oversized, sizeof(oversized));
    serviceOnce();
    CHECK_EQ(ram::app().hostCmd.clusterNewData, 0U);

    const std::uint8_t payload[2] = { 0x55U, 0x66U };
    std::uint8_t wire[8];
    const std::size_t n = buildRaiseFrame(wire, 0xADU, 2U, payload);
    pushRx(wire, n);
    serviceOnce();
    serviceOnce();
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[0], 0x55U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[1], 0x66U);
}

/* A frame split across two service() passes must be held, not dropped: the
 * tail stays parked on the sync byte and the frame is re-parsed when the rest
 * arrives. This is the stock tail-commit discipline, reproduced. */
TEST(raise_holds_a_fragmented_frame_until_complete)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t payload[2] = { 0xABU, 0xCDU };
    std::uint8_t wire[8];
    const std::size_t n = buildRaiseFrame(wire, 0xADU, 2U, payload);

    pushRx(wire, 3U);                  /* 2E AD 02 and nothing else */
    serviceOnce();
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 0U);

    pushRx(&wire[3], n - 3U);
    serviceOnce();
    CHECK_EQ(ram::app().hostCmd.periodicNewData, 1U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[0], 0xABU);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[1], 0xCDU);
}

/* There is no escaping in this protocol: a payload byte of 0x2E is ordinary
 * data and must not be mistaken for the start of a new frame. */
TEST(raise_payload_may_contain_the_sync_byte)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t payload[2] = { 0x2EU, 0x2EU };
    std::uint8_t wire[8];
    const std::size_t n = buildRaiseFrame(wire, 0xADU, 2U, payload);

    pushRx(wire, n);
    serviceOnce();

    CHECK_EQ(ram::app().hostCmd.periodicNewData, 1U);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[0], 0x2EU);
    CHECK_EQ(ram::app().hostCmd.periodicTxCfg[1], 0x2EU);
}

/* ===========================================================================
 * 5. The IAP escape hatch.
 *
 * These stop ONE BYTE SHORT of completing the pattern on purpose.
 * fatalErrorReset() is [[noreturn]] and the host fake's wwdg_force_reset()
 * returns, so the real firmware function would spin forever in a test -- the
 * same reason the jeep suite never exercises CMD 0xE0 either. What is
 * assertable, and what actually matters, is that the matcher counts the right
 * bytes in the right order and is not disturbed by the traffic around it.
 * ========================================================================= */

/* The radio writes fourteen bytes once a second while the Update box screen is
 * open: the same reset command in both Hiworld sync variants, back to back
 * (docs/hiworld-iap-update-sequence.md section 3). The 5A A5 half must slide
 * past the matcher without derailing the AA 55 half that follows. */
TEST(raise_iap_hatch_counts_the_radios_reset_sequence)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t fromRadio[13] = {
        /* the 5A A5 variant, which this matcher ignores */
        0x5AU, 0xA5U, 0x02U, 0xE0U, 0x00U, 0x00U, 0xE1U,
        /* the AA 55 variant, less its final checksum byte */
        0xAAU, 0x55U, 0x02U, 0xE0U, 0x00U, 0x00U,
    };
    pushRx(fromRadio, sizeof(fromRadio));
    serviceOnce();

    /* Six of the seven pattern bytes matched; the next byte (0xE1) would
     * reset the box. */
    CHECK_EQ(ram::app().uartIapMatch,
             static_cast<std::uint8_t>(cfg::kIapResetFrameLen - 1U));
}

/* A false start must not cost the real one: AA AA 55 ... still matches,
 * because the failing byte is itself the first byte of the pattern. */
TEST(raise_iap_hatch_restarts_on_a_repeated_first_byte)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t stutter[7] = {
        0xAAU, 0xAAU, 0x55U, 0x02U, 0xE0U, 0x00U, 0x00U,
    };
    pushRx(stutter, sizeof(stutter));
    serviceOnce();

    CHECK_EQ(ram::app().uartIapMatch,
             static_cast<std::uint8_t>(cfg::kIapResetFrameLen - 1U));
}

/* An unrelated byte clears a partial match. */
TEST(raise_iap_hatch_forgets_a_broken_partial_match)
{
    fake::reset();
    svc::hu::resetState();

    const std::uint8_t broken[4] = { 0xAAU, 0x55U, 0x02U, 0x99U };
    pushRx(broken, sizeof(broken));
    serviceOnce();

    CHECK_EQ(ram::app().uartIapMatch, 0U);
}

/* THE ONE THAT MATTERS FOR SAFETY. The hatch is fed only between frames, so
 * the reset pattern appearing INSIDE a legitimate payload must not arm it --
 * otherwise ordinary traffic could reboot the box mid-drive. */
TEST(raise_iap_hatch_ignores_the_pattern_inside_a_payload)
{
    fake::reset();
    svc::hu::resetState();

    std::uint8_t payload[13] = {
        0xAAU, 0x55U, 0x02U, 0xE0U, 0x00U, 0x00U, 0xE1U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    std::uint8_t wire[24];
    const std::size_t n = buildRaiseFrame(wire, 0xD2U, 13U, payload);

    pushRx(wire, n);
    serviceOnce();

    /* The frame was accepted as cluster text ... */
    CHECK_EQ(ram::app().hostCmd.clusterNewData, 1U);
    CHECK_EQ(ram::app().hostCmd.cluster[0], 0xAAU);
    /* ... and the matcher never saw any of it. */
    CHECK_EQ(ram::app().uartIapMatch, 0U);
}
