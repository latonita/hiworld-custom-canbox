// app subsystem: util  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= i2c_start_DEAD @ 0x08002558 =================

void i2c_start_DEAD(void)

{
  gpio_init_pin(REG_GPIOB,0x40,0x11);
  gpio_init_pin(REG_GPIOB,0x80,0x11);
  gpio_write_pin(REG_GPIOB,0x80,1);
  util_busy_delay_DEAD(5);
  gpio_write_pin(REG_GPIOB,0x40,1);
  util_busy_delay_DEAD(5);
  gpio_write_pin(REG_GPIOB,0x80,0);
  util_busy_delay_DEAD(5);
  gpio_write_pin(REG_GPIOB,0x40,0);
  util_busy_delay_DEAD(5);
  return;
}


// ================= i2c_stop_DEAD @ 0x080025b0 =================

void i2c_stop_DEAD(void)

{
  byte bVar1;
  int iVar2;
  
  gpio_init_pin(REG_GPIOB,0x80,0x11);
  gpio_write_pin(REG_GPIOB,0x80,0);
  gpio_init_pin(REG_GPIOB,0x40,0);
  gpio_write_pin(REG_GPIOB,0x40,1);
  bVar1 = 0;
  while ((bVar1 < 0x14 && (iVar2 = gpio_read_pin(REG_GPIOB,0x40), iVar2 == 0))) {
    util_busy_delay_long_DEAD(1);
    bVar1 = bVar1 + 1;
  }
  gpio_init_pin(REG_GPIOB,0x40,0x11);
  gpio_write_pin(REG_GPIOB,0x40,1);
  util_busy_delay_DEAD(5);
  gpio_write_pin(REG_GPIOB,0x80,1);
  util_busy_delay_DEAD(5);
  return;
}


// ================= i2c_write_byte_DEAD @ 0x08002624 =================

bool i2c_write_byte_DEAD(uint txByte)

{
  int ackBit;
  uint bitMask;
  
  gpio_init_pin(REG_GPIOB,0x40,0x11);
  gpio_init_pin(REG_GPIOB,0x80,0x11);
  for (bitMask = 0x80; bitMask != 0; bitMask = (int)bitMask >> 1) {
    gpio_write_pin(REG_GPIOB,0x40,0);
    if ((txByte & bitMask) == 0) {
      gpio_write_pin(REG_GPIOB,0x80,0);
    }
    else {
      gpio_write_pin(REG_GPIOB,0x80,1);
    }
    util_busy_delay_DEAD(5);
    gpio_write_pin(REG_GPIOB,0x40,1);
    util_busy_delay_DEAD(5);
  }
  gpio_write_pin(REG_GPIOB,0x40,0);
  gpio_init_pin(REG_GPIOB,0x80,0);
  util_busy_delay_DEAD(5);
  gpio_write_pin(REG_GPIOB,0x40,1);
  util_busy_delay_DEAD(5);
  ackBit = gpio_read_pin(REG_GPIOB,0x80);
  gpio_write_pin(REG_GPIOB,0x40,0);
  util_busy_delay_DEAD(5);
  return ackBit == 0;
}


// ================= i2c_read_byte_DEAD @ 0x080026d0 =================

uint i2c_read_byte_DEAD(void)

{
  byte retry;
  int iVar1;
  uint bitMask;
  uint uVar2;
  
  uVar2 = 0;
  gpio_init_pin(REG_GPIOB,0x40,0);
  gpio_write_pin(REG_GPIOB,0x40,1);
  retry = 0;
  while ((retry < 0x14 && (iVar1 = gpio_read_pin(REG_GPIOB,0x40), iVar1 == 0))) {
    util_busy_delay_long_DEAD(1);
    retry = retry + 1;
  }
  gpio_init_pin(REG_GPIOB,0x40,0x11);
  gpio_init_pin(REG_GPIOB,0x80,0);
  for (bitMask = 0x80; bitMask != 0; bitMask = (int)bitMask >> 1) {
    gpio_write_pin(REG_GPIOB,0x40,1);
    util_busy_delay_DEAD(5);
    iVar1 = gpio_read_pin(REG_GPIOB,0x80);
    if (iVar1 != 0) {
      uVar2 = uVar2 | bitMask;
    }
    gpio_write_pin(REG_GPIOB,0x40,0);
    util_busy_delay_DEAD(5);
  }
  return uVar2;
}


