// app subsystem: can  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= can_init @ 0x08002a90 =================

/* can_init � bxCAN bring-up: PB8/PB9 to CAN alt-func, prescaler = 2000/kbps, calls can_init_regs
   (CAN_Init) + can_filter_init (one accept-all mask filter, ID=0/mask=0 -> FIFO0). No hardware
   acceptance filtering; IDs filtered in software by the profile dispatchers. */

void can_init(int param_1,undefined1 param_2)

{
  undefined1 local_2c;
  undefined1 local_2b;
  undefined1 local_2a;
  undefined1 local_29;
  undefined2 local_28;
  undefined2 local_26;
  undefined2 local_24;
  undefined2 local_22;
  undefined2 local_20;
  undefined1 local_1e;
  undefined1 local_1d;
  undefined1 local_1c;
  undefined1 local_1b;
  undefined2 local_18;
  undefined1 local_16;
  undefined1 local_15;
  undefined1 local_14;
  undefined1 local_13;
  undefined1 local_12;
  undefined1 local_11;
  undefined1 local_10;
  undefined1 local_f;
  undefined1 local_e;
  undefined1 local_d;
  
  nvic_set_priority_group(0x300);
  local_2c = 0x36;
  local_2b = 3;
  local_2a = 0;
  local_29 = 1;
  nvic_init(&local_2c);
  rcc_apb2_enable(9,1);
  gpio_init_pin(REG_GPIOB,0x100);
  gpio_init_pin(REG_GPIOB,0x200);
  rcc_apb1_periph_clock_cmd(0x2000000);
  can_rcc_clock_enable(REG_CAN);
  can_struct_init_defaults(&local_18);
  local_12 = 0;
  local_11 = 0;
  local_10 = 0;
  local_f = 0;
  local_e = 0;
  local_d = 1;
  local_15 = 0;
  local_14 = 3;
  local_13 = 2;
  local_18 = (undefined2)(2000 / param_1);
  local_16 = param_2;
  can_init_regs(REG_CAN,&local_18);
  local_1e = 0;
  local_1d = 0;
  local_1c = 1;
  local_28 = 0;
  local_26 = 0;
  local_24 = 0;
  local_22 = 0;
  local_20 = 0;
  local_1b = 1;
  can_filter_init(&local_28);
  can_it_config(REG_CAN,2,1);
  return;
}


// ================= can_init_normal_mode @ 0x08002b76 =================

void can_init_normal_mode(undefined4 param_1)

{
  can_init(param_1,0);
  return;
}


// ================= can_init_silent_mode @ 0x08002b84 =================

void can_init_silent_mode(undefined4 param_1)

{
  can_init(param_1,2);
  return;
}


// ================= can_transmit_frame @ 0x08002b98 =================

bool can_transmit_frame(undefined2 *pFrame)

{
  int iVar1;
  uint uVar2;
  undefined4 txId;
  undefined4 local_1c;
  undefined1 ide;
  undefined1 local_17;
  byte dlc;
  undefined1 data [9];
  
  dlc = *(byte *)((int)pFrame + 5);
  local_17 = 0;
  if (*(char *)(pFrame + 2) == '\0') {
    txId = CONCAT22(*pFrame,pFrame[1]);
    ide = 0;
  }
  else {
    ide = 4;
    local_1c = CONCAT22(*pFrame,pFrame[1]);
  }
  for (uVar2 = 0; (uVar2 < dlc && (uVar2 < 8)); uVar2 = uVar2 + 1 & 0xff) {
    data[uVar2] = *(undefined1 *)((int)pFrame + uVar2 + 6);
  }
  iVar1 = can_transmit(REG_CAN,&txId);
  return iVar1 == 4;
}


// ================= can_rx0_irq_push_ring @ 0x08002c0a =================

void can_rx0_irq_push_ring(void)

{
  uint freeSlots;
  undefined1 rxMsg [8];
  char ideFlag;
  byte dlc;
  undefined1 data [9];
  
  if (*p_can_rx_ring_head < *p_can_rx_ring_tail) {
    freeSlots = *p_can_rx_ring_tail - *p_can_rx_ring_head;
  }
  else {
    freeSlots = (*p_can_rx_ring_tail + 0x32) - *p_can_rx_ring_head;
  }
  if (freeSlots < 2) {
    can_receive_fifo(REG_CAN,0,rxMsg);
    *p_can_err_flags = *p_can_err_flags | 1;
  }
  else {
    can_receive_fifo(REG_CAN,0,rxMsg);
    if (ideFlag == '\0') {
      *(short *)(p_can_rx_ring_buf + *p_can_rx_ring_head * 0x10 + 2) = (short)rxMsg._0_4_;
      *(short *)(p_can_rx_ring_buf + *p_can_rx_ring_head * 0x10) = SUB42(rxMsg._0_4_,2);
    }
    else {
      *(short *)(p_can_rx_ring_buf + *p_can_rx_ring_head * 0x10 + 2) = (short)rxMsg._4_4_;
      *(short *)(p_can_rx_ring_buf + *p_can_rx_ring_head * 0x10) = SUB42(rxMsg._4_4_,2);
    }
    *(byte *)(p_can_rx_ring_buf + *p_can_rx_ring_head * 0x10 + 5) = dlc;
    for (freeSlots = 0; (freeSlots < dlc && (freeSlots < 8)); freeSlots = freeSlots + 1 & 0xff) {
      *(undefined1 *)(p_can_rx_ring_buf + *p_can_rx_ring_head * 0x10 + 6 + freeSlots) =
           data[freeSlots];
    }
    *p_can_rx_ring_head = (*p_can_rx_ring_head + 1) % 0x32;
  }
  return;
}


