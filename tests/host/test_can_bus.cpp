/* ============================================================================
 * test_can_bus.cpp -- svc::can, the two 50-entry rings and the bring-up.
 *
 * Written from the BEHAVIOURAL SPEC in reference/decompile/app/can.c (the Ghidra decompile),
 * not from can_bus.cpp. The expected ring-entry byte layout, the index
 * arithmetic, the error-flag polarity (one path ORs, the other ASSIGNS) and
 * the two bit-rate constants below are all restated here independently; a test
 * that merely echoed the implementation would prove nothing.
 *
 * Stock functions covered, by original flash address:
 *     0x08002A90 can_init                 0x080074C0 can_tx_enqueue
 *     0x08002B98 can_transmit_frame       0x08007698 can_tx_ring_drain
 *     0x08002C0A can_rx0_irq_push_ring    0x080074B6 can_bus_active_flag
 *     0x08002D1A can_enter_sleep_mode     0x080077E8 can_autobaud_timers_tick
 *     0x08007482 can_reinit_at_bitrate_A  0x08007B2C can_set_baud_by_profile
 *     0x0800744E can_reinit_at_bitrate_B  0x08007B4C can_apply_bitrate_B
 * ==========================================================================*/

#include "test_framework.hpp"

#include "can_bus.hpp"
#include "fake_hal.hpp"
#include "ram_map.hpp"

extern "C" {
#include "hal_can.h"
#include "hal_gpio.h"
}

namespace {

/* Ring geometry restated from the decompile (entry stride 0x10, 0x32 entries,
 * id halves at +0/+2, IDE at +4, DLC at +5, payload at +6) rather than taken
 * from ram_map.hpp, so a change to either side shows up as a failure. */
constexpr std::size_t kStride  = 0x10U;
constexpr std::size_t kEntries = 0x32U;
constexpr std::size_t kIdHi    = 0U;
constexpr std::size_t kIdLo    = 2U;
constexpr std::size_t kIde     = 4U;
constexpr std::size_t kDlc     = 5U;
constexpr std::size_t kData    = 6U;

/* The two bit rates, as the stock wrappers pass them: 0x53 = 83, 0x7D = 125.
 * The prescaler the bring-up must program is 2000 / kbps, truncated. */
constexpr std::uint32_t kJeepKbps = 0x53U;
constexpr std::uint32_t kFcaKbps  = 0x7DU;
constexpr std::uint32_t kJeepBrp  = 2000U / kJeepKbps;   /* 24 */
constexpr std::uint32_t kFcaBrp   = 2000U / kFcaKbps;    /* 16 */

volatile std::uint8_t* txEntry(std::size_t slot) noexcept
{
    return ram::app().canTxRing + slot * kStride;
}

volatile std::uint8_t* rxEntry(std::size_t slot) noexcept
{
    return ram::app().canRxRing + slot * kStride;
}

std::uint32_t entryId(const volatile std::uint8_t* e) noexcept
{
    const std::uint32_t hi = static_cast<std::uint32_t>(e[kIdHi])
                           | (static_cast<std::uint32_t>(e[kIdHi + 1U]) << 8);
    const std::uint32_t lo = static_cast<std::uint32_t>(e[kIdLo])
                           | (static_cast<std::uint32_t>(e[kIdLo + 1U]) << 8);
    return (hi << 16) | lo;
}

/* Fill one RX ring slot exactly the way the stock ISR does, so the reader can
 * be tested without the fake FIFO (which can only produce an all-zero frame). */
void pushRxFrame(std::size_t slot, std::uint32_t id, std::uint8_t dlc,
                 const std::uint8_t* data) noexcept
{
    volatile std::uint8_t* e = rxEntry(slot);
    e[kIdHi]      = static_cast<std::uint8_t>(id >> 16);
    e[kIdHi + 1U] = static_cast<std::uint8_t>(id >> 24);
    e[kIdLo]      = static_cast<std::uint8_t>(id);
    e[kIdLo + 1U] = static_cast<std::uint8_t>(id >> 8);
    e[kDlc]       = dlc;
    for (std::size_t i = 0U; i < static_cast<std::size_t>(dlc) && i < 8U; ++i) {
        e[kData + i] = data[i];
    }
}

const std::uint8_t kPayload[8] = {0x11U, 0x22U, 0x33U, 0x44U,
                                  0x55U, 0x66U, 0x77U, 0x88U};

}  // namespace

