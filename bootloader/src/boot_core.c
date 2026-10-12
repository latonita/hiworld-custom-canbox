// bootloader subsystem: boot_core  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= DelaySpinLoopA @ 0x08000188 =================

void DelaySpinLoopA(uint param_1)

{
  char cVar1;
  
  for (; 0 < (int)param_1; param_1 = param_1 - 1 & 0xff) {
    for (cVar1 = -0x7e; cVar1 != '\0'; cVar1 = cVar1 + -1) {
    }
  }
  return;
}


// ================= DelaySpinLoop @ 0x0800019a =================

/* DelaySpinLoop - innermost calibrated busy-wait spin.
   Algorithm: decrement loop
   Parameters: r0=count
   Returns: void */

void DelaySpinLoop(uint param_1)

{
  char cVar1;
  
  for (; 0 < (int)param_1; param_1 = param_1 - 1 & 0xff) {
    for (cVar1 = -0x7e; cVar1 != '\0'; cVar1 = cVar1 + -1) {
    }
  }
  return;
}


// ================= DelayMs @ 0x080001a0 =================

/* DelayMs - coarse busy-wait delay (calls DelaySpinLoop(100) param_1 times).
   Algorithm: nested busy loops
   Parameters: r0=count
   Returns: void */

void DelayMs(uint param_1)

{
  int extraout_r3;
  
  while (0 < (int)param_1) {
    DelaySpinLoopA(100);
    param_1 = extraout_r3 - 1U & 0xff;
  }
  return;
}


// ================= GetPeripheralClock @ 0x080001b6 =================

/* GetPeripheralClock - compute current PCLK (from RCC PLL/HSE/prescalers) for baud/timing.
   Algorithm: read RCC CFGR, derive sysclk then APB clock
   Parameters: r0=out struct
   Returns: clock Hz */

void GetPeripheralClock(uint *param_1)

{
  uint uVar1;
  uint uVar2;
  int iVar3;
  uint uVar4;
  
  uVar2 = *(uint *)(REG_RCC + 4) & DAT_080005c4;
  uVar4 = (*(uint *)(REG_RCC + 0x24) & 0x7f) >> 4;
  if ((uVar2 & 0x8000000) == 0) {
    iVar3 = (uVar2 >> 0x12) + 2;
  }
  else {
    iVar3 = (uVar2 >> 0x12) - 0x1ef;
  }
  if ((*(uint *)(REG_RCC + 4) & 0x10000) == 0) {
    if ((*(uint *)(REG_RCC + 0x40) & 1) == 0) {
      uVar2 = iVar3 * DAT_080005cc;
    }
    else {
      uVar2 = iVar3 * DAT_080005c8;
    }
  }
  else if ((*(uint *)(REG_RCC + 4) & 0x20000) == 0) {
    uVar2 = iVar3 * DAT_080005cc;
  }
  else {
    uVar2 = iVar3 * DAT_080005c8;
  }
  if ((*(uint *)(REG_RCC + 0x40) & 2) != 0) {
    uVar2 = uVar2 >> 1;
  }
  uVar1 = *(uint *)(REG_RCC + 4) & 0xc;
  if (uVar1 == 0) {
    *param_1 = *(uint *)(DAT_080005d0 + uVar4 * 4);
  }
  else if (uVar1 == 4) {
    *param_1 = DAT_080005cc;
  }
  else if (uVar1 == 8) {
    *param_1 = DAT_080005cc;
  }
  else if (uVar1 == 0xc) {
    *param_1 = uVar2;
  }
  else {
    *param_1 = *(uint *)(DAT_080005d0 + uVar4 * 4);
  }
  param_1[1] = *param_1 >> *(sbyte *)(DAT_080005d4 + ((*(uint *)(REG_RCC + 4) & 0xf0) >> 4));
  param_1[2] = param_1[1] >> *(sbyte *)(DAT_080005d4 + ((*(uint *)(REG_RCC + 4) & 0x700) >> 8));
  param_1[3] = param_1[1] >> *(sbyte *)(DAT_080005d4 + ((*(uint *)(REG_RCC + 4) & 0x3800) >> 0xb));
  param_1[5] = param_1[1] / (uint)*(byte *)(DAT_080005d8 + (*(uint *)(REG_RCC + 0x2c) & 0xf));
  param_1[4] = uVar2 / *(ushort *)
                        (DAT_080005dc + ((*(uint *)(REG_RCC + 0x2c) & 0x1f0) >> 4 & 0xf) * 2);
  return;
}


