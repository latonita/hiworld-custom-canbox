// app subsystem: sys  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= image_entry @ 0x08002000 =================

undefined8 image_entry(undefined4 param_1,undefined4 param_2)

{
  clear_impure_ptr();
  return CONCAT44(param_2,param_1);
}


// ================= sys_nop @ 0x08002008 =================

void sys_nop(void)

{
  return;
}


// ================= startup_crt0 @ 0x0800200c =================

void startup_crt0(void)

{
  undefined4 uVar1;
  undefined4 extraout_r2;
  undefined8 uVar2;
  
  uVar1 = sys_libc_unwind_frame();
  image_entry(uVar1,extraout_r2);
  call_main();
  uVar2 = sys_exit();
  sys_nop();
  sys_abort_bkpt((int)uVar2,(int)((ulonglong)uVar2 >> 0x20));
  sys_fault_reset_100();
                    /* WARNING: Could not recover jumptable at 0x08002030. Too many branches */
                    /* WARNING: Treating indirect jump as call */
  (*(code *)0x800202b)();
  return;
}


// ================= sys_exit_halt @ 0x0800201e =================

void sys_exit_halt(undefined4 param_1,undefined4 param_2)

{
  sys_nop();
  sys_abort_bkpt(param_1,param_2);
  sys_fault_reset_100();
                    /* WARNING: Could not recover jumptable at 0x08002030. Too many branches */
                    /* WARNING: Treating indirect jump as call */
  (*(code *)0x800202b)();
  return;
}


// ================= app_entry_veneer @ 0x08002202 =================

void app_entry_veneer(void)

{
  func_0x08002400();
  return;
}


// ================= call_main @ 0x0800220a =================

undefined4 call_main(void)

{
  main_loop();
  return 1;
}


// ================= sys_libc_unwind_frame @ 0x0800221a =================

void sys_libc_unwind_frame(void)

{
  uint uVar1;
  undefined4 *puVar2;
  undefined4 unaff_lr;
  uint *puVar3;
  
  uVar1 = get_exit_var_DEAD();
  *(undefined4 *)((uVar1 & 0xfffffff8) + 0x5c) = unaff_lr;
  puVar3 = (uint *)((uVar1 & 0xfffffff8) + 0x58);
  *puVar3 = uVar1;
  FUN_08000164();
  puVar2 = (undefined4 *)*puVar3;
  *puVar2 = 0;
  puVar2[1] = 0;
  puVar2[2] = 0;
  puVar2[3] = 0;
  puVar2[4] = 0;
  puVar2[5] = 0;
  puVar2[6] = 0;
  puVar2[7] = 0;
  puVar2[8] = 0;
  puVar2[9] = 0;
  puVar2[10] = 0;
  puVar2[0xb] = 0;
  puVar2[0xc] = 0;
  puVar2[0xd] = 0;
  puVar2[0xe] = 0;
  puVar2[0xf] = 0;
  return;
}


// ================= sys_exit @ 0x08002264 =================

undefined4 sys_exit(undefined4 param_1)

{
  sys_exit_halt(param_1);
  return p_libc_exit_var;
}


// ================= sys_abort_bkpt @ 0x08002280 =================

void sys_abort_bkpt(void)

{
  software_bkpt(0xab);
  do {
                    /* WARNING: Do nothing block with infinite loop */
  } while( true );
}


// ================= sys_nop_stub @ 0x0800286c =================

void sys_nop_stub(void)

{
  return;
}


// ================= param_page_save_0801F800 @ 0x08002d46 =================

/* param_page_save_0801F800 — the ONLY live flash writer. Erases + programs the 32-byte parameter
   page at flash 0x0801F800 (emulated EEPROM). Sole caller of the live FLASH_ErasePage /
   FLASH_ProgramWord. Never writes the app or bootloader region. */

void param_page_save_0801F800(void)

