// app subsystem: uart  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= usart_aux_rxtx_service @ 0x0800286e =================

void usart_aux_rxtx_service(void)

{
  undefined1 rxByte;
  int iVar1;
  uint uVar2;
  
  iVar1 = usart_get_it_status(REG_USART2,0x525);
  if (iVar1 != 0) {
    rxByte = usart_receive_data(REG_USART2);
    iVar1 = p_uart_frame_buf;
    uVar2 = (*(byte *)(p_uart_frame_buf + 0x107) + 1) % 200;
    if (*(byte *)(p_uart_frame_buf + 0x108) == uVar2) {
      *p_uart_aux_rx_overflow_flag = 1;
    }
    else {
      *(undefined1 *)(p_uart_frame_buf + 0x3f + (uint)*(byte *)(p_uart_frame_buf + 0x107)) = rxByte;
      *(char *)(iVar1 + 0x107) = (char)uVar2;
    }
  }
  iVar1 = usart_get_it_status(REG_USART2,0x727);
  if (iVar1 != 0) {
    if (*(byte *)(p_uart_frame_buf + 0x3d) < *(byte *)(p_uart_frame_buf + 0x3c)) {
      usart_send_data(REG_USART2,
                      *(undefined1 *)(p_uart_frame_buf + (uint)*(byte *)(p_uart_frame_buf + 0x3d)));
      *(char *)(p_uart_frame_buf + 0x3d) = *(char *)(p_uart_frame_buf + 0x3d) + '\x01';
    }
    else {
      usart_cr_bit_write(REG_USART2,0x727,0);
      *(undefined1 *)(p_uart_frame_buf + 0x3e) = 0;
    }
  }
  return;
}


// ================= usart_init_uart1_uart3_DEAD @ 0x080029b2 =================

void usart_init_uart1_uart3_DEAD(int param_1,undefined4 param_2,undefined4 param_3)

{
  undefined1 local_24;
  undefined1 local_23;
  undefined1 local_22;
  undefined1 local_21;
  undefined4 local_20;
  undefined2 local_1c;
  undefined2 local_1a;
  undefined2 local_18;
  undefined2 local_16;
  undefined2 local_14;
  
  nvic_set_priority_group(0x300);
  if (param_1 == REG_USART1) {
    rcc_apb2_enable(0x4000);
    local_24 = 0x25;
    gpio_init_pin(REG_GPIOA,param_1 >> 0x14,0);
    gpio_init_pin(REG_GPIOA,0x200);
  }
  else if (param_1 == REG_USART3) {
    rcc_apb1_periph_clock_cmd(0x40000);
    local_24 = 0x27;
    gpio_init_pin(REG_GPIOB,0x800);
    gpio_init_pin(REG_GPIOB,0x400);
  }
  local_23 = 2;
  local_22 = 0;
  local_21 = 1;
  nvic_init(&local_24);
  local_1c = 0;
  local_1a = 0;
  local_18 = 0;
  local_14 = 0;
  local_16 = 0xc;
  local_20 = param_3;
  usart_configure_baud(param_1,&local_20);
  usart_lin_break_len_config(param_1,0x20);
  usart_lin_cmd(param_1,1);
  usart_cr_bit_write(param_1,0x525,1);
  usart_cr_bit_write(param_1,0x727,0);
  usart_cr_bit_write(param_1,0x846,1);
  usart_cmd_enable(param_1,1);
  return;
}


// ================= uart_tx_start @ 0x08003078 =================

void uart_tx_start(void)

{
  usart_cr_bit_write(REG_USART2,0x727,1);
  return;
}


// ================= uart_disable_usart2 @ 0x08003088 =================

void uart_disable_usart2(void)

{
  usart_cmd_enable(REG_USART2,0);
  usart_cr_bit_write(REG_USART2,0x727,0);
  return;
}


// ================= uart_reinit_DEAD @ 0x080030a0 =================

void uart_reinit_DEAD(void)

{
  uart_reset_state();
  usart_cmd_enable(REG_USART2,1);
  return;
}