/* ===========================================================================
 * TX ring -- producer
 * ======================================================================== */

/* can_tx_enqueue @0x080074C0 writes the id as two 16-bit halves, then IDE at
 * +4, DLC at +5 and the payload at +6, and only then advances the head. */
TEST(can_tx_enqueue_writes_a_well_formed_entry)
{
    fake::reset();

    svc::can::txEnqueue(0x416U, 0U, 8U, kPayload);

    const volatile std::uint8_t* e = txEntry(0U);
    CHECK_EQ(entryId(e), 0x416U);
    CHECK_EQ(e[kIdHi], 0x00U);        /* id >> 16 lives at +0 (an 11-bit id ... */
    CHECK_EQ(e[kIdHi + 1U], 0x00U);   /* ... leaves the high half zero)         */
    CHECK_EQ(e[kIdLo], 0x16U);        /* id & 0xFFFF at +2, little endian       */
    CHECK_EQ(e[kIdLo + 1U], 0x04U);
    CHECK_EQ(e[kIde], 0U);
    CHECK_EQ(e[kDlc], 8U);
    CHECK_BYTES(e + kData, kPayload, 8U);

    CHECK_EQ(ram::app().canRing.txHead, 1U);
    CHECK_EQ(ram::app().canRing.txTail, 0U);
}

/* The identifier really is split across BOTH 16-bit halves: an extended id
 * puts bits 31..16 at +0 and bits 15..0 at +2. */
TEST(can_tx_enqueue_splits_an_extended_id_across_both_halves)
{
    fake::reset();

    svc::can::txEnqueue(0x12345678U, CAN_ID_EXTENDED, 1U, kPayload);

    const volatile std::uint8_t* e = txEntry(0U);
    CHECK_EQ(e[kIdHi], 0x34U);
    CHECK_EQ(e[kIdHi + 1U], 0x12U);
    CHECK_EQ(e[kIdLo], 0x78U);
    CHECK_EQ(e[kIdLo + 1U], 0x56U);
    CHECK_EQ(entryId(e), 0x12345678U);
    CHECK_EQ(e[kIde], CAN_ID_EXTENDED);
}

/* The stock copy loop is `i < dlc && i < 8`: bytes past DLC are not written. */
TEST(can_tx_enqueue_copies_only_dlc_bytes)
{
    fake::reset();

    svc::can::txEnqueue(0x100U, 0U, 3U, kPayload);

    const volatile std::uint8_t* e = txEntry(0U);
    CHECK_EQ(e[kData + 0U], 0x11U);
    CHECK_EQ(e[kData + 1U], 0x22U);
    CHECK_EQ(e[kData + 2U], 0x33U);
    CHECK_EQ(e[kData + 3U], 0x00U);   /* untouched */
}

/* Full ring: (head + 1) % 50 == tail. The frame is DROPPED and the error bit
 * is OR-ed in (kCanErrTxRingFull = 0x04). */
TEST(can_tx_enqueue_on_a_full_ring_drops_the_frame)
{
    fake::reset();
    ram::app().canRing.txHead = static_cast<std::uint32_t>(kEntries - 1U);  /* 49 */
    ram::app().canRing.txTail = 0U;
    ram::app().canErrFlags = 0x01U;   /* a pre-existing RX-full flag */

    svc::can::txEnqueue(0x416U, 0U, 8U, kPayload);

    CHECK_EQ(ram::app().canRing.txHead, kEntries - 1U);   /* not advanced */
    CHECK_EQ(ram::app().canErrFlags, 0x05U);              /* OR-ed, not assigned */
    CHECK_EQ(txEntry(kEntries - 1U)[kDlc], 0U);           /* nothing written */
}

