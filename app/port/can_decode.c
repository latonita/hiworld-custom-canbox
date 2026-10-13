/* ============================================================================
 * can_decode.c -- CAN receive-frame decoders, ported from app/src/can_rx_decode.c
 *
 * Three stock functions live here:
 *     0x08007d44  can_profile_dispatch_gate   -- entry point / profile select
 *     0x08009010  can_dispatch_profile0_83k   -- THIS box: Jeep WK, 83.333 kbit/s
 *     0x08009680  can_dispatch_profile1_125k  -- the 125 kbit/s FCA variant
 *
 * Both profiles drain the SAME 50-entry RX ring and write the SAME RAM block;
 * only the CAN IDs and a handful of extra signals differ. The vendor service
 * protocol (0x400-0x43F, 0x7FC, 0x7FF) is byte-for-byte identical in both, so
 * it is factored out here into one copy -- the stock image simply duplicated
 * the code in each decoder.
 *
 * RX ring entry (16 bytes, base g_can_rx_ring_buf @0x20001284, 50 entries):
 *     +0  u16  id high half   (0 for the 11-bit IDs this box uses)
 *     +2  u16  id low  half
 *     +4  u8   IDE / RTR flags
 *     +5  u8   DLC
 *     +6  u8   data[0] .. data[7]        <-- "d[n]" below is entry[6+n]
 * The stock code rebuilds the id as (u16@+0 << 16) | u16@+2; reproduced exactly.
 *
 * Where the decoded values land (see docs/hiworld-firmware-analysis.md 3.8):
 *     g_status_block       0x20001B49  UART 0x72 payload, 14 B (flags, SWC key,
 *                                      steering angle, dimmer/fuel %)
 *     g_status_block[0x15] 0x20001B5E  UART 0x12 payload[2] -- door bitfield
 *     g_status_block[0x1E] 0x20001B67  UART 0x32 payload[1] -- gear (PRNDL)
 *     g_status_block[0x1F..0x20]       UART 0x32 payload[2..3] -- RPM, BE u16
 * The [0x15]/[0x1E..0x20] offsets run past the 14-byte 0x72 payload on purpose:
 * the stock firmware reaches into the neighbouring UART payload buffers through
 * the same base pointer. Preserved as-is.
 *
 * EVERY CAN ID, bit position, mask, scale and clamp below is exactly what the
 * decompile has. Nothing was rounded or "tidied". Comments name the signal
 * where it is known; the magic numbers themselves are untouched.
 * ==========================================================================*/

#include <stdint.h>
#include "port.h"
#include "can_decode.h"

/* ---------------------------------------------------------------------------
 * Globals that app/include/globals.h does not describe correctly for this file.
 *
 * (a) The three ring/debounce words are declared uint8_t in globals.h, but the
 *     stock code accesses them with 32-bit ldr/str. Verified in
 *     reference/2429-H1N4JPF20A-220524.app.bin:
 *         0x08007d54 ldr / 0x08007d84 str -> 0x20001194 (debounce, loaded 800)
 *         0x08009018 ldr -> 0x2000111c (g_can_rx_ring_tail)
 *         0x0800901c ldr -> 0x20001118 (g_can_rx_ring_head)
 *     800 does not even fit in the uint8_t the header declares, so the byte
 *     macro would be a behaviour change. Re-pointed at the same addresses with
 *     the correct width. FLAGGED for the globals.h owner -- do not "fix" this
 *     by editing globals.h from here.
 *
 * (b) The CAN TX ring indices are not in globals.h at all: 0x20001120 and
 *     0x20001124 are listed there as g_swc_var_120 / g_swc_var_124. Their real
 *     identity is in ghidra/merged_globals.tsv and is confirmed by can.c
 *     (can_tx_enqueue / the TX pump) and by the 32-bit copy at 0x080091b2.
 * ------------------------------------------------------------------------- */
