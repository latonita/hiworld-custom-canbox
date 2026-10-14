// app subsystem: tim  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= tim2_init @ 0x080027f8 =================

void tim2_init(void)

{
  undefined2 local_1c;
  undefined2 local_1a;
  undefined2 local_18;
  undefined2 local_16;
  undefined1 local_c;
  undefined1 local_b;
  undefined1 local_a;
  undefined1 local_9;
  
  nvic_set_priority_group(0x300);
  local_c = 0x1c;
  local_b = 4;
  local_a = 0;
  local_9 = 1;
  nvic_init(&local_c);
  rcc_apb1_periph_clock_cmd(1);
  local_18 = 4000;
  local_1c = 0;
  local_16 = 0;
  local_1a = 0;
  tim_timebase_init(0x40000000,&local_1c);
  tim_prescaler_config(0x40000000,7);
  tim_it_config(0x40000000,1);
  tim_cmd(0x40000000);
  return;
}


// ================= tim2_isr_scheduler_tick @ 0x08002f72 =================

/* tim2_isr_scheduler_tick � TIM2 IRQ. Calls tim2_sched_rate_flags, then runs a 10ms->100ms->1s
   divider chain (g_tick_10ms_div/100ms_div/1s_div @0x20001000..02): every ~100ms runs FUN_080083e2
   + FUN_0800853c; every ~1s runs sys_soft_clock_tick + FUN_080077e8. */

void tim2_isr_scheduler_tick(void)

{
  byte *pbVar1;
  int iVar2;
  code *UNRECOVERED_JUMPTABLE;
  
  iVar2 = tim_get_it_status(0x40000000);
  if (iVar2 != 0) {
    tim_clear_it_pending(0x40000000);
    tim2_sched_rate_flags();
    pbVar1 = p_tick_10ms_div;
    *p_tick_10ms_div = *p_tick_10ms_div + 1;
    if (9 < *pbVar1) {
      *pbVar1 = 0;
      sys_misc_timers_tick();
      sys_cycle_counter_tick();
      *p_tick_100ms_div = *p_tick_100ms_div + 1;
    }
    if (9 < *p_tick_100ms_div) {
      *p_tick_100ms_div = 0;
      sys_soft_clock_tick();
      can_autobaud_timers_tick();
      *p_tick_1s_div = *p_tick_1s_div + 1;
    }
    if (9 < *p_tick_1s_div) {
      *p_tick_1s_div = 0;
    }
  }
                    /* WARNING: Could not recover jumptable at 0x08002fe2. Too many branches */
                    /* WARNING: Treating indirect jump as call */
  (*UNRECOVERED_JUMPTABLE)();
  return;
}


// ================= tim2_sched_nvic_init @ 0x080032d2 =================

void tim2_sched_nvic_init(void)

{
  rcc_set_apb1_prescaler(0x500);
  rcc_set_apb2_prescaler(0x500);
  tim2_init();
  sys_nop_stub();
  rcc_apb1_periph_clock_cmd(0x10000000);
  return;
}


// ================= tim_deinit_DEAD @ 0x08006064 =================

void tim_deinit_DEAD(int param_1)