{
  int iVar1;
  int iVar2;
  uint i;
  bool dirty;
  int pSrc;
  
  pSrc = p_param_shadow;
  dirty = false;
  i = 0;
  do {
    if (0x1f < i) {
code_r0x08002d6e:
      if (dirty) {
        disableIRQinterrupts();
        wwdg_kick();
        RCC_EnableHSI_forFlash();
        FLASH_Unlock();
        wwdg_kick();
        FLASH_ErasePage(DAT_08002e60);
        wwdg_kick();
        iVar2 = DAT_08002e60;
        iVar1 = p_param_struct;
        for (i = 0; i < 0x20; i = i + 1 & 0xff) {
          *(undefined1 *)(pSrc + i) = *(undefined1 *)(iVar1 + i);
          FLASH_ProgramWord(iVar2 + i * 4,*(undefined1 *)(iVar1 + i));
          wwdg_kick();
        }
        FLASH_Lock();
        enableIRQinterrupts();
      }
      return;
    }
    if (*(char *)(p_param_struct + i) != *(char *)(p_param_shadow + i)) {
      dirty = true;
      goto code_r0x08002d6e;
    }
    i = i + 1 & 0xff;
  } while( true );
}


// ================= param_page_load_0801F800 @ 0x08002dbc =================

/* param_page_load_0801F800 — reads the 32-byte parameter page (0x0801F800) into RAM at boot
   (model/profile byte -> 0x200011D1). */

void param_page_load_0801F800(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  uint uVar4;
  
  iVar2 = p_param_shadow;
  iVar1 = p_param_struct;
  for (uVar4 = 0; iVar3 = DAT_08002e60, uVar4 < 0x20; uVar4 = uVar4 + 1 & 0xff) {
    *(undefined1 *)(iVar1 + uVar4) = 0;
  }
  for (uVar4 = 0; uVar4 < 0x20; uVar4 = uVar4 + 1 & 0xff) {
    *(undefined1 *)(iVar1 + uVar4) = *(undefined1 *)(iVar3 + uVar4 * 4);
    *(undefined1 *)(iVar2 + uVar4) = *(undefined1 *)(iVar1 + uVar4);
  }
  return;
}


// ================= reinit_can_filters_and_state @ 0x08002dec =================

void reinit_can_filters_and_state(void)

{
  int iVar1;
  
  iVar1 = rcc_get_flag_status(0x7e);
  if (iVar1 != 0) {
    rcc_clear_reset_flags();
  }
  rcc_apb1_periph_clock_cmd(0x800);
  wwdg_set_prescaler(0x100);
  wwdg_set_window(0x7f);
  wwdg_enable_set_counter(0x7f);
  return;
}


// ================= fatal_error_reset @ 0x08002e64 =================

/* fatal_error_reset — unrecoverable-error path; forces a reset. */

void fatal_error_reset(undefined1 param_1)

{
  do {
    *p_fatal_error_code = param_1;
    reinit_can_filters_and_state();
    wwdg_force_reset_thunk();
  } while( true );
}


// ================= sys_fault_reset_100 @ 0x08002e76 =================

void sys_fault_reset_100(void)

{
  int iVar1;
  
  fatal_error_reset(100);
  fatal_error_reset(0x65);
  fatal_error_reset(0x66);
  fatal_error_reset(0x67);
  fatal_error_reset(0x68);
  fatal_error_reset(0x6e);
  fatal_error_reset(0x6f);
  fatal_error_reset(0x70);
  fatal_error_reset(0x71);
  fatal_error_reset(0x72);
  fatal_error_reset(0x73);
  fatal_error_reset(0x74);
  fatal_error_reset(0x75);
  fatal_error_reset(0x76);
  fatal_error_reset(0x77);
  fatal_error_reset(0x78);
  fatal_error_reset(0x79);
  iVar1 = exti_get_it_status(4);
  if (iVar1 != 0) {
    exti_clear_it_pending(4);
    fatal_error_reset(0x7a);
  }
                    /* WARNING: Could not recover jumptable at 0x08002ef2. Too many branches */
                    /* WARNING: Treating indirect jump as call */
  (*(code *)0x8002edd)();
  return;
}