#undef  g_can_rx_ring_head
#undef  g_can_rx_ring_tail
#undef  g_bus_present_debounce
#define g_can_rx_ring_head      (*(volatile uint32_t *)0x20001118U)
#define g_can_rx_ring_tail      (*(volatile uint32_t *)0x2000111cU)
#define g_bus_present_debounce  (*(volatile uint32_t *)0x20001194U)
#define g_can_tx_ring_head      (*(volatile uint32_t *)0x20001120U)  /* = g_swc_var_120 */
#define g_can_tx_ring_tail      (*(volatile uint32_t *)0x20001124U)  /* = g_swc_var_124 */

/* ---------------------------------------------------------------------------
 * Owned by other subsystems. Declared extern with the stock address; the weak
 * no-op stubs at the bottom of this file are TEMPORARY SCAFFOLDING so the image
 * links before can.c / util.c land, and are overridden by the real definitions.
 * ------------------------------------------------------------------------- */
extern void can_tx_enqueue(uint32_t can_id, uint8_t ide_flags,
                           uint8_t dlc, const uint8_t *data);  /* can.c  0x080074c0 */
extern void softtimer_arm_at_index(void);                      /* util.c 0x08007cd2 */

/* ---- ring geometry ------------------------------------------------------ */
#define RX_RING_ENTRIES   0x32U   /* 50 */
#define RX_ENTRY_STRIDE   0x10U   /* 16 bytes */
#define RX_ENTRY_DATA     6U      /* offset of data[0] inside an entry */

/* Each decoder makes at most 20 passes per call (stock guard: "i > 0x13"
 * returns), so a flooded bus cannot starve the main loop. */
#define RX_DRAIN_PASSES   0x14U   /* 20 */

/* ---- small helpers ------------------------------------------------------ */

/* The stock code does x = (x & ~m) | (v << n) everywhere (ARM "bfi"). */
static inline void bit_assign(volatile uint8_t *p, unsigned bit, int value)
{
    if (value)
        *p = (uint8_t)(*p | (uint8_t)(1U << bit));
    else
        *p = (uint8_t)(*p & (uint8_t)~(1U << bit));
}

static inline const volatile uint8_t *rx_entry(void)
{
    return g_can_rx_ring_buf + g_can_rx_ring_tail * RX_ENTRY_STRIDE;
}

/* id = (u16 @ +0) << 16 | (u16 @ +2) -- exactly the stock CONCAT22. */
static inline uint32_t rx_id(void)
{
    const volatile uint8_t *e = rx_entry();
    uint32_t hi = *(const volatile uint16_t *)(const volatile void *)(e + 0);
    uint32_t lo = *(const volatile uint16_t *)(const volatile void *)(e + 2);

    return (hi << 16) | lo;
}

static inline void rx_advance_tail(void)
{
    g_can_rx_ring_tail = (g_can_rx_ring_tail + 1U) % RX_RING_ENTRIES;
}

/* ===========================================================================
 * Decoders shared by both profiles
 * ========================================================================= */

/* Door bitfield -> UART 0x12 payload[2].
 * Source bit 0->LF, 1->RF, 3->LR, 2->RR, 4->trunk; destination bits 7,6,5,4,3.
 * Profile 0 feeds this from 0x013 d[2]; profile 1 from 0x244 d[0]. */
static void decode_doors(uint8_t b)
{
    bit_assign(&g_status_block[0x15], 7, b & 0x01U);   /* left  front */
    bit_assign(&g_status_block[0x15], 6, b & 0x02U);   /* right front */
    bit_assign(&g_status_block[0x15], 5, b & 0x08U);   /* left  rear  */
    bit_assign(&g_status_block[0x15], 4, b & 0x04U);   /* right rear  */
    bit_assign(&g_status_block[0x15], 3, b & 0x10U);   /* tailgate    */
}

/* Steering-wheel buttons -> UART 0x72 payload[2], as the key codes the Android
 * plugin's car_key_3009001.json expects. Profile 0: CAN 0x3A0; profile 1: 0x3A3.
 * Note the deliberate ordering: later matches overwrite earlier ones, so a
 * multi-bit frame reports the LAST matching group. Preserved verbatim. */