// ================= can_rx0_msg_pending @ 0x08002d1a =================

bool can_rx0_msg_pending(void)

{
  int iVar1;
  
  iVar1 = can_sleep(REG_CAN);
  return iVar1 == 1;
}


// ================= can_rx0_fifo_overrun_DEAD @ 0x08002d2c =================

bool can_rx0_fifo_overrun_DEAD(void)

{
  int iVar1;
  
  iVar1 = can_wakeup(REG_CAN);
  return iVar1 == 1;
}


// ================= can_clock_disable @ 0x08002e18 =================

void can_clock_disable(void)

{
  rcc_apb1_periph_clock_cmd(0x800,0);
  return;
}


// ================= can_rcc_clock_enable @ 0x08003974 =================

void can_rcc_clock_enable(void)

{
  rcc_apb1_periph_reset(0x2000000);
  rcc_apb1_periph_reset(0x2000000,0);
  return;
}


// ================= can_init_regs @ 0x0800398c =================

undefined4 can_init_regs(uint *pCanRegs,ushort *pCanInit)

{
  undefined4 uVar1;
  int timeout;
  
  *pCanRegs = *pCanRegs & 0xfffffffd;
  *pCanRegs = *pCanRegs | 1;
  for (timeout = 0; ((pCanRegs[1] & 1) == 0 && (timeout != 0xffff)); timeout = timeout + 1) {
  }
  if ((pCanRegs[1] & 1) == 0) {
    uVar1 = 0;
  }
  else {
    if ((char)pCanInit[3] == '\x01') {
      *pCanRegs = *pCanRegs | 0x80;
    }
    else {
      *pCanRegs = *pCanRegs & 0xffffff7f;
    }
    if (*(char *)((int)pCanInit + 7) == '\x01') {
      *pCanRegs = *pCanRegs | 0x40;
    }
    else {
      *pCanRegs = *pCanRegs & 0xffffffbf;
    }
    if ((char)pCanInit[4] == '\x01') {
      *pCanRegs = *pCanRegs | 0x20;
    }
    else {
      *pCanRegs = *pCanRegs & 0xffffffdf;
    }
    if (*(char *)((int)pCanInit + 9) == '\x01') {
      *pCanRegs = *pCanRegs | 0x10;
    }
    else {
      *pCanRegs = *pCanRegs & 0xffffffef;
    }
    if ((char)pCanInit[5] == '\x01') {
      *pCanRegs = *pCanRegs | 8;
    }
    else {
      *pCanRegs = *pCanRegs & 0xfffffff7;
    }
    if (*(char *)((int)pCanInit + 0xb) == '\x01') {
      *pCanRegs = *pCanRegs | 4;
    }
    else {
      *pCanRegs = *pCanRegs & 0xfffffffb;
    }
    pCanRegs[7] = (uint)(byte)pCanInit[1] << 0x1e | (uint)*(byte *)((int)pCanInit + 3) << 0x18 |
                  (uint)(byte)pCanInit[2] << 0x10 | (uint)*(byte *)((int)pCanInit + 5) << 0x14 |
                  *pCanInit - 1;
    *pCanRegs = *pCanRegs & 0xfffffffe;
    for (timeout = 0; ((pCanRegs[1] & 1) != 0 && (timeout != 0xffff)); timeout = timeout + 1) {
    }
    if ((pCanRegs[1] & 1) == 0) {
      uVar1 = 1;
    }
    else {
      uVar1 = 0;
    }
  }
  return uVar1;
}


// ================= can_filter_init @ 0x08003aa0 =================

void can_filter_init(undefined2 *pFilterInit)

{
  uint filterBit;
  
  filterBit = 1 << *(sbyte *)(pFilterInit + 5);
  *REG_CAN_FILTER = *REG_CAN_FILTER | 1;
  REG_CAN_FILTER[7] = REG_CAN_FILTER[7] & ~filterBit;
  if (*(char *)(pFilterInit + 6) == '\0') {
    REG_CAN_FILTER[3] = REG_CAN_FILTER[3] & ~filterBit;
    REG_CAN_FILTER[(uint)*(byte *)(pFilterInit + 5) * 2 + 0x10] =
         CONCAT22(pFilterInit[3],pFilterInit[1]);
    REG_CAN_FILTER[(uint)*(byte *)(pFilterInit + 5) * 2 + 0x11] =
         CONCAT22(pFilterInit[2],*pFilterInit);
  }
  if (*(char *)(pFilterInit + 6) == '\x01') {
    REG_CAN_FILTER[3] = REG_CAN_FILTER[3] | filterBit;
    REG_CAN_FILTER[(uint)*(byte *)(pFilterInit + 5) * 2 + 0x10] =
         CONCAT22(*pFilterInit,pFilterInit[1]);
    REG_CAN_FILTER[(uint)*(byte *)(pFilterInit + 5) * 2 + 0x11] =
         CONCAT22(pFilterInit[2],pFilterInit[3]);
  }
  if (*(char *)((int)pFilterInit + 0xb) == '\0') {
    REG_CAN_FILTER[1] = REG_CAN_FILTER[1] & ~filterBit;
  }
  else {
    REG_CAN_FILTER[1] = REG_CAN_FILTER[1] | filterBit;
  }
  if (pFilterInit[4] == 0) {
    REG_CAN_FILTER[5] = REG_CAN_FILTER[5] & ~filterBit;
  }
  if (pFilterInit[4] == 1) {
    REG_CAN_FILTER[5] = REG_CAN_FILTER[5] | filterBit;
  }
  if (*(char *)((int)pFilterInit + 0xd) == '\x01') {
    REG_CAN_FILTER[7] = REG_CAN_FILTER[7] | filterBit;
  }
  *REG_CAN_FILTER = *REG_CAN_FILTER & 0xfffffffe;
  return;
}