// ================= usart_configure_baud @ 0x0800707e =================

void usart_configure_baud(int param_1,int *param_2)

{
  uint uVar1;
  undefined1 auStack_38 [8];
  int local_30;
  int local_2c;
  
  *(ushort *)(param_1 + 0x10) = *(ushort *)(param_1 + 0x10) & 0xcfff | *(ushort *)((int)param_2 + 6)
  ;
  *(ushort *)(param_1 + 0xc) =
       *(ushort *)(param_1 + 0xc) & 0xe9f3 |
       *(ushort *)(param_2 + 1) | *(ushort *)(param_2 + 2) | *(ushort *)((int)param_2 + 10);
  *(ushort *)(param_1 + 0x14) = *(ushort *)(param_1 + 0x14) & 0xfcff | *(ushort *)(param_2 + 3);
  rcc_get_clocks_freq(auStack_38);
  if (((param_1 != REG_USART1) && (param_1 != DAT_080073a0)) && (param_1 != DAT_080073a4)) {
    local_2c = local_30;
  }
  uVar1 = (uint)(local_2c * 0x19) / (uint)(*param_2 << 2);
  *(ushort *)(param_1 + 8) =
       (ushort)(uVar1 / 100 << 4) | (ushort)(((uVar1 % 100) * 0x10 + 0x32) / 100) & 0xf;
  return;
}


// ================= usart_struct_init @ 0x08007126 =================

void usart_struct_init(undefined4 *param_1)

{
  *param_1 = 0x2580;
  *(undefined2 *)(param_1 + 1) = 0;
  *(undefined2 *)((int)param_1 + 6) = 0;
  *(undefined2 *)(param_1 + 2) = 0;
  *(undefined2 *)((int)param_1 + 10) = 0xc;
  *(undefined2 *)(param_1 + 3) = 0;
  return;
}


// ================= usart_clock_init @ 0x0800713e =================

void usart_clock_init(int param_1,ushort *param_2)

{
  *(ushort *)(param_1 + 0x10) =
       *(ushort *)(param_1 + 0x10) & 0xf0ff | *param_2 | param_2[1] | param_2[2] | param_2[3];
  return;
}


// ================= usart_cmd_enable @ 0x0800716c =================

void usart_cmd_enable(int param_1,int param_2)

{
  if (param_2 == 0) {
    *(ushort *)(param_1 + 0xc) = *(ushort *)(param_1 + 0xc) & 0xdfff;
  }
  else {
    *(ushort *)(param_1 + 0xc) = *(ushort *)(param_1 + 0xc) | 0x2000;
  }
  return;
}


// ================= usart_cr_bit_write @ 0x08007184 =================

void usart_cr_bit_write(int param_1,uint param_2,int param_3)

{
  uint *puVar1;
  uint regSel;
  uint uVar2;
  
  regSel = (param_2 & 0xff) >> 5;
  uVar2 = 1 << (param_2 & 0x1f);
  if (regSel == 1) {
    puVar1 = (uint *)(param_1 + 0xc);
  }
  else if (regSel == 2) {
    puVar1 = (uint *)(param_1 + 0x10);
  }
  else {
    puVar1 = (uint *)(param_1 + 0x14);
  }
  if (param_3 == 0) {
    *puVar1 = *puVar1 & ~uVar2;
  }
  else {
    *puVar1 = *puVar1 | uVar2;
  }
  return;
}


// ================= usart_lin_break_len_config @ 0x0800721c =================

void usart_lin_break_len_config(int param_1,ushort param_2)

{
  *(ushort *)(param_1 + 0x10) = *(ushort *)(param_1 + 0x10) & 0xffdf;
  *(ushort *)(param_1 + 0x10) = *(ushort *)(param_1 + 0x10) | param_2;
  return;
}


// ================= usart_lin_cmd @ 0x0800722e =================

void usart_lin_cmd(int param_1,int param_2)