static void decode_swc_buttons(const volatile uint8_t *d)
{
    if (d[0] == 0x00 && d[1] == 0x00) {
        g_status_block[2] = 0;                                  /* no key */
        return;
    }

    if ((d[0] & 0x01U) != 0)
        g_status_block[2] = 0x0B;                               /* MODE / source */

    if ((d[0] & 0x06U) == 0x02)
        g_status_block[2] = 0x01;                               /* volume +  */
    else if ((d[0] & 0x06U) == 0x04)
        g_status_block[2] = 0x02;                               /* volume -  */

    if ((d[0] & 0x18U) == 0x08)
        g_status_block[2] = 0x08;                               /* next / seek up   */
    else if ((d[0] & 0x18U) == 0x10)
        g_status_block[2] = 0x09;                               /* prev / seek down */

    if ((d[0] & 0x60U) == 0x20)
        g_status_block[2] = 0x03;                               /* mute */
    else if ((d[0] & 0x60U) == 0x40)
        g_status_block[2] = 0x12;                               /* unmapped in the plugin */

    if ((d[1] & 0x01U) != 0)
        g_status_block[2] = 0x05;                               /* answer / phone  */

    if ((d[1] & 0x02U) != 0)
        g_status_block[2] = 0x07;                               /* voice assistant */
}

/* Illumination-on + dimmer percent (best guess; could be fuel level -- see
 * docs/hiworld-firmware-analysis.md 3.8). Profile 0: 0x210 d[1]; profile 1:
 * 0x308 d[1]. The flag bit also drives the ILLUM GPIO. */
static void decode_illum_level(uint8_t v)
{
    bit_assign(&g_status_block[0], 1, v != 0);
    g_status_block[3] = (uint8_t)(v >> 1);
    if (g_status_block[3] > 100)
        g_status_block[3] = 100;
}

/* Ignition / ACC. Profile 0: 0x000 d[0]; profile 1: 0x20B d[0].
 * bit 4 <- d[0] bit 0 (unidentified, set in every capture)
 * bit 0 <- (d[0] & 0xE0) > 0x10, i.e. any of d[0] bits 5..7 -- gates the whole
 *          TX block and drives the ACC GPIO. */
static void decode_ignition(uint8_t v)
{
    bit_assign(&g_status_block[0], 4, v & 0x01U);
    bit_assign(&g_status_block[0], 0, (v & 0xE0U) > 0x10U);
}

/* Vendor handshake, CAN 0x7FC: magic 3F F3 F2 01 .. .. 3A A3 -> echo as 0x7FD.
 * The compare order is the stock one. */
static void decode_vendor_7fc(const volatile uint8_t *d)
{
    if (d[0] == 0x3F && d[1] == 0xF3 &&
        d[6] == 0x3A && d[7] == 0xA3 &&
        d[2] == 0xF2 && d[3] == 0x01) {
        can_tx_enqueue(0x7FD, 0, 8, (const uint8_t *)d);
    }
}

/* Vendor status injection, CAN 0x7FF: magic 7F F7 x x y y 3A A3 with d[2]==d[3]
 * and d[4]==d[5]. Returns non-zero when the magic matched (profile 1 appends
 * more work on a match). Writes 0x72 payload[0] bits 0..3 from d[2]. */
static int decode_vendor_7ff_common(const volatile uint8_t *d)
{
    if (!(d[0] == 0x7F && d[1] == 0xF7 &&
          d[6] == 0x3A && d[7] == 0xA3 &&
          d[2] == d[3] && d[4] == d[5]))
        return 0;

    bit_assign(&g_status_block[0], 0, d[2] & 0x01U);
    bit_assign(&g_status_block[0], 1, d[2] & 0x02U);
    bit_assign(&g_status_block[0], 2, d[2] & 0x04U);
    bit_assign(&g_status_block[0], 3, d[2] & 0x08U);
    return 1;
}

/* Hiworld node-addressing / SWC service protocol on 0x400..0x43F.
 * The low 6 bits of the CAN id ARE the key index; d[0]==0xFD marks a key
 * report, d[1] the key data, d[2] a bus-state probe.
 * Identical in both profiles (the stock image duplicated the whole block). */
