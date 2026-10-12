// app subsystem: can_rx_decode  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= can_profile_dispatch_gate @ 0x08007d44 =================

/* can_profile_dispatch_gate — routes received CAN frames to profile0 (83k) or profile1 (125k) by
   the persisted model byte (RAM 0x200011D1): ==1 -> profile1 else profile0. */

void can_profile_dispatch_gate(void)

{
  if ((*(byte *)(p_vstate_mid + -0x17) & 1) == 0) {
    *p_bus_present_debounce = 800;
    *p_tx_frame_18c = (*p_tx_frame_18c & 0xef) + 0x10;
  }
  else if (*p_bus_present_debounce == 0) {
    *p_tx_frame_18c = *p_tx_frame_18c & 0xef;
    *p_tx_frame_amp = *(undefined1 *)(p_vstate_mid + 0x15);
  }
  else {
    *p_bus_present_debounce = *p_bus_present_debounce + -1;
  }
  if (*p_profile_active == '\x01') {
    can_dispatch_profile1_125k();
  }
  else {
    can_dispatch_profile0_83k();
  }
  return;
}


// ================= can_dispatch_profile0_83k @ 0x08009010 =================

/* can_dispatch_profile0_83k - CAN RX decoder for THIS car (Jeep WK, 83.333k body bus). Drains the
   50-entry RX ring [g_can_rx_ring_tail..g_can_rx_ring_head] at g_can_rx_ring_buf (entry=16B: id as
   2x u16 at +0/+2, data[8] at +6..+13); resets g_profile0_activity_ctr=100 per frame; advances tail
   mod 50. Decodes into g_status_block @0x20001b49 (read by gpio_write_status_lines):
     0x210: byte0 bit1=ILLUM (d7!=0); byte3=FUEL level 0..100 (d7>>1, cap 100).
     0x000: byte0 bit4=ACC (d6 bit0); byte0 bit0 (d6 top bits > 0x10).
     0x003: gear nibble d8&0xf; ==1 or ==9 -> byte0 bit2=REVERSE, else clear.
     0x013: byte0x15 lamp bitfield from d8 bits (headlight/park/etc).
     0x015: STEERING angle, 16-bit d10:d11 centered 0x1000 -> byte4:byte5 (gated by status bit2).
     0x3a0: byte2 = turn/indicator state enum {0,1,2,3,5,7,8,9,0xb,0x12}.
     0x400..0x43f (id&0xfc0==0x400): SWC steering-wheel keys -> g_swc_keycode/g_swc_keydata, arms
   softtimer, builds 8B g_swc_tx_buf and can_tx_enqueue(0x416); keycode 0x16 = handshake.
     0x500: echo d6:d7 -> can_tx_enqueue(0x516) (g_can_echo_buf_516).
     0x7fc: diagnostic magic match -> reply can_tx_enqueue(0x7fd).
     0x7ff: config/diag match -> writes g_can_bus_status low bits + g_status_block low nibble.
   Note: no RPM/gear-ratio/vehicle-speed decode on this profile. */

void can_dispatch_profile0_83k(void)