// ================= can_struct_init_defaults @ 0x08003ba2 =================

void can_struct_init_defaults(undefined2 *param_1)

{
  *(undefined1 *)(param_1 + 3) = 0;
  *(undefined1 *)((int)param_1 + 7) = 0;
  *(undefined1 *)(param_1 + 4) = 0;
  *(undefined1 *)((int)param_1 + 9) = 0;
  *(undefined1 *)(param_1 + 5) = 0;
  *(undefined1 *)((int)param_1 + 0xb) = 0;
  *(undefined1 *)(param_1 + 1) = 0;
  *(undefined1 *)((int)param_1 + 3) = 0;
  *(undefined1 *)(param_1 + 2) = 3;
  *(undefined1 *)((int)param_1 + 5) = 2;
  *param_1 = 1;
  return;
}


// ================= can_transmit @ 0x08003c4e =================

void can_transmit(int pCanRegs,ushort *pTxMsg)

{
  int mailboxIdx;
  
  mailboxIdx = 0;
  if ((*(uint *)(pCanRegs + 8) & 0x4000000) != 0x4000000) {
    if ((*(uint *)(pCanRegs + 8) & 0x8000000) == 0x8000000) {
      mailboxIdx = 1;
    }
    else if ((*(uint *)(pCanRegs + 8) & 0x10000000) == 0x10000000) {
      mailboxIdx = 2;
    }
    else {
      mailboxIdx = 4;
    }
  }
  if (mailboxIdx != 4) {
    *(uint *)(pCanRegs + 0x180 + mailboxIdx * 0x10) =
         *(uint *)(pCanRegs + 0x180 + mailboxIdx * 0x10) & 1;
    if ((char)pTxMsg[4] == '\0') {
      *(uint *)(pCanRegs + 0x180 + mailboxIdx * 0x10) =
           (uint)*(byte *)((int)pTxMsg + 9) | (uint)*pTxMsg << 0x15 |
           *(uint *)(pCanRegs + 0x180 + mailboxIdx * 0x10);
    }
    else {
      *(uint *)(pCanRegs + 0x180 + mailboxIdx * 0x10) =
           (uint)(byte)pTxMsg[4] | *(int *)(pTxMsg + 2) << 3 | (uint)*(byte *)((int)pTxMsg + 9) |
           *(uint *)(pCanRegs + 0x180 + mailboxIdx * 0x10);
    }
    *(byte *)(pTxMsg + 5) = (byte)pTxMsg[5] & 0xf;
    *(uint *)(pCanRegs + mailboxIdx * 0x10 + 0x184) =
         *(uint *)(pCanRegs + mailboxIdx * 0x10 + 0x184) & 0xfffffff0;
    *(uint *)(pCanRegs + mailboxIdx * 0x10 + 0x184) =
         *(uint *)(pCanRegs + mailboxIdx * 0x10 + 0x184) | (uint)(byte)pTxMsg[5];
    *(uint *)(pCanRegs + mailboxIdx * 0x10 + 0x188) =
         (uint)(byte)pTxMsg[7] << 0x18 | (uint)*(byte *)((int)pTxMsg + 0xd) << 0x10 |
         (uint)(byte)pTxMsg[6] << 8 | (uint)*(byte *)((int)pTxMsg + 0xb);
    *(uint *)(pCanRegs + mailboxIdx * 0x10 + 0x18c) =
         (uint)(byte)pTxMsg[9] << 0x18 | (uint)*(byte *)((int)pTxMsg + 0x11) << 0x10 |
         (uint)(byte)pTxMsg[8] << 8 | (uint)*(byte *)((int)pTxMsg + 0xf);
    *(uint *)(pCanRegs + 0x180 + mailboxIdx * 0x10) =
         *(uint *)(pCanRegs + 0x180 + mailboxIdx * 0x10) | 1;
  }
  return;
}


// ================= can_get_transmit_status_DEAD @ 0x08003d74 =================

undefined1 can_get_transmit_status_DEAD(int param_1,int param_2)

{
  int iVar1;
  uint uVar2;
  
  if (param_2 == 0) {
    uVar2 = *(uint *)(param_1 + 8) & DAT_08003eac;
  }
  else if (param_2 == 1) {
    uVar2 = *(uint *)(param_1 + 8) & DAT_08003eb0;
  }
  else if (param_2 == 2) {
    uVar2 = *(uint *)(param_1 + 8) & DAT_08003eb4;
  }
  else {
    uVar2 = 0;
  }
  if (uVar2 == DAT_08003eb8) {
    return 0;
  }
  if ((int)uVar2 < (int)DAT_08003eb8) {
    if (uVar2 == 0) {
      return 2;
    }
    if (uVar2 == 0x4000001) {
      return 0;
    }
    if (uVar2 == 0x4000003) {
      return 1;
    }
  }
  else {
    if (uVar2 - DAT_08003eb8 == 0x200) {
      return 1;
    }
    iVar1 = (uVar2 - DAT_08003eb8) + DAT_08003ebc;
    if (iVar1 == 0) {
      return 0;
    }
    if (iVar1 == 0x20000) {
      return 1;
    }
  }
  return 0;
}