static void decode_swc_service(uint32_t can_id, const volatile uint8_t *d)
{
    if ((can_id & 0xFC0U) != 0x400U)
        return;

    g_swc_keycode = (uint8_t)(can_id & 0x3FU);
    softtimer_arm_at_index();          /* arms g_softtimer_slots[keycode] = 150 */

    if (d[0] != 0xFD)
        return;

    g_swc_keydata = d[1];

    if (g_swc_keydata == 0x16) {       /* 0x16 = handshake */
        g_swc_resp_mode = 9;
        g_swc_tx_buf[0] = 0xFD;
        g_swc_tx_buf[1] = g_swc_keydata_ref;
    } else if (g_swc_keydata == g_swc_keydata_ref) {
        g_swc_tx_buf[0] = 0xFE;
        g_swc_tx_buf[1] = g_swc_keydata_ref;
        can_tx_enqueue(0x416, 0, 8, (const uint8_t *)g_swc_tx_buf);
    }

    if (g_swc_keycode == g_swc_keycode_prev) {
        if (g_swc_keydata == 0x16) {
            g_swc_timer_118c = 0x50;
        } else {
            g_swc_tx_buf[0] = 0xFE;
            g_swc_tx_buf[1] = g_swc_keydata_ref;
            can_tx_enqueue(0x416, 0, 8, (const uint8_t *)g_swc_tx_buf);
        }
    } else if (g_swc_keycode == g_swc_keydata_ref) {
        g_swc_flag_118b = 0;
    }

    /* d[2] probe, qualified by the ACC/run flag (0x72 payload[0] bit 0). */
    if (d[2] == 0x3F && (g_status_block[0] & 0x01U) != 0) {
        g_swc_tx_buf[2] = 0x3F;
    } else if (d[2] == 0xBF && (g_status_block[0] & 0x01U) == 0) {
        g_swc_tx_buf[2] = 0xBF;
    } else if (d[2] == 0xFF && (g_status_block[0] & 0x01U) == 0) {
        /* bus declared down: clear the status and FLUSH the CAN TX queue. */
        g_can_bus_status = 0;
        g_can_tx_ring_tail = g_can_tx_ring_head;
    }
}

/* ===========================================================================
 * Profile 0 -- 83.333 kbit/s. THIS box (Jeep WK family, H1N4JPF20A / JPF2).
 * No RPM, gear or vehicle-speed decode exists on this profile.
 * ========================================================================= */

/* 0x015 d[4]:d[5] -- steering angle, centre 0x1000, 0.5 deg/LSB, BE signed 16.
 * Only updated while the reverse flag (0x72 payload[0] bit 2) is set, i.e. it
 * exists to drive the reverse-camera trajectory lines. No clamp on profile 0. */
static void p0_decode_steering(const volatile uint8_t *d)
{
    uint16_t raw;
    uint16_t angle;

    if ((g_status_block[0] & 0x04U) == 0)
        return;

    raw   = (uint16_t)(((uint16_t)d[4] << 8) | d[5]);
    angle = raw;                                   /* raw == 0 -> reported as 0 */
    if (raw != 0) {
        if (raw < 0x1000U)
            angle = (uint16_t)((0x1000U - raw) >> 1);
        else if (raw == 0x1000U)
            angle = 0;
        else
            angle = (uint16_t)(0U - (uint32_t)((raw - 0x1000U) >> 1));
    }

    g_status_block[4] = (uint8_t)(angle >> 8);
    g_status_block[5] = (uint8_t)angle;
}

/* 0x003 d[2] low nibble -- reverse gear (1 or 9). */
static void p0_decode_reverse(const volatile uint8_t *d)
{
    uint8_t gear = (uint8_t)(d[2] & 0x0FU);

    bit_assign(&g_status_block[0], 2, (gear == 1) || (gear == 9));
}

/* 0x013 d[2] -- the door / lamp bitfield this profile reports. */
static void p0_decode_0x013(const volatile uint8_t *d)
{
    decode_doors(d[2]);
}

