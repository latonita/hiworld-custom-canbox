// app subsystem: rcc  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= RCC_EnableHSI_forFlash @ 0x080046b4 =================

undefined8 RCC_EnableHSI_forFlash(void)

{
  undefined4 uVar1;
  int iVar2;
  undefined4 local_8;
  
  iVar2 = 0;
  local_8 = 0;
  uVar1 = 0;
  if ((*REG_RCC & 2) == 0) {
    *REG_RCC = *REG_RCC | 1;
    do {
      local_8 = local_8 + 1;
      if ((int)(*REG_RCC << 0x1e) < 0) break;
    } while (local_8 != 0x500);
    iVar2 = -((int)(*REG_RCC << 0x1e) >> 0x1f);
    if (iVar2 == 0) {
      uVar1 = 1;
    }
  }
  return CONCAT44(iVar2,uVar1);
}


// ================= pwr_enter_sleep @ 0x08005602 =================

void pwr_enter_sleep(int param_1,int param_2)

{
  uint *puVar1;
  
  puVar1 = REG_SCB_SCR;
  *REG_SCB_SCR = *REG_SCB_SCR & 0xfffffffb;
  if (param_1 == 1) {
    *puVar1 = *puVar1 | 2;
  }
  else if (param_1 == 0) {
    *REG_SCB_SCR = *REG_SCB_SCR & 0xfffffffd;
  }
  if (param_2 == 1) {
    WaitForInterrupt();
  }
  else {
    WaitForEvent();
    WaitForEvent();
  }
  return;
}


// ================= pwr_enter_stop @ 0x0800563c =================

void pwr_enter_stop(int param_1,uint param_2)

{
  uint *puVar1;
  
  puVar1 = REG_PWR;
  REG_PWR[2] = REG_PWR[2] & 0xffffcfff | param_2;
  *puVar1 = *puVar1 & 0xfffffff8 | 2;
  *REG_SCB_SCR = *REG_SCB_SCR | 4;
  if (param_1 == 1) {
    WaitForInterrupt();
  }
  else {
    WaitForEvent();
    WaitForEvent();
  }
  *REG_SCB_SCR = *REG_SCB_SCR & 0xfffffffb;
  return;
}


// ================= rcc_system_clock_init @ 0x08005682 =================

void rcc_system_clock_init(void)

{
  int iVar1;
  
  rcc_deinit();
  iVar1 = rcc_get_flag_status(99);
  if (iVar1 == 0) {
    rcc_csr_clk_config(4,0x60);
    do {
      iVar1 = rcc_wait_flag_ready();
    } while (iVar1 != 1);
  }
  flash_set_prefetch(0x10);
  rcc_set_sysclk_source(0);
  do {
    iVar1 = rcc_get_sysclk_source();
  } while (iVar1 != 0);
  rcc_set_ahb_prescaler();
  rcc_set_apb2_prescaler(0);
  rcc_set_apb1_prescaler(0);
  return;
}


// ================= rcc_clock_restore @ 0x080056ca =================

void rcc_clock_restore(void)

{
  uint *puVar1;
  
  rcc_system_clock_init();
  flash_set_latency(2);
  *REG_FLASH = *REG_FLASH | 0x200;
  do {
  } while ((*REG_FLASH & 0x100) != 0x100);
  flash_set_latency(0);
  puVar1 = DAT_080058ac;
  *DAT_080058ac = *DAT_080058ac & 0xfdffffff;
  *puVar1 = *puVar1;
  puVar1 = REG_PWR;
  REG_PWR[2] = REG_PWR[2] & 0xfffffeff;
  puVar1[2] = puVar1[2];
  puVar1[2] = puVar1[2] & 0xfffeffff;
  puVar1[2] = puVar1[2];
  *puVar1 = *puVar1 & 0xfffffff8 | 0x4000;
  do {
  } while ((REG_PWR[4] & 1) != 0);
  return;
}