{
  if (param_2 == 0) {
    *(ushort *)(param_1 + 0x10) = *(ushort *)(param_1 + 0x10) & 0xbfff;
  }
  else {
    *(ushort *)(param_1 + 0x10) = *(ushort *)(param_1 + 0x10) | 0x4000;
  }
  return;
}


// ================= usart_send_data @ 0x08007246 =================

void usart_send_data(int param_1,int param_2)

{
  *(ushort *)(param_1 + 4) = (ushort)((uint)(param_2 << 0x17) >> 0x17);
  return;
}


// ================= usart_receive_data @ 0x0800724e =================

ushort usart_receive_data(int param_1)

{
  return *(ushort *)(param_1 + 4) & 0x1ff;
}


// ================= usart_get_it_status @ 0x08007320 =================

undefined4 usart_get_it_status(ushort *param_1,uint param_2)

{
  undefined4 uVar1;
  uint bitMask;
  uint regSel;
  
  regSel = (param_2 & 0xff) >> 5;
  bitMask = 1 << (param_2 & 0x1f);
  if (regSel == 1) {
    bitMask = bitMask & param_1[6];
  }
  else if (regSel == 2) {
    bitMask = bitMask & param_1[8];
  }
  else {
    bitMask = bitMask & param_1[10];
  }
  if ((bitMask == 0) || ((1 << ((int)param_2 >> 8 & 0xffU) & (uint)*param_1) == 0)) {
    uVar1 = 0;
  }
  else {
    uVar1 = 1;
  }
  return uVar1;
}


// ================= usart_clear_flag @ 0x08007374 =================

void usart_clear_flag(ushort *param_1,int param_2)

{
  *param_1 = ~(ushort)(1 << (param_2 >> 8 & 0xffU));
  return;
}


// ================= uart_D2_cluster_text_to_can @ 0x08007da8 =================

/* uart_D2_cluster_text_to_can — handles the 0xD2 cluster-text message from the head unit toward
   CAN. */

void uart_D2_cluster_text_to_can(undefined2 param_1)

