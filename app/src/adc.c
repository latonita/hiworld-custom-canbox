// app subsystem: adc  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= adc_subsystem_init_DEAD @ 0x080030b0 =================

void adc_subsystem_init_DEAD(void)

{
  int iVar1;
  int local_50;
  undefined4 local_4c;
  undefined4 local_48;
  undefined4 local_44;
  undefined4 local_40;
  undefined4 local_3c;
  undefined4 local_38;
  undefined4 local_34;
  undefined4 local_30;
  undefined4 local_2c;
  undefined4 local_28;
  undefined1 local_24;
  undefined1 local_23;
  undefined4 local_20;
  undefined4 local_1c;
  undefined1 local_18;
  undefined2 local_14 [4];
  undefined4 local_c;
  
  rcc_ahb_periph_clock_cmd(1);
  rcc_apb2_enable(0x10,1);
  rcc_ahb_periph_clock_cmd(0x1000);
  adc_clock_config(0,7);
  rcc_cfgr2_clk_config(0x20000,0x7000);
  cfg_struct_set_defaults(local_14);
  local_14[0] = 0x10;
  local_c = 3;
  gpio_apply_pin_config(REG_GPIOA,local_14);
  local_14[0] = 0x20;
  local_c = 3;
  gpio_apply_pin_config(REG_GPIOA,local_14);
  local_14[0] = 0x40;
  local_c = 3;
  gpio_apply_pin_config(REG_GPIOA,local_14);
  local_14[0] = 0x80;
  local_c = 3;
  gpio_apply_pin_config(REG_GPIOA,local_14);
  dma_channel_deinit(REG_DMA1_CH1);
  local_50 = DAT_08003394;
  local_4c = DAT_08003398;
  local_48 = 0;
  local_44 = 10;
  local_40 = 0;
  local_3c = 0x80;
  local_38 = 0x100;
  local_34 = 0x400;
  local_30 = 0x20;
  local_2c = 0x2000;
  local_28 = 0;
  dma_channel_init(REG_DMA1_CH1,&local_50);
  dma_channel_cfg_reg10(0,REG_DMA1_CH1 + -8,REG_DMA1_CH1,1);
  dma_channel_cmd(REG_DMA1_CH1,1);
  local_24 = 1;
  local_23 = 1;
  local_20 = 0xe0000;
  local_1c = 0;
  local_18 = 2;
  adc_init(DAT_08003394 + -0x4c,&local_24);
  adc_regular_channel_config(DAT_08003394 + -0x4c,5,1);
  adc_regular_channel_config(DAT_08003394 + -0x4c,6,2,5);
  adc_regular_channel_config(DAT_08003394 + -0x4c,7,3,5);
  adc_regular_channel_config(DAT_08003394 + -0x4c,8,4,5);
  adc_dma_cmd(DAT_08003394 + -0x4c,1);
  adc_enable(DAT_08003394 + -0x4c,1);
  do {
    iVar1 = adc_get_flag_status(DAT_08003394 + -0x4c,0x20);
  } while (iVar1 == 0);
  adc_start_calibration(DAT_08003394 + -0x4c);
  do {
    iVar1 = adc_calibration_busy(DAT_08003394 + -0x4c);
  } while (iVar1 != 0);
  adc_sw_start_regular(DAT_08003394 + -0x4c,1);
  return;
}


// ================= adc_init @ 0x0800349c =================

void adc_init(int param_1,byte *param_2)

{
  *(uint *)(param_1 + 4) = *(uint *)(param_1 + 4) & DAT_08003878 | (uint)*param_2 << 8;
  *(uint *)(param_1 + 8) =
       *(uint *)(param_1 + 8) & DAT_0800387c |
       *(uint *)(param_2 + 8) | *(uint *)(param_2 + 4) | (uint)param_2[1] << 1;
  *(uint *)(param_1 + 0x2c) =
       *(uint *)(param_1 + 0x2c) & 0xff0fffff | (param_2[0xc] - 1 & 0xff) << 0x14;
  return;
}


// ================= adc_enable @ 0x080034ee =================

void adc_enable(int param_1,int param_2)

{
  if (param_2 == 0) {
    *(uint *)(param_1 + 8) = *(uint *)(param_1 + 8) & 0xfffffffe;
  }
  else {
    *(uint *)(param_1 + 8) = *(uint *)(param_1 + 8) | 1;
  }
  return;
}


// ================= adc_dma_cmd @ 0x08003504 =================

void adc_dma_cmd(int param_1,int param_2)

{
  if (param_2 == 0) {
    *(uint *)(param_1 + 8) = *(uint *)(param_1 + 8) & 0xfffffeff;
  }
  else {
    *(uint *)(param_1 + 8) = *(uint *)(param_1 + 8) | 0x100;
  }
  return;
}


// ================= adc_it_config_DEAD @ 0x0800351a =================

void adc_it_config_DEAD(int param_1,byte param_2,int param_3)

{
  if (param_3 == 0) {
    *(uint *)(param_1 + 4) = *(uint *)(param_1 + 4) & ~(uint)param_2;
  }
  else {
    *(uint *)(param_1 + 4) = *(uint *)(param_1 + 4) | (uint)param_2;
  }
  return;
}


// ================= adc_start_calibration @ 0x08003532 =================