// ================= ResetViaWatchdog @ 0x080002e4 =================

/* ResetViaWatchdog - force an immediate MCU reset by driving WWDG_CR=0xBF in an infinite loop
   (never returns). Used after a successful IAP flash to reboot into the new app.
   Algorithm: config then loop WWDG_CR=0xBF
   Parameters: none
   Returns: never */

void ResetViaWatchdog(void)

{
  uint *puVar1;
  
  *(uint *)(REG_RCC + 0x24) = *(uint *)(REG_RCC + 0x24) | 0x1000000;
  *(uint *)(REG_RCC + 0x24) = *(uint *)(REG_RCC + 0x24) & 0xfeffffff;
  *(uint *)(REG_RCC + 0x1c) = *(uint *)(REG_RCC + 0x1c) | 0x800;
  puVar1 = DAT_080005e0;
  *DAT_080005e0 = *DAT_080005e0 & 0xfffffe7f | 0x100;
  *puVar1 = *puVar1 & 0xffffff80 | 0x7f;
  puVar1[-1] = 0xff;
  do {
    DAT_080005e0[-1] = 0xbf;
  } while( true );
}


// ================= SetGpioPinConfig @ 0x0800034a =================

/* SetGpioPinConfig - low-level GPIO register programming (MODER/AFR/OTYPER/PUPDR) for a pin mask.
   Algorithm: per-bit apply mode/AF
   Parameters: r0=GPIO base, r1=config
   Returns: void */

uint SetGpioPinConfig(uint *param_1,ushort *param_2)

{
  uint *puVar1;
  uint *puVar2;
  uint uVar3;
  uint uVar4;
  
  uVar3 = 0;
  for (uVar4 = 0; (int)(uint)*param_2 >> (uVar4 & 0xff) != 0; uVar4 = uVar4 + 1) {
    if (((uint)*param_2 & 1 << (uVar4 & 0xff)) != 0) {
      uVar3 = uVar4 << 1;
      if ((((*(int *)(param_2 + 4) == 2) || (*(int *)(param_2 + 4) == 0x12)) ||
          (*(int *)(param_2 + 4) == 0)) || (*(int *)(param_2 + 4) == 3)) {
        if ((uVar4 & 8) == 0) {
          param_1[8] = param_1[8] & ~(0xf << ((uVar4 & 7) << 2)) |
                       *(int *)(param_2 + 6) << ((uVar4 & 7) << 2);
        }
        else {
          param_1[9] = param_1[9] & ~(0xf << ((uVar4 & 7) << 2)) |
                       *(int *)(param_2 + 6) << ((uVar4 & 7) << 2);
        }
      }
      *param_1 = *param_1 & ~(3 << (uVar3 & 0xff)) | ((byte)param_2[4] & 0xf) << (uVar3 & 0xff);
      param_1[3] = param_1[3] & ~(3 << (uVar3 & 0xff)) | ((byte)param_2[2] & 3) << (uVar3 & 0xff);
      if (((param_2[4] & 3) != 0) && (*(int *)(param_2 + 4) != 3)) {
        param_1[0xb] = param_1[0xb] & ~(3 << (uVar3 & 0xff)) |
                       ((byte)param_2[1] & 3) << (uVar3 & 0xff);
      }
      uVar3 = param_1[2] & ~(1 << (uVar4 & 0xff)) |
              (*(byte *)((int)param_2 + 3) & 1) << (uVar4 & 0xff);
      param_1[2] = uVar3;
      if ((char)param_2[2] == '\x02') {
        param_1[10] = param_1[10] | 1 << (uVar4 & 0xff);
      }
      else if ((char)param_2[2] == '\x01') {
        param_1[6] = param_1[6] | 1 << (uVar4 & 0xff);
      }
      if (((*(int *)(param_2 + 4) == 1) || (*(int *)(param_2 + 4) == 2)) ||
         ((*(int *)(param_2 + 4) == 0x11 || (*(int *)(param_2 + 4) == 0x12)))) {
        uVar3 = param_1[1] & ~(1 << (uVar4 & 0xff)) |
                (((byte)param_2[4] & 0x1f) >> 4) << (uVar4 & 0xff);
        param_1[1] = uVar3;
      }
      puVar1 = REG_EXTI;
      if ((*(uint *)(param_2 + 4) & 0x10000000) != 0) {
        uVar3 = *REG_EXTI & ~(1 << (uVar4 & 0xff));
        if ((*(uint *)(param_2 + 4) & 0x10000) == 0x10000) {
          uVar3 = uVar3 | 1 << (uVar4 & 0xff);
        }
        *REG_EXTI = uVar3;
        puVar2 = REG_EXTI;
        uVar3 = puVar1[1] & ~(1 << (uVar4 & 0xff));
        if ((*(uint *)(param_2 + 4) & 0x20000) == 0x20000) {
          uVar3 = uVar3 | 1 << (uVar4 & 0xff);
        }
        REG_EXTI[1] = uVar3;
        puVar1 = REG_EXTI;
        uVar3 = puVar2[2] & ~(1 << (uVar4 & 0xff));
        if ((*(uint *)(param_2 + 4) & 0x100000) == 0x100000) {
          uVar3 = uVar3 | 1 << (uVar4 & 0xff);
        }
        REG_EXTI[2] = uVar3;
        uVar3 = puVar1[3] & ~(1 << (uVar4 & 0xff));
        if ((*(uint *)(param_2 + 4) & 0x200000) == 0x200000) {
          uVar3 = uVar3 | 1 << (uVar4 & 0xff);
        }
        REG_EXTI[3] = uVar3;
      }
    }
  }
  return uVar3;
}