{
  byte bVar1;
  byte bVar2;
  int iVar3;
  byte *pbVar4;
  uint uVar5;
  uint uVar6;
  
  *p_d2_cluster_state = 0;
  pbVar4 = p_tx_frame_190;
  switch(*p_cmd_block) {
  case 0:
    *p_tx_frame_190 = (*p_tx_frame_190 & 0xe0) + 0x1d;
    break;
  case 1:
  case 2:
  case 3:
    *p_tx_frame_190 = (*p_tx_frame_190 & 0xe0) + 1;
    iVar3 = p_tx_frame_18c;
    if (p_cmd_block[4] == '1') {
      uVar5 = ((byte)p_cmd_block[8] - 0x30) +
              (((byte)p_cmd_block[4] - 0x30) * 1000 & 0xffff) + ((byte)p_cmd_block[5] - 0x30) * 100
              + ((byte)p_cmd_block[6] - 0x30) * 10;
    }
    else {
      uVar5 = ((byte)p_cmd_block[8] - 0x30) +
              (((byte)p_cmd_block[5] - 0x30) * 100 & 0xffff) + ((byte)p_cmd_block[6] - 0x30) * 10;
    }
    bVar2 = (byte)((uVar5 & 0xffff) >> 8);
    *(byte *)(p_tx_frame_18c + 1) = *(byte *)(p_tx_frame_18c + 1) & 0xf0 | bVar2 & 0xf;
    bVar1 = (byte)(uVar5 & 0xffff);
    *(byte *)(iVar3 + 2) = bVar1;
    pbVar4 = p_tx_frame_9f;
    *p_tx_frame_9f = *p_tx_frame_190 & 0x1f;
    pbVar4[1] = bVar2 | 0x20;
    pbVar4[2] = bVar1;
    break;
  case 4:
  case 5:
    *p_tx_frame_190 = *p_tx_frame_190 & 0xe0;
    iVar3 = p_tx_frame_18c;
    if (p_cmd_block[4] == '1') {
      uVar5 = ((byte)p_cmd_block[7] - 0x30) +
              (((byte)p_cmd_block[4] - 0x30) * 1000 & 0xffff) + ((byte)p_cmd_block[5] - 0x30) * 100
              + ((byte)p_cmd_block[6] - 0x30) * 10;
    }
    else {
      uVar5 = ((byte)p_cmd_block[7] - 0x30) +
              (((byte)p_cmd_block[5] - 0x30) * 100 & 0xffff) + ((byte)p_cmd_block[6] - 0x30) * 10;
    }
    bVar2 = (byte)((uVar5 & 0xffff) >> 8);
    *(byte *)(p_tx_frame_18c + 1) = *(byte *)(p_tx_frame_18c + 1) & 0xf0 | bVar2 & 0xf;
    bVar1 = (byte)(uVar5 & 0xffff);
    *(byte *)(iVar3 + 2) = bVar1;
    pbVar4 = p_tx_frame_9f;
    *p_tx_frame_9f = *p_tx_frame_190 & 0x1f;
    pbVar4[1] = bVar2 | 0x20;
    pbVar4[2] = bVar1;
    break;
  case 6:
  case 7:
  case 0xd:
  case 0xe:
  case 0x15:
  case 0x16:
    p_tx_frame_190[1] = (p_tx_frame_190[1] & 0xe0) + 3;
    iVar3 = p_tx_frame_18c;
    *(byte *)(p_tx_frame_18c + 1) = *(byte *)(p_tx_frame_18c + 1) & 0xf0;
    *(undefined1 *)(iVar3 + 2) = 0;
    pbVar4 = p_tx_frame_190;
    bVar1 = *p_cmd_block;
    if (bVar1 == 0xe) {
      *p_tx_frame_190 = (*p_tx_frame_190 & 0xe0) + 4;
    }
    else if (bVar1 < 0xf) {
      if (bVar1 == 6) {
        *p_tx_frame_190 = (*p_tx_frame_190 & 0xe0) + 4;
      }
      else if (bVar1 == 7) {
        *p_tx_frame_190 = (*p_tx_frame_190 & 0xe0) + 4;
        pbVar4[1] = (pbVar4[1] & 0xe0) + 8;
      }
      else if (bVar1 == 0xd) {
        *p_tx_frame_190 = (*p_tx_frame_190 & 0xe0) + 4;
      }
    }
    else if (bVar1 == 0x15) {
      *p_tx_frame_190 = (*p_tx_frame_190 & 0xe0) + 4;
    }
    else if (bVar1 == 0x16) {
      *p_tx_frame_190 = (*p_tx_frame_190 & 0xe0) + 4;
    }
    iVar3 = p_tx_frame_18c;
    if ((0x2f < p_cmd_block[1]) && (p_cmd_block[1] < 0x3a)) {
      uVar6 = (p_cmd_block[3] - 0x30) +
              ((p_cmd_block[1] - 0x30) * 100 & 0xffff) + (p_cmd_block[2] - 0x30) * 10;
      uVar5 = uVar6 & 0xffff;
      param_1 = (undefined2)uVar6;
      *(byte *)(p_tx_frame_18c + 1) =
           *(byte *)(p_tx_frame_18c + 1) & 0xf0 | (byte)(uVar5 >> 8) & 0xf;
      *(char *)(iVar3 + 2) = (char)uVar5;
    }
    pbVar4 = p_tx_frame_9f;
    *p_tx_frame_9f = *p_tx_frame_190 & 0x1f;
    pbVar4[1] = (byte)((ushort)param_1 >> 8);
    pbVar4[2] = (byte)param_1;
    break;
  case 8:
    break;
  case 9:
    break;
  case 10:
    *p_tx_frame_190 = (*p_tx_frame_190 & 0xe0) + 8;
    break;
  case 0xb:
    break;
  case 0xc:
    *p_tx_frame_190 = (*p_tx_frame_190 & 0xe0) + 6;
    *p_tx_frame_9f = *pbVar4 & 0x1f;
    break;
  case 0xf:
    break;
  case 0x10:
    break;
  case 0x11:
    break;
  case 0x12:
    break;
  case 0x13:
    break;
  case 0x14:
    break;
  case 0x17:
  }
  return;
}