{
  byte gearNibble;
  byte iterGuard;
  ushort steerVal;
  uint idLoOrKey;
  int canId;
  bool isSwcId;
  byte *pState;
  undefined1 *pTxBuf;
  
  iterGuard = 0;
  do {
    if (0x13 < iterGuard) {
      return;
    }
    if (*p_can_rx_ring_tail != *p_can_rx_ring_head) {
      *p_profile0_activity_ctr = 100;
      pState = p_status_block;
      steerVal = *(ushort *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 2);
      canId = CONCAT22(*(undefined2 *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10),steerVal);
      if (canId == 0x210) {
        *p_status_block =
             *p_status_block & 0xfd |
             (*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7) != '\0') << 1;
        pState = p_status_block;
        p_status_block[3] =
             (byte)((int)(uint)*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7) >> 1);
        if (100 < pState[3]) {
          pState[3] = 100;
        }
      }
      else if (canId < 0x211) {
        if (canId == 0) {
          *p_status_block =
               *p_status_block & 0xef |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 1) << 4;
          *p_status_block =
               *p_status_block & 0xfe |
               0x10 < (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 0xe0);
        }
        else if (canId == 3) {
          gearNibble = *(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) & 0xf;
          if (gearNibble == 1) {
            *p_status_block = (*p_status_block & 0xfb) + 4;
          }
          else if (gearNibble == 9) {
            *p_status_block = (*p_status_block & 0xfb) + 4;
          }
          else {
            *p_status_block = *p_status_block & 0xfb;
          }
        }
        else if (canId == 0x13) {
          p_status_block[0x15] =
               p_status_block[0x15] & 0x7f |
               *(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) << 7;
          p_status_block[0x15] =
               p_status_block[0x15] & 0xbf |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) >> 1 & 1) << 6;
          p_status_block[0x15] =
               p_status_block[0x15] & 0xdf |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) >> 3 & 1) << 5;
          p_status_block[0x15] =
               p_status_block[0x15] & 0xef |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) >> 2 & 1) << 4;
          p_status_block[0x15] =
               p_status_block[0x15] & 0xf7 |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) >> 4 & 1) << 3;
        }
        else if (canId == 0x15) {
          if ((int)((uint)*p_status_block << 0x1d) < 0) {
            steerVal = CONCAT11(*(undefined1 *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 10)
                                ,*(undefined1 *)
                                  (p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 0xb));
            idLoOrKey = (uint)steerVal;
            if (idLoOrKey != 0) {
              if (idLoOrKey < 0x1000) {
                steerVal = (ushort)(0x1000 - idLoOrKey >> 1);
              }
              else if (idLoOrKey == 0x1000) {
                steerVal = 0;
              }
              else {
                steerVal = -(short)(idLoOrKey - 0x1000 >> 1);
              }
            }
            p_status_block[4] = (byte)(steerVal >> 8);
            pState[5] = (byte)steerVal;
          }
        }
        else {
LAB_08009076:
          isSwcId = (steerVal & 0xfc0) == 0x400;
          do {
            if (!isSwcId) goto code_r0x08009652;
            *p_swc_keycode = (byte)steerVal & 0x3f;
            softtimer_arm_at_index();
            pState = p_swc_keydata;
            isSwcId = *(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) == -3;
          } while (!isSwcId);
          *p_swc_keydata = *(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7);
          pTxBuf = p_swc_tx_buf;
          if (*pState == 0x16) {
            *p_swc_resp_mode = 9;
            pTxBuf = p_swc_tx_buf;
            *p_swc_tx_buf = 0xfd;
            pTxBuf[1] = *p_swc_keydata_ref;
          }
          else if (*p_swc_keydata == *p_swc_keydata_ref) {
            *p_swc_tx_buf = 0xfe;
            pTxBuf[1] = *p_swc_keydata_ref;
            can_tx_enqueue(0x416,0,8,pTxBuf);
          }
          pTxBuf = p_swc_tx_buf;
          if (*p_swc_keycode == *p_swc_keycode_prev) {
            if (*p_swc_keydata == 0x16) {
              *p_swc_timer_118c = 0x50;
            }
            else {
              *p_swc_tx_buf = 0xfe;
              pTxBuf[1] = *p_swc_keydata_ref;
              can_tx_enqueue(0x416,0,8,pTxBuf);
            }
          }
          else if (*p_swc_keycode == *p_swc_keydata_ref) {
            *p_swc_flag_118b = 0;
          }
          if ((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) == '?') &&
             ((*p_status_block & 1) != 0)) {
            p_swc_tx_buf[2] = 0x3f;
          }
          else if ((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) == -0x41) &&
                  ((*p_status_block & 1) == 0)) {
            p_swc_tx_buf[2] = 0xbf;
          }
          else if ((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) == -1) &&
                  ((*p_status_block & 1) == 0)) {
            *p_can_bus_status = 0;
            *p_can_tx_ring_tail = *p_can_tx_ring_head;
          }
        }
      }
      else if (canId == 0x3a0) {
        if ((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) == '\0') &&
           (*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7) == '\0')) {
          p_status_block[2] = 0;
        }
        else {
          if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 1) != 0) {
            p_status_block[2] = 0xb;
          }
          if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 6) == 2) {
            p_status_block[2] = 1;
          }
          else if ((*(byte *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 6) & 6) == 4) {
            p_status_block_b[2] = 2;
          }
          if ((*(byte *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 6) & 0x18) == 8) {
            p_status_block_b[2] = 8;
          }
          else if ((*(byte *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 6) & 0x18) ==
                   0x10) {
            p_status_block_b[2] = 9;
          }
          if ((*(byte *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 6) & 0x60) == 0x20) {
            p_status_block_b[2] = 3;
          }
          else if ((*(byte *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 6) & 0x60) ==
                   0x40) {
            p_status_block_b[2] = 0x12;
          }
          if ((*(byte *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 7) & 1) != 0) {
            p_status_block_b[2] = 5;
          }
          if ((*(byte *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 7) & 2) != 0) {
            p_status_block_b[2] = 7;
          }
        }
      }
      else if (canId == 0x500) {
        *p_can_echo_buf_516 = *(undefined1 *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6);
        pTxBuf = p_can_echo_buf_516;
        p_can_echo_buf_516[1] = *(undefined1 *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7);
        pTxBuf[2] = 0;
        pTxBuf[3] = 0;
        pTxBuf[4] = 0;
        pTxBuf[5] = 0;
        pTxBuf[6] = 0;
        pTxBuf[7] = 0;
        can_tx_enqueue(0x516,0,8,pTxBuf);
      }
      else if (canId == 0x7fc) {
        if (((((*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 6) == '?') &&
              (*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 7) == -0xd)) &&
             (*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 0xc) == ':')) &&
            ((*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 0xd) == -0x5d &&
             (*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 8) == -0xe)))) &&
           (*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 9) == '\x01')) {
          can_tx_enqueue(0x7fd,0,8,p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 6);
        }
      }
      else {
        if (canId != 0x7ff) goto LAB_08009076;
        if (((*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 6) == '\x7f') &&
            (*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 7) == -9)) &&
           ((*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 0xc) == ':' &&
            (((*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 0xd) == -0x5d &&
              (*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 8) ==
               *(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 9))) &&
             (*(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 10) ==
              *(char *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 0xb))))))) {
          *p_status_block_b =
               *p_status_block_b & 0xfe |
               *(byte *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 8) & 1;
          *p_status_block_b =
               *p_status_block_b & 0xfd |
               (*(byte *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 8) >> 1 & 1) << 1;
          *p_status_block_b =
               *p_status_block_b & 0xfb |
               (*(byte *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 8) >> 2 & 1) << 2;
          *p_status_block_b =
               *p_status_block_b & 0xf7 |
               (*(byte *)(p_can_rx_ring_buf_b + *p_can_rx_ring_tail_b * 0x10 + 8) >> 3 & 1) << 3;
        }
      }