// ================= SetGpioAltFunction @ 0x0800052e =================

/* SetGpioAltFunction - configure one GPIO pin's alternate function (wrapper over SetGpioPinConfig).
   Algorithm: build config, call SetGpioPinConfig
   Parameters: r0=port, r1=pin mask, r2=AF/mode
   Returns: void */

undefined8 SetGpioAltFunction(undefined4 param_1,undefined4 param_2,int param_3)

{
  undefined2 local_20;
  undefined2 uStack_1e;
  undefined4 local_1c;
  int local_18;
  undefined4 local_14;
  
  uStack_1e = (undefined2)((uint)param_1 >> 0x10);
  local_20 = (undefined2)param_2;
  local_1c = CONCAT31((int3)((uint)param_2 >> 8),1);
  if (param_3 == 2) {
    local_14 = 4;
  }
  else {
    local_14 = 0xf;
  }
  local_18 = param_3;
  SetGpioPinConfig(param_1,&local_20);
  return CONCAT44(local_1c,CONCAT22(uStack_1e,local_20));
}


// ================= SetUsartRegisters @ 0x0800055a =================

/* SetUsartRegisters - program USART CR1/CR2/CR3 and BRR from GetPeripheralClock.
   Algorithm: compute BRR=pclk/baud; set word length/stop/parity
   Parameters: r0=USART base, r1=config struct
   Returns: void */

void SetUsartRegisters(int param_1,int *param_2)

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
  GetPeripheralClock(auStack_38);
  if (((param_1 == REG_USART1) || (param_1 == DAT_080005ec)) || (param_1 == DAT_080005f0)) {
    local_30 = local_2c;
  }
  uVar1 = (uint)(local_30 * 0x19) / (uint)(*param_2 << 2);
  *(ushort *)(param_1 + 8) =
       (ushort)(uVar1 / 100 << 4) | (ushort)(((uVar1 % 100) * 0x10 + 0x32) / 100) & 0xf;
  return;
}


// ================= InitializeUsart2 @ 0x08000630 =================

/* InitializeUsart2 - bring up USART2 (PA2/PA3 AF) at the given baud (0x9600=38400 8N1).
   Algorithm: SetGpioAltFunction PA2/PA3; SetUsartRegisters; enable USART
   Parameters: r0=baud
   Returns: void */

undefined8 InitializeUsart2(undefined4 param_1,undefined4 param_2,undefined4 param_3,uint param_4)

{
  undefined4 local_18;
  undefined2 local_14;
  undefined2 local_12;
  undefined4 local_10;
  uint local_c;
  
  local_14 = (undefined2)param_2;
  local_12 = (undefined2)((uint)param_2 >> 0x10);
  local_18 = param_1;
  local_10 = param_3;
  local_c = param_4;
  SetGpioAltFunction(p_GPIOA,8,2);
  SetGpioAltFunction(p_GPIOA,4,2);
  local_14 = 0;
  local_12 = 0;
  local_c = local_c & 0xffff0000;
  local_10 = 0xc0000;
  local_18 = param_1;
  SetUsartRegisters(p_USART2_A,&local_18);
  *(ushort *)(p_USART2_A + 0xc) = *(ushort *)(p_USART2_A + 0xc) | 0x2000;
  return CONCAT26(local_12,CONCAT24(local_14,local_18));
}