// ================= rcc_clock_switch_hsi_DEAD @ 0x08005758 =================

void rcc_clock_switch_hsi_DEAD(void)

{
  uint *puVar1;
  
  puVar1 = REG_PWR;
  *REG_PWR = *REG_PWR & 0xffffbfff;
  *puVar1 = *puVar1 & 0xfffff9ff;
  *puVar1 = *puVar1 | 0x600;
  do {
  } while ((REG_PWR[4] & 2) != 2);
  do {
  } while ((REG_PWR[4] & 1) == 0);
  flash_set_latency(2);
  *REG_FLASH = *REG_FLASH & 0xfffffdff;
  do {
  } while ((*REG_FLASH & 0x100) != 0);
  flash_set_latency();
  puVar1 = REG_PWR;
  *REG_PWR = *REG_PWR & 0xfffff9ff;
  *puVar1 = *puVar1 | 0x400;
  do {
  } while ((REG_PWR[4] & 2) != 0);
  do {
  } while ((REG_PWR[4] & 2) != 2);
  return;
}


// ================= pwr_sleep_entry_DEAD @ 0x080057f6 =================

void pwr_sleep_entry_DEAD(undefined4 param_1,undefined4 param_2)

{
  rcc_clock_restore();
  pwr_enter_sleep(param_1,param_2);
  return;
}


// ================= rcc_deinit @ 0x080058b0 =================

void rcc_deinit(void)

{
  uint *puVar1;
  uint *puVar2;
  
  puVar1 = REG_RCC;
  REG_RCC[9] = REG_RCC[9] | 4;
  puVar2 = REG_RCC;
  REG_RCC[1] = puVar1[1] & DAT_08005c98;
  puVar1 = REG_RCC;
  *REG_RCC = *puVar2 & DAT_08005c9c;
  *puVar1 = *puVar1 & 0xfffbffff;
  puVar2 = REG_RCC;
  REG_RCC[1] = puVar1[1] & DAT_08005ca0;
  puVar2[0xb] = 0x3800;
  puVar2[0xc] = 0x3800;
  puVar2[0xd] = 0;
  puVar2[0x10] = 0;
  puVar2[2] = 0x9f0000;
  return;
}


// ================= rcc_get_flag_status @ 0x08005946 =================

bool rcc_get_flag_status(uint flagId)

{
  uint regVal;
  
  if ((int)flagId >> 5 == 1) {
    regVal = *REG_RCC;
  }
  else if ((int)flagId >> 5 == 2) {
    regVal = REG_RCC[8];
  }
  else {
    regVal = REG_RCC[9];
  }
  return (1 << (flagId & 0x1f) & regVal) != 0;
}


// ================= rcc_wait_hse_ready @ 0x0800597e =================

bool rcc_wait_hse_ready(void)

{
  int iVar1;
  int local_10;
  
  local_10 = 0;
  do {
    iVar1 = rcc_get_flag_status(0x31);
    local_10 = local_10 + 1;
    if (local_10 == 0x2000) break;
  } while (iVar1 == 0);
  iVar1 = rcc_get_flag_status(0x31);
  return iVar1 != 0;
}


// ================= rcc_csr_clk_config @ 0x08005a2a =================

void rcc_csr_clk_config(int param_1,uint param_2)

{
  int iVar1;
  
  iVar1 = REG_RCC;
  *(uint *)(REG_RCC + 0x24) = *(uint *)(REG_RCC + 0x24) & 0xffffff8f;
  *(uint *)(iVar1 + 0x24) = *(uint *)(iVar1 + 0x24) | param_2;
  if (param_1 == 0) {
    *(uint *)(REG_RCC + 0x24) = *(uint *)(REG_RCC + 0x24) & 0xfffffffb;
  }
  else if (param_1 == 4) {
    *(uint *)(iVar1 + 0x24) = *(uint *)(iVar1 + 0x24) | 4;
  }
  return;
}