code_r0x08009652:
      *p_can_rx_ring_tail_b = (*p_can_rx_ring_tail_b + 1) % 0x32;
    }
    iterGuard = iterGuard + 1;
  } while( true );
}


// ================= can_dispatch_profile1_125k @ 0x08009680 =================

/* can_dispatch_profile1_125k - CAN RX decoder for the 125k FCA variant (NOT this car; WK bus is
   83.333k). Drains g_can_rx_ring_buf [tail..head], reloads g_profile0_activity_ctr=0x14/frame,
   decodes into g_status_block @0x20001b49:
     0x20B d0: [0] bit0=run((d0&0xE0)>0x10), bit4=ACC(d0&1).
     0x308 d1: [0] bit1=fuel present; [3]=d1>>1 cap100 fuel%.
     0x20E d2: [0x1e]=PRNDL gear(nib 0->1,2->2,4->4,9->hold); d2 bit3 -> [0] bit2 reverse +
   [0x1e]=3.
     0x1E1 d2:d3 BE: [4:5] steering angle signed (gated on run bit, +-0x21B).
     0x211 d0:d1 BE: [0x1f]=d0,[0x20]=d1 RPM raw (<8000, 1-in-21 via g_rpm_decimator_ctr).
     0x244 d0: [0x15] door bits LF/RF/LR/RR/trunk.
     0x3A3: [2]=SWC keycode enum.
     0x513 d0..d3 -> g_can_echo_buf_516, TX 0x516.
     0x400-0x43F: SWC keys -> g_swc_keycode, softtimer_arm, TX 0x416.
     0x7FC: vendor diag -> TX 0x7FD.  0x7FF: PRNDL diag -> builds g_prndl_tx_7fe, TX 0x7FE.
   Matches hiworld-all-car-profiles.md section 3. */