// ================= SendUartByte @ 0x0800067c =================

/* SendUartByte - blocking USART2 TX with ~30000-iteration timeout.
   Algorithm: DR=byte; wait TXE(SR&0x40) or timeout
   Parameters: r0=byte
   Returns: bool (true=timed out) */

bool SendUartByte(ushort param_1,undefined4 param_2,undefined4 param_3)

{
  undefined4 extraout_r1;
  undefined4 extraout_r2;
  int iVar1;
  int extraout_r3;
  
  p_USART2_A[2] = param_1;
  iVar1 = 30000;
  while (((*p_USART2_A & 0x40) == 0 && (iVar1 != 0))) {
    DelaySpinLoopA(10,param_2,param_3,iVar1 + -1);
    iVar1 = extraout_r3;
    param_2 = extraout_r1;
    param_3 = extraout_r2;
  }
  return iVar1 == 0;
}


// ================= ReceiveUartByte @ 0x080006ac =================

/* ReceiveUartByte - blocking USART2 RX into buf[idx], fixed ~60000 timeout; buf[idx]=0 on timeout.
   Algorithm: wait RXNE(SR&0x20) or timeout; store DR
   Parameters: r0=buffer index
   Returns: bool (true=timed out) */

bool ReceiveUartByte(int param_1)

{
  int extraout_r3;
  int iVar1;
  
  for (iVar1 = 60000; ((*p_USART2_A & 0x20) == 0 && (iVar1 != 0)); iVar1 = iVar1 + -1) {
    DelaySpinLoopA(1);
    param_1 = extraout_r3;
  }
  if (iVar1 == 0) {
    *(undefined1 *)(p_rxRecord + param_1) = 0;
  }
  else {
    *(char *)(p_rxRecord + param_1) = (char)p_USART2_A[2];
  }
  return iVar1 == 0;
}


// ================= ReceiveUartByteTimeout @ 0x080006e6 =================

/* ReceiveUartByteTimeout - like ReceiveUartByte but configurable timeout (global DAT_08000a38).
   Algorithm: wait RXNE or timeout; store DR at buf[idx]
   Parameters: r0=buffer index
   Returns: bool (true=timed out) */

bool ReceiveUartByteTimeout(int param_1)

{
  int extraout_r3;
  int iVar1;
  
  for (iVar1 = g_rxTimeout; ((*p_USART2_A & 0x20) == 0 && (iVar1 != 0)); iVar1 = iVar1 + -1) {
    DelaySpinLoopA(1);
    param_1 = extraout_r3;
  }
  if (iVar1 == 0) {
    *(undefined1 *)(p_rxRecord + param_1) = 0;
  }
  else {
    *(char *)(p_rxRecord + param_1) = (char)p_USART2_A[2];
  }
  return iVar1 == 0;
}


// ================= EnableHsiClock @ 0x0800071e =================

/* EnableHsiClock - turn on HSI (RCC HSION) and wait HSIRDY. HSI is required for flash
   program/erase.
   Parameters: none
   Returns: status */

undefined8 EnableHsiClock(void)

{
  undefined4 uVar1;
  int iVar2;
  undefined4 local_8;
  
  iVar2 = 0;
  local_8 = 0;
  uVar1 = 0;
  if ((*p_RCC_A & 2) == 0) {
    *p_RCC_A = *p_RCC_A | 1;
    do {
      local_8 = local_8 + 1;
      if ((int)(*p_RCC_A << 0x1e) < 0) break;
    } while (local_8 != 0x500);
    iVar2 = -((int)(*p_RCC_A << 0x1e) >> 0x1f);
    if (iVar2 == 0) {
      uVar1 = 1;
    }
  }
  return CONCAT44(iVar2,uVar1);
}


// ================= GetFlashStatus @ 0x08000766 =================

/* GetFlashStatus - decode FLASH_STS into a status code (1=busy,3/4/5=err,6=ok,7=wrp).
   Algorithm: read FLASH SR error/busy bits
   Parameters: none
   Returns: status code */

undefined4 GetFlashStatus(void)