// ================= sys_soft_clock_recalc_12h @ 0x08007898 =================

void sys_soft_clock_recalc_12h(void)

{
  byte *pbVar1;
  
  if (*p_soft_clock < 0xc) {
    *(uint *)(p_soft_clock + 4) = *(uint *)(p_soft_clock + 4) & 0xfffffffd;
  }
  else {
    *(uint *)(p_soft_clock + 4) = (*(uint *)(p_soft_clock + 4) & 0xfffffffd) + 2;
  }
  pbVar1 = p_soft_clock;
  p_soft_clock[5] = *p_soft_clock % 0xc;
  if (pbVar1[5] == 0) {
    pbVar1[5] = 0xc;
  }
  return;
}


// ================= sys_soft_clock_init @ 0x080078da =================

void sys_soft_clock_init(void)

{
  undefined1 *puVar1;
  
  puVar1 = p_soft_clock;
  *p_soft_clock = 0;
  puVar1[1] = 0;
  puVar1[2] = 0;
  puVar1[3] = 0;
  sys_soft_clock_recalc_12h();
  return;
}


// ================= sys_soft_clock_tick @ 0x080078ee =================

void sys_soft_clock_tick(void)

{
  char *pcVar1;
  char *pcVar2;
  
  if ((uint)(byte)p_soft_clock[3] == ((byte)p_soft_clock[3] / 5) * 5) {
    *p_blink_halfsec = *p_blink_halfsec & 0xfffffffe | ~(uint)(byte)*p_blink_halfsec & 1;
  }
  pcVar1 = p_soft_clock;
  p_soft_clock[3] = p_soft_clock[3] + '\x01';
  pcVar2 = p_soft_clock;
  if (9 < (byte)pcVar1[3]) {
    p_soft_clock[3] = '\0';
    pcVar2[2] = pcVar2[2] + '\x01';
    pcVar1 = p_soft_clock;
    if (0x3b < (byte)pcVar2[2]) {
      p_soft_clock[2] = '\0';
      pcVar1[1] = pcVar1[1] + '\x01';
      *(uint *)(pcVar1 + 4) = (*(uint *)(pcVar1 + 4) & 0xfffffffe) + 1;
      pcVar2 = p_soft_clock;
      if (0x3b < (byte)pcVar1[1]) {
        p_soft_clock[1] = '\0';
        *pcVar2 = *pcVar2 + '\x01';
        if (*pcVar2 == '\x18') {
          *pcVar2 = '\0';
        }
        sys_soft_clock_recalc_12h();
      }
    }
  }
  return;
}


// ================= sys_soft_clock_set_time_DEAD @ 0x08007984 =================

void sys_soft_clock_set_time_DEAD(int param_1,int param_2)

{
  undefined1 uVar1;
  
  if (param_1 == 0x40) {
    uVar1 = util_wrap_index(*p_soft_clock,0xffffffff,0x17,0,0);
    *p_soft_clock = uVar1;
  }
  else if (param_1 == 0x80) {
    uVar1 = util_wrap_index(*p_soft_clock,1,0x17,0,0);
    *p_soft_clock = uVar1;
  }
  else if (param_1 != 0xc0) {
    *p_soft_clock = (char)param_1;
  }
  if (param_2 == 0x40) {
    uVar1 = util_wrap_index(p_soft_clock[1],0xffffffff,0x3b,0,0);
    p_soft_clock[1] = uVar1;
  }
  else if (param_2 == 0x80) {
    uVar1 = util_wrap_index(p_soft_clock[1],1,0x3b,0,0);
    p_soft_clock[1] = uVar1;
  }
  else if (param_2 != 0xc0) {
    p_soft_clock[1] = (char)param_2;
  }
  p_soft_clock[2] = 0;
  sys_soft_clock_recalc_12h();
  *(uint *)(p_soft_clock + 4) = (*(uint *)(p_soft_clock + 4) & 0xfffffffe) + 1;
  return;
}


// ================= sys_misc_timers_tick @ 0x080083e2 =================