// ================= rcc_wait_flag_ready @ 0x08005a64 =================

bool rcc_wait_flag_ready(void)

{
  int iVar1;
  int local_10;
  
  local_10 = 0;
  do {
    iVar1 = rcc_get_flag_status(99);
    local_10 = local_10 + 1;
    if (local_10 == 0x500) break;
  } while (iVar1 == 0);
  iVar1 = rcc_get_flag_status(99);
  return iVar1 != 0;
}


// ================= rcc_pll_config_DEAD @ 0x08005ab6 =================

void rcc_pll_config_DEAD(uint param_1,uint param_2,uint param_3)

{
  uint *puVar1;
  int iVar2;
  
  iVar2 = REG_RCC;
  puVar1 = (uint *)(REG_RCC + 0x40);
  if ((param_1 == 0) || (param_1 == 1)) {
    param_3 = param_1 | param_3;
  }
  else {
    param_2 = param_1 | param_2;
  }
  *(uint *)(REG_RCC + 4) = *(uint *)(REG_RCC + 4) & DAT_08005cac | param_2;
  *(uint *)(iVar2 + 0x40) = *puVar1 & 0xfffffffc | param_3;
  return;
}


// ================= rcc_set_sysclk_source @ 0x08005af4 =================

void rcc_set_sysclk_source(uint param_1)

{
  *(uint *)(REG_RCC + 4) = *(uint *)(REG_RCC + 4) & 0xfffffffc | param_1;
  return;
}


// ================= rcc_get_sysclk_source @ 0x08005b06 =================

uint rcc_get_sysclk_source(void)

{
  return *(uint *)(REG_RCC + 4) & 0xc;
}


// ================= rcc_set_ahb_prescaler @ 0x08005b10 =================

void rcc_set_ahb_prescaler(uint param_1)

{
  *(uint *)(REG_RCC + 4) = *(uint *)(REG_RCC + 4) & 0xffffff0f | param_1;
  return;
}


// ================= rcc_set_apb1_prescaler @ 0x08005b22 =================

void rcc_set_apb1_prescaler(uint param_1)

{
  *(uint *)(REG_RCC + 4) = *(uint *)(REG_RCC + 4) & 0xfffff8ff | param_1;
  return;
}


// ================= rcc_set_apb2_prescaler @ 0x08005b34 =================

void rcc_set_apb2_prescaler(int param_1)

{
  *(uint *)(REG_RCC + 4) = *(uint *)(REG_RCC + 4) & 0xffffc7ff | param_1 << 3;
  return;
}


// ================= rcc_cfgr2_clk_config @ 0x08005b9a =================

void rcc_cfgr2_clk_config(uint param_1,uint param_2)

{
  *(uint *)(REG_RCC + 0x2c) = *(uint *)(REG_RCC + 0x2c) & 0xfffc0fff | param_1 | param_2;
  return;
}


// ================= rcc_cfgr2_clk_config_b @ 0x08005bb2 =================

void rcc_cfgr2_clk_config_b(uint param_1,int param_2)

{
  if (param_2 == 0) {
    param_1 = param_1 & 0xfffffeff;
  }
  *(uint *)(REG_RCC + 0x2c) = *(uint *)(REG_RCC + 0x2c) & 0xfffffe0f | param_1;
  return;
}


// ================= rcc_cfgr2_set_src @ 0x08005bd0 =================

void rcc_cfgr2_set_src(uint param_1)

{
  *(uint *)(REG_RCC + 0x2c) = *(uint *)(REG_RCC + 0x2c) & 0xfffffff0 | param_1;
  return;
}


// ================= rcc_cfgr3_config_DEAD @ 0x08005c18 =================

void rcc_cfgr3_config_DEAD(uint param_1,int param_2)

