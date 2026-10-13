/* ============================================================================
 * can_decode.h -- CAN receive-frame decoders (the car profiles).
 *
 * Reconstructed from app/src/can_rx_decode.c (Ghidra output). These three
 * functions are the behavioural heart of the box: they drain the CAN RX ring
 * and turn incoming frames into the state block the UART side reports to the
 * head unit (ignition, doors, lamps, steering angle, SWC keys, gear, RPM...).
 *
 * Only can_profile_dispatch_gate() is called from outside (the main loop);
 * the two profile decoders are exported so the dispatch can be unit-tested
 * and so the flash-address mapping stays visible.
 * ==========================================================================*/

#ifndef CAN_DECODE_H
#define CAN_DECODE_H

/* --- decoder entry point, called once per main-loop pass ----------------- */
void can_profile_dispatch_gate(void);      /* 0x08007d44 */

/* --- the two car profiles ----------------------------------------------- */
void can_dispatch_profile0_83k(void);      /* 0x08009010 -- Jeep WK, 83.333k */
void can_dispatch_profile1_125k(void);     /* 0x08009680 -- FCA variant, 125k */

#endif /* CAN_DECODE_H */