void can_dispatch_profile1_125k(void)

{
  byte bVar1;
  ushort uVar2;
  byte *pbVar3;
  undefined1 *puVar4;
  byte loopGuard;
  byte bVar5;
  uint uVar6;
  int iVar7;
  bool bVar8;
  
  bVar5 = 0;
  do {
    if (0x13 < bVar5) {
      return;
    }
    if (*p_can_rx_ring_tail != *p_can_rx_ring_head) {
      *p_profile0_activity_ctr = 0x14;
      pbVar3 = p_status_block;
      uVar2 = *(ushort *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 2);
      iVar7 = CONCAT22(*(undefined2 *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10),uVar2);
      if (iVar7 == 0x308) {
        *p_status_block =
             *p_status_block & 0xfd |
             (*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7) != '\0') << 1;
        pbVar3 = p_status_block;
        p_status_block[3] =
             (byte)((int)(uint)*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7) >> 1);
        if (100 < pbVar3[3]) {
          pbVar3[3] = 100;
        }
      }
      else if (iVar7 < 0x309) {
        if (iVar7 == 0x20e) {
          bVar1 = *(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8);
          loopGuard = bVar1 & 0xf;
          if ((bVar1 & 0xf) == 0) {
            p_status_block[0x1e] = 1;
          }
          else if (loopGuard == 2) {
            p_status_block[0x1e] = 2;
          }
          else if (loopGuard == 4) {
            p_status_block[0x1e] = 4;
          }
          else if (loopGuard != 9) {
            p_status_block[0x1e] = 0;
          }
          if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) & 8) == 0) {
            *p_status_block = *p_status_block & 0xfb;
          }
          else {
            *p_status_block = (*p_status_block & 0xfb) + 4;
            p_status_block[0x1e] = 3;
          }
        }
        else if (iVar7 < 0x20f) {
          if (iVar7 == 0x1e1) {
            if ((int)((uint)*p_status_block << 0x1d) < 0) {
              uVar6 = (uint)CONCAT11(*(undefined1 *)
                                      (p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8),
                                     *(undefined1 *)
                                      (p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 9));
              if (uVar6 < 0x1000) {
                uVar6 = 0x1000 - uVar6 >> 1;
                if (0x21b < uVar6) {
                  uVar6 = 0x21b;
                }
              }
              else if (uVar6 == 0x1000) {
                uVar6 = 0;
              }
              else {
                uVar6 = uVar6 - 0x1000 >> 1;
                if (0x21b < uVar6) {
                  uVar6 = 0x21c;
                }
                uVar6 = 0x10001 - uVar6;
              }
              p_status_block[4] = (byte)(uVar6 >> 8);
              pbVar3[5] = (byte)uVar6;
            }
          }
          else if (iVar7 == 0x20b) {
            *p_status_block =
                 *p_status_block & 0xef |
                 (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 1) << 4;
            *p_status_block =
                 *p_status_block & 0xfe |
                 0x10 < (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 0xe0);
          }
          else {
LAB_080096fc:
            bVar8 = (uVar2 & 0xfc0) == 0x400;
            do {
              if (!bVar8) goto code_r0x08009e92;
              *p_swc_keycode = (byte)uVar2 & 0x3f;
              softtimer_arm_at_index();
              pbVar3 = p_swc_keydata;
              bVar8 = *(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) == -3;
            } while (!bVar8);
            *p_swc_keydata = *(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7);
            puVar4 = p_swc_tx_buf;
            if (*pbVar3 == 0x16) {
              *p_swc_resp_mode = 9;
              puVar4 = p_swc_tx_buf;
              *p_swc_tx_buf = 0xfd;
              puVar4[1] = *p_swc_keydata_ref;
            }
            else if (*p_swc_keydata == *p_swc_keydata_ref) {
              *p_swc_tx_buf = 0xfe;
              puVar4[1] = *p_swc_keydata_ref;
              can_tx_enqueue(0x416,0,8,puVar4);
            }
            puVar4 = p_swc_tx_buf;
            if (*p_swc_keycode == *p_swc_keycode_prev) {
              if (*p_swc_keydata == 0x16) {
                *p_swc_timer_118c = 0x50;
              }
              else {
                *p_swc_tx_buf = 0xfe;
                puVar4[1] = *p_swc_keydata_ref;
                can_tx_enqueue(0x416,0,8,puVar4);
              }
            }
            else if (*p_swc_keycode == *p_swc_keydata_ref) {
              *p_swc_flag_118b = 0;
            }
            if ((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) == '?') &&
               ((*p_status_block & 1) != 0)) {
              p_swc_tx_buf[2] = 0x3f;
            }
            else if ((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) == -0x41) &&
                    ((*p_status_block & 1) == 0)) {
              p_swc_tx_buf[2] = 0xbf;
            }
            else if ((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) == -1) &&
                    ((*p_status_block & 1) == 0)) {
              *p_can_bus_status = 0;
              *p_can_tx_ring_tail = *p_can_tx_ring_head;
            }
          }
        }
        else if (iVar7 == 0x211) {
          if (*p_rpm_decimator_ctr < 0x14) {
            *p_rpm_decimator_ctr = *p_rpm_decimator_ctr + 1;
          }
          else {
            *p_rpm_decimator_ctr = 0;
            if ((uint)*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7) +
                (uint)*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) * 0x100 < 8000)
            {
              p_status_block[0x1f] = *(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6);
              p_status_block[0x20] = *(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7);
            }
          }
        }
        else {
          if (iVar7 != 0x244) goto LAB_080096fc;
          p_status_block[0x15] =
               p_status_block[0x15] & 0x7f |
               *(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) << 7;
          p_status_block[0x15] =
               p_status_block[0x15] & 0xbf |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) >> 1 & 1) << 6;
          p_status_block[0x15] =
               p_status_block[0x15] & 0xdf |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) >> 3 & 1) << 5;
          p_status_block[0x15] =
               p_status_block[0x15] & 0xef |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) >> 2 & 1) << 4;
          p_status_block[0x15] =
               p_status_block[0x15] & 0xf7 |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) >> 4 & 1) << 3;
        }
      }
      else if (iVar7 == 0x3a3) {
        if ((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) == '\0') &&
           (*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7) == '\0')) {
          p_status_block[2] = 0;
        }
        else {
          if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 1) != 0) {
            p_status_block[2] = 0xb;
          }
          if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 6) == 2) {
            p_status_block[2] = 1;
          }
          else if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 6) == 4) {
            p_status_block[2] = 2;
          }
          if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 0x18) == 8) {
            p_status_block[2] = 8;
          }
          else if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 0x18) == 0x10) {
            p_status_block[2] = 9;
          }
          if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 0x60) == 0x20) {
            p_status_block[2] = 3;
          }
          else if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) & 0x60) == 0x40) {
            p_status_block[2] = 0x12;
          }
          if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7) & 1) != 0) {
            p_status_block[2] = 5;
          }
          if ((*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7) & 2) != 0) {
            p_status_block[2] = 7;
          }
        }
      }
      else if (iVar7 == 0x513) {
        *p_can_echo_buf_516 = *(undefined1 *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6);
        p_can_echo_buf_516[1] = *(undefined1 *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7);
        p_can_echo_buf_516[2] = *(undefined1 *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8);
        puVar4 = p_can_echo_buf_516;
        p_can_echo_buf_516[3] = *(undefined1 *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 9);
        puVar4[4] = 0;
        puVar4[5] = 0;
        puVar4[6] = 0;
        puVar4[7] = 0;
        can_tx_enqueue(0x516,0,8,puVar4);
      }
      else if (iVar7 == 0x7fc) {
        if (((((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) == '?') &&
              (*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7) == -0xd)) &&
             (*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 0xc) == ':')) &&
            ((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 0xd) == -0x5d &&
             (*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) == -0xe)))) &&
           (*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 9) == '\x01')) {
          can_tx_enqueue(0x7fd,0,8,p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6);
        }
      }
      else {
        if (iVar7 != 0x7ff) goto LAB_080096fc;
        bVar8 = *(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 6) == '\x7f';
        do {
          if (!bVar8) goto code_r0x08009e92;
          bVar8 = *(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 7) == -9;
        } while (!bVar8);
        if (((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 0xc) == ':') &&
            (*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 0xd) == -0x5d)) &&
           ((*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) ==
             *(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 9) &&
            (*(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 10) ==
             *(char *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 0xb))))) {
          *p_status_block =
               *p_status_block & 0xfe |
               *(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) & 1;
          *p_status_block =
               *p_status_block & 0xfd |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) >> 1 & 1) << 1;
          *p_status_block =
               *p_status_block & 0xfb |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) >> 2 & 1) << 2;
          *p_status_block =
               *p_status_block & 0xf7 |
               (*(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 8) >> 3 & 1) << 3;
          switch(*(undefined1 *)(p_can_rx_ring_buf + *p_can_rx_ring_tail * 0x10 + 10)) {
          case 0:
            p_status_block[2] = 0;
            break;
          case 1:
            p_status_block[2] = 2;
            break;
          case 2:
            p_status_block[2] = 1;
            break;
          case 3:
            p_status_block[2] = 9;
            break;
          case 4:
            p_status_block[2] = 8;
            break;
          case 5:
            p_status_block[2] = 0xb;
            break;
          case 6:
            p_status_block[2] = 3;
          }
          pbVar3 = p_prndl_tx_7fe;
          *p_prndl_tx_7fe = 0;
          if (*p_cmd_block == '\x01') {
            *pbVar3 = *pbVar3 | 1;
          }
          else {
            *p_prndl_tx_7fe = *p_prndl_tx_7fe & 0xfe;
          }
          if (*p_cmd_block == '\x02') {
            *p_prndl_tx_7fe = *p_prndl_tx_7fe | 2;
          }
          else {
            *p_prndl_tx_7fe = *p_prndl_tx_7fe & 0xfd;
          }
          if (*p_cmd_block == '\x03') {
            *p_prndl_tx_7fe = *p_prndl_tx_7fe | 4;
          }
          else {
            *p_prndl_tx_7fe = *p_prndl_tx_7fe & 0xfb;
          }
          if (*p_cmd_block == '\x04') {
            *p_prndl_tx_7fe = *p_prndl_tx_7fe | 4;
          }
          else {
            *p_prndl_tx_7fe = *p_prndl_tx_7fe & 0xfb;
          }
          can_tx_enqueue(0x7fe,0,1,p_prndl_tx_7fe);
        }
      }
code_r0x08009e92:
      *p_can_rx_ring_tail = (*p_can_rx_ring_tail + 1U) % 0x32;
    }
    bVar5 = bVar5 + 1;
  } while( true );
}


