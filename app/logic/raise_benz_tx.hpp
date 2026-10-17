/* ============================================================================
 * raise_benz_tx.hpp -- packing the car's telemetry into Mercedes-Benz (Raise)
 * frames.  Namespace `logic`.
 *
 * NOT a port of anything. The stock firmware speaks one protocol; this unit
 * exists only when the image is built with --protocol benz, and its spec is
 * the decompiled Raise Benz plugin (my-radio-files/CanBus/plugin/1/34/
 * P1_301.apk), written up in ../richest-raise-benz.md.
 *
 * ---------------------------------------------------------------------------
 * WHERE THIS SITS
 *
 * The CAN decoders write ram::Telemetry in physical units. This unit packs
 * that into the five Benz payload buffers in ram::AppRam::benzFrames. It does
 * NOT transmit: svc::hu's round-robin notices a payload differs from its
 * shadow and sends it, exactly as it does for the Hiworld messages. So the
 * cadence is the stock one -- a changed message every 48 ms, a keep-alive
 * sweep about every 720 ms -- and nothing new schedules anything.
 *
 * That cadence is comfortably inside what the plugin needs. It has NO
 * staleness timeout and NO blanking: one valid frame marks the box connected
 * for the rest of the session and every field simply holds its last value.
 *
 * ---------------------------------------------------------------------------
 * THE FIVE MESSAGES
 *
 *   0x08  LEN 2   engine rpm                      u16 big-endian
 *   0x07  LEN 2   road speed, km/h                u16 big-endian
 *   0x41  LEN 2   page 1: doors + tailgate        bitfield
 *   0x41  LEN 6   page 2: ambient 0.1 C, odometer km (24-bit)
 *   0x41  LEN 11  page 3: coolant C, oil C, volts 0.1 V, power hp, torque lb-ft
 *
 * 0x41 is multiplexed on payload[0]. The page byte is written once at init
 * and never changes, so it takes no part in change detection.
 *
 * DELIBERATELY NOT SENT, and why:
 *   0x21 / 0x06 HVAC   every frame force-shows the climate overlay on screen
 *                      (the plugin ships no car_aircon.json, so its
 *                      "has anything changed" test returns false forever).
 *                      A WK's manual ATC is not on the bus anyway.
 *   0x22 / 0x23 radar  same overlay behaviour, 5 s auto-hide re-armed per
 *                      frame; and a WK has no sensors to populate them.
 *   0x25 park assist   nothing to report without PDC.
 *   0x26 steering      the WK's steering-angle sensor is on CAN-C at 500 k,
 *                      which this box cannot see. 0x015 d[4]:d[5] -- what the
 *                      stock firmware reads for the Hiworld message -- is
 *                      constant on this car.
 *   0x41 page 0xA8     ESP/SPORT/drive-mode: no CAN-B source.
 *   0x20 / 0x27 SWC    see the note in raise_benz_tx.cpp. This is a real
 *                      functional gap, not an omission for tidiness.
 * ==========================================================================*/

#ifndef RAISE_BENZ_TX_HPP
#define RAISE_BENZ_TX_HPP

namespace logic {

/* Write the page selector bytes into the 0x41 payloads. Call once, after the
 * RAM block is zeroed and before the first transmit -- a zero page byte would
 * otherwise match no page and the head unit would discard the frame. */
void benzPrimeFrames() noexcept;

/* Repack ram::Telemetry (and the door bits, which live in the Hiworld-shaped
 * ram::VehicleState because the stock decoder already fills them) into
 * ram::AppRam::benzFrames. Cheap, branch-light and idempotent: called from
 * the 8 ms task, it simply overwrites the payloads, and svc::hu decides
 * whether anything actually goes out. */
void benzPublish() noexcept;

}  // namespace logic

#endif /* RAISE_BENZ_TX_HPP */