{
  int iVar1;
  
  iVar1 = REG_RCC;
  *(uint *)(REG_RCC + 0x30) = *(uint *)(REG_RCC + 0x30) & 0xfffffdff | param_1;
  if (param_2 == 0) {
    *(uint *)(REG_RCC + 0x30) = *(uint *)(REG_RCC + 0x30) & 0xffffff7f;
  }
  else {
    *(uint *)(REG_RCC + 0x30) = *(uint *)(iVar1 + 0x30) | 0x80;
  }
  return;
}


// ================= rcc_apb1_periph_clock_cmd @ 0x08005cb0 =================

void rcc_apb1_periph_clock_cmd(uint param_1,int param_2)

{
  if (param_2 == 0) {
    *(uint *)(REG_RCC + 0x1c) = *(uint *)(REG_RCC + 0x1c) & ~param_1;
  }
  else {
    *(uint *)(REG_RCC + 0x1c) = *(uint *)(REG_RCC + 0x1c) | param_1;
  }
  return;
}


// ================= rcc_rtc_bdcr_config_DEAD @ 0x08005cca =================

void rcc_rtc_bdcr_config_DEAD(uint param_1)

{
  int iVar1;
  
  rcc_apb1_periph_clock_cmd(0x10000000);
  *REG_PWR = *REG_PWR | 0x100;
  iVar1 = REG_RCC;
  *(uint *)(REG_RCC + 0x34) = *(uint *)(REG_RCC + 0x34) & 0xfffffff8;
  *(uint *)(iVar1 + 0x34) = *(uint *)(iVar1 + 0x34) | param_1;
  return;
}


// ================= rcc_rtc_clk_config_DEAD @ 0x08005d62 =================

void rcc_rtc_clk_config_DEAD(int param_1)

{
  rcc_apb1_periph_clock_cmd(0x10000000);
  *REG_PWR = *REG_PWR | 0x100;
  *(uint *)(REG_RCC + 0x20) = *(uint *)(REG_RCC + 0x20) & 0xfffffff2;
  if (param_1 == 1) {
    *(uint *)(REG_RCC + 0x20) = *(uint *)(REG_RCC + 0x20) | 1;
  }
  else if (param_1 == 4) {
    *(uint *)(REG_RCC + 0x20) = *(uint *)(REG_RCC + 0x20) | 5;
  }
  return;
}


// ================= rcc_get_clocks_freq @ 0x08005e00 =================

void rcc_get_clocks_freq(uint *pClocks)

{
  uint uVar1;
  uint pllClkFreq;
  int iVar2;
  uint uVar3;
  
  pllClkFreq = *(uint *)(REG_RCC + 4) & DAT_08006044;
  uVar3 = (*(uint *)(REG_RCC + 0x24) & 0x7f) >> 4;
  if ((pllClkFreq & 0x8000000) == 0) {
    iVar2 = (pllClkFreq >> 0x12) + 2;
  }
  else {
    iVar2 = (pllClkFreq >> 0x12) - 0x1ef;
  }
  if ((*(uint *)(REG_RCC + 4) & 0x10000) == 0) {
    if ((*(uint *)(REG_RCC + 0x40) & 1) == 0) {
      pllClkFreq = iVar2 * DAT_0800604c;
    }
    else {
      pllClkFreq = iVar2 * DAT_08006048;
    }
  }
  else if ((*(uint *)(REG_RCC + 4) & 0x20000) == 0) {
    pllClkFreq = iVar2 * DAT_0800604c;
  }
  else {
    pllClkFreq = iVar2 * DAT_08006048;
  }
  if ((*(uint *)(REG_RCC + 0x40) & 2) != 0) {
    pllClkFreq = pllClkFreq >> 1;
  }
  uVar1 = *(uint *)(REG_RCC + 4) & 0xc;
  if (uVar1 == 0) {
    *pClocks = *(uint *)(DAT_08006050 + uVar3 * 4);
  }
  else if (uVar1 == 4) {
    *pClocks = DAT_0800604c;
  }
  else if (uVar1 == 8) {
    *pClocks = DAT_0800604c;
  }
  else if (uVar1 == 0xc) {
    *pClocks = pllClkFreq;
  }
  else {
    *pClocks = *(uint *)(DAT_08006050 + uVar3 * 4);
  }
  pClocks[1] = *pClocks >> *(sbyte *)(DAT_08006054 + ((*(uint *)(REG_RCC + 4) & 0xf0) >> 4));
  pClocks[2] = pClocks[1] >> *(sbyte *)(DAT_08006054 + ((*(uint *)(REG_RCC + 4) & 0x700) >> 8));
  pClocks[3] = pClocks[1] >> *(sbyte *)(DAT_08006054 + ((*(uint *)(REG_RCC + 4) & 0x3800) >> 0xb));
  pClocks[5] = pClocks[1] / (uint)*(byte *)(DAT_08006058 + (*(uint *)(REG_RCC + 0x2c) & 0xf));
  pClocks[4] = pllClkFreq /
               *(ushort *)(DAT_0800605c + ((*(uint *)(REG_RCC + 0x2c) & 0x1f0) >> 4 & 0xf) * 2);
  return;
}