void sys_misc_timers_tick(void)

{
  if (*p_misc_timer_118d != '\0') {
    *p_misc_timer_118d = *p_misc_timer_118d + -1;
  }
  if (1 < *p_swc_resp_mode) {
    *p_swc_resp_mode = *p_swc_resp_mode - 1;
  }
  if (1 < *p_swc_flag_118b) {
    *p_swc_flag_118b = *p_swc_flag_118b - 1;
  }
  if (1 < *p_swc_timer_118c) {
    *p_swc_timer_118c = *p_swc_timer_118c - 1;
  }
  softtimer_tick_all();
  return;
}


// ================= sys_cycle_counter_tick @ 0x0800853c =================

void sys_cycle_counter_tick(void)

{
  byte *pbVar1;
  
  pbVar1 = p_cycle_ctr;
  *p_cycle_ctr = *p_cycle_ctr + 1;
  if (*p_cycle_ctr_max <= *pbVar1) {
    *p_cycle_ctr = 1;
  }
  return;
}


// ================= main_loop @ 0x080088d8 =================

/* main_loop � two phases. PHASE 1 (autobaud): toggles g_param_profile_sel and re-sets CAN baud
   each g_autobaud_tick_flag until g_can_rx_ring_head>=3 frames (max 8 tries), persisting the
   winning profile; after 8 fails -> exti8_bus_wake_setup. PHASE 2 (steady superloop): publishes
   g_profile_active, inits g_can_bus_status=3 / g_bus_present_debounce=800, then services three
   scheduler flags: fast=status-change+CAN TX drain+softtimer scan; med=autobaud watchdog+UART
   RX/TX+GPIO status lines; slow=param-page save. Runs on bootloader VTOR=0x08000000,
   MSP=0x20000600. */

void main_loop(void)

{
  byte *pbVar1;
  
  disableIRQinterrupts();
  param_page_load_0801F800();
  tim2_sched_nvic_init();
  gpio_static_init_a();
  sys_soft_clock_init();
  uart_init_38400_and_version();
  can_config_load_defaults();
  can_set_baud_by_profile();
  reinit_can_filters_and_state();
  *p_autobaud_try_count = 0;
  enableIRQinterrupts();
  while (wwdg_kick(), *p_can_rx_ring_head < 3) {
    if (*p_autobaud_tick_flag == '\x01') {
      *p_status_block = (*p_status_block & 0xbf) + 0x40;
      if (*p_autobaud_try_count < 8) {
        *(bool *)(p_param_struct + 1) = *(char *)(p_param_struct + 1) != '\x01';
        param_page_save_0801F800();
        disableIRQinterrupts();
        can_set_baud_by_profile();
        enableIRQinterrupts();
        *p_autobaud_try_count = *p_autobaud_try_count + 1;
        *p_autobaud_tick_flag = '\0';
      }
      else {
        exti8_bus_wake_setup();
      }
    }
  }
  *p_profile_active = *(undefined1 *)(p_param_struct + 1);
  can_config_load_defaults();
  can_apply_bitrate_B();
  *p_can_bus_status = 3;
  *p_bus_present_debounce = 800;
  do {
    wwdg_kick();
    pbVar1 = p_status_block;
    if (p_status_block[0x11] != p_status_block[0x10]) {
      p_status_block[0x11] = p_status_block[0x10];
      pbVar1[0xf] = pbVar1[0xf] + 1;
    }
    if (*p_tick_flag_fast != '\0') {
      *p_tick_flag_fast = '\0';
      status_bit_change_detect();
      can_tx_ring_drain();
      softtimer_scan_active_hi();
    }
    if (*p_tick_flag_med != '\0') {
      *p_tick_flag_med = '\0';
      can_autobaud_state_machine();
      uart_rx_tx_service();
      gpio_write_status_lines();
    }
    if (*p_tick_flag_slow != '\0') {
      *p_tick_flag_slow = '\0';
      param_page_save_0801F800();
    }
  } while( true );
}