/* One slot is always left unused, so 49 frames fit in a 50-entry ring. */
TEST(can_tx_enqueue_fills_all_but_one_slot)
{
    fake::reset();

    for (std::size_t i = 0U; i < kEntries - 1U; ++i) {
        svc::can::txEnqueue(0x200U, 0U, 1U, kPayload);
    }
    CHECK_EQ(ram::app().canRing.txHead, kEntries - 1U);
    CHECK_EQ(ram::app().canErrFlags, 0U);

    svc::can::txEnqueue(0x200U, 0U, 1U, kPayload);    /* the 50th */
    CHECK_EQ(ram::app().canErrFlags, 0x04U);
}

/* ===========================================================================
 * TX ring -- drain
 * ======================================================================== */

/* can_tx_ring_drain @0x08007698 hands entries to can_transmit oldest first and
 * advances the tail behind each one. */
TEST(can_tx_drain_transmits_in_order_and_advances_the_tail)
{
    fake::reset();

    svc::can::txEnqueue(0x111U, 0U, 2U, kPayload);
    svc::can::txEnqueue(0x222U, 0U, 8U, kPayload);
    svc::can::txEnqueue(0x333U, 0U, 1U, kPayload);

    svc::can::txDrain();

    CHECK_EQ(fake::canFrameCount(), 3U);
    CHECK_EQ(fake::canFrames()[0].id, 0x111U);
    CHECK_EQ(fake::canFrames()[1].id, 0x222U);
    CHECK_EQ(fake::canFrames()[2].id, 0x333U);
    CHECK_EQ(fake::canFrames()[1].dlc, 8U);
    CHECK_BYTES(fake::canFrames()[1].data, kPayload, 8U);

    CHECK_EQ(ram::app().canRing.txTail, 3U);
    CHECK_EQ(ram::app().canRing.txTail, ram::app().canRing.txHead);
    CHECK_EQ(ram::app().canErrFlags, 0U);
}

/* Bytes past DLC are zeroed into the mailbox even when the ring slot still
 * holds a longer previous frame (can_transmit_frame @0x08002B98). */
TEST(can_tx_drain_zeroes_the_mailbox_past_dlc)
{
    fake::reset();

    /* Queue a full 8-byte frame, then shorten the DLC in the ring slot so the
     * stale payload bytes 2..7 are still sitting there. */
    svc::can::txEnqueue(0x111U, 0U, 8U, kPayload);
    txEntry(0U)[kDlc] = 2U;
    CHECK_EQ(txEntry(0U)[kData + 2U], 0x33U);   /* the stale byte is present */

    svc::can::txDrain();

    const fake::CanFrame* f = fake::lastCanFrame();
    CHECK(f != nullptr);
    CHECK_EQ(f->dlc, 2U);
    CHECK_EQ(f->data[0], 0x11U);
    CHECK_EQ(f->data[1], 0x22U);
    CHECK_EQ(f->data[2], 0x00U);   /* past DLC: zeroed, not the stale 0x33 */
    CHECK_EQ(f->data[7], 0x00U);
}

/* The pass limit is 10 entries (stock: `if (9 < drainCount) return`). */
TEST(can_tx_drain_stops_after_ten_frames_per_pass)
{
    fake::reset();

    for (std::size_t i = 0U; i < 12U; ++i) {
        svc::can::txEnqueue(0x400U, 0U, 1U, kPayload);
    }

    svc::can::txDrain();
    CHECK_EQ(fake::canFrameCount(), 10U);
    CHECK_EQ(ram::app().canRing.txTail, 10U);

    svc::can::txDrain();
    CHECK_EQ(fake::canFrameCount(), 12U);
    CHECK_EQ(ram::app().canRing.txTail, 12U);
}

/* All three mailboxes busy: kCanErrTxNoMailbox (0x02), the tail does NOT move
 * and the frame is retried on the next pass. */