// ================= uart_reset_state @ 0x08008a18 =================

void uart_reset_state(void)

{
  int iVar1;
  uint uVar2;
  
  iVar1 = p_uart_frame_buf;
  *(undefined1 *)(p_uart_frame_buf + 0x3c) = 0;
  *(undefined1 *)(iVar1 + 0x3d) = 0;
  *(undefined1 *)(iVar1 + 0x3e) = 0;
  *(undefined1 *)(iVar1 + 0x107) = *(undefined1 *)(iVar1 + 0x108);
  *(undefined1 *)(p_uart_ack_queue + 10) = *(undefined1 *)(p_uart_ack_queue + 0xb);
  *p_uart_tx_slot_idx = 6;
  for (uVar2 = 0; uVar2 < 6; uVar2 = uVar2 + 1 & 0xff) {
    *(undefined1 *)(p_uart_slot_state + uVar2) = 0;
  }
  return;
}


// ================= uart_init_38400_and_version @ 0x08008a54 =================

void uart_init_38400_and_version(void)

{
  uart_reset_state();
  usart2_init_wrapper(0x9600);
  memcpy_bytes(DAT_08008e2c,s_H1N4JPF20A_220524_08008e18,0x11);
  return;
}


// ================= uart_rx_dispatch @ 0x08008a96 =================

/* uart_rx_dispatch — dispatches received head-unit commands. 0xFC->0xFD handshake; 0xE0 ->
   wwdg_force_reset. No 0xFF handler: 0xFF is an ACK, the box cannot be polled, only listened to. */

void uart_rx_dispatch(void)

{
  undefined1 *puVar1;
  int iVar2;
  uint uVar3;
  
  for (uVar3 = 0; puVar1 = p_uart_frame_buf, uVar3 < 2; uVar3 = uVar3 + 1 & 0xff) {
    if (((*(char *)(p_uart_rx_frame + 3) == *(char *)(DAT_08008e38 + uVar3 * 0xc)) &&
        (*(char *)(p_uart_rx_frame + 2) == *(char *)(DAT_08008e38 + uVar3 * 0xc + 1))) &&
       (iVar2 = util_bytes_differ(*(undefined4 *)(DAT_08008e38 + uVar3 * 0xc + 4),
                                  p_uart_rx_frame + 4,
                                  *(undefined1 *)(DAT_08008e38 + uVar3 * 0xc + 1)), iVar2 != 0)) {
      memcpy_bytes(*(undefined4 *)(DAT_08008e38 + uVar3 * 0xc + 4),p_uart_rx_frame + 4,
                   *(undefined1 *)(DAT_08008e38 + uVar3 * 0xc + 1));
      *(undefined1 *)
       (*(int *)(DAT_08008e38 + uVar3 * 0xc + 4) + (uint)*(byte *)(DAT_08008e38 + uVar3 * 0xc + 1))
           = 1;
    }
  }
  if (((((*(char *)(p_uart_rx_frame + 3) == -4) && (*(char *)(p_uart_rx_frame + 4) == '?')) &&
       ((*(char *)(p_uart_rx_frame + 5) == -0xd &&
        ((*(char *)(p_uart_rx_frame + 6) == -0xf && (*(char *)(p_uart_rx_frame + 7) == '\x01'))))))
      && (*(char *)(p_uart_rx_frame + 10) == ':')) &&
     ((*(char *)(p_uart_rx_frame + 0xb) == -0x5d && (p_uart_frame_buf[0x3e] == '\0')))) {
    *p_uart_frame_buf = 0xaa;
    puVar1[1] = 0x55;
    memcpy_bytes(p_uart_frame_buf + 2,p_uart_rx_frame + 2,0xb);
    puVar1 = p_uart_frame_buf;
    p_uart_frame_buf[3] = 0xfd;
    puVar1[0xd] = *(char *)(p_uart_rx_frame + 0xd) + '\x01';
    puVar1[0x3d] = 0;
    puVar1[0x3c] = 0xe;
    puVar1[0x3e] = 1;
    uart_tx_start();
  }
  if (*(char *)(p_uart_rx_frame + 3) == -0x20) {
    wwdg_force_reset_thunk();
  }
  return;
}