{
  undefined4 uVar1;
  
  if ((*(uint *)(p_FLASH_A + 0xc) & 1) == 0) {
    if ((*(uint *)(p_FLASH_A + 0xc) & 4) == 0) {
      if ((*(uint *)(p_FLASH_A + 0xc) & 8) == 0) {
        if ((*(uint *)(p_FLASH_A + 0xc) & 0x10) == 0) {
          if ((*(uint *)(p_FLASH_A + 0xc) & 0x40) == 0) {
            uVar1 = 6;
          }
          else {
            uVar1 = 7;
          }
        }
        else {
          uVar1 = 5;
        }
      }
      else {
        uVar1 = 4;
      }
    }
    else {
      uVar1 = 3;
    }
  }
  else {
    uVar1 = 1;
  }
  return uVar1;
}


// ================= WaitFlashReady @ 0x080007b2 =================

/* WaitFlashReady - poll GetFlashStatus while busy up to a timeout.
   Algorithm: loop GetFlashStatus; return 10 on timeout
   Parameters: r0=timeout
   Returns: final status */

int WaitFlashReady(undefined4 param_1,undefined4 param_2)

{
  int iVar1;
  int extraout_r2;
  int extraout_r2_00;
  int iVar2;
  
  iVar1 = GetFlashStatus(param_1,param_2,param_1,6);
  iVar2 = extraout_r2;
  while ((iVar1 == 1 && (iVar2 != 0))) {
    iVar1 = GetFlashStatus();
    iVar2 = extraout_r2_00 + -1;
  }
  if (iVar2 == 0) {
    iVar1 = 10;
  }
  return iVar1;
}


// ================= ProgramFlashWord @ 0x080007d8 =================

/* ProgramFlashWord - word-aligned flash program of one 32-bit word.
   Algorithm: check align; set PG; *addr=word; WaitFlashReady; clear PG
   Parameters: r0=addr, r1=word
   Returns: status (9=misaligned) */

int ProgramFlashWord(undefined4 *param_1,undefined4 param_2)

{
  int iVar1;
  
  if (((uint)param_1 & 3) == 0) {
    *(uint *)(p_FLASH_A + 0xc) = *(uint *)(p_FLASH_A + 0xc) | 0x7c;
    iVar1 = WaitFlashReady(0x2000);
    if (iVar1 == 6) {
      *(uint *)(p_FLASH_A + 0x10) = *(uint *)(p_FLASH_A + 0x10) | 1;
      *param_1 = param_2;
      iVar1 = WaitFlashReady(0x2000);
      *(uint *)(p_FLASH_A + 0x10) = *(uint *)(p_FLASH_A + 0x10) & 0x3ffe;
    }
  }
  else {
    iVar1 = 9;
  }
  return iVar1;
}


// ================= EraseFlashPage @ 0x08000834 =================

/* EraseFlashPage - page erase (2KB) of the given flash address.
   Algorithm: set PER; WaitFlashReady; FLASH_AR=addr; set START; WaitFlashReady; clear PER
   Parameters: r0=page address
   Returns: status */

int EraseFlashPage(undefined4 param_1)

{
  int iVar1;
  int iVar2;
  
  *(uint *)(p_FLASH_A + 0xc) = *(uint *)(p_FLASH_A + 0xc) | 0x7c;
  iVar2 = WaitFlashReady(0xb0000);
  iVar1 = p_FLASH_A;
  if (iVar2 == 6) {
    *(uint *)(p_FLASH_A + 0x10) = *(uint *)(p_FLASH_A + 0x10) | 2;
    *(undefined4 *)(iVar1 + 0x14) = param_1;
    *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x40;
    iVar2 = WaitFlashReady(0xb0000);
    *(uint *)(p_FLASH_A + 0x10) = *(uint *)(p_FLASH_A + 0x10) & 0x3ffd;
  }
  return iVar2;
}


// ================= HandleIapProtocol @ 0x08000888 =================

/* HandleIapProtocol - the bootloader core (entry 0x08000bcc for normal boot; 0x08000888 is the
   transfer engine).
   Boot decision: if *0x08001800 == 0x19780217 (valid-app marker, written by IAP on a good flash) ->
   bl 0x08002202 (app reset veneer -> __main -> scatterload -> main); else advertise the IAP window.
   IAP window: InitializeUsart2(38400); loop: send U(0x55), recv byte; 19 78 02 17 -> transfer; 20
   08 10 20 -> fast-baud(600000) transfer; timeout -> send E(0x45).
   Transfer: send B,S; recv 136B record0; verify 16-byte target magic (12 universal + 4 per-box tag
   4e 33 14 f2); UnlockFlash(HSI); erase 0x08001800 + app region 0x08002000..0x0801F800 (2KB steps);
   loop: send S, recv 136B record, verify 33-word LE sum, de-obfuscate
   plain[i]=cipher[i]-i-key[i&3], ProgramFlashWord 128B at 0x08002000+128*(seq-1); on last-flag
   write valid-app markers, send E, ResetViaWatchdog.
   Parameters: none. Returns: void (resets or launches app). */

