// app subsystem: core  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= exti_line_setup_DEAD @ 0x0800327c =================

void exti_line_setup_DEAD
               (undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4)

{
  undefined4 local_10;
  undefined4 local_c;
  undefined4 local_8;
  
  local_10 = param_2;
  local_c = param_3;
  local_8 = param_4;
  nvic_set_priority_group(0x500);
  local_8 = 0x1000008;
  nvic_init(&local_8);
  gpio_init_pin(REG_GPIOB,4,DAT_0800339c);
  afio_pin_config(1,2);
  local_10 = 4;
  local_c = CONCAT13((char)((uint)local_c >> 0x18),0x10800);
  exti_init(&local_10);
  return;
}


// ================= nvic_set_priority_group @ 0x080033a4 =================

void nvic_set_priority_group(uint param_1)

{
  *REG_SCB_AIRCR = DAT_0800346c | param_1;
  return;
}


// ================= nvic_init @ 0x080033ae =================

void nvic_init(byte *pNvicCfg)

{
  uint uVar1;
  
  if (pNvicCfg[3] == 0) {
    *(int *)(REG_NVIC_ICER + ((int)(uint)*pNvicCfg >> 5) * 4) = 1 << (*pNvicCfg & 0x1f);
  }
  else {
    uVar1 = 0x700 - (*REG_SCB_AIRCR & 0x700) >> 8;
    *(char *)(REG_NVIC_IPR + (uint)*pNvicCfg) =
         (char)(((uint)pNvicCfg[1] << (4 - uVar1 & 0xff) |
                (uint)pNvicCfg[2] & 0xfU >> (uVar1 & 0xff)) << 4);
    *(int *)(((int)(uint)*pNvicCfg >> 5) * 4 + -0x1fff1f00) = 1 << (*pNvicCfg & 0x1f);
  }
  return;
}


// ================= exti_init @ 0x080044ea =================

void exti_init(uint *pExtiCfg)

{
  uint *puVar1;
  uint *pExtiReg;
  
  pExtiReg = REG_EXTI;
  if (*(char *)((int)pExtiCfg + 6) == '\0') {
    pExtiReg = (uint *)((int)REG_EXTI + (uint)(byte)pExtiCfg[1]);
    *pExtiReg = *pExtiReg & ~*pExtiCfg;
  }
  else {
    *REG_EXTI = *REG_EXTI & ~*pExtiCfg;
    pExtiReg[1] = pExtiReg[1] & ~*pExtiCfg;
    pExtiReg = (uint *)((int)pExtiReg + (uint)(byte)pExtiCfg[1]);
    *pExtiReg = *pExtiReg | *pExtiCfg;
    pExtiReg = REG_EXTI;
    REG_EXTI[2] = REG_EXTI[2] & ~*pExtiCfg;
    puVar1 = REG_EXTI;
    REG_EXTI[3] = pExtiReg[3] & ~*pExtiCfg;
    pExtiReg = REG_EXTI;
    if (*(char *)((int)pExtiCfg + 5) == '\x10') {
      REG_EXTI[2] = puVar1[2] | *pExtiCfg;
      REG_EXTI[3] = pExtiReg[3] | *pExtiCfg;
    }
    else {
      pExtiReg = (uint *)((int)REG_EXTI + (uint)*(byte *)((int)pExtiCfg + 5));
      *pExtiReg = *pExtiReg | *pExtiCfg;
    }
  }
  return;
}


// ================= exti_get_it_status @ 0x080045b6 =================

undefined4 exti_get_it_status(uint param_1)

{
  undefined4 uVar1;
  
  if (((REG_EXTI[5] & param_1) == 0) || ((*REG_EXTI & param_1) == 0)) {
    uVar1 = 0;
  }
  else {
    uVar1 = 1;
  }
  return uVar1;
}


// ================= exti_clear_it_pending @ 0x080045d8 =================

void exti_clear_it_pending(undefined4 param_1)

{
  *(undefined4 *)(REG_EXTI + 0x14) = param_1;
  return;
}