// ================= uart_rx_parser_AA55 @ 0x08008bb2 =================

/* uart_rx_parser_AA55 — parses inbound Hiworld frames: AA 55 | LEN | CMD | payload[LEN] | CKS,
   CKS = sum(all bytes) & 0xFF. */

void uart_rx_parser_AA55(void)

{
  byte bVar1;
  byte bVar2;
  int iVar3;
  uint uVar4;
  uint uVar5;
  uint unaff_r6;
  uint uVar6;
  uint unaff_r7;
  
  bVar2 = *(byte *)(p_uart_frame_buf + 0x107);
  uVar4 = (uint)*(byte *)(p_uart_frame_buf + 0x108);
  uVar5 = 0;
  do {
    while( true ) {
      while( true ) {
        while( true ) {
          iVar3 = p_uart_rx_frame;
          if (bVar2 == uVar4) {
            return;
          }
          if (uVar5 != 0) break;
          uVar5 = (uint)(*(char *)(p_uart_frame_buf + 0x3f + uVar4) == -0x56);
          uVar4 = (uVar4 + 1) % 200;
          if (uVar5 == 0) {
            *(char *)(p_uart_frame_buf + 0x108) = (char)uVar4;
          }
        }
        if (uVar5 != 1) break;
        if (*(char *)(p_uart_frame_buf + 0x3f + uVar4) == 'U') {
          uVar5 = 2;
        }
        else {
          uVar5 = 0;
        }
        uVar4 = (uVar4 + 1) % 200;
        if (uVar5 == 0) {
          *(char *)(p_uart_frame_buf + 0x108) = (char)uVar4;
        }
      }
      if (uVar5 != 2) break;
      bVar1 = *(byte *)(p_uart_frame_buf + 0x3f + uVar4);
      uVar6 = (uint)bVar1;
      if (uVar6 < 0x38) {
        if (((int)((bVar2 - uVar4) + 200) % 200 & 0xffU) < uVar6 + 2) {
          return;
        }
        *(byte *)(p_uart_rx_frame + 2) = bVar1;
        uVar5 = 3;
        unaff_r6 = uVar6 + 4 & 0xff;
        unaff_r7 = uVar6;
      }
      else {
        uVar5 = 0;
        unaff_r6 = uVar6;
      }
      uVar4 = (uVar4 + 1) % 200;
      if (uVar5 == 0) {
        *(char *)(p_uart_frame_buf + 0x108) = (char)uVar4;
      }
    }
    *(undefined1 *)(p_uart_rx_frame + uVar5) = *(undefined1 *)(p_uart_frame_buf + 0x3f + uVar4);
    if ((int)uVar5 < (int)unaff_r6) {
      unaff_r7 = *(byte *)(iVar3 + uVar5) + unaff_r7 & 0xff;
      uVar5 = uVar5 + 1 & 0xff;
    }
    else {
      unaff_r7 = unaff_r7 - 1 & 0xff;
      if (*(byte *)(p_uart_rx_frame + uVar5) == unaff_r7) {
        uart_rx_dispatch();
      }
      uVar5 = 0;
    }
    uVar4 = (uVar4 + 1) % 200;
  } while (uVar5 != 0);
  *(char *)(p_uart_frame_buf + 0x108) = (char)uVar4;
  return;
}


// ================= uart_tx_ack_frame @ 0x08008cce =================

void uart_tx_ack_frame(char param_1,char param_2)