// ================= can_receive_fifo @ 0x08003e2e =================

void can_receive_fifo(int pCanRegs,int fifoNum,uint *pRxMsg)

{
  *(byte *)(pRxMsg + 2) = (byte)*(undefined4 *)(pCanRegs + 0x1b0 + fifoNum * 0x10) & 4;
  if ((char)pRxMsg[2] == '\0') {
    *pRxMsg = *(uint *)(pCanRegs + 0x1b0 + fifoNum * 0x10) >> 0x15;
  }
  else {
    pRxMsg[1] = *(uint *)(pCanRegs + 0x1b0 + fifoNum * 0x10) >> 3;
  }
  *(byte *)((int)pRxMsg + 9) = (byte)*(undefined4 *)(pCanRegs + 0x1b0 + fifoNum * 0x10) & 2;
  *(byte *)((int)pRxMsg + 10) = (byte)*(undefined4 *)(pCanRegs + fifoNum * 0x10 + 0x1b4) & 0xf;
  *(char *)((int)pRxMsg + 0x13) =
       (char)((uint)*(undefined4 *)(pCanRegs + fifoNum * 0x10 + 0x1b4) >> 8);
  *(char *)((int)pRxMsg + 0xb) = (char)*(undefined4 *)(pCanRegs + fifoNum * 0x10 + 0x1b8);
  *(char *)(pRxMsg + 3) = (char)((uint)*(undefined4 *)(pCanRegs + fifoNum * 0x10 + 0x1b8) >> 8);
  *(char *)((int)pRxMsg + 0xd) =
       (char)((uint)*(undefined4 *)(pCanRegs + fifoNum * 0x10 + 0x1b8) >> 0x10);
  *(char *)((int)pRxMsg + 0xe) =
       (char)((uint)*(undefined4 *)(pCanRegs + fifoNum * 0x10 + 0x1b8) >> 0x18);
  *(char *)((int)pRxMsg + 0xf) = (char)*(undefined4 *)(pCanRegs + fifoNum * 0x10 + 0x1bc);
  *(char *)(pRxMsg + 4) = (char)((uint)*(undefined4 *)(pCanRegs + fifoNum * 0x10 + 0x1bc) >> 8);
  *(char *)((int)pRxMsg + 0x11) =
       (char)((uint)*(undefined4 *)(pCanRegs + fifoNum * 0x10 + 0x1bc) >> 0x10);
  *(char *)((int)pRxMsg + 0x12) =
       (char)((uint)*(undefined4 *)(pCanRegs + fifoNum * 0x10 + 0x1bc) >> 0x18);
  if (fifoNum == 0) {
    *(uint *)(pCanRegs + 0xc) = *(uint *)(pCanRegs + 0xc) | 0x20;
  }
  else {
    *(uint *)(pCanRegs + 0x10) = *(uint *)(pCanRegs + 0x10) | 0x20;
  }
  return;
}


// ================= can_set_operating_mode_DEAD @ 0x08003f6c =================

undefined4 can_set_operating_mode_DEAD(uint *param_1,int param_2)

{
  undefined4 uVar1;
  int iVar2;
  
  iVar2 = 0xffff;
  if (param_2 == 0) {
    *param_1 = *param_1 & 0xfffffffd | 1;
    for (; ((param_1[1] & 3) != 1 && (iVar2 != 0)); iVar2 = iVar2 + -1) {
    }
    if ((param_1[1] & 3) == 1) {
      uVar1 = 1;
    }
    else {
      uVar1 = 0;
    }
  }
  else if (param_2 == 1) {
    *param_1 = *param_1 & 0xfffffffc;
    for (; ((param_1[1] & 3) != 0 && (iVar2 != 0)); iVar2 = iVar2 + -1) {
    }
    if ((param_1[1] & 3) == 0) {
      uVar1 = 1;
    }
    else {
      uVar1 = 0;
    }
  }
  else if (param_2 == 2) {
    *param_1 = *param_1 & 0xfffffffe | 2;
    for (; ((param_1[1] & 3) != 2 && (iVar2 != 0)); iVar2 = iVar2 + -1) {
    }
    if ((param_1[1] & 3) == 2) {
      uVar1 = 1;
    }
    else {
      uVar1 = 0;
    }
  }
  else {
    uVar1 = 0;
  }
  return uVar1;
}


// ================= can_sleep @ 0x0800400e =================

bool can_sleep(uint *param_1)

{
  *param_1 = *param_1 & 0xfffffffe | 2;
  return (param_1[1] & 3) == 2;
}


// ================= can_wakeup @ 0x0800402c =================

bool can_wakeup(uint *param_1)

{
  int iVar1;
  
  *param_1 = *param_1 & 0xfffffffd;
  for (iVar1 = 0xffff; ((param_1[1] & 2) == 2 && (iVar1 != 0)); iVar1 = iVar1 + -1) {
  }
  return (param_1[1] & 2) != 2;
}


// ================= can_it_config @ 0x0800407e =================

void can_it_config(int param_1,uint param_2,int param_3)

{
  if (param_3 == 0) {
    *(uint *)(param_1 + 0x14) = *(uint *)(param_1 + 0x14) & ~param_2;
  }
  else {
    *(uint *)(param_1 + 0x14) = *(uint *)(param_1 + 0x14) | param_2;
  }
  return;
}