/* 0x500 d[0]:d[1] -- vendor echo, retransmitted as 0x516 zero-padded to 8. */
static void p0_decode_echo_500(const volatile uint8_t *d)
{
    g_can_echo_buf_516[0] = d[0];
    g_can_echo_buf_516[1] = d[1];
    g_can_echo_buf_516[2] = 0;
    g_can_echo_buf_516[3] = 0;
    g_can_echo_buf_516[4] = 0;
    g_can_echo_buf_516[5] = 0;
    g_can_echo_buf_516[6] = 0;
    g_can_echo_buf_516[7] = 0;
    can_tx_enqueue(0x516, 0, 8, (const uint8_t *)g_can_echo_buf_516);
}

/* --- 0x08009010 can_dispatch_profile0_83k --- */
void can_dispatch_profile0_83k(void)
{
    uint8_t pass;

    for (pass = 0; pass < RX_DRAIN_PASSES; pass++) {
        uint32_t id;
        const volatile uint8_t *d;

        if (g_can_rx_ring_tail == g_can_rx_ring_head)
            continue;              /* stock spins the guard out, it does not exit */

        g_profile0_activity_ctr = 100;            /* bus-alive watchdog reload */

        id = rx_id();
        d  = rx_entry() + RX_ENTRY_DATA;

        switch (id) {
        case 0x000: decode_ignition(d[0]);             break; /* ACC / run          */
        case 0x003: p0_decode_reverse(d);              break; /* reverse gear       */
        case 0x013: p0_decode_0x013(d);                break; /* doors              */
        case 0x015: p0_decode_steering(d);             break; /* steering angle     */
        case 0x210: decode_illum_level(d[1]);          break; /* illum + dimmer %   */
        case 0x3A0: decode_swc_buttons(d);             break; /* SWC keys           */
        case 0x500: p0_decode_echo_500(d);             break; /* vendor echo ->0x516*/
        case 0x7FC: decode_vendor_7fc(d);              break; /* handshake   ->0x7FD*/
        case 0x7FF: (void)decode_vendor_7ff_common(d); break; /* status injection   */
        default:    decode_swc_service(id, d);         break; /* 0x400..0x43F       */
        }

        rx_advance_tail();
    }
}

/* ===========================================================================
 * Profile 1 -- 125 kbit/s FCA variant (NOT this car).
 * ========================================================================= */

/* 0x1E1 d[2]:d[3] -- steering angle, same centre/scale as profile 0 but
 * clamped to +-539 (0x21B) and the negative half encoded as 0x10001 - v.
 * The 0x10001 (not 0x10000) and the 0x21C clamp on the negative side are what
 * the stock code does; truncation to 16 bits makes the reported value 1 - v.
 * The +-540 clamp matches assets/car_trajectory_3009001.json. */
static void p1_decode_steering(const volatile uint8_t *d)
{
    uint32_t raw;
    uint32_t v;

    if ((g_status_block[0] & 0x04U) == 0)
        return;

    raw = ((uint32_t)d[2] << 8) | (uint32_t)d[3];

    if (raw < 0x1000U) {
        v = (0x1000U - raw) >> 1;
        if (v > 0x21BU)
            v = 0x21BU;
    } else if (raw == 0x1000U) {
        v = 0;
    } else {
        v = (raw - 0x1000U) >> 1;
        if (v > 0x21BU)
            v = 0x21CU;
        v = 0x10001U - v;
    }

    g_status_block[4] = (uint8_t)(v >> 8);
    g_status_block[5] = (uint8_t)v;
}

/* 0x20E d[2] -- PRNDL. Low nibble 0->P(1), 2->N(2), 4->D(4), 9->hold, else 0;
 * then d[2] bit 3 is the reverse flag and forces gear R(3). */
static void p1_decode_gear(const volatile uint8_t *d)
{
    uint8_t n = (uint8_t)(d[2] & 0x0FU);

    if (n == 0)
        g_status_block[0x1E] = 1;              /* P */
    else if (n == 2)
        g_status_block[0x1E] = 2;              /* N */
    else if (n == 4)
        g_status_block[0x1E] = 4;              /* D */
    else if (n != 9)
        g_status_block[0x1E] = 0;              /* invalid (9 leaves it alone) */

    if ((d[2] & 0x08U) == 0) {
        bit_assign(&g_status_block[0], 2, 0);
    } else {
        bit_assign(&g_status_block[0], 2, 1);
        g_status_block[0x1E] = 3;              /* R */
    }
}

