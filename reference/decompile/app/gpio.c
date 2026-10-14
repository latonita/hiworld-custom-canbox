// app subsystem: gpio  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= gpio_init_pin @ 0x08002494 =================

undefined8 gpio_init_pin(int port,uint pinMask,undefined4 mode,undefined4 param_4)

{
  undefined2 cfg;
  undefined1 uStack_1e;
  undefined1 local_1d;
  uint local_1c;
  undefined4 local_18;
  undefined4 afMode;
  
  cfg = (undefined2)port;
  uStack_1e = (undefined1)((uint)port >> 0x10);
  local_1d = (undefined1)((uint)port >> 0x18);
  local_1c = pinMask;
  local_18 = mode;
  afMode = param_4;
  rcc_apb2_enable(0x3d,1);
  cfg = (undefined2)pinMask;
  local_1c = local_1c & 0xffffff00;
  local_1d = 0;
  if ((port == REG_GPIOA) && (pinMask == 4)) {
    afMode = 4;
  }
  else if ((port == REG_GPIOA) && (pinMask == 8)) {
    afMode = 4;
  }
  else if ((port == REG_GPIOB) && (pinMask == 0x400)) {
    afMode = 0;
  }
  else if ((port == REG_GPIOB) && (pinMask == 0x800)) {
    afMode = 5;
  }
  else if ((port == REG_GPIOB) && (pinMask == 0x100)) {
    afMode = 5;
  }
  else if ((port == REG_GPIOB) && (pinMask == 0x200)) {
    afMode = 5;
  }
  else {
    afMode = 0xf;
  }
  local_18 = mode;
  gpio_apply_pin_config(port,&cfg);
  return CONCAT44(local_1c,CONCAT13(local_1d,CONCAT12(uStack_1e,cfg)));
}


// ================= gpio_write_pin @ 0x0800252a =================

void gpio_write_pin(undefined4 param_1,undefined4 param_2,int param_3)

{
  if (param_3 == 0) {
    gpio_clear_bit(param_1,param_2);
  }
  else {
    gpio_set_bit(param_1,param_2);
  }
  return;
}


// ================= gpio_read_pin @ 0x08002548 =================

void gpio_read_pin(undefined4 param_1,undefined4 param_2)

{
  util_test_reg_bits_DEAD(param_1,param_2);
  return;
}


// ================= gpio_deinit_DEAD @ 0x08005008 =================

void gpio_deinit_DEAD(uint *port)

{
  uint *puVar1;
  int portSel;
  uint pinIdx;
  uint uVar2;
  
  pinIdx = 0;
  if (port == REG_GPIOA) {
    rcc_apb2_periph_reset(4,1);
    rcc_apb2_periph_reset(4,0);
  }
  else if (port == REG_GPIOB) {
    rcc_apb2_periph_reset(8,1);
    rcc_apb2_periph_reset(8,0);
  }
  else if (port == REG_GPIOC) {
    rcc_apb2_periph_reset(0x10,1);
    rcc_apb2_periph_reset(0x10,0);
  }
  else {
    if (port != REG_GPIOD) {
      return;
    }
    rcc_apb2_periph_reset(0x20,1);
    rcc_apb2_periph_reset(0x20,0);
  }
  for (; puVar1 = REG_EXTI, 0xffffU >> (pinIdx & 0xff) != 0; pinIdx = pinIdx + 1) {
    uVar2 = 1 << (pinIdx & 0xff) & 0xffff;
    if (uVar2 != 0) {
      if (port == REG_GPIOA) {
        portSel = 0;
      }
      else if (port == REG_GPIOB) {
        portSel = 1;
      }
      else if (port == REG_GPIOC) {
        portSel = 2;
      }
      else {
        portSel = 3;
      }
      if (portSel << ((pinIdx & 3) << 2) ==
          (*(uint *)(DAT_08005424 + (pinIdx & 0xfffffffc)) & 0xf << ((pinIdx & 3) << 2))) {
        *REG_EXTI = *REG_EXTI & ~uVar2;
        puVar1[1] = puVar1[1] & ~uVar2;
        puVar1[2] = puVar1[2] & ~uVar2;
        puVar1[3] = puVar1[3] & ~uVar2;
        *(uint *)(DAT_08005424 + (pinIdx & 0xfffffffc)) =
             *(uint *)(DAT_08005424 + (pinIdx & 0xfffffffc)) & ~(0xf << ((pinIdx & 3) << 2));
      }
      *port = *port & ~(3 << ((pinIdx & 0x7f) << 1));
      if ((pinIdx & 8) == 0) {
        port[8] = port[8] | 0xf << ((pinIdx & 7) << 2);
      }
      else {
        port[9] = port[9] | 0xf << ((pinIdx & 7) << 2);
      }
      port[1] = port[1] & ~(1 << (pinIdx & 0xff));
      port[3] = port[3] & ~(3 << ((pinIdx & 0x7f) << 1));
    }
  }
  return;
}