// ================= can_get_flag_status_DEAD @ 0x08004090 =================

undefined4 can_get_flag_status_DEAD(int param_1,uint param_2)

{
  undefined4 uVar1;
  
  if ((param_2 & 0xf00000) == 0) {
    if ((param_2 & 0x1000000) == 0) {
      if ((param_2 & 0x8000000) == 0) {
        if ((param_2 & 0x2000000) == 0) {
          if ((*(uint *)(param_1 + 0x10) & param_2 & 0xfffff) == 0) {
            uVar1 = 0;
          }
          else {
            uVar1 = 1;
          }
        }
        else if ((*(uint *)(param_1 + 0xc) & param_2 & 0xfffff) == 0) {
          uVar1 = 0;
        }
        else {
          uVar1 = 1;
        }
      }
      else if ((*(uint *)(param_1 + 8) & param_2 & 0xfffff) == 0) {
        uVar1 = 0;
      }
      else {
        uVar1 = 1;
      }
    }
    else if ((*(uint *)(param_1 + 4) & param_2 & 0xfffff) == 0) {
      uVar1 = 0;
    }
    else {
      uVar1 = 1;
    }
  }
  else if ((*(uint *)(param_1 + 0x18) & param_2 & 0xfffff) == 0) {
    uVar1 = 0;
  }
  else {
    uVar1 = 1;
  }
  return uVar1;
}


// ================= can_get_it_status_DEAD @ 0x0800414e =================

undefined4 can_get_it_status_DEAD(int param_1,uint param_2)

{
  undefined4 uVar1;
  
  if ((*(uint *)(param_1 + 0x14) & param_2) == 0) {
    uVar1 = 0;
  }
  else if (param_2 == 0x100) {
    uVar1 = util_bit_test(*(undefined4 *)(param_1 + 0x18),1);
  }
  else {
    if ((int)param_2 < 0x101) {
      if (param_2 == 8) {
        uVar1 = util_bit_test(*(undefined4 *)(param_1 + 0xc),0x10);
        return uVar1;
      }
      if ((int)param_2 < 9) {
        if (param_2 == 1) {
          uVar1 = util_bit_test(*(undefined4 *)(param_1 + 8),DAT_08004310);
          return uVar1;
        }
        if (param_2 == 2) {
          uVar1 = util_bit_test(*(undefined4 *)(param_1 + 0xc),3);
          return uVar1;
        }
        if (param_2 == 4) {
          uVar1 = util_bit_test(*(undefined4 *)(param_1 + 0xc),8);
          return uVar1;
        }
      }
      else {
        if (param_2 == 0x10) {
          uVar1 = util_bit_test(*(undefined4 *)(param_1 + 0x10),3);
          return uVar1;
        }
        if (param_2 == 0x20) {
          uVar1 = util_bit_test(*(undefined4 *)(param_1 + 0x10),8);
          return uVar1;
        }
        if (param_2 == 0x40) {
          uVar1 = util_bit_test(*(undefined4 *)(param_1 + 0x10),0x10);
          return uVar1;
        }
      }
    }
    else {
      if (param_2 == 0x8000) {
        uVar1 = util_bit_test(*(undefined4 *)(param_1 + 4),4);
        return uVar1;
      }
      if ((int)param_2 < 0x8001) {
        if (param_2 == 0x200) {
          uVar1 = util_bit_test(*(undefined4 *)(param_1 + 0x18),2);
          return uVar1;
        }
        if (param_2 == 0x400) {
          uVar1 = util_bit_test(*(undefined4 *)(param_1 + 0x18),4);
          return uVar1;
        }
        if (param_2 == 0x800) {
          uVar1 = util_bit_test(*(undefined4 *)(param_1 + 0x18),0x70);
          return uVar1;
        }
      }
      else {
        if (param_2 == 0x10000) {
          uVar1 = util_bit_test(*(undefined4 *)(param_1 + 4),8);
          return uVar1;
        }
        if (param_2 == 0x20000) {
          uVar1 = util_bit_test(*(undefined4 *)(param_1 + 4),0x10);
          return uVar1;
        }
      }
    }
    uVar1 = 0;
  }
  return uVar1;
}


// ================= can_reinit_at_bitrate_B @ 0x0800744e =================

void can_reinit_at_bitrate_B(undefined4 param_1)

{
  can_init_normal_mode(param_1);
  gpio_init_pin(REG_GPIOB,0x1000);
  gpio_init_pin(REG_GPIOB,0x1000);
  gpio_can_xcvr_standby_pb12();
  *p_can_reinit_bitrate = param_1;
  *p_can_bus_status = 1;
  *p_profile0_activity_ctr = 200;
  return;
}


// ================= can_reinit_at_bitrate_A @ 0x08007482 =================

void can_reinit_at_bitrate_A(undefined4 param_1)

{
  can_init_normal_mode(param_1);
  gpio_init_pin(REG_GPIOB,0x1000);
  gpio_init_pin(REG_GPIOB,0x1000);
  gpio_can_xcvr_standby_pb12();
  *p_can_reinit_bitrate = param_1;
  *p_can_bus_status = 1;
  *p_profile0_activity_ctr = 200;
  return;
}


// ================= can_bus_active_flag @ 0x080074b6 =================

byte can_bus_active_flag(void)

{
  return *p_status_block & 1;
}


// ================= can_tx_enqueue @ 0x080074c0 =================