{
  undefined1 *puVar1;
  
  puVar1 = p_uart_frame_buf;
  *p_uart_frame_buf = 0xaa;
  puVar1[1] = 0x55;
  puVar1[2] = 1;
  puVar1[3] = param_1;
  puVar1[4] = param_2;
  puVar1[5] = param_1 + param_2;
  puVar1[0x3c] = 6;
  puVar1[0x3d] = 0;
  puVar1[0x3e] = 1;
  uart_tx_start();
  return;
}


// ================= uart_build_frame_AA55 @ 0x08008d04 =================

/* uart_build_frame_AA55 — assembles an outbound AA55 frame and appends CKS = sum(all bytes) &
   0xFF. */

void uart_build_frame_AA55(int param_1)

{
  char cVar1;
  undefined1 *puVar2;
  uint uVar3;
  uint uVar4;
  
  memcpy_bytes(p_uart_frame_buf + 4,*(undefined4 *)(DAT_08008e30 + param_1 * 0xc + 4),
               *(undefined1 *)(DAT_08008e30 + param_1 * 0xc + 1));
  puVar2 = p_uart_frame_buf;
  cVar1 = *(char *)(DAT_08008e30 + param_1 * 0xc + 1);
  *p_uart_frame_buf = 0xaa;
  puVar2[1] = 0x55;
  puVar2[2] = cVar1;
  puVar2 = p_uart_frame_buf;
  p_uart_frame_buf[3] = *(undefined1 *)(DAT_08008e30 + param_1 * 0xc);
  uVar3 = (uint)(byte)(cVar1 + 4);
  puVar2[uVar3] = 0;
  for (uVar4 = 2; puVar2 = p_uart_frame_buf, uVar4 < uVar3; uVar4 = uVar4 + 1 & 0xff) {
    p_uart_frame_buf[uVar3] = p_uart_frame_buf[uVar3] + p_uart_frame_buf[uVar4];
  }
  p_uart_frame_buf[uVar3] = p_uart_frame_buf[uVar3] + -1;
  puVar2[uVar3] = puVar2[uVar3];
  puVar2[0x3d] = 0;
  puVar2[0x3c] = cVar1 + '\x05';
  puVar2[0x3e] = 1;
  uart_tx_start();
  return;
}


// ================= uart_tx_round_robin @ 0x08008d9c =================

/* uart_tx_round_robin — walks the periodic status-frame table and transmits one entry per turn.
    */

void uart_tx_round_robin(void)

{
  byte bVar1;
  int iVar2;
  byte *pbVar3;
  byte bVar4;
  undefined4 in_r3;
  int iVar5;
  
  bVar1 = *p_uart_tx_slot_idx;
  do {
    iVar2 = DAT_08008e30;
    iVar5 = DAT_08008e30 + (uint)*p_uart_tx_slot_idx * 0xc;
    iVar5 = util_bytes_differ(*(undefined4 *)(iVar5 + 8),
                              *(undefined4 *)(DAT_08008e30 + (uint)*p_uart_tx_slot_idx * 0xc + 4),
                              *(undefined1 *)(DAT_08008e30 + (uint)*p_uart_tx_slot_idx * 0xc + 1),
                              iVar5,in_r3);
    if (iVar5 != 0) {
      memcpy_bytes(*(undefined4 *)(iVar2 + (uint)*p_uart_tx_slot_idx * 0xc + 8),
                   *(undefined4 *)(iVar2 + (uint)*p_uart_tx_slot_idx * 0xc + 4),
                   *(undefined1 *)(iVar2 + (uint)*p_uart_tx_slot_idx * 0xc + 1));
      return;
    }
    in_r3 = 0;
    bVar4 = util_wrap_index(*p_uart_tx_slot_idx,1,5,0);
    pbVar3 = p_uart_tx_slot_idx;
    *p_uart_tx_slot_idx = bVar4;
  } while (*pbVar3 != bVar1);
  *pbVar3 = 6;
  return;
}


// ================= uart_tx_sched @ 0x08008e62 =================

/* uart_tx_sched — UART transmit scheduler entry; feeds uart_tx_round_robin. */

void uart_tx_sched(void)