{
  if (param_1 == REG_TIM1) {
    rcc_apb2_periph_reset(REG_TIM1 >> 0x13,1);
    rcc_apb2_periph_reset(param_1 >> 0x13,0);
  }
  else if (param_1 == 0x40000000) {
    rcc_apb1_periph_reset(1);
    rcc_apb1_periph_reset(1,0);
  }
  else if (param_1 == DAT_08006468) {
    rcc_apb1_periph_reset(2,1);
    rcc_apb1_periph_reset(2,0);
  }
  else if (param_1 == DAT_0800646c) {
    rcc_apb1_periph_reset(4,1);
    rcc_apb1_periph_reset(4,0);
  }
  else if (param_1 == DAT_08006470) {
    rcc_apb1_periph_reset(8,1);
    rcc_apb1_periph_reset(8,0);
  }
  else if (param_1 == DAT_08006474) {
    rcc_apb1_periph_reset(0x10,1);
    rcc_apb1_periph_reset(0x10,0);
  }
  else if (param_1 == DAT_08006478) {
    rcc_apb1_periph_reset(0x20,1);
    rcc_apb1_periph_reset(0x20,0);
  }
  else if (param_1 == DAT_0800647c) {
    rcc_apb2_periph_reset(DAT_0800647c >> 0x11,1);
    rcc_apb2_periph_reset(param_1 >> 0x11,0);
  }
  else if (param_1 == DAT_08006480) {
    rcc_apb1_periph_reset(DAT_08006480 >> 0x15,1);
    rcc_apb1_periph_reset(param_1 >> 0x15,0);
  }
  return;
}


// ================= tim_timebase_init @ 0x08006140 =================

void tim_timebase_init(uint *timBase,undefined2 *pInit)

{
  uint uVar1;
  
  uVar1 = *timBase;
  if ((((timBase == REG_TIM1) || (timBase == DAT_0800647c)) || (timBase == (uint *)0x40000000)) ||
     (((timBase == DAT_08006468 || (timBase == DAT_0800646c)) ||
      ((timBase == DAT_08006470 || (timBase == DAT_08006480)))))) {
    uVar1 = uVar1 & 0xffffff8f | (uint)(ushort)pInit[1];
  }
  if ((timBase != DAT_08006474) && (timBase != DAT_08006478)) {
    uVar1 = uVar1 & 0xfffffcff | (uint)(ushort)pInit[3];
  }
  *timBase = uVar1;
  *(undefined2 *)(timBase + 0xb) = pInit[2];
  *(undefined2 *)(timBase + 10) = *pInit;
  if ((timBase == REG_TIM1) || (timBase == DAT_0800647c)) {
    *(ushort *)(timBase + 0xc) = (ushort)*(byte *)(pInit + 4);
  }
  *(undefined2 *)(timBase + 5) = 1;
  uVar1 = *timBase;
  if (((((timBase == REG_TIM1) || (timBase == DAT_0800647c)) || (timBase == (uint *)0x40000000)) ||
      ((timBase == DAT_08006468 || (timBase == DAT_0800646c)))) ||
     ((timBase == DAT_08006470 || (timBase == DAT_08006480)))) {
    if (*(char *)((int)pInit + 9) == '\0') {
      uVar1 = uVar1 & 0xfffff7ff;
    }
    else {
      uVar1 = uVar1 | 0x800;
    }
  }
  if (timBase == DAT_08006480) {
    if (*(char *)(pInit + 5) == '\0') {
      uVar1 = uVar1 & 0xffffefff;
    }
    else {
      uVar1 = uVar1 | 0x1000;
    }
    if (*(char *)((int)pInit + 0xb) == '\0') {
      uVar1 = uVar1 & 0xffffdfff;
    }
    else {
      uVar1 = uVar1 | 0x2000;
    }
    if (*(char *)(pInit + 6) == '\0') {
      uVar1 = uVar1 & 0xffffbfff;
    }
    else {
      uVar1 = uVar1 | 0x4000;
    }
  }
  if (((timBase == REG_TIM1) || (timBase == DAT_0800647c)) ||
     ((timBase == (uint *)0x40000000 ||
      (((timBase == DAT_08006468 || (timBase == DAT_0800646c)) || (timBase == DAT_08006480)))))) {
    if (*(char *)((int)pInit + 0xd) == '\0') {
      uVar1 = uVar1 & 0xffff7fff;
    }
    else {
      uVar1 = uVar1 | 0x8000;
    }
  }
  *timBase = uVar1;
  uVar1 = timBase[1];
  if ((timBase == (uint *)0x40000000) || (timBase == DAT_0800646c)) {
    if (*(char *)(pInit + 7) == '\0') {
      uVar1 = uVar1 & 0xfffffeff;
    }
    else {
      uVar1 = uVar1 | 0x100;
    }
  }
  timBase[1] = uVar1;
  return;
}