/* 0x211 d[0]:d[1] -- engine speed, BE u16, 1 rpm/LSB. Decimated 1-in-21 by
 * g_rpm_decimator_ctr and rejected at or above 8000 rpm. */
static void p1_decode_rpm(const volatile uint8_t *d)
{
    if (g_rpm_decimator_ctr < 0x14) {
        g_rpm_decimator_ctr = (uint8_t)(g_rpm_decimator_ctr + 1);
        return;
    }

    g_rpm_decimator_ctr = 0;
    if (((uint32_t)d[0] * 0x100U + (uint32_t)d[1]) < 8000U) {
        g_status_block[0x1F] = d[0];
        g_status_block[0x20] = d[1];
    }
}

/* 0x244 d[0] -- doors. */
static void p1_decode_0x244(const volatile uint8_t *d)
{
    decode_doors(d[0]);
}

/* 0x513 d[0..3] -- vendor echo, retransmitted as 0x516 zero-padded to 8. */
static void p1_decode_echo_513(const volatile uint8_t *d)
{
    g_can_echo_buf_516[0] = d[0];
    g_can_echo_buf_516[1] = d[1];
    g_can_echo_buf_516[2] = d[2];
    g_can_echo_buf_516[3] = d[3];
    g_can_echo_buf_516[4] = 0;
    g_can_echo_buf_516[5] = 0;
    g_can_echo_buf_516[6] = 0;
    g_can_echo_buf_516[7] = 0;
    can_tx_enqueue(0x516, 0, 8, (const uint8_t *)g_can_echo_buf_516);
}

/* 0x7FF on this profile: the shared status injection, PLUS a PRNDL byte in
 * d[4] that is mapped into the SWC-key slot (0x72 payload[2] -- yes, [2], not
 * the gear slot; that is what the stock code does), PLUS a one-byte reply on
 * 0x7FE reflecting the head unit's current display mode (RX 0xD2 sub-command
 * byte at g_cmd_block @0x20001BE7). */
static void p1_decode_vendor_7ff(const volatile uint8_t *d)
{
    if (!decode_vendor_7ff_common(d))
        return;

    switch (d[4]) {
    case 0:  g_status_block[2] = 0x00; break;
    case 1:  g_status_block[2] = 0x02; break;
    case 2:  g_status_block[2] = 0x01; break;
    case 3:  g_status_block[2] = 0x09; break;
    case 4:  g_status_block[2] = 0x08; break;
    case 5:  g_status_block[2] = 0x0B; break;
    case 6:  g_status_block[2] = 0x03; break;
    default: break;                      /* 7..255 leave the slot unchanged */
    }

    g_prndl_tx_7fe[0] = 0;
    if (g_cmd_block[0] == 1)
        g_prndl_tx_7fe[0] = (uint8_t)(g_prndl_tx_7fe[0] | 0x01U);
    else
        g_prndl_tx_7fe[0] = (uint8_t)(g_prndl_tx_7fe[0] & 0xFEU);

    if (g_cmd_block[0] == 2)
        g_prndl_tx_7fe[0] = (uint8_t)(g_prndl_tx_7fe[0] | 0x02U);
    else
        g_prndl_tx_7fe[0] = (uint8_t)(g_prndl_tx_7fe[0] & 0xFDU);

    if (g_cmd_block[0] == 3)
        g_prndl_tx_7fe[0] = (uint8_t)(g_prndl_tx_7fe[0] | 0x04U);
    else
        g_prndl_tx_7fe[0] = (uint8_t)(g_prndl_tx_7fe[0] & 0xFBU);

    /* NOTE: the stock image uses bit 2 (0x04 / 0xFB) again here, not bit 3.
     * It looks like a copy-paste slip in the original, but it is reproduced
     * exactly -- mode 4 is indistinguishable from mode 3 on the wire. */
    if (g_cmd_block[0] == 4)
        g_prndl_tx_7fe[0] = (uint8_t)(g_prndl_tx_7fe[0] | 0x04U);
    else
        g_prndl_tx_7fe[0] = (uint8_t)(g_prndl_tx_7fe[0] & 0xFBU);

    can_tx_enqueue(0x7FE, 0, 1, (const uint8_t *)g_prndl_tx_7fe);
}