void adc_start_calibration(int param_1)

{
  if (*(int *)(param_1 + 0x54) == 0) {
    *(uint *)(param_1 + 8) = *(uint *)(param_1 + 8) | 4;
  }
  return;
}


// ================= adc_calibration_busy @ 0x08003540 =================

bool adc_calibration_busy(int param_1)

{
  return *(int *)(param_1 + 0x54) == 0 && (*(uint *)(param_1 + 8) & 4) != 0;
}


// ================= adc_sw_start_regular @ 0x0800355a =================

void adc_sw_start_regular(int param_1,int param_2)

{
  if (param_2 == 0) {
    *(uint *)(param_1 + 8) = *(uint *)(param_1 + 8) & 0xffafffff;
  }
  else {
    *(uint *)(param_1 + 8) = *(uint *)(param_1 + 8) | 0x500000;
  }
  return;
}


// ================= adc_set_discontinuous_num_DEAD @ 0x08003584 =================

void adc_set_discontinuous_num_DEAD(int param_1,int param_2)

{
  *(uint *)(param_1 + 4) = *(uint *)(param_1 + 4) & 0xffff1fff | (param_2 + -1) * 0x2000;
  return;
}


// ================= adc_regular_channel_config @ 0x080035b2 =================

void adc_regular_channel_config(int param_1,int param_2,int param_3,uint param_4)

{
  if (param_2 == 0x12) {
    *(uint *)(param_1 + 0x5c) = *(uint *)(param_1 + 0x5c) & 0xfffffff8 | param_4;
  }
  if (param_2 < 10) {
    *(uint *)(param_1 + 0x10) =
         *(uint *)(param_1 + 0x10) & ~(7 << (param_2 * 3 & 0xffU)) |
         param_4 << (param_2 * 3 & 0xffU);
  }
  else {
    *(uint *)(param_1 + 0xc) =
         *(uint *)(param_1 + 0xc) & ~(7 << ((param_2 + -10) * 3 & 0xffU)) |
         param_4 << ((param_2 + -10) * 3 & 0xffU);
  }
  if (param_3 < 7) {
    *(uint *)(param_1 + 0x34) =
         *(uint *)(param_1 + 0x34) & ~(0x1f << ((param_3 + -1) * 5 & 0xffU)) |
         param_2 << ((param_3 + -1) * 5 & 0xffU);
  }
  else if (param_3 < 0xd) {
    *(uint *)(param_1 + 0x30) =
         *(uint *)(param_1 + 0x30) & ~(0x1f << ((param_3 + -7) * 5 & 0xffU)) |
         param_2 << ((param_3 + -7) * 5 & 0xffU);
  }
  else {
    *(uint *)(param_1 + 0x2c) =
         *(uint *)(param_1 + 0x2c) & ~(0x1f << ((param_3 + -0xd) * 5 & 0xffU)) |
         param_2 << ((param_3 + -0xd) * 5 & 0xffU);
  }
  return;
}


// ================= adc_injected_channel_config_DEAD @ 0x08003712 =================

void adc_injected_channel_config_DEAD(int param_1,int param_2,int param_3,uint param_4)

{
  uint uVar1;
  
  if (param_2 == 0x12) {
    *(uint *)(param_1 + 0x5c) = *(uint *)(param_1 + 0x5c) & 0xfffffff8 | param_4;
  }
  else if (param_2 < 10) {
    *(uint *)(param_1 + 0x10) =
         *(uint *)(param_1 + 0x10) & ~(7 << (param_2 * 3 & 0xffU)) |
         param_4 << (param_2 * 3 & 0xffU);
  }
  else {
    *(uint *)(param_1 + 0xc) =
         *(uint *)(param_1 + 0xc) & ~(7 << ((param_2 + -10) * 3 & 0xffU)) |
         param_4 << ((param_2 + -10) * 3 & 0xffU);
  }
  uVar1 = (*(uint *)(param_1 + 0x38) & 0x3fffff) >> 0x14;
  *(uint *)(param_1 + 0x38) =
       *(uint *)(param_1 + 0x38) & ~(0x1f << (((param_3 + 3) - (uVar1 + 1)) * 5 & 0xff)) |
       param_2 << (((param_3 + 3) - (uVar1 + 1)) * 5 & 0xff);
  return;
}


// ================= adc_injected_seq_length_DEAD @ 0x080037a4 =================

void adc_injected_seq_length_DEAD(int param_1,int param_2)

{
  *(uint *)(param_1 + 0x38) = *(uint *)(param_1 + 0x38) & 0xffcfffff | (param_2 + -1) * 0x100000;
  return;
}


// ================= adc_get_flag_status @ 0x08003910 =================

bool adc_get_flag_status(int param_1,uint param_2)

{
  return (*(uint *)(param_1 + 0x58) & param_2) != 0;
}


// ================= adc_clock_config @ 0x08003948 =================

void adc_clock_config(int param_1,undefined4 param_2)

{
  if (param_1 == 0) {
    rcc_cfgr2_clk_config_b(0x100,0);
    rcc_cfgr2_set_src(param_2);
  }
  else {
    rcc_cfgr2_clk_config_b(param_2,1);
    rcc_cfgr2_set_src(0);
  }
  return;
}