// ================= tim_oc4_set_mode @ 0x08006502 =================

void tim_oc4_set_mode(int param_1,short param_2)

{
  *(ushort *)(param_1 + 0x1c) = *(ushort *)(param_1 + 0x1c) & 0xf3ff;
  *(ushort *)(param_1 + 0x1c) = *(ushort *)(param_1 + 0x1c) | param_2 << 8;
  return;
}


// ================= tim_oc_config_ch34 @ 0x0800651c =================

void tim_oc_config_ch34(int param_1,uint param_2,short param_3,short param_4)

{
  uint uVar1;
  
  *(uint *)(param_1 + 0x20) = *(uint *)(param_1 + 0x20) & 0xffffefff;
  if ((((param_1 == REG_TIM1) || (param_1 == DAT_080068d0)) || (param_1 == 0x40000000)) ||
     (((param_1 == DAT_080068d4 || (param_1 == DAT_080068d8)) ||
      ((param_1 == DAT_080068dc || (param_1 == DAT_080068e0)))))) {
    uVar1 = *(uint *)(param_1 + 0x20) & 0xffffdfff | param_2 << 0xc | 0x1000;
  }
  else {
    uVar1 = *(uint *)(param_1 + 0x20) & 0xffffdfff | param_2 | 0x1000;
  }
  *(ushort *)(param_1 + 0x1c) = *(ushort *)(param_1 + 0x1c) & 0xcff | param_3 << 8 | param_4 << 0xc;
  *(uint *)(param_1 + 0x20) = uVar1;
  return;
}


// ================= tim_oc3_set_mode @ 0x08006596 =================

void tim_oc3_set_mode(int param_1,ushort param_2)

{
  *(ushort *)(param_1 + 0x1c) = *(ushort *)(param_1 + 0x1c) & 0xfff3;
  *(ushort *)(param_1 + 0x1c) = *(ushort *)(param_1 + 0x1c) | param_2;
  return;
}


// ================= tim_oc_config_ch12 @ 0x080065a8 =================

void tim_oc_config_ch12(int param_1,uint param_2,ushort param_3,short param_4)

{
  uint uVar1;
  
  *(uint *)(param_1 + 0x20) = *(uint *)(param_1 + 0x20) & 0xfffffeff;
  if ((((param_1 == REG_TIM1) || (param_1 == DAT_080068d0)) || (param_1 == 0x40000000)) ||
     (((param_1 == DAT_080068d4 || (param_1 == DAT_080068d8)) ||
      ((param_1 == DAT_080068dc || (param_1 == DAT_080068e0)))))) {
    uVar1 = *(uint *)(param_1 + 0x20) & 0xfffffdff | param_2 << 8 | 0x100;
  }
  else {
    uVar1 = *(uint *)(param_1 + 0x20) & 0xfffff5ff | param_2 | 0x100;
  }
  *(ushort *)(param_1 + 0x1c) = *(ushort *)(param_1 + 0x1c) & 0xff0c | param_4 << 4 | param_3;
  *(uint *)(param_1 + 0x20) = uVar1;
  return;
}


// ================= tim_oc2_set_mode @ 0x0800661a =================

void tim_oc2_set_mode(int param_1,short param_2)

{
  *(ushort *)(param_1 + 0x18) = *(ushort *)(param_1 + 0x18) & 0xf3ff;
  *(ushort *)(param_1 + 0x18) = *(ushort *)(param_1 + 0x18) | param_2 << 8;
  return;
}


// ================= tim_oc2_init @ 0x08006634 =================

void tim_oc2_init(int param_1,uint param_2,short param_3,short param_4)