/* can_tx_enqueue — queues a CAN frame into a bxCAN transmit mailbox. */

void can_tx_enqueue(undefined4 canId,undefined1 ideFlags,undefined1 dlc,undefined1 *pData)

{
  uint i;
  uint nextHead;
  
  nextHead = (*p_can_tx_ring_head + 1) % 0x32;
  if (nextHead == *p_can_tx_ring_tail) {
    *p_can_err_flags = *p_can_err_flags | 4;
  }
  else {
    *(short *)(p_can_tx_ring_buf + *p_can_tx_ring_head * 0x10) = (short)((uint)canId >> 0x10);
    *(short *)(p_can_tx_ring_buf + *p_can_tx_ring_head * 0x10 + 2) = (short)canId;
    *(undefined1 *)(p_can_tx_ring_buf + *p_can_tx_ring_head * 0x10 + 5) = dlc;
    *(undefined1 *)(p_can_tx_ring_buf + *p_can_tx_ring_head * 0x10 + 4) = ideFlags;
    for (i = 0; (i < *(byte *)(p_can_tx_ring_buf + *p_can_tx_ring_head * 0x10 + 5) && (i < 8));
        i = i + 1 & 0xff) {
      *(undefined1 *)(p_can_tx_ring_buf + *p_can_tx_ring_head * 0x10 + 6 + i) = *pData;
      pData = pData + 1;
    }
    *p_can_tx_ring_head = nextHead;
  }
  return;
}


// ================= can_autobaud_state_machine @ 0x0800755c =================

/* can_autobaud_state_machine — boot autobaud: cycles 83.333k / 125k until >=3 valid frames,
   persists the winner to the param page (factory default profile 1). */

void can_autobaud_state_machine(void)

{
  undefined1 *puVar1;
  int linkUp;
  
  can_profile_dispatch_gate();
  if (*p_swc_resp_mode == '\x01') {
    *p_swc_resp_mode = '\0';
    can_tx_enqueue(0x416,0,8,p_swc_tx_buf);
    *p_swc_flag_118b = '(';
  }
  if (*p_swc_flag_118b == '\x01') {
    *p_swc_flag_118b = '\0';
    softtimer_clear_current();
    softtimer_scan_active_hi();
    puVar1 = p_swc_tx_buf;
    p_swc_tx_buf[1] = *p_swc_keydata_ref;
    can_tx_enqueue(0x416,0,8,puVar1);
  }
  can_periodic_tx_scheduler();
  if ((int)((uint)*p_status_block << 0x1d) < 0) {
    *p_canerr_hold_timer = '\x05';
  }
  else if (*p_canerr_hold_timer == '\0') {
    *p_status_block = *p_status_block & 0xdf;
  }
  puVar1 = p_swc_tx_buf;
  switch(*p_can_bus_status) {
  case 0:
    linkUp = can_bus_active_flag();
    if (linkUp == 0) {
      exti8_bus_wake_setup();
      reinit_can_filters_and_state();
      wwdg_force_reset_thunk();
    }
    else {
      *p_can_bus_status = 1;
      *p_profile0_activity_ctr = '\n';
    }
    break;
  case 1:
  case 4:
    linkUp = can_bus_active_flag();
    if (linkUp == 1) {
      *p_can_bus_status = 2;
      p_swc_tx_buf[2] = 0x3f;
    }
    else if (*p_profile0_activity_ctr == '\0') {
      *p_can_bus_status = 0;
    }
    break;
  case 2:
    linkUp = can_bus_active_flag();
    if (linkUp == 0) {
      *p_can_bus_status = 4;
      p_swc_tx_buf[2] = 0xbf;
      *p_profile0_activity_ctr = 'd';
    }
    else if (*p_profile0_activity_ctr == '\0') {
      reinit_can_filters_and_state();
      wwdg_force_reset_thunk();
    }
    break;
  case 3:
    *p_swc_tx_buf = 0xfe;
    puVar1[1] = 0x16;
    puVar1[2] = 0x3f;
    puVar1[3] = 0xff;
    puVar1[4] = 0xff;
    puVar1[5] = 0xff;
    puVar1[6] = 0xff;
    puVar1[7] = 0xff;
    can_tx_enqueue(0x416,0,8,puVar1);
    *p_can_bus_status = 2;
    break;
  default:
    *p_can_bus_status = 1;
  }
  return;
}


// ================= can_tx_ring_drain @ 0x08007698 =================

void can_tx_ring_drain(void)

{
  byte drainCount;
  int iVar1;
  
  drainCount = 0;
  while( true ) {
    if (9 < drainCount) {
      return;
    }
    if (*p_can_tx_ring_tail == *p_can_tx_ring_head) break;
    iVar1 = can_transmit_frame(p_can_tx_ring_buf + *p_can_tx_ring_tail * 0x10);
    if (iVar1 != 0) {
      *p_can_err_flags = 2;
      if (*(ushort *)(p_can_tx_busy_block + 2) < 100) {
        *(short *)(p_can_tx_busy_block + 2) = *(short *)(p_can_tx_busy_block + 2) + 1;
      }
      else {
        *(undefined2 *)(p_can_tx_busy_block + 2) = 0;
      }
      return;
    }
    *p_can_tx_ring_tail = (*p_can_tx_ring_tail + 1) % 0x32;
    drainCount = drainCount + 1;
  }
  return;
}


// ================= can_autobaud_timers_tick @ 0x080077e8 =================

void can_autobaud_timers_tick(void)