TEST(can_tx_drain_with_no_free_mailbox_keeps_the_frame)
{
    fake::reset();
    fake::returns().canTxResult = CAN_TX_NO_MAILBOX;

    svc::can::txEnqueue(0x111U, 0U, 1U, kPayload);
    svc::can::txEnqueue(0x222U, 0U, 1U, kPayload);

    svc::can::txDrain();
    CHECK_EQ(ram::app().canErrFlags, 0x02U);
    CHECK_EQ(ram::app().canRing.txTail, 0U);   /* nothing consumed */
    CHECK_EQ(ram::app().canRing.txHead, 2U);

    /* Mailboxes free up: both frames still go out, oldest first. */
    fake::returns().canTxResult = 0U;
    svc::can::txDrain();
    CHECK_EQ(ram::app().canRing.txTail, 2U);
    CHECK_EQ(fake::lastCanFrame()->id, 0x222U);
    CHECK_EQ(fake::canFrames()[fake::canFrameCount() - 2U].id, 0x111U);
}

/* The stock mailbox-stall path ASSIGNS the flag byte rather than OR-ing it, so
 * it wipes any pending RX/TX-ring-full bit. Deliberate, and load-bearing if
 * anyone ever reads these diagnostics. */
TEST(can_tx_drain_assigns_the_error_flag_rather_than_or_ing_it)
{
    fake::reset();
    fake::returns().canTxResult = CAN_TX_NO_MAILBOX;
    ram::app().canErrFlags = 0x05U;   /* RX ring full + TX ring full */

    svc::can::txEnqueue(0x111U, 0U, 1U, kPayload);
    svc::can::txDrain();

    CHECK_EQ(ram::app().canErrFlags, 0x02U);   /* not 0x07 */
}

/* The 16-bit stall counter at 0x200018E6 counts to 100 and then snaps to 0
 * instead of saturating. */
TEST(can_tx_drain_stall_counter_wraps_at_one_hundred)
{
    fake::reset();
    fake::returns().canTxResult = CAN_TX_NO_MAILBOX;
    svc::can::txEnqueue(0x111U, 0U, 1U, kPayload);

    svc::can::txDrain();
    CHECK_EQ(ram::app().canTxBusyBlock[2], 1U);
    CHECK_EQ(ram::app().canTxBusyBlock[3], 0U);

    ram::app().canTxBusyBlock[2] = 99U;
    svc::can::txDrain();
    CHECK_EQ(ram::app().canTxBusyBlock[2], 100U);

    svc::can::txDrain();               /* now at the limit -> wraps to 0 */
    CHECK_EQ(ram::app().canTxBusyBlock[2], 0U);
}

/* The bus-down probe in the SWC service block does `tail = head`. */
TEST(can_tx_flush_drops_the_whole_queue)
{
    fake::reset();

    svc::can::txEnqueue(0x111U, 0U, 1U, kPayload);
    svc::can::txEnqueue(0x222U, 0U, 1U, kPayload);
    svc::can::txEnqueue(0x333U, 0U, 1U, kPayload);

    svc::can::txFlush();

    CHECK_EQ(ram::app().canRing.txTail, ram::app().canRing.txHead);
    CHECK_EQ(ram::app().canRing.txTail, 3U);

    svc::can::txDrain();
    CHECK_EQ(fake::canFrameCount(), 0U);
}

/* ===========================================================================
 * RX ring
 * ======================================================================== */

/* can_rx0_irq_push_ring @0x08002C0A pops FIFO0 and advances the head.
 * (The fake FIFO yields an all-zero message, so this checks the plumbing:
 * one pop, one slot consumed, no error flag.) */
TEST(can_rx_interrupt_pushes_a_frame_and_advances_the_head)
{
    fake::reset();

    svc::can::onRxInterrupt();

    CHECK_EQ(fake::countOf("can_receive_fifo"), 1U);
    CHECK_EQ(ram::app().canRing.rxHead, 1U);
    CHECK_EQ(ram::app().canRing.rxTail, 0U);
    CHECK_EQ(ram::app().canErrFlags, 0U);
    CHECK(svc::can::rxAvailable());
}

/* Free slots = tail - head when head < tail. One free slot is "full": the
 * message is still POPPED (that is what releases the FIFO and clears FMP0)
 * but discarded, with kCanErrRxRingFull (0x01) raised. */