{
  uint uVar1;
  
  *(uint *)(param_1 + 0x20) = *(uint *)(param_1 + 0x20) & 0xffffffef;
  if ((((param_1 == REG_TIM1) || (param_1 == DAT_080068d0)) || (param_1 == 0x40000000)) ||
     (((param_1 == DAT_080068d4 || (param_1 == DAT_080068d8)) ||
      ((param_1 == DAT_080068dc || (param_1 == DAT_080068e0)))))) {
    uVar1 = *(uint *)(param_1 + 0x20) & 0xffffffdf | param_2 << 4 | 0x10;
  }
  else {
    uVar1 = *(uint *)(param_1 + 0x20) & 0xffffff5f | param_2 | 0x10;
  }
  *(ushort *)(param_1 + 0x18) = *(ushort *)(param_1 + 0x18) & 0xcff | param_4 << 0xc | param_3 << 8;
  *(uint *)(param_1 + 0x20) = uVar1;
  return;
}


// ================= tim_oc1_preload_cfg @ 0x080066ae =================

void tim_oc1_preload_cfg(int param_1,ushort param_2)

{
  *(ushort *)(param_1 + 0x18) = *(ushort *)(param_1 + 0x18) & 0xfff3;
  *(ushort *)(param_1 + 0x18) = *(ushort *)(param_1 + 0x18) | param_2;
  return;
}


// ================= tim_oc1_init @ 0x080066c0 =================

void tim_oc1_init(int param_1,uint param_2,ushort param_3,short param_4)

{
  uint uVar1;
  
  *(uint *)(param_1 + 0x20) = *(uint *)(param_1 + 0x20) & 0xfffffffe;
  if ((((param_1 == REG_TIM1) || (param_1 == DAT_080068d0)) || (param_1 == 0x40000000)) ||
     (((param_1 == DAT_080068d4 || (param_1 == DAT_080068d8)) ||
      ((param_1 == DAT_080068dc || (param_1 == DAT_080068e0)))))) {
    uVar1 = *(uint *)(param_1 + 0x20) & 0xfffffffd | param_2 | 1;
  }
  else {
    uVar1 = *(uint *)(param_1 + 0x20) & 0xfffffff5 | param_2 | 1;
  }
  *(ushort *)(param_1 + 0x18) = *(ushort *)(param_1 + 0x18) & 0xff0c | param_4 << 4 | param_3;
  *(uint *)(param_1 + 0x20) = uVar1;
  return;
}


// ================= tim_oc_init_dispatch @ 0x0800672e =================

/* WARNING: Type propagation algorithm not settling */

void tim_oc_init_dispatch(undefined4 param_1,short *param_2)

{
  if (*param_2 == 0) {
    tim_oc1_init(param_1,param_2[1],param_2[2],param_2[4]);
    tim_oc1_preload_cfg(param_1,param_2[3]);
  }
  else if (*param_2 == 4) {
    tim_oc2_init(param_1,param_2[1],param_2[2],param_2[4]);
    tim_oc2_set_mode(param_1,param_2[3]);
  }
  else if (*param_2 == 8) {
    tim_oc_config_ch12(param_1,param_2[1],param_2[2],param_2[4]);
    tim_oc3_set_mode(param_1,param_2[3]);
  }
  else {
    tim_oc_config_ch34(param_1,param_2[1],param_2[2],param_2[4]);
    tim_oc4_set_mode(param_1,param_2[3]);
  }
  return;
}


// ================= tim_oc_pwm_pair_init @ 0x080067ca =================

void tim_oc_pwm_pair_init(undefined4 param_1,short *param_2)

{
  undefined4 uVar1;
  undefined4 uVar2;
  
  if (param_2[1] == 0) {
    uVar1 = 2;
  }
  else {
    uVar1 = 0;
  }
  if (param_2[2] == 1) {
    uVar2 = 2;
  }
  else {
    uVar2 = 1;
  }
  if (*param_2 == 0) {
    tim_oc1_init(param_1,param_2[1],param_2[2],param_2[4]);
    tim_oc1_preload_cfg(param_1,param_2[3]);
    tim_oc2_init(param_1,uVar1,uVar2,param_2[4]);
    tim_oc2_set_mode(param_1,param_2[3]);
  }
  else {
    tim_oc2_init(param_1,param_2[1],param_2[2],param_2[4]);
    tim_oc2_set_mode(param_1,param_2[3]);
    tim_oc1_init(param_1,uVar1,uVar2,param_2[4]);
    tim_oc1_preload_cfg(param_1,param_2[3]);
  }
  return;
}