{
  if (*p_profile0_activity_ctr != '\0') {
    *p_profile0_activity_ctr = *p_profile0_activity_ctr + -1;
  }
  if (*p_canerr_hold_timer != '\0') {
    *p_canerr_hold_timer = *p_canerr_hold_timer + -1;
  }
  return;
}


// ================= can_set_baud_by_profile @ 0x08007b2c =================

/* can_set_baud_by_profile — sets CAN bit timing: profile 0 -> 0x53 (83.333 kbit), 1 -> 0x7D (125
   kbit). Baud is locked to the decode profile. */

void can_set_baud_by_profile(void)

{
  if (*(char *)(p_param_struct + 1) == '\x01') {
    can_reinit_at_bitrate_A(0x7d);
  }
  else {
    *(undefined1 *)(p_param_struct + 1) = 0;
    can_reinit_at_bitrate_A(0x53);
  }
  return;
}


// ================= can_apply_bitrate_B @ 0x08007b4c =================

void can_apply_bitrate_B(void)

{
  if (*(char *)(p_param_struct + 1) == '\x01') {
    can_reinit_at_bitrate_B(0x7d);
  }
  else {
    *(undefined1 *)(p_param_struct + 1) = 0;
    can_reinit_at_bitrate_B(0x53);
  }
  return;
}


// ================= can_config_load_defaults @ 0x08007b6c =================

void can_config_load_defaults(void)

{
  char *pcVar1;
  undefined1 *puVar2;
  uint uVar3;
  char cVar4;
  
  pcVar1 = p_param_struct;
  if (*p_param_struct != -0x56) {
    *p_param_struct = -0x56;
    pcVar1[1] = '\x01';
    pcVar1[0x10] = '\0';
    builtin_strncpy(p_param_struct + 0x11,"\n\n\n\n\n",6);
  }
  if (1 < (byte)p_param_struct[1]) {
    p_param_struct[1] = '\x01';
  }
  *p_profile_active = p_param_struct[1];
  puVar2 = p_swc_tx_buf;
  *p_swc_tx_buf = 0xfe;
  puVar2[1] = 0x16;
  puVar2[2] = 0x3f;
  puVar2[3] = 0xff;
  puVar2[4] = 0xff;
  puVar2[5] = 0xff;
  puVar2[6] = 0xff;
  puVar2[7] = 0xff;
  for (uVar3 = 1; uVar3 < 6; uVar3 = uVar3 + 1 & 0xff) {
    cVar4 = p_param_struct[uVar3 + 0x10];
    *(char *)(p_tx_frame_amp + uVar3) = cVar4;
    p_vstate_mid[uVar3] = cVar4;
  }
  *p_vstate_mid = p_param_struct[0x10];
  p_vstate_mid[0x16] = *(char *)(p_tx_frame_amp + 1);
  p_vstate_mid[0x17] = *(char *)(p_tx_frame_amp + 2);
  p_vstate_mid[0x18] = *(char *)(p_tx_frame_amp + 3);
  p_vstate_mid[0x19] = *(char *)(p_tx_frame_amp + 4);
  p_vstate_mid[0x1a] = *(char *)(p_tx_frame_amp + 5);
  if (*p_profile_active == '\x01') {
    cVar4 = '\x1e';
  }
  else {
    cVar4 = '\x14';
  }
  p_vstate_mid[0x15] = cVar4;
  puVar2 = p_tx_frame_f0;
  *p_tx_frame_f0 = 0;
  puVar2[1] = 0;
  puVar2[2] = 0;
  puVar2[3] = 0;
  puVar2 = p_tx_frame_326;
  if (*p_profile_active == '\x01') {
    *p_tx_frame_326 = 0;
    puVar2[1] = 0;
    puVar2[2] = 0;
    puVar2[3] = 0;
    puVar2[4] = 0;
    puVar2[5] = 0;
    puVar2[6] = 0;
  }
  else {
    *p_tx_frame_326 = 0;
    puVar2[1] = 7;
    puVar2[2] = 0xf;
    puVar2[3] = 7;
    puVar2[4] = 0;
    puVar2[5] = 0;
    puVar2[6] = 0;
  }
  puVar2 = p_tx_frame_190;
  *p_tx_frame_190 = 1;
  puVar2[1] = 0xd;
  puVar2[2] = 0x65;
  puVar2[3] = 0;
  puVar2[4] = 0x10;
  puVar2[5] = 0;
  puVar2[6] = 7;
  puVar2 = p_tx_frame_18c;
  *p_tx_frame_18c = 0x50;
  puVar2[1] = 0;
  puVar2[2] = 0;
  puVar2[3] = 0x20;
  puVar2[4] = 0;
  puVar2[5] = 0;
  puVar2[6] = 0;
  puVar2[7] = 0;
  puVar2 = p_tx_frame_9f;
  *p_tx_frame_9f = 1;
  puVar2[1] = 0x23;
  puVar2[2] = 0x6b;
  puVar2[3] = 0;
  puVar2[4] = 0;
  puVar2[5] = 0;
  puVar2[6] = 0;
  puVar2[7] = 0x1f;
  return;
}


// ================= can_periodic_tx_scheduler @ 0x08008110 =================

/* can_periodic_tx_scheduler � free-running table that periodically enqueues CAN frames to the
   vehicle (e.g. 0x3d0, 0xf0, 0x326). Table-driven, not poll-driven. (The head-unit UART periodic
   round-robin is separate: uart_tx_sched.) */

void can_periodic_tx_scheduler(void)