// ================= gpio_apply_pin_config @ 0x08005172 =================

uint gpio_apply_pin_config(uint *port,ushort *pCfg)

{
  uint *puVar1;
  uint *puVar2;
  uint bitPos;
  uint pinIdx;
  
  bitPos = 0;
  for (pinIdx = 0; (int)(uint)*pCfg >> (pinIdx & 0xff) != 0; pinIdx = pinIdx + 1) {
    if (((uint)*pCfg & 1 << (pinIdx & 0xff)) != 0) {
      bitPos = pinIdx << 1;
      if ((((*(int *)(pCfg + 4) == 2) || (*(int *)(pCfg + 4) == 0x12)) || (*(int *)(pCfg + 4) == 0))
         || (*(int *)(pCfg + 4) == 3)) {
        if ((pinIdx & 8) == 0) {
          port[8] = port[8] & ~(0xf << ((pinIdx & 7) << 2)) |
                    *(int *)(pCfg + 6) << ((pinIdx & 7) << 2);
        }
        else {
          port[9] = port[9] & ~(0xf << ((pinIdx & 7) << 2)) |
                    *(int *)(pCfg + 6) << ((pinIdx & 7) << 2);
        }
      }
      *port = *port & ~(3 << (bitPos & 0xff)) | ((byte)pCfg[4] & 0xf) << (bitPos & 0xff);
      port[3] = port[3] & ~(3 << (bitPos & 0xff)) | ((byte)pCfg[2] & 3) << (bitPos & 0xff);
      if (((pCfg[4] & 3) != 0) && (*(int *)(pCfg + 4) != 3)) {
        port[0xb] = port[0xb] & ~(3 << (bitPos & 0xff)) | ((byte)pCfg[1] & 3) << (bitPos & 0xff);
      }
      bitPos = port[2] & ~(1 << (pinIdx & 0xff)) | (*(byte *)((int)pCfg + 3) & 1) << (pinIdx & 0xff)
      ;
      port[2] = bitPos;
      if ((char)pCfg[2] == '\x02') {
        port[10] = port[10] | 1 << (pinIdx & 0xff);
      }
      else if ((char)pCfg[2] == '\x01') {
        port[6] = port[6] | 1 << (pinIdx & 0xff);
      }
      if (((*(int *)(pCfg + 4) == 1) || (*(int *)(pCfg + 4) == 2)) ||
         ((*(int *)(pCfg + 4) == 0x11 || (*(int *)(pCfg + 4) == 0x12)))) {
        bitPos = port[1] & ~(1 << (pinIdx & 0xff)) |
                 (((byte)pCfg[4] & 0x1f) >> 4) << (pinIdx & 0xff);
        port[1] = bitPos;
      }
      puVar1 = REG_EXTI;
      if ((*(uint *)(pCfg + 4) & 0x10000000) != 0) {
        bitPos = *REG_EXTI & ~(1 << (pinIdx & 0xff));
        if ((*(uint *)(pCfg + 4) & 0x10000) == 0x10000) {
          bitPos = bitPos | 1 << (pinIdx & 0xff);
        }
        *REG_EXTI = bitPos;
        puVar2 = REG_EXTI;
        bitPos = puVar1[1] & ~(1 << (pinIdx & 0xff));
        if ((*(uint *)(pCfg + 4) & 0x20000) == 0x20000) {
          bitPos = bitPos | 1 << (pinIdx & 0xff);
        }
        REG_EXTI[1] = bitPos;
        puVar1 = REG_EXTI;
        bitPos = puVar2[2] & ~(1 << (pinIdx & 0xff));
        if ((*(uint *)(pCfg + 4) & 0x100000) == 0x100000) {
          bitPos = bitPos | 1 << (pinIdx & 0xff);
        }
        REG_EXTI[2] = bitPos;
        bitPos = puVar1[3] & ~(1 << (pinIdx & 0xff));
        if ((*(uint *)(pCfg + 4) & 0x200000) == 0x200000) {
          bitPos = bitPos | 1 << (pinIdx & 0xff);
        }
        REG_EXTI[3] = bitPos;
      }
    }
  }
  return bitPos;
}


// ================= gpio_set_bit @ 0x080053a2 =================

void gpio_set_bit(int param_1,undefined4 param_2)

{
  *(undefined4 *)(param_1 + 0x18) = param_2;
  return;
}


// ================= gpio_clear_bit @ 0x080053aa =================

void gpio_clear_bit(int param_1,undefined4 param_2)

{
  *(undefined4 *)(param_1 + 0x28) = param_2;
  return;
}


// ================= gpio_set_af @ 0x080053ce =================

void gpio_set_af(int param_1,uint pin,int afVal)