// ================= tim_bdtr_config @ 0x08006846 =================

void tim_bdtr_config(uint *param_1,ushort *param_2)

{
  uint uVar1;
  
  *(ushort *)(param_1 + 0x11) =
       *param_2 | param_2[1] | param_2[2] | param_2[3] | param_2[4] | param_2[5] | param_2[6];
  if ((char)param_2[7] == '\0') {
    uVar1 = *param_1 | 0x400;
  }
  else {
    uVar1 = *param_1 & 0xfffffbff;
  }
  if (*(char *)((int)param_2 + 0xf) == '\0') {
    uVar1 = uVar1 & 0xfffeffff;
  }
  else {
    uVar1 = uVar1 | 0x10000;
  }
  if ((char)param_2[8] == '\0') {
    uVar1 = uVar1 & 0xfffdffff;
  }
  else {
    uVar1 = uVar1 | 0x20000;
  }
  *param_1 = uVar1;
  return;
}


// ================= tim_cmd @ 0x08006908 =================

void tim_cmd(uint *param_1,int param_2)

{
  if (param_2 == 0) {
    *param_1 = *param_1 & 0xfffffffe;
  }
  else {
    *param_1 = *param_1 | 1;
  }
  return;
}


// ================= tim_it_config @ 0x0800693c =================

void tim_it_config(int param_1,ushort param_2,int param_3)

{
  if (param_3 == 0) {
    *(ushort *)(param_1 + 0xc) = *(ushort *)(param_1 + 0xc) & ~param_2;
  }
  else {
    *(ushort *)(param_1 + 0xc) = *(ushort *)(param_1 + 0xc) | param_2;
  }
  return;
}


// ================= tim_slave_trigger_select @ 0x0800697a =================

void tim_slave_trigger_select(int param_1,ushort param_2)

{
  *(ushort *)(param_1 + 8) = *(ushort *)(param_1 + 8) & 0xff8f | param_2;
  return;
}


// ================= tim_select_slave_mode @ 0x0800698c =================

void tim_select_slave_mode(int param_1,undefined4 param_2)

{
  tim_slave_trigger_select(param_1,param_2);
  *(ushort *)(param_1 + 8) = *(ushort *)(param_1 + 8) | 7;
  return;
}


// ================= tim_input_trigger_oc_init @ 0x080069a4 =================

void tim_input_trigger_oc_init(int param_1,int param_2,undefined4 param_3,undefined4 param_4)

{
  if (param_2 == 0x60) {
    tim_oc2_init(param_1,param_3,1,param_4);
  }
  else {
    tim_oc1_init(param_1,param_3,1,param_4);
  }
  tim_slave_trigger_select(param_1,param_2);
  *(ushort *)(param_1 + 8) = *(ushort *)(param_1 + 8) | 7;
  return;
}


// ================= tim_smcr_config @ 0x080069e2 =================

void tim_smcr_config(int param_1,ushort param_2,ushort param_3,short param_4)

{
  *(ushort *)(param_1 + 8) = *(ushort *)(param_1 + 8) & 0xff | param_4 << 8 | param_3 | param_2;
  return;
}


// ================= tim_slave_input_config @ 0x080069fe =================

void tim_slave_input_config(int param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4)

{
  tim_smcr_config(param_1,param_2,param_3,param_4);
  *(ushort *)(param_1 + 8) = *(ushort *)(param_1 + 8) & 0xff88 | 0x77;
  return;
}