// ================= i2c_write_bit0_DEAD @ 0x08002754 =================

void i2c_write_bit0_DEAD(void)

{
  gpio_init_pin(REG_GPIOB,0x40,0x11);
  gpio_init_pin(REG_GPIOB,0x80,0x11);
  gpio_write_pin(REG_GPIOB,0x80,0);
  util_busy_delay_DEAD(5);
  gpio_write_pin(REG_GPIOB,0x40,1);
  util_busy_delay_DEAD(5);
  gpio_write_pin(REG_GPIOB,0x40,0);
  gpio_init_pin(REG_GPIOB,0x80,0);
  util_busy_delay_DEAD(5);
  return;
}


// ================= i2c_write_bit1_DEAD @ 0x080027a6 =================

void i2c_write_bit1_DEAD(void)

{
  gpio_init_pin(REG_GPIOB,0x40,0x11);
  gpio_init_pin(REG_GPIOB,0x80,0x11);
  gpio_write_pin(REG_GPIOB,0x80,1);
  util_busy_delay_DEAD(5);
  gpio_write_pin(REG_GPIOB,0x40,1);
  util_busy_delay_DEAD(5);
  gpio_write_pin(REG_GPIOB,0x40,0);
  gpio_init_pin(REG_GPIOB,0x80,0);
  util_busy_delay_DEAD(5);
  return;
}


// ================= util_shl32_DEAD @ 0x08002c04 =================

longlong util_shl32_DEAD(uint param_1)

{
  return (ulonglong)param_1 << 0x20;
}


// ================= util_busy_delay_DEAD @ 0x08003342 =================

void util_busy_delay_DEAD(uint param_1)

{
  char cVar1;
  
  for (; 0 < (int)param_1; param_1 = param_1 - 1 & 0xff) {
    for (cVar1 = '\r'; cVar1 != '\0'; cVar1 = cVar1 + -1) {
    }
  }
  return;
}


// ================= util_busy_delay_DEAD @ 0x08003354 =================

void util_busy_delay_DEAD(uint param_1)

{
  char cVar1;
  
  for (; 0 < (int)param_1; param_1 = param_1 - 1 & 0xff) {
    for (cVar1 = '\r'; cVar1 != '\0'; cVar1 = cVar1 + -1) {
    }
  }
  return;
}


// ================= util_busy_delay_long_DEAD @ 0x0800335a =================

void util_busy_delay_long_DEAD(uint param_1)

{
  char cVar1;
  
  for (; 0 < (int)param_1; param_1 = param_1 - 1 & 0xff) {
    for (cVar1 = -0x7e; cVar1 != '\0'; cVar1 = cVar1 + -1) {
    }
  }
  return;
}


// ================= util_busy_delay_long_DEAD @ 0x0800336c =================

void util_busy_delay_long_DEAD(uint param_1)

{
  char cVar1;
  
  for (; 0 < (int)param_1; param_1 = param_1 - 1 & 0xff) {
    for (cVar1 = -0x7e; cVar1 != '\0'; cVar1 = cVar1 + -1) {
    }
  }
  return;
}


// ================= util_delay_ms_DEAD @ 0x08003372 =================

void util_delay_ms_DEAD(uint param_1)

{
  int extraout_r3;
  
  while (0 < (int)param_1) {
    util_busy_delay_long_DEAD(100);
    param_1 = extraout_r3 - 1U & 0xff;
  }
  return;
}


// ================= util_reg_write32_DEAD @ 0x080037bc =================

void util_reg_write32_DEAD(int param_1,int param_2,undefined4 param_3)

{
  *(undefined4 *)(param_1 + param_2) = param_3;
  return;
}


// ================= util_bit_test @ 0x0800413c =================

