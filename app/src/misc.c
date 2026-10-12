// app subsystem: misc  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= get_exit_var_DEAD @ 0x08002270 =================

undefined4 get_exit_var_DEAD(void)

{
  return p_libc_exit_var;
}


// ================= get_impure_ptr @ 0x08002278 =================

undefined4 get_impure_ptr(void)

{
  return p_libc_impure_ptr;
}


// ================= clear_impure_ptr @ 0x0800228e =================

void clear_impure_ptr(void)

{
  undefined4 *puVar1;
  
  puVar1 = (undefined4 *)get_impure_ptr();
  *puVar1 = 0;
  return;
}


// ================= usart2_init @ 0x0800291e =================

void usart2_init(undefined4 param_1)

{
  undefined1 local_1c;
  undefined1 local_1b;
  undefined1 local_1a;
  undefined1 local_19;
  undefined4 local_18;
  undefined2 local_14;
  undefined2 local_12;
  undefined2 local_10;
  undefined2 local_e;
  undefined2 local_c;
  
  rcc_apb1_periph_clock_cmd(0x20000);
  nvic_set_priority_group(0x300);
  local_1c = 0x26;
  local_1b = 2;
  local_1a = 0;
  local_19 = 1;
  nvic_init(&local_1c);
  gpio_init_pin(REG_GPIOA,8,2);
  gpio_init_pin(REG_GPIOA,4,2);
  usart_struct_init(&local_18);
  local_14 = 0;
  local_12 = 0;
  local_10 = 0;
  local_c = 0;
  local_e = 0xc;
  local_18 = param_1;
  usart_configure_baud(REG_USART2,&local_18);
  usart_cr_bit_write(REG_USART2,0x525,1);
  usart_cr_bit_write(REG_USART2,0x727,0);
  usart_cmd_enable(REG_USART2,1);
  return;
}


// ================= usart2_init_wrapper @ 0x0800306c =================

void usart2_init_wrapper(undefined4 param_1)

{
  usart2_init(param_1);
  return;
}


// ================= update_path_exti_setup @ 0x08003222 =================

void update_path_exti_setup
               (undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4)

{
  undefined4 local_10;
  undefined4 local_c;
  undefined4 local_8;
  
  local_10 = param_2;
  local_c = param_3;
  local_8 = param_4;
  nvic_set_priority_group(0x500);
  local_8 = 0x1000017;
  nvic_init(&local_8);
  gpio_init_pin(REG_GPIOB,0x100,DAT_0800339c);
  afio_pin_config(1,8);
  local_10 = 0x100;
  local_c = CONCAT13((char)((uint)local_c >> 0x18),0x10c00);
  exti_init(&local_10);
  return;
}


// ================= exti8_bus_wake_setup @ 0x080032f6 =================

void exti8_bus_wake_setup(void)

{
  int iVar1;
  
  iVar1 = can_rx0_msg_pending();
  if (iVar1 == 1) {
    gpio_can_xcvr_standby_on_pb12();
    gpio_static_init_b();
    adc_dma_cmd(DAT_08003394 + -0x4c,0);
    adc_enable(DAT_08003394 + -0x4c,0);
    adc_sw_start_regular(DAT_08003394 + -0x4c,0);
    uart_disable_usart2();
    can_clock_disable();
    update_path_exti_setup();
    pwr_enter_stop(1,0x1000);
    wwdg_force_reset_thunk();
  }
  return;
}


// ================= irq_source_active_check_DEAD @ 0x08003852 =================

undefined4 irq_source_active_check_DEAD(uint *param_1,uint param_2)

{
  undefined4 uVar1;
  
  if (((*param_1 & (int)param_2 >> 8) == 0) || ((param_1[1] & param_2 & 0xff) == 0)) {
    uVar1 = 0;
  }
  else {
    uVar1 = 1;
  }
  return uVar1;
}


// ================= cfg_struct_set_defaults @ 0x08005356 =================

void cfg_struct_set_defaults(undefined2 *param_1)

{
  *param_1 = 0xffff;
  *(undefined1 *)((int)param_1 + 3) = 0;
  *(undefined4 *)(param_1 + 4) = 0;
  *(undefined4 *)(param_1 + 6) = 0xf;
  *(undefined1 *)(param_1 + 2) = 0;
  *(undefined1 *)(param_1 + 1) = 0;
  return;
}


// ================= status_bit_change_detect @ 0x08007858 =================

void status_bit_change_detect(void)

{
  uint *puVar1;
  
  puVar1 = p_blink_halfsec;
  if (((byte)*p_blink_halfsec & 1) == ((byte)*p_blink_halfsec & 3) >> 1) {
    *p_blink_halfsec = *p_blink_halfsec & 0xfffffffb;
  }
  else {
    *p_blink_halfsec = (*p_blink_halfsec & 0xfffffffb) + 4;
    *p_blink_halfsec = *puVar1 & 0xfffffffd | ((byte)*puVar1 & 1) << 1;
  }
  return;
}