void HandleIapProtocol(void)

{
  byte byteIdx;
  char progErr;
  char rxByte;
  uint deobfPos;
  uint j;
  int tmp;
  uint idx;
  uint addr;
  int chkWordOff;
  char *pBuf;
  byte seqHi;
  
  *p_checksum = '\0';
  DelayMs(10);
  SendUartByte(0x42);
  DelayMs(10);
  SendUartByte(0x53);
  for (byteIdx = 0; pBuf = p_chkField, byteIdx < 0x88; byteIdx = byteIdx + 1) {
    progErr = ReceiveUartByteTimeout(byteIdx);
    pBuf = p_checksum;
    *p_checksum = progErr + *p_checksum;
    if (*pBuf != '\0') {
      SendUartByte(0x52);
      SendUartByte(0x45);
      return;
    }
  }
  *p_chkField = p_rxRecord[0x84];
  pBuf[1] = p_rxRecord[0x85];
  pBuf[2] = p_rxRecord[0x86];
  pBuf[3] = p_rxRecord[0x87];
  if ((((*p_rxRecord == '\x01') && (p_rxRecord[1] == '\t')) && (p_rxRecord[2] == '\a')) &&
     (((((p_rxRecord[3] == '\b' && (p_rxRecord[4] == '\0')) &&
        ((p_rxRecord[5] == '\x02' && ((p_rxRecord[6] == '\x01' && (p_rxRecord[7] == '\a')))))) &&
       (p_rxRecord[8] == -0x6f)) &&
      ((((p_rxRecord[9] == '2' && (p_rxRecord[10] == 'G')) && (p_rxRecord[0xb] == -0x5c)) &&
       (((p_rxRecord[0xc] == 'N' && (p_rxRecord[0xd] == '3')) &&
        ((p_rxRecord[0xe] == '\x14' && (p_rxRecord[0xf] == -0xe)))))))))) {
    EnableHsiClock();
    chkWordOff = p_FLASH_A;
    *(undefined4 *)(p_FLASH_A + 4) = g_flashKey1;
    *(undefined4 *)(chkWordOff + 4) = g_flashKey2;
    *(undefined4 *)(chkWordOff + 0xc) = 0x34;
    EraseFlashPage(g_bootFlagPage);
    for (j = g_appRegionStart; j < g_appRegionEnd; j = j + 0x800) {
      EraseFlashPage(j);
    }
    DelayMs(10);
    while( true ) {
      SendUartByte(0x53);
      *p_checksum = '\0';
      for (byteIdx = 0; byteIdx < 0x88; byteIdx = byteIdx + 1) {
        progErr = ReceiveUartByteTimeout(byteIdx);
        pBuf = p_checksum;
        *p_checksum = progErr + *p_checksum;
        if (*pBuf != '\0') {
          SendUartByte(0x52);
          return;
        }
      }
      if (*p_checksum != '\0') break;
      chkWordOff = 0;
      for (j = 0; j < 0x21; j = j + 1 & 0xff) {
        tmp = (j & 0x3f) * 4;
        chkWordOff = chkWordOff +
                     (uint)(byte)p_rxRecordB[tmp + 3] * 0x1000000 +
                     (uint)(byte)p_rxRecordB[tmp + 2] * 0x10000 +
                     (uint)(byte)p_rxRecordB[tmp + 1] * 0x100 + (uint)(byte)p_rxRecordB[tmp];
      }
      if ((uint)(byte)p_rxRecordB[0x87] * 0x1000000 + (uint)(byte)p_rxRecordB[0x86] * 0x10000 +
          (uint)(byte)p_rxRecordB[0x85] * 0x100 + (uint)(byte)p_rxRecordB[0x84] != chkWordOff) {
        SendUartByte(0x52);
        return;
      }
      for (j = 0; j < 0x20; j = j + 1 & 0xff) {
        for (idx = 0; idx < 4; idx = idx + 1 & 0xff) {
          deobfPos = idx + j * 4;
          addr = deobfPos & 0xff;
          p_rxRecordB[addr] = (p_rxRecordB[addr] - (char)deobfPos) - *(char *)(p_deobfKey + idx);
        }
      }
      byteIdx = p_rxRecordB[0x80];
      seqHi = p_rxRecordB[0x81];
      progErr = '\0';
      *p_progStatus = '\x06';
      for (j = 0; j < 0x80; j = j + 4 & 0xff) {
        if (*p_progStatus != '\x06') {
          progErr = progErr + '\x01';
        }
        rxByte = ProgramFlashWord(g_appBase + ((uint)byteIdx + (uint)seqHi * 0x100 + -1) * 0x80 + j,
                                  (uint)(byte)p_rxRecordB[j + 3] * 0x1000000 +
                                  (uint)(byte)p_rxRecordB[j + 2] * 0x10000 +
                                  (uint)(byte)p_rxRecordB[j + 1] * 0x100 +
                                  (uint)(byte)p_rxRecordB[j]);
        *p_progStatus = rxByte;
      }
      if (progErr != '\0') {
        SendUartByte(0x52);
        return;
      }
      if (p_rxRecordB[0x83] != '\0') {
        ProgramFlashWord(g_bootFlagAddr,g_bootMagic1);
        ProgramFlashWord(g_appBase + -0x80,g_bootMagic2);
        ProgramFlashWord(g_appBase + -0x7c,g_bootMagic3);
        DelayMs(10);
        SendUartByte(0x45);
        *(uint *)(p_FLASH_B + 0x10) = *(uint *)(p_FLASH_B + 0x10) | 0x80;
        ResetViaWatchdog();
        chkWordOff = p_RCC_B;
        disableIRQinterrupts();
        *(uint *)(p_RCC_B + 0x1c) = *(uint *)(p_RCC_B + 0x1c) | 0x20000;
        *(uint *)(chkWordOff + 0x18) = *(uint *)(chkWordOff + 0x18) | 0x3d;
        if (*p_updateFlag == 0xaa) {
          SetGpioAltFunction(p_GPIOB,0x40,1);
          *(undefined4 *)(p_GPIOB + 0x18) = 0x40;
          *p_updateFlag = 0;
        }
        InitializeUsart2(0x9600);
        DelayMs(100);
        while( true ) {
          SendUartByte(0x55);
          chkWordOff = ReceiveUartByte(0);
          if (chkWordOff == 0) {
            if (*p_rxRecordB == '\x19') {
              ReceiveUartByte(1);
              ReceiveUartByte(2);
              ReceiveUartByte(3);
              if (((p_rxRecordB[1] == 'x') && (p_rxRecordB[2] == '\x02')) &&
                 (p_rxRecordB[3] == '\x17')) {
                SetGpioAltFunction(p_GPIOB,0x40,1);
                *(undefined4 *)(p_GPIOB + 0x18) = 0x40;
                HandleIapProtocol();
              }
            }
            else if (*p_rxRecordB == ' ') {
              ReceiveUartByte(1);
              ReceiveUartByte(2);
              ReceiveUartByte(3);
              if (((p_rxRecordB[1] == '\b') && (p_rxRecordB[2] == '\x10')) &&
                 (p_rxRecordB[3] == ' ')) {
                InitializeUsart2(g_fastBaud);
                SetGpioAltFunction(p_GPIOB,0x40,1);
                *(undefined4 *)(p_GPIOB + 0x18) = 0x40;
                for (byteIdx = 0; byteIdx < 5; byteIdx = byteIdx + 1) {
                  DelayMs(200);
                }
                HandleIapProtocol();
              }
            }
          }
          else {
            SendUartByte(0x45);
          }
          if (*g_bootFlagAddr == g_bootMagic1) break;
          SetGpioAltFunction(p_GPIOB,0x40,1);
          *(undefined4 *)(p_GPIOB + 0x18) = 0x40;
          for (byteIdx = 0; byteIdx < 0x19; byteIdx = byteIdx + 1) {
            DelayMs(200);
          }
          DelayMs((char)*p_USART2_DR);
        }
        *(undefined4 *)(p_GPIOB + 0x28) = 0x40;
        app_entry_veneer();
                    /* WARNING: Could not recover jumptable at 0x08000d32. Too many branches */
                    /* WARNING: Treating indirect jump as call */
        (*(code *)0x8000bcd)();
        return;
      }
    }
    SendUartByte(0x52);
  }
  else {
    SendUartByte(0x52);
    SendUartByte(0x45);
  }
  return;
}