{
  byte bVar1;
  int iVar2;
  undefined1 uVar3;
  
  if ((p_status_block[0x12] & 1) != (*p_status_block & 1)) {
    if ((*p_status_block & 1) == 0) {
      uVar3 = 7;
    }
    else {
      uVar3 = 1;
    }
    *p_tx_frame_9f = uVar3;
    p_status_block[0x12] = *p_status_block & 1;
  }
  if ((*p_status_block & 1) == 0) {
    if (*p_can_tx_hold_timer != 0) {
      *p_can_tx_hold_timer = *p_can_tx_hold_timer + -1;
    }
  }
  else {
    *p_can_tx_hold_timer = 400;
  }
  if (*p_can_tx_hold_timer != 0) {
    *p_status_block = *p_status_block & 0x7f | *p_profile_active << 7;
    if (*p_periodic_tx_phase < 0x96) {
      *p_periodic_tx_phase = *p_periodic_tx_phase + 1;
    }
    else {
      *p_periodic_tx_phase = 0;
    }
    if (*p_profile_active == '\0') {
      switch(*p_periodic_tx_phase) {
      case 1:
        can_tx_enqueue(0x3d0,0,7,p_tx_frame_amp);
        break;
      case 3:
        can_tx_enqueue(0xf0,0,4,p_tx_frame_f0);
        break;
      case 5:
        can_tx_enqueue(0x326,0,8,p_tx_frame_326);
        break;
      case 7:
        can_tx_enqueue(400,0,7,p_tx_frame_190);
        break;
      case 9:
        can_tx_enqueue(0x18c,0,8,p_tx_frame_18c);
        break;
      case 0xb:
        can_tx_enqueue(0x9f,0,8,p_tx_frame_9f);
        break;
      case 0xd:
      }
    }
    else if (*p_profile_active == '\x01') {
      switch(*p_periodic_tx_phase) {
      case 1:
        can_tx_enqueue(0x3d9,0,7,p_tx_frame_amp);
        break;
      case 3:
        can_tx_enqueue(0x2e9,0,4,p_tx_frame_f0);
        break;
      case 5:
        can_tx_enqueue(0x290,0,8,p_tx_frame_326);
        break;
      case 7:
        can_tx_enqueue(0x291,0,7,p_tx_frame_190);
        break;
      case 9:
        can_tx_enqueue(0x293,0,8,p_tx_frame_18c);
        break;
      case 0xb:
        break;
      case 0xd:
      }
    }
    if (*(char *)(p_cmd_block + 0xd) == '\x01') {
      *(undefined1 *)(p_cmd_block + 0xd) = 0;
      uart_D2_cluster_text_to_can();
    }
    iVar2 = p_cmd_block;
    if (*(char *)(p_cmd_block + 0x10) == '\x01') {
      *(undefined1 *)(p_cmd_block + 0x10) = 0;
      if (*(char *)(iVar2 + 0xe) == '\x01') {
        if ((*(char *)(p_cmd_block + 0xe) == '\x01') && (0x26 < *(byte *)(p_cmd_block + 0xf))) {
          *(undefined1 *)(p_cmd_block + 0xf) = 0x26;
        }
      }
      else if (*(byte *)(p_cmd_block + 0xf) < 0x14) {
        if (*(char *)(p_cmd_block + 0xf) == '\0') {
          *(undefined1 *)(p_cmd_block + 0xf) = 1;
        }
      }
      else {
        *(undefined1 *)(p_cmd_block + 0xf) = 0x13;
      }
      switch(*(undefined1 *)(p_cmd_block + 0xe)) {
      case 1:
        bVar1 = *(byte *)(p_cmd_block + 0xf);
        *p_tx_frame_amp = bVar1;
        p_status_block[0x2c] = bVar1;
        *(byte *)(p_param_struct + 0x10) = bVar1;
        break;
      case 2:
        bVar1 = *(byte *)(p_cmd_block + 0xf);
        p_tx_frame_amp[1] = bVar1;
        p_status_block[0x2d] = bVar1;
        *(byte *)(p_param_struct + 0x11) = bVar1;
        break;
      case 3:
        bVar1 = *(byte *)(p_cmd_block + 0xf);
        p_tx_frame_amp[2] = bVar1;
        p_status_block[0x2e] = bVar1;
        *(byte *)(p_param_struct + 0x12) = bVar1;
        break;
      case 4:
        bVar1 = *(byte *)(p_cmd_block + 0xf);
        p_tx_frame_amp[3] = bVar1;
        p_status_block[0x2f] = bVar1;
        *(byte *)(p_param_struct + 0x13) = bVar1;
        break;
      case 5:
        bVar1 = *(byte *)(p_cmd_block + 0xf);
        p_tx_frame_amp[4] = bVar1;
        p_status_block[0x30] = bVar1;
        *(byte *)(p_param_struct + 0x14) = bVar1;
        break;
      case 6:
        uVar3 = *(undefined1 *)(p_cmd_block + 0xf);
        *(undefined1 *)(p_tx_frame_amp + 5) = uVar3;
        *(undefined1 *)(p_status_block + 0x31) = uVar3;
        *(undefined1 *)(p_param_struct + 0x15) = uVar3;
      }
      if (*p_profile_active == '\x01') {
        can_tx_enqueue(0x3d9,0,7,p_tx_frame_amp);
      }
      else {
        can_tx_enqueue(0x3d0,0,7,p_tx_frame_amp);
      }
    }
  }
  return;
}