{
  byte *pbVar1;
  undefined1 *puVar2;
  undefined1 uVar3;
  uint uVar4;
  undefined4 in_r3;
  
  *p_uart_tx_tick = *p_uart_tx_tick + 1;
  if (*(char *)(p_uart_frame_buf + 0x3e) == '\0') {
    if (*(char *)(p_uart_ack_queue + 0xb) != *(char *)(p_uart_ack_queue + 10)) {
      if ((*(char *)(p_uart_ack_queue + (uint)*(byte *)(p_uart_ack_queue + 0xb)) != -2) &&
         (*(char *)(p_uart_ack_queue + (uint)*(byte *)(p_uart_ack_queue + 0xb)) != -1)) {
        uart_tx_ack_frame(0xff,*(undefined1 *)
                                (p_uart_ack_queue + (uint)*(byte *)(p_uart_ack_queue + 0xb)),
                          p_uart_ack_queue,in_r3,in_r3);
        *p_uart_tx_tick = *p_uart_tx_tick - 1;
      }
      uVar4 = *(byte *)(p_uart_ack_queue + 0xb) + 1;
      *(char *)(p_uart_ack_queue + 0xb) = (char)uVar4 + (char)(uVar4 / 10) * -10;
    }
    if (5 < *p_uart_tx_tick) {
      *p_uart_tx_tick = 0;
      if (5 < *p_uart_tx_slot_idx) {
        *p_uart_tx_slot_idx = 0;
        *(undefined1 *)(p_uart_slot_state + (uint)*p_uart_tx_slot_idx) = 0;
      }
      if ((*(char *)(p_uart_slot_state + (uint)*p_uart_tx_slot_idx) == '\x03') ||
         (*(char *)(p_uart_slot_state + (uint)*p_uart_tx_slot_idx) == '\x02')) {
        *(undefined1 *)(p_uart_slot_state + (uint)*p_uart_tx_slot_idx) = 0;
        pbVar1 = p_uart_tx_slot_idx;
        *p_uart_tx_slot_idx = *p_uart_tx_slot_idx + 1;
        if (5 < *pbVar1) {
          *pbVar1 = 0;
          *(undefined1 *)(p_uart_slot_state + (uint)*p_uart_tx_slot_idx) = 0;
        }
      }
      if (*(char *)(p_uart_slot_state + (uint)*p_uart_tx_slot_idx) == '\x01') {
        memcpy_bytes(*(undefined4 *)(DAT_08009004 + (uint)*p_uart_tx_slot_idx * 0xc + 8),
                     *(undefined4 *)(DAT_08009004 + (uint)*p_uart_tx_slot_idx * 0xc + 4),
                     *(undefined1 *)(DAT_08009004 + (uint)*p_uart_tx_slot_idx * 0xc + 1));
        uart_build_frame_AA55(*p_uart_tx_slot_idx);
        *(undefined1 *)(p_uart_slot_state + (uint)*p_uart_tx_slot_idx) = 3;
      }
      else {
        if (*(char *)(p_uart_slot_state + (uint)*p_uart_tx_slot_idx) == '\0') {
          uart_tx_round_robin();
        }
        if (*p_uart_tx_slot_idx < 6) {
          uart_build_frame_AA55(*p_uart_tx_slot_idx);
          *(undefined1 *)(p_uart_slot_state + (uint)*p_uart_tx_slot_idx) = 3;
        }
        else if (*p_uart_idle_delay == '\0') {
          *p_uart_idle_delay = '\x0f';
          uVar3 = util_wrap_index(*p_uart_aux_slot_idx,1,5,0,0);
          puVar2 = p_uart_aux_slot_idx;
          *p_uart_aux_slot_idx = uVar3;
          uart_build_frame_AA55(*puVar2);
        }
        else {
          *p_uart_idle_delay = *p_uart_idle_delay + -1;
        }
      }
    }
  }
  return;
}


// ================= uart_rx_tx_service @ 0x08008fe2 =================

void uart_rx_tx_service(void)

{
  uart_rx_parser_AA55();
  uart_tx_sched();
  return;
}