// ================= SetClockStep @ 0x08000d34 =================

/* SetClockStep - clock-source enable/wait substep used by InitializeSysClock.
   Algorithm: set/clear RCC bits, poll ready
   Parameters: none
   Returns: void */

void SetClockStep(void)

{
  uint *puVar1;
  
  puVar1 = DAT_08000e98;
  *DAT_08000e98 = *DAT_08000e98 & 0xfdffffff;
  *puVar1 = *puVar1 | 0x2000000;
  puVar1 = REG_PWR;
  *REG_PWR = *REG_PWR & 0xfffff9ff;
  *puVar1 = *puVar1 | 0x400;
  do {
  } while ((REG_PWR[4] & 2) != 0);
  do {
  } while ((REG_PWR[4] & 2) != 2);
  return;
}


// ================= InitializeSysClock @ 0x08000d82 =================

/* InitializeSysClock - HSE 16MHz -> PLL -> 64MHz, waits HSE ready and PLL lock, switches SYSCLK.
   Algorithm: enable HSE, poll RDY, config PLL, poll lock, select PLL as sysclk
   Parameters: none
   Returns: void */

void InitializeSysClock(void)

{
  uint *puVar1;
  uint *puVar2;
  int iVar3;
  
  puVar1 = p_RCC_B;
  iVar3 = 0;
  *p_RCC_B = *p_RCC_B | 0x10000;
  do {
    iVar3 = iVar3 + 1;
    if ((int)(*p_RCC_B << 0xe) < 0) break;
  } while (iVar3 != 0x2000);
  if ((int)(*p_RCC_B << 0xe) < 0) {
    SetClockStep(*p_RCC_B,puVar1,0,0);
    puVar1 = p_FLASH_B;
    *p_FLASH_B = *p_FLASH_B & 0xfffffffc;
    *puVar1 = *puVar1 | 1;
    puVar1 = p_RCC_B;
    p_RCC_B[1] = p_RCC_B[1];
    puVar1[1] = puVar1[1] | 0x2000;
    puVar1[1] = puVar1[1] | 0x500;
    puVar2 = p_RCC_B;
    p_RCC_B[1] = puVar1[1] & DAT_08000ea8;
    puVar2[0x10] = puVar2[0x10] & 0xfffffffe;
    puVar2[1] = puVar2[1] | 0x1b0000;
    puVar2[0x10] = puVar2[0x10];
    *puVar2 = *puVar2 | 0x1000000;
    puVar1 = p_RCC_B;
    do {
    } while ((*p_RCC_B & 0x2000000) == 0);
    p_RCC_B[1] = p_RCC_B[1] & 0xfffffffc;
    puVar1[1] = puVar1[1] | 3;
    do {
    } while ((p_RCC_B[1] & 0xc) != 0xc);
  }
  else {
    *p_boot_sysclk_hz = DAT_08000ea0;
  }
  return;
}