bool util_bit_test(uint param_1,uint param_2)

{
  return (param_1 & param_2) != 0;
}


// ================= util_test_reg_bits_DEAD @ 0x0800536e =================

bool util_test_reg_bits_DEAD(int param_1,uint param_2)

{
  return (*(uint *)(param_1 + 0x10) & param_2) != 0;
}


// ================= softtimer_arm_at_index @ 0x08007cd2 =================

void softtimer_arm_at_index(void)

{
  *(undefined4 *)(p_softtimer_slots + (uint)*p_swc_keycode * 4) = 0x96;
  return;
}


// ================= softtimer_clear_current @ 0x08007ce0 =================

void softtimer_clear_current(void)

{
  *(undefined4 *)(p_softtimer_slots + (uint)*p_swc_keydata_ref * 4) = 0;
  return;
}


// ================= softtimer_tick_all @ 0x08007cee =================

void softtimer_tick_all(void)

{
  uint uVar1;
  
  for (uVar1 = 0; uVar1 < 0x40; uVar1 = uVar1 + 1 & 0xff) {
    if (*(int *)(p_softtimer_slots + uVar1 * 4) != 0) {
      *(int *)(p_softtimer_slots + uVar1 * 4) = *(int *)(p_softtimer_slots + uVar1 * 4) + -1;
    }
  }
  return;
}


// ================= softtimer_scan_active_hi @ 0x08007d12 =================

/* softtimer_scan_active_hi � scans the 64-slot software-timer array (base ptr @0x08007f50) over
   the upper range [0x17..0x3f] for the first non-zero (still-running) slot and records its index at
   0x08007f58. Group: softtimer_arm_at_index (set slot=150), softtimer_clear_current (slot=0),
   softtimer_tick_all (decrement all 64 each tick). Shared array used by multiple subsystems (incl.
   autobaud) � the specific consumer of the upper range is not pinned down. */

void softtimer_scan_active_hi(void)

{
  uint uVar1;
  
  uVar1 = 0x17;
  while( true ) {
    if (0x3f < uVar1) {
      return;
    }
    if (*(int *)(p_softtimer_slots + uVar1 * 4) != 0) break;
    if (uVar1 == 0x3f) {
      uVar1 = 0;
    }
    else {
      if (uVar1 == 0x16) {
        *p_swc_keydata_ref = 0x16;
        return;
      }
      uVar1 = uVar1 + 1 & 0xff;
    }
  }
  *p_swc_keydata_ref = (char)uVar1;
  return;
}


// ================= util_bytes_differ @ 0x08008574 =================

undefined4 util_bytes_differ(int a,int b,int len)

{
  uint uVar1;
  
  uVar1 = 0;
  while( true ) {
    if (len <= (int)uVar1) {
      return 0;
    }
    if (*(char *)(a + uVar1) != *(char *)(b + uVar1)) break;
    uVar1 = uVar1 + 1 & 0xff;
  }
  return 1;
}


// ================= memcpy_bytes @ 0x08008598 =================

void memcpy_bytes(undefined1 *param_1,undefined1 *param_2,int param_3)

{
  uint uVar1;
  
  for (uVar1 = 0; (int)uVar1 < param_3; uVar1 = uVar1 + 1 & 0xff) {
    *param_1 = *param_2;
    param_1 = param_1 + 1;
    param_2 = param_2 + 1;
  }
  return;
}


// ================= util_wrap_index @ 0x080085c4 =================

char util_wrap_index(int base,int delta,int hi,int lo,int wrapMode)

{
  char cVar1;
  int iVar2;
  
  iVar2 = base + delta;
  if ((iVar2 < lo) || (hi < iVar2)) {
    if (iVar2 < lo) {
      cVar1 = (char)lo;
      if (wrapMode == 0) {
        cVar1 = (char)hi;
      }
    }
    else {
      cVar1 = (char)hi;
      if (wrapMode == 0) {
        cVar1 = (char)lo;
      }
    }
  }
  else {
    cVar1 = (char)base + (char)delta;
  }
  return cVar1;
}