// ================= rcc_ahb_periph_clock_cmd @ 0x08005f2e =================

void rcc_ahb_periph_clock_cmd(uint param_1,int param_2)

{
  if (param_2 == 0) {
    *(uint *)(REG_RCC + 0x14) = *(uint *)(REG_RCC + 0x14) & ~param_1;
  }
  else {
    *(uint *)(REG_RCC + 0x14) = *(uint *)(REG_RCC + 0x14) | param_1;
  }
  return;
}


// ================= rcc_apb2_enable @ 0x08005f48 =================

void rcc_apb2_enable(uint param_1,int param_2)

{
  if (param_2 == 0) {
    *(uint *)(REG_RCC + 0x18) = *(uint *)(REG_RCC + 0x18) & ~param_1;
  }
  else {
    *(uint *)(REG_RCC + 0x18) = *(uint *)(REG_RCC + 0x18) | param_1;
  }
  return;
}


// ================= rcc_ahb_periph_reset @ 0x08005f62 =================

void rcc_ahb_periph_reset(uint param_1,int param_2)

{
  if (param_2 == 0) {
    *(uint *)(REG_RCC + 0x28) = *(uint *)(REG_RCC + 0x28) & ~param_1;
  }
  else {
    *(uint *)(REG_RCC + 0x28) = *(uint *)(REG_RCC + 0x28) | param_1;
  }
  return;
}


// ================= rcc_apb2_periph_reset @ 0x08005f7c =================

void rcc_apb2_periph_reset(uint param_1,int param_2)

{
  if (param_2 == 0) {
    *(uint *)(REG_RCC + 0xc) = *(uint *)(REG_RCC + 0xc) & ~param_1;
  }
  else {
    *(uint *)(REG_RCC + 0xc) = *(uint *)(REG_RCC + 0xc) | param_1;
  }
  return;
}


// ================= rcc_apb1_periph_reset @ 0x08005f96 =================

void rcc_apb1_periph_reset(uint param_1,int param_2)

{
  if (param_2 == 0) {
    *(uint *)(REG_RCC + 0x10) = *(uint *)(REG_RCC + 0x10) & ~param_1;
  }
  else {
    *(uint *)(REG_RCC + 0x10) = *(uint *)(REG_RCC + 0x10) | param_1;
  }
  return;
}


// ================= rcc_clear_reset_flags @ 0x08006000 =================

void rcc_clear_reset_flags(void)

{
  int iVar1;
  
  iVar1 = REG_RCC;
  *(uint *)(REG_RCC + 0x24) = *(uint *)(REG_RCC + 0x24) | 0x1000000;
  *(uint *)(iVar1 + 0x24) = *(uint *)(iVar1 + 0x24) & 0xfeffffff;
  return;
}