// ================= InitializeSystem @ 0x08000eac =================

/* InitializeSystem - SystemInit. Enables GPIO/periph clocks, sets flash latency, calls
   InitializeSysClock, sets SCB->VTOR=0x08000000.
   Algorithm: RCC enable + flash ACR + clock tree
   Parameters: none
   Returns: void */

void InitializeSystem(void)

{
  uint *puVar1;
  uint *puVar2;
  
  *DAT_08000f6c = *DAT_08000f6c | 0xf00000;
  puVar1 = REG_RCC;
  REG_RCC[9] = REG_RCC[9] | 4;
  puVar2 = REG_RCC;
  REG_RCC[1] = puVar1[1] & DAT_08000f74;
  puVar1 = REG_RCC;
  *REG_RCC = *puVar2 & DAT_08000f78;
  *puVar1 = *puVar1 & 0xfffbffff;
  puVar2 = REG_RCC;
  REG_RCC[1] = puVar1[1] & DAT_08000f7c;
  puVar2[0xb] = 0;
  puVar2[0xc] = 0x3800;
  puVar2[0xd] = 0;
  puVar2[0x10] = 0;
  puVar2[2] = DAT_08000f80;
  puVar2[7] = puVar2[7] | 0x10000000;
  puVar1 = DAT_08000f88;
  if ((*REG_PWR & 0x400) == 0x400) {
    *DAT_08000f88 = *DAT_08000f88 & 0xfdffffff;
    *puVar1 = *puVar1 | 0x2000000;
  }
  puVar1 = REG_FLASH;
  *REG_FLASH = *REG_FLASH | 0x90;
  if ((*puVar1 & 0x100) != 0) {
    *puVar1 = *puVar1 & 0xfffffdff;
  }
  InitializeSysClock();
  DAT_08000f6c[-0x20] = 0x8000000;
  return;
}