/* --- 0x08009680 can_dispatch_profile1_125k --- */
void can_dispatch_profile1_125k(void)
{
    uint8_t pass;

    for (pass = 0; pass < RX_DRAIN_PASSES; pass++) {
        uint32_t id;
        const volatile uint8_t *d;

        if (g_can_rx_ring_tail == g_can_rx_ring_head)
            continue;

        /* NOTE the different reload value: 0x14 here vs 100 on profile 0. */
        g_profile0_activity_ctr = 0x14;

        id = rx_id();
        d  = rx_entry() + RX_ENTRY_DATA;

        switch (id) {
        case 0x1E1: p1_decode_steering(d);     break; /* steering angle      */
        case 0x20B: decode_ignition(d[0]);     break; /* ACC / run           */
        case 0x20E: p1_decode_gear(d);         break; /* PRNDL + reverse     */
        case 0x211: p1_decode_rpm(d);          break; /* engine speed        */
        case 0x244: p1_decode_0x244(d);        break; /* doors               */
        case 0x308: decode_illum_level(d[1]);  break; /* illum + dimmer %    */
        case 0x3A3: decode_swc_buttons(d);     break; /* SWC keys            */
        case 0x513: p1_decode_echo_513(d);     break; /* vendor echo ->0x516 */
        case 0x7FC: decode_vendor_7fc(d);      break; /* handshake   ->0x7FD */
        case 0x7FF: p1_decode_vendor_7ff(d);   break; /* status + PRNDL diag */
        default:    decode_swc_service(id, d); break; /* 0x400..0x43F        */
        }

        rx_advance_tail();
    }
}

/* ===========================================================================
 * 0x08007d44 can_profile_dispatch_gate -- the decoder entry point.
 *
 * Before dispatching it maintains the "bus present" debounce off the ACC/run
 * flag (0x72 payload[0] bit 0):
 *   - run flag clear -> reload the 800-tick debounce and SET bit 4 of the
 *                       periodic 0x18C TX frame (the "no ignition" marker);
 *   - run flag set and the debounce expired -> clear that bit and re-arm the
 *                       0x3D0 amplifier TX frame from g_amp_level;
 *   - otherwise      -> count the debounce down.
 * Then: g_profile_active (persisted model byte @0x2000118F, taken from the
 * param page byte 0x200011D1) == 1 selects the 125 kbit/s decoder, anything
 * else the 83.333 kbit/s one.
 * ========================================================================= */
void can_profile_dispatch_gate(void)
{
    if ((g_status_block[0] & 0x01U) == 0) {
        g_bus_present_debounce = 800;
        g_tx_frame_18c[0] = (uint8_t)((g_tx_frame_18c[0] & 0xEFU) + 0x10U);
    } else if (g_bus_present_debounce == 0) {
        g_tx_frame_18c[0] = (uint8_t)(g_tx_frame_18c[0] & 0xEFU);
        g_tx_frame_amp[0] = g_amp_level;
    } else {
        g_bus_present_debounce = g_bus_present_debounce - 1U;
    }

    if (g_profile_active == 1)
        can_dispatch_profile1_125k();
    else
        can_dispatch_profile0_83k();
}

/* ===========================================================================
 * TEMPORARY SCAFFOLDING -- weak no-op stubs for the cross-subsystem calls.
 * Delete these the moment can.c (can_tx_enqueue @0x080074c0) and util.c
 * (softtimer_arm_at_index @0x08007cd2) are ported; the strong definitions
 * there override them automatically, so leaving them in costs nothing but
 * they silently disable every CAN reply if the real code never lands.
 * ========================================================================= */
__attribute__((weak))
void can_tx_enqueue(uint32_t can_id, uint8_t ide_flags,
                    uint8_t dlc, const uint8_t *data)
{
    (void)can_id;
    (void)ide_flags;
    (void)dlc;
    (void)data;
}

__attribute__((weak))
void softtimer_arm_at_index(void)
{
}