{
  int iVar1;
  
  iVar1 = REG_GPIOA;
  if (((param_1 != 0) && (iVar1 = REG_GPIOB, param_1 != 1)) && (iVar1 = REG_GPIOD, param_1 == 2)) {
    iVar1 = REG_GPIOC;
  }
  if ((pin & 8) == 0) {
    *(uint *)(iVar1 + 0x20) =
         *(uint *)(iVar1 + 0x20) & ~(0xf << ((pin & 7) << 2)) | afVal << ((pin & 7) << 2);
  }
  else {
    *(uint *)(iVar1 + 0x24) =
         *(uint *)(iVar1 + 0x24) & ~(0xf << ((pin & 7) << 2)) | afVal << ((pin & 7) << 2);
  }
  return;
}


// ================= gpio_af_config_fixed @ 0x08005446 =================

void gpio_af_config_fixed(int param_1,uint param_2)

{
  int iVar1;
  
  iVar1 = REG_GPIOA;
  if (((param_1 != 0) && (iVar1 = REG_GPIOB, param_1 != 1)) && (iVar1 = REG_GPIOD, param_1 == 2)) {
    iVar1 = REG_GPIOC;
  }
  if ((param_2 & 8) == 0) {
    *(uint *)(iVar1 + 0x20) = *(uint *)(iVar1 + 0x20) & ~(0xf << ((param_2 & 7) << 2)) | 3;
  }
  else {
    *(uint *)(iVar1 + 0x24) = *(uint *)(iVar1 + 0x24) & ~(0xf << ((param_2 & 7) << 2)) | 3;
  }
  return;
}


// ================= afio_pin_config @ 0x080054a0 =================

void afio_pin_config(int param_1,uint param_2)

{
  int iVar1;
  int iVar2;
  
  iVar1 = DAT_08005560;
  iVar2 = (int)param_2 >> 2;
  *(uint *)(DAT_08005560 + iVar2 * 4) =
       *(uint *)(DAT_08005560 + iVar2 * 4) & ~(3 << ((param_2 & 3) << 2));
  *(uint *)(DAT_08005560 + iVar2 * 4) =
       param_1 << ((param_2 & 3) << 2) | *(uint *)(iVar1 + iVar2 * 4);
  return;
}


// ================= gpio_can_xcvr_standby_pb12 @ 0x08007430 =================

/* gpio_can_xcvr_standby_pb12 — drives PB12 = CAN transceiver standby/enable (pulled low during
   autobaud). */

void gpio_can_xcvr_standby_pb12(void)

{
  gpio_write_pin(REG_GPIOB,0x1000,0);
  return;
}


// ================= gpio_can_xcvr_standby_on_pb12 @ 0x08007440 =================

void gpio_can_xcvr_standby_on_pb12(void)

{
  gpio_write_pin(REG_GPIOB,0x1000);
  return;
}


// ================= gpio_static_init_a @ 0x08008458 =================

void gpio_static_init_a(void)

{
  gpio_write_pin(REG_GPIOB,0x20,0);
  gpio_write_pin(REG_GPIOC,0x8000,0);
  gpio_write_pin(REG_GPIOB,0x40,0);
  gpio_write_pin(REG_GPIOB,0x10,0);
  gpio_init_pin(REG_GPIOB,0x20,1);
  gpio_init_pin(REG_GPIOC,0x8000);
  gpio_init_pin(REG_GPIOB,0x40,1);
  gpio_init_pin(REG_GPIOB,0x10,1);
  gpio_init_pin(REG_GPIOB,4,0);
  *p_gpio_initialized = 0xff;
  return;
}


// ================= gpio_static_init_b @ 0x080084be =================

void gpio_static_init_b(void)

{
  gpio_write_pin(REG_GPIOC,0x8000,0);
  gpio_write_pin(REG_GPIOB,0x40,0);
  gpio_write_pin(REG_GPIOB,0x10,0);
  gpio_init_pin(REG_GPIOB,8,0);
  gpio_init_pin(REG_GPIOB,4,0);
  return;
}


// ================= gpio_write_status_lines @ 0x080084f6 =================

/* gpio_write_status_lines — each main-loop tick, drives hardwired output lines from 4 bits of
   decoded-CAN state (RAM 0x20001b49): PB6=reverse, PC15=illum, PB5/PB4=acc/park. */

void gpio_write_status_lines(void)

{
  gpio_write_pin(REG_GPIOB,0x40,(*p_status_block & 7) >> 2);
  gpio_write_pin(REG_GPIOB,0x20,*p_status_block & 1);
  gpio_write_pin(REG_GPIOB,0x10,(*p_status_block & 0xf) >> 3);
  gpio_write_pin(REG_GPIOC,0x8000,(*p_status_block & 3) >> 1);
  return;
}