// ================= tim_etr_clock_mode2_config @ 0x08006a34 =================

void tim_etr_clock_mode2_config
               (int param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4)

{
  tim_smcr_config(param_1,param_2,param_3,param_4);
  *(ushort *)(param_1 + 8) = *(ushort *)(param_1 + 8) | 0x4000;
  return;
}


// ================= tim_prescaler_config @ 0x08006a54 =================

void tim_prescaler_config(int param_1,undefined2 param_2,undefined2 param_3)

{
  *(undefined2 *)(param_1 + 0x28) = param_2;
  *(undefined2 *)(param_1 + 0x14) = param_3;
  return;
}


// ================= tim_encoder_interface_config @ 0x08006a6a =================

void tim_encoder_interface_config(int param_1,ushort param_2,uint param_3,uint param_4)

{
  *(ushort *)(param_1 + 8) = *(ushort *)(param_1 + 8) & 0xfff8 | param_2;
  *(ushort *)(param_1 + 0x18) = *(ushort *)(param_1 + 0x18) & 0xfcfc | 0x101;
  *(uint *)(param_1 + 0x20) =
       *(uint *)(param_1 + 0x20) & 0xffffffdd | (param_4 & 0xfff) << 4 | param_3;
  return;
}


// ================= tim_get_it_status @ 0x08006fb4 =================

undefined4 tim_get_it_status(int param_1,uint param_2)

{
  undefined4 uVar1;
  
  if (((*(uint *)(param_1 + 0x10) & param_2) == 0) || ((*(ushort *)(param_1 + 0xc) & param_2) == 0))
  {
    uVar1 = 0;
  }
  else {
    uVar1 = 1;
  }
  return uVar1;
}


// ================= tim_clear_it_pending @ 0x08006fd6 =================

void tim_clear_it_pending(int param_1,uint param_2)

{
  *(uint *)(param_1 + 0x10) = *(uint *)(param_1 + 0x10) & ~param_2;
  return;
}


// ================= tim2_sched_rate_flags @ 0x08007a30 =================

/* tim2_sched_rate_flags � increments the free-running scheduler tick counter
   (g_sched_tick_counter @0x20001108) and raises 10 cadence flags at 2^n boundaries:
   g_tick_flag_fast(/4), _med(/8), _div16, _div32, _div64, _div128, _div256, _div512, _slow(/1024),
   g_autobaud_tick_flag(/2048). main_loop consumes fast/med/slow + autobaud; the rest are spare. */

void tim2_sched_rate_flags(void)

{
  uint *puVar1;
  
  puVar1 = p_sched_tick_counter;
  *p_sched_tick_counter = *p_sched_tick_counter + 1;
  if ((*puVar1 & 3) == 1) {
    *p_tick_flag_fast = 1;
  }
  if ((*p_sched_tick_counter & 7) == 3) {
    *p_tick_flag_med = 1;
  }
  if ((*p_sched_tick_counter & 0xf) == 7) {
    *p_tick_flag_div16 = 1;
  }
  if ((*p_sched_tick_counter & 0x1f) == 0xf) {
    *p_tick_flag_div32 = 1;
  }
  if ((*p_sched_tick_counter & 0x3f) == 0x3f) {
    *p_tick_flag_div64 = 1;
  }
  if ((*p_sched_tick_counter & 0x7f) == 0x7f) {
    *p_tick_flag_div128 = 1;
  }
  if ((*p_sched_tick_counter & 0xff) == 0xff) {
    *p_tick_flag_div256 = 1;
  }
  if ((*p_sched_tick_counter & 0x1ff) == 0x1ff) {
    *p_tick_flag_div512 = 1;
  }
  if ((*p_sched_tick_counter & 0x3ff) == 0x3ff) {
    *p_tick_flag_slow = 1;
  }
  if ((*p_sched_tick_counter & 0x7ff) == 0x7ff) {
    *p_autobaud_tick_flag = 1;
  }
  return;
}