TEST(can_rx_interrupt_with_one_free_slot_sets_the_ring_full_flag)
{
    fake::reset();
    ram::app().canRing.rxHead = 0U;
    ram::app().canRing.rxTail = 1U;

    svc::can::onRxInterrupt();

    CHECK_EQ(fake::countOf("can_receive_fifo"), 1U);   /* popped anyway */
    CHECK_EQ(ram::app().canRing.rxHead, 0U);           /* not advanced */
    CHECK_EQ(ram::app().canErrFlags, 0x01U);
}

/* The other branch of the free-slot arithmetic: head >= tail wraps through
 * (tail + 50) - head, so head = 49 / tail = 0 is also "one free slot". */
TEST(can_rx_interrupt_free_slot_count_wraps_through_the_end)
{
    fake::reset();
    ram::app().canRing.rxHead = static_cast<std::uint32_t>(kEntries - 1U);
    ram::app().canRing.rxTail = 0U;

    svc::can::onRxInterrupt();

    CHECK_EQ(ram::app().canRing.rxHead, kEntries - 1U);
    CHECK_EQ(ram::app().canErrFlags, 0x01U);

    /* Two free slots (head 48, tail 0) is enough, and the head wraps to 0. */
    fake::reset();
    ram::app().canRing.rxHead = static_cast<std::uint32_t>(kEntries - 2U);
    ram::app().canRing.rxTail = 0U;

    svc::can::onRxInterrupt();

    CHECK_EQ(ram::app().canRing.rxHead, kEntries - 1U);
    CHECK_EQ(ram::app().canErrFlags, 0U);
}

/* The reader the decoders use: id rebuilt as CONCAT22(u16@+0, u16@+2), data
 * pointing at entry+6. */
TEST(can_rx_reader_round_trips_a_frame)
{
    fake::reset();
    CHECK_FALSE(svc::can::rxAvailable());

    pushRxFrame(0U, 0x3D0U, 8U, kPayload);
    ram::app().canRing.rxHead = 1U;

    CHECK(svc::can::rxAvailable());
    CHECK_EQ(svc::can::rxId(), 0x3D0U);
    CHECK_BYTES(svc::can::rxData(), kPayload, 8U);

    svc::can::rxAdvance();
    CHECK_FALSE(svc::can::rxAvailable());
    CHECK_EQ(ram::app().canRing.rxTail, 1U);
}

/* An extended identifier survives the two-half split in the reader too. */
TEST(can_rx_reader_rebuilds_a_29_bit_id)
{
    fake::reset();

    pushRxFrame(7U, 0x18DAF110U, 2U, kPayload);
    ram::app().canRing.rxTail = 7U;
    ram::app().canRing.rxHead = 8U;

    CHECK_EQ(svc::can::rxId(), 0x18DAF110U);
    CHECK_EQ(svc::can::rxData()[0], 0x11U);
}

/* tail = (tail + 1) % 50: entry 49 wraps back to 0. */
TEST(can_rx_advance_wraps_at_the_last_entry)
{
    fake::reset();
    ram::app().canRing.rxTail = static_cast<std::uint32_t>(kEntries - 2U);

    svc::can::rxAdvance();
    CHECK_EQ(ram::app().canRing.rxTail, kEntries - 1U);   /* 49 */

    svc::can::rxAdvance();
    CHECK_EQ(ram::app().canRing.rxTail, 0U);              /* wrapped */
}

/* ===========================================================================
 * Bring-up and bit rate
 * ======================================================================== */

/* can_init @0x08002A90: IRQ 54, PB8/PB9 as AF, accept-all filter bank 0,
 * FMPIE0 on, and BRP = 2000 / kbps. */
