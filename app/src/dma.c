// app subsystem: dma  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= dma_channel_deinit @ 0x08004314 =================

void dma_channel_deinit(uint *param_1)

{
  *param_1 = *param_1 & 0xfffe;
  *param_1 = 0;
  param_1[1] = 0;
  param_1[2] = 0;
  param_1[3] = 0;
  if (param_1 == REG_DMA1_CH1) {
    REG_DMA1_CH1[-1] = REG_DMA1_CH1[-1] | 0xf;
  }
  else if (param_1 == REG_DMA1_CH1 + 5) {
    REG_DMA1_CH1[-1] = REG_DMA1_CH1[-1] | 0xf0;
  }
  else if (param_1 == REG_DMA1_CH1 + 10) {
    REG_DMA1_CH1[-1] = REG_DMA1_CH1[-1] | 0xf00;
  }
  else if (param_1 == REG_DMA1_CH1 + 0xf) {
    REG_DMA1_CH1[-1] = REG_DMA1_CH1[-1] | 0xf000;
  }
  else if (param_1 == REG_DMA1_CH1 + 0x14) {
    REG_DMA1_CH1[-1] = REG_DMA1_CH1[-1] | 0xf0000;
  }
  else if (param_1 == REG_DMA1_CH1 + 0x19) {
    REG_DMA1_CH1[-1] = REG_DMA1_CH1[-1] | 0xf00000;
  }
  else if (param_1 == REG_DMA1_CH1 + 0x1e) {
    REG_DMA1_CH1[-1] = REG_DMA1_CH1[-1] | 0xf000000;
  }
  else if (param_1 == REG_DMA1_CH1 + 0x23) {
    REG_DMA1_CH1[-1] = REG_DMA1_CH1[-1] | 0xf0000000;
  }
  return;
}


// ================= dma_channel_init @ 0x080043f6 =================

void dma_channel_init(uint *param_1,uint *param_2)

{
  *param_1 = *param_1 & 0xffff800f |
             param_2[2] | param_2[8] | param_2[4] | param_2[5] | param_2[6] | param_2[7] |
             param_2[9] | param_2[10];
  param_1[1] = param_2[3];
  param_1[2] = *param_2;
  param_1[3] = param_2[1];
  return;
}


// ================= dma_channel_cmd @ 0x0800444c =================

void dma_channel_cmd(uint *param_1,int param_2)

{
  if (param_2 == 0) {
    *param_1 = *param_1 & 0xfffe;
  }
  else {
    *param_1 = *param_1 | 1;
  }
  return;
}


// ================= dma_get_flag_status_DEAD @ 0x08004482 =================

bool dma_get_flag_status_DEAD(uint param_1,uint *param_2)

{
  return (*param_2 & param_1) != 0;
}


// ================= dma_get_it_status_DEAD @ 0x0800449e =================

bool dma_get_it_status_DEAD(uint param_1,uint *param_2)

{
  return (*param_2 & param_1) != 0;
}


// ================= dma_channel_cfg_reg10 @ 0x080044ba =================

void dma_channel_cfg_reg10(undefined4 param_1,undefined4 param_2,int param_3,int param_4)

{
  if (param_4 == 0) {
    *(undefined4 *)(param_3 + 0x10) = 0;
  }
  else {
    *(undefined4 *)(param_3 + 0x10) = param_1;
  }
  return;
}