TEST(can_init_configures_pins_filter_and_interrupt)
{
    fake::reset();

    svc::can::init(static_cast<int>(kFcaKbps), CAN_MODE_NORMAL);

    const fake::Call* nv = fake::lastCall("nvic_init");
    CHECK(nv != nullptr);
    CHECK_EQ(nv->a0, 54U);          /* CAN_RX0_IRQn on the N32L406, NOT 20 */
    CHECK_EQ(nv->a2, 1U);           /* enabled */

    const fake::Call* rx = fake::nthCall("gpio_init_pin", 0U);
    const fake::Call* tx = fake::nthCall("gpio_init_pin", 1U);
    CHECK(rx != nullptr);
    CHECK(tx != nullptr);
    CHECK_EQ(rx->a1, 0x0100U);      /* PB8 = CAN_RX */
    CHECK_EQ(rx->a2, GPIO_MODE_AF_PP);
    CHECK_EQ(tx->a1, 0x0200U);      /* PB9 = CAN_TX */
    CHECK_EQ(tx->a2, GPIO_MODE_AF_PP);

    CHECK_EQ(fake::countOf("can_deinit"), 1U);

    const fake::Call* ini = fake::lastCall("can_init_regs");
    CHECK(ini != nullptr);
    CHECK_EQ(ini->a1, kFcaBrp);     /* prescaler */
    CHECK_EQ(ini->a2, CAN_MODE_NORMAL);

    const fake::Call* flt = fake::lastCall("can_filter_init");
    CHECK(flt != nullptr);
    CHECK_EQ(flt->a0, 0U);          /* bank 0   */
    CHECK_EQ(flt->a1, 1U);          /* activated: accept everything */

    const fake::Call* it = fake::lastCall("can_it_config");
    CHECK(it != nullptr);
    CHECK_EQ(it->a1, CAN_IT_FMP0);
    CHECK_EQ(it->a2, 1U);
}

/* The silent-mode wrapper only changes the BTIM mode field. */
TEST(can_init_silent_selects_silent_mode)
{
    fake::reset();
    svc::can::initSilent(static_cast<int>(kJeepKbps));
    CHECK_EQ(fake::lastCall("can_init_regs")->a2, CAN_MODE_SILENT);

    fake::reset();
    svc::can::initNormal(static_cast<int>(kJeepKbps));
    CHECK_EQ(fake::lastCall("can_init_regs")->a2, CAN_MODE_NORMAL);
}

/* can_reinit_at_bitrate_A/B @0x08007482 / 0x0800744E: re-baud, configure PB12
 * twice, wake the transceiver, then reset the bus-presence state. */
TEST(can_reinit_bitrate_wakes_the_transceiver_and_resets_bus_state)
{
    fake::reset();
    ram::app().canBusStatus = 4U;

    svc::can::reinitBitrateA(static_cast<int>(kJeepKbps));

    CHECK_EQ(fake::lastCall("can_init_regs")->a1, kJeepBrp);

    /* PB8, PB9, then PB12 twice -- the stock duplicate is reproduced. */
    CHECK_EQ(fake::countOf("gpio_init_pin"), 4U);
    CHECK_EQ(fake::nthCall("gpio_init_pin", 2U)->a1, 0x1000U);
    CHECK_EQ(fake::nthCall("gpio_init_pin", 3U)->a1, 0x1000U);

    /* svc::io::setCanTransceiverStandby(false) drives PB12 LOW. */
    const fake::Call* w = fake::lastCall("gpio_write_pin");
    CHECK(w != nullptr);
    CHECK_EQ(w->a1, 0x1000U);
    CHECK_EQ(w->a2, 0U);

    CHECK_EQ(ram::app().canReinitBitrate, kJeepKbps);
    CHECK_EQ(ram::app().canBusStatus, 1U);
    CHECK_EQ(ram::app().canActivityCtr, 200U);
}

/* can_set_baud_by_profile @0x08007B2C: profile byte param[0x01] picks 125
 * kbit/s (BRP 16) for 1 and 83.333 kbit/s (BRP 24) for anything else. */
TEST(can_set_baud_by_profile_picks_125k_for_profile_one)
{
    fake::reset();
    ram::app().param[0x01U] = 1U;

    svc::can::setBaudByProfile();

    CHECK_EQ(fake::lastCall("can_init_regs")->a1, kFcaBrp);
    CHECK_EQ(ram::app().canReinitBitrate, kFcaKbps);
    CHECK_EQ(ram::app().param[0x01U], 1U);   /* left alone */
}

TEST(can_set_baud_by_profile_picks_83k_for_profile_zero)
{
    fake::reset();
    ram::app().param[0x01U] = 0U;

    svc::can::setBaudByProfile();

    CHECK_EQ(fake::lastCall("can_init_regs")->a1, kJeepBrp);
    CHECK_EQ(ram::app().canReinitBitrate, kJeepKbps);
}

/* Anything that is not 1 is normalised back to profile 0 -- the last line of
 * defence against a half-written parameter page. */
TEST(can_set_baud_by_profile_normalises_a_corrupt_profile_byte)
{
    fake::reset();
    ram::app().param[0x01U] = 0x5AU;

    svc::can::setBaudByProfile();

    CHECK_EQ(ram::app().param[0x01U], 0U);
    CHECK_EQ(fake::lastCall("can_init_regs")->a1, kJeepBrp);
}

/* can_apply_bitrate_B @0x08007B4C is the same test through the _B wrapper. */
TEST(can_apply_bitrate_b_follows_the_same_profile_rule)
{
    fake::reset();
    ram::app().param[0x01U] = 1U;
    svc::can::applyBitrateB();
    CHECK_EQ(fake::lastCall("can_init_regs")->a1, kFcaBrp);

    fake::reset();
    ram::app().param[0x01U] = 2U;
    svc::can::applyBitrateB();
    CHECK_EQ(fake::lastCall("can_init_regs")->a1, kJeepBrp);
    CHECK_EQ(ram::app().param[0x01U], 0U);
}

/* ===========================================================================
 * State / shutdown
 * ======================================================================== */

/* can_bus_active_flag @0x080074B6 is bit 0 of the vehicle-state block. */
TEST(can_bus_active_flag_reads_bit_zero_of_the_vehicle_block)
{
    fake::reset();
    CHECK_EQ(svc::can::busActiveFlag(), 0U);

    ram::app().vehicle.at(0x00U) = 0xFEU;     /* every bit but bit 0 */
    CHECK_EQ(svc::can::busActiveFlag(), 0U);

    ram::app().vehicle.at(0x00U) = 0x01U;
    CHECK_EQ(svc::can::busActiveFlag(), 1U);
}

/* can_autobaud_timers_tick @0x080077E8: both counters age by one and floor
 * at zero -- they must not wrap round to 255. */
TEST(can_timers_tick_ages_both_counters_and_floors_at_zero)
{
    fake::reset();
    ram::app().canActivityCtr  = 2U;
    ram::app().canErrHoldTimer = 1U;

    svc::can::timersTick();
    CHECK_EQ(ram::app().canActivityCtr, 1U);
    CHECK_EQ(ram::app().canErrHoldTimer, 0U);

    svc::can::timersTick();
    CHECK_EQ(ram::app().canActivityCtr, 0U);
    CHECK_EQ(ram::app().canErrHoldTimer, 0U);

    svc::can::timersTick();
    CHECK_EQ(ram::app().canActivityCtr, 0U);     /* no wrap to 255 */
    CHECK_EQ(ram::app().canErrHoldTimer, 0U);
}

/* can_enter_sleep_mode @0x08002D1A returns "the controller acknowledged".
 * The shutdown path does nothing at all when it is false, so the polarity
 * here is what decides whether the box ever wakes up again. */
TEST(can_enter_sleep_reports_whether_the_controller_acknowledged)
{
    fake::reset();
    fake::returns().canSleepResult = 1;
    CHECK(svc::can::enterSleep());
    CHECK_EQ(fake::countOf("can_sleep"), 1U);

    fake::reset();
    fake::returns().canSleepResult = 0;
    CHECK_FALSE(svc::can::enterSleep());

    /* Anything other than exactly 1 counts as a refusal (stock `== 1`). */
    fake::reset();
    fake::returns().canSleepResult = 2;
    CHECK_FALSE(svc::can::enterSleep());
}
