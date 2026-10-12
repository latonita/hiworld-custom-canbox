// app subsystem: flash  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= flash_set_latency @ 0x080045fc =================

void flash_set_latency(uint param_1)

{
  *REG_FLASH = *REG_FLASH & 0xf8 | param_1;
  return;
}


// ================= flash_set_prefetch @ 0x0800460e =================

void flash_set_prefetch(uint param_1)

{
  uint *puVar1;
  
  puVar1 = REG_FLASH;
  *REG_FLASH = *REG_FLASH & 0xffffffef;
  *puVar1 = *puVar1 | param_1;
  return;
}


// ================= FLASH_Unlock @ 0x080046fc =================

void FLASH_Unlock(void)

{
  int iVar1;
  
  iVar1 = REG_FLASH;
  *(undefined4 *)(REG_FLASH + 4) = DAT_08004a04;
  *(undefined4 *)(iVar1 + 4) = DAT_08004a08;
  return;
}


// ================= FLASH_Lock @ 0x08004708 =================

void FLASH_Lock(void)

{
  *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) | 0x80;
  return;
}


// ================= FLASH_GetBank1Status @ 0x08004716 =================

undefined4 FLASH_GetBank1Status(void)

{
  undefined4 uVar1;
  
  if ((*(uint *)(REG_FLASH + 0xc) & 1) == 0) {
    if ((*(uint *)(REG_FLASH + 0xc) & 4) == 0) {
      if ((*(uint *)(REG_FLASH + 0xc) & 8) == 0) {
        if ((*(uint *)(REG_FLASH + 0xc) & 0x10) == 0) {
          if ((*(uint *)(REG_FLASH + 0xc) & 0x40) == 0) {
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


// ================= FLASH_WaitForLastOperation @ 0x08004762 =================

int FLASH_WaitForLastOperation(undefined4 param_1,undefined4 param_2)

{
  int status;
  int extraout_r2;
  int extraout_r2_00;
  int iVar1;
  
  status = FLASH_GetBank1Status(param_1,param_2,param_1,6);
  iVar1 = extraout_r2;
  while ((status == 1 && (iVar1 != 0))) {
    status = FLASH_GetBank1Status();
    iVar1 = extraout_r2_00 + -1;
  }
  if (iVar1 == 0) {
    status = 10;
  }
  return status;
}


// ================= FLASH_ClearStatusFlags @ 0x08004788 =================

void FLASH_ClearStatusFlags(uint param_1)

{
  *(uint *)(REG_FLASH + 0xc) = *(uint *)(REG_FLASH + 0xc) | param_1;
  return;
}


// ================= FLASH_ErasePage @ 0x08004794 =================

/* FLASH_ErasePage — LIVE flash op, used only by param_page_save for the 0x0801F800 parameter
   page. */

int FLASH_ErasePage(undefined4 param_1)

{
  int iVar1;
  int iVar2;
  
  FLASH_ClearStatusFlags(0x7c);
  iVar2 = FLASH_WaitForLastOperation(0xb0000);
  iVar1 = REG_FLASH;
  if (iVar2 == 6) {
    *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) | 2;
    *(undefined4 *)(iVar1 + 0x14) = param_1;
    *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x40;
    iVar2 = FLASH_WaitForLastOperation(0xb0000);
    *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3ffd;
  }
  return iVar2;
}


// ================= FLASH_MassErase_DEAD @ 0x080047e2 =================

/* FLASH_MassErase_DEAD — present but zero callers. The app never mass-erases. */

int FLASH_MassErase_DEAD(void)

{
  int iVar1;
  int iVar2;
  
  FLASH_ClearStatusFlags(0x7c);
  iVar2 = FLASH_WaitForLastOperation(0xb0000);
  iVar1 = REG_FLASH;
  if (iVar2 == 6) {
    *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) | 4;
    *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x40;
    iVar2 = FLASH_WaitForLastOperation(0xb0000);
    *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3ffb;
  }
  return iVar2;
}


// ================= FLASH_GetReadOutProtStatus @ 0x0800482c =================

bool FLASH_GetReadOutProtStatus(void)

{
  return (*(uint *)(REG_FLASH + 0x1c) & 2) != 0;
}


// ================= FLASH_OB_precheck @ 0x08004840 =================

bool FLASH_OB_precheck(void)

{
  return (*(uint *)(REG_FLASH + 0x1c) & 0x80000000) != 0;
}


// ================= FLASH_OB_RDPprogram_DEAD @ 0x08004854 =================

/* FLASH_OB_RDPprogram_DEAD — option-byte / RDP routine present but never called by the app. */

int FLASH_OB_RDPprogram_DEAD(void)

{
  int iVar1;
  int iVar2;
  undefined4 uVar3;
  
  uVar3 = DAT_08004a0c;
  iVar1 = FLASH_OB_precheck();
  if (iVar1 == 0) {
    iVar1 = FLASH_GetReadOutProtStatus();
    if (iVar1 != 0) {
      uVar3 = 0xff0000;
    }
    FLASH_ClearStatusFlags(0x7c);
    iVar2 = FLASH_WaitForLastOperation(0xb0000);
    iVar1 = REG_FLASH;
    if (iVar2 == 6) {
      *(undefined4 *)(REG_FLASH + 8) = DAT_08004a04;
      *(undefined4 *)(iVar1 + 8) = DAT_08004a08;
      *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x20;
      *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x40;
      iVar2 = FLASH_WaitForLastOperation(0xb0000);
      if (iVar2 == 6) {
        FLASH_ClearStatusFlags(0x7c);
        iVar1 = REG_FLASH;
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
        *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x10;
        *DAT_08004a10 = uVar3;
        iVar2 = FLASH_WaitForLastOperation(0x2000);
        if (iVar2 != 10) {
          *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fef;
        }
      }
      else if (iVar2 != 10) {
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
      }
    }
  }
  else {
    iVar2 = 8;
  }
  return iVar2;
}


// ================= FLASH_ProgramOptionBytes_DEAD @ 0x08004906 =================

undefined8
FLASH_ProgramOptionBytes_DEAD
          (uint param_1,uint param_2,uint param_3,uint param_4,uint param_5,uint param_6,
          undefined4 param_7,uint param_8,uint param_9,uint param_10,uint param_11,uint param_12,
          uint param_13)

{
  int iVar1;
  int iVar2;
  uint local_30;
  
  iVar1 = FLASH_OB_precheck();
  iVar2 = REG_FLASH;
  if (iVar1 == 0) {
    param_8 = ~param_8;
    local_30 = param_9 | (param_10 | param_11 | param_12 | param_13) << 0x10;
    *(undefined4 *)(REG_FLASH + 8) = DAT_08004a04;
    *(undefined4 *)(iVar2 + 8) = DAT_08004a08;
    FLASH_ClearStatusFlags(0x7c);
    iVar2 = FLASH_WaitForLastOperation(0xb0000);
    iVar1 = REG_FLASH;
    if (iVar2 == 6) {
      *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) | 0x20;
      *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x40;
      iVar2 = FLASH_WaitForLastOperation(0xb0000);
      if (iVar2 == 6) {
        FLASH_ClearStatusFlags(0x7c);
        iVar2 = REG_FLASH;
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
        *(uint *)(iVar2 + 0x10) = *(uint *)(iVar2 + 0x10) | 0x10;
        *DAT_08004a10 = param_1 | (param_2 | param_3 | param_4 | param_5) << 0x10;
        iVar2 = FLASH_WaitForLastOperation(0x2000);
        if (iVar2 == 6) {
          FLASH_ClearStatusFlags(0x7c);
          DAT_08004a10[1] = param_6 | param_6 << 0x10;
          iVar2 = FLASH_WaitForLastOperation(0x2000);
          if (iVar2 == 6) {
            FLASH_ClearStatusFlags(0x7c);
            *DAT_08004e20 = param_8 & 0xff | (param_8 & 0xff00) << 8;
            iVar2 = FLASH_WaitForLastOperation(0x2000);
            if (iVar2 == 6) {
              FLASH_ClearStatusFlags(0x7c);
              DAT_08004e20[1] = param_8 >> 8 & 0xff0000 | (param_8 & 0xffffff) >> 0x10;
              iVar2 = FLASH_WaitForLastOperation(0x2000);
              if (iVar2 == 6) {
                FLASH_ClearStatusFlags(0x7c);
                DAT_08004e20[2] = local_30;
                iVar2 = FLASH_WaitForLastOperation(0x2000);
              }
            }
          }
        }
        if (iVar2 != 10) {
          *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fef;
        }
      }
      else if (iVar2 != 10) {
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
      }
    }
  }
  else {
    iVar2 = 8;
    local_30 = param_2;
  }
  return CONCAT44(local_30,iVar2);
}


// ================= FLASH_ProgramWord @ 0x08004a96 =================

/* FLASH_ProgramWord — LIVE flash op, used only by param_page_save for the 0x0801F800 parameter
   page. */

int FLASH_ProgramWord(undefined4 *param_1,undefined4 param_2)

{
  int iVar1;
  
  if (((uint)param_1 & 3) == 0) {
    FLASH_ClearStatusFlags(0x7c);
    iVar1 = FLASH_WaitForLastOperation(0x2000);
    if (iVar1 == 6) {
      *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) | 1;
      *param_1 = param_2;
      iVar1 = FLASH_WaitForLastOperation(0x2000);
      *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3ffe;
    }
  }
  else {
    iVar1 = 9;
  }
  return iVar1;
}


// ================= FLASH_ProgramOBData_DEAD @ 0x08004aec =================

int FLASH_ProgramOBData_DEAD(undefined4 *param_1,undefined4 param_2)

{
  int iVar1;
  int iVar2;
  
  iVar1 = FLASH_OB_precheck();
  if (iVar1 == 0) {
    FLASH_ClearStatusFlags(0x7c);
    iVar2 = FLASH_WaitForLastOperation(0x2000);
    iVar1 = REG_FLASH;
    if (iVar2 == 6) {
      *(undefined4 *)(REG_FLASH + 8) = DAT_08004e28;
      *(undefined4 *)(iVar1 + 8) = DAT_08004e2c;
      *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x10;
      *param_1 = param_2;
      iVar2 = FLASH_WaitForLastOperation(0x2000);
      if (iVar2 != 10) {
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fef;
      }
    }
  }
  else {
    iVar2 = 8;
  }
  return iVar2;
}


// ================= FLASH_OB_WriteProt_DEAD @ 0x08004b4a =================

int FLASH_OB_WriteProt_DEAD(uint param_1)

{
  int iVar1;
  int iVar2;
  uint uVar3;
  uint uVar4;
  
  iVar1 = FLASH_OB_precheck();
  if (iVar1 == 0) {
    param_1 = ~param_1;
    uVar3 = (param_1 & 0xffff) >> 8;
    uVar4 = (param_1 & 0xffffff) >> 0x10;
    FLASH_ClearStatusFlags(0x7c);
    iVar2 = FLASH_WaitForLastOperation(0x2000);
    iVar1 = REG_FLASH;
    if (iVar2 == 6) {
      *(undefined4 *)(REG_FLASH + 8) = DAT_08004e28;
      *(undefined4 *)(iVar1 + 8) = DAT_08004e2c;
      *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x10;
      if (((param_1 & 0xff) != 0xff) || (uVar3 != 0xff)) {
        *DAT_08004e20 = param_1 & 0xff | uVar3 << 0x10;
        iVar2 = FLASH_WaitForLastOperation(0x2000);
      }
      if (((uVar4 != 0xff) || (param_1 >> 0x18 != 0xff)) && (iVar2 == 6)) {
        DAT_08004e20[1] = uVar4 | (param_1 >> 0x18) << 0x10;
        iVar2 = FLASH_WaitForLastOperation(0x2000);
      }
      if (iVar2 != 10) {
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fef;
      }
    }
  }
  else {
    iVar2 = 8;
  }
  return iVar2;
}


// ================= FLASH_OB_RDPtoggle_DEAD @ 0x08004bf6 =================

int FLASH_OB_RDPtoggle_DEAD(int param_1)

{
  int iVar1;
  int iVar2;
  uint uVar3;
  
  uVar3 = (*(uint *)(REG_FLASH + 0x1c) & 0x1c) << 0xe;
  iVar1 = FLASH_OB_precheck();
  if (iVar1 == 0) {
    FLASH_ClearStatusFlags(0x7c);
    iVar2 = FLASH_WaitForLastOperation(0xb0000);
    iVar1 = REG_FLASH;
    if (iVar2 == 6) {
      *(undefined4 *)(REG_FLASH + 8) = DAT_08004e28;
      *(undefined4 *)(iVar1 + 8) = DAT_08004e2c;
      *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x20;
      *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x40;
      iVar2 = FLASH_WaitForLastOperation(0xb0000);
      if (iVar2 == 6) {
        FLASH_ClearStatusFlags(0x7c);
        iVar1 = REG_FLASH;
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
        *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x10;
        if (param_1 == 0) {
          *(uint *)(DAT_08004e20 + -8) = uVar3 | 0xa5;
        }
        else {
          *(uint *)(DAT_08004e20 + -8) = uVar3;
        }
        iVar2 = FLASH_WaitForLastOperation(0xb0000);
        if (iVar2 != 10) {
          *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fef;
        }
      }
      else if (iVar2 != 10) {
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
      }
    }
  }
  else {
    iVar2 = 8;
  }
  return iVar2;
}


// ================= FLASH_OB_routineA_DEAD @ 0x08004cbc =================

int FLASH_OB_routineA_DEAD(void)

{
  int iVar1;
  int iVar2;
  uint uVar3;
  
  uVar3 = (*(uint *)(REG_FLASH + 0x1c) & 0x1c) << 0xe;
  iVar1 = FLASH_GetReadOutProtStatus();
  if (iVar1 == 0) {
    uVar3 = uVar3 | 0xa5;
  }
  iVar1 = FLASH_OB_precheck();
  if (iVar1 == 0) {
    FLASH_ClearStatusFlags(0x7c);
    iVar2 = FLASH_WaitForLastOperation(0xb0000);
    iVar1 = REG_FLASH;
    if (iVar2 == 6) {
      *(undefined4 *)(REG_FLASH + 8) = DAT_08004e28;
      *(undefined4 *)(iVar1 + 8) = DAT_08004e2c;
      *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x20;
      *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x40;
      iVar2 = FLASH_WaitForLastOperation(0xb0000);
      if (iVar2 == 6) {
        FLASH_ClearStatusFlags(0x7c);
        iVar1 = REG_FLASH;
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
        *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x10;
        *(uint *)(DAT_08004e20 + -8) = uVar3;
        iVar2 = FLASH_WaitForLastOperation(0xb0000);
        if (iVar2 == 6) {
          FLASH_ClearStatusFlags(0x7c);
          *(undefined4 *)(DAT_08004e20 + 8) = DAT_08004e30;
          iVar2 = FLASH_WaitForLastOperation(0xb0000);
        }
        if (iVar2 != 10) {
          *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fef;
        }
      }
      else if (iVar2 != 10) {
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
      }
    }
  }
  else {
    iVar2 = 8;
  }
  return iVar2;
}


// ================= FLASH_OB_routineB_DEAD @ 0x08004d94 =================

int FLASH_OB_routineB_DEAD(uint param_1,uint param_2,uint param_3,uint param_4)

{
  int iVar1;
  int iVar2;
  uint uVar3;
  
  uVar3 = DAT_08004e34;
  iVar1 = FLASH_OB_precheck();
  if (iVar1 == 0) {
    iVar2 = FLASH_GetReadOutProtStatus();
    iVar1 = REG_FLASH;
    if (iVar2 != 0) {
      uVar3 = DAT_08004e34 - 0xa5;
    }
    *(undefined4 *)(REG_FLASH + 8) = DAT_08004e28;
    *(undefined4 *)(iVar1 + 8) = DAT_08004e2c;
    FLASH_ClearStatusFlags(0x7c);
    iVar2 = FLASH_WaitForLastOperation(0xb0000);
    iVar1 = REG_FLASH;
    if (iVar2 == 6) {
      *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) | 0x20;
      *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x40;
      iVar2 = FLASH_WaitForLastOperation(0xb0000);
      if (iVar2 == 6) {
        FLASH_ClearStatusFlags(0x7c);
        iVar1 = REG_FLASH;
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
        *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x10;
        *DAT_08004ff4 = uVar3 | (param_1 | param_2 | param_3 | param_4) << 0x10;
        iVar2 = FLASH_WaitForLastOperation(0x2000);
        if (iVar2 != 10) {
          *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fef;
        }
      }
      else if (iVar2 != 10) {
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
      }
    }
  }
  else {
    iVar2 = 8;
  }
  return iVar2;
}


// ================= FLASH_OB_routineC_DEAD @ 0x08004e84 =================

int FLASH_OB_routineC_DEAD(uint param_1,uint param_2,uint param_3,uint param_4)

{
  int iVar1;
  int iVar2;
  int iVar3;
  
  iVar3 = DAT_08004ffc;
  iVar1 = FLASH_OB_precheck();
  if (iVar1 == 0) {
    iVar2 = FLASH_GetReadOutProtStatus();
    iVar1 = REG_FLASH;
    if (iVar2 != 0) {
      iVar3 = DAT_08004ffc + -0xa5;
    }
    *(undefined4 *)(REG_FLASH + 8) = DAT_08005000;
    *(undefined4 *)(iVar1 + 8) = DAT_08005004;
    FLASH_ClearStatusFlags(0x7c);
    iVar2 = FLASH_WaitForLastOperation(0xb0000);
    iVar1 = REG_FLASH;
    if (iVar2 == 6) {
      *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) | 0x20;
      *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x40;
      iVar2 = FLASH_WaitForLastOperation(0xb0000);
      if (iVar2 == 6) {
        FLASH_ClearStatusFlags(0x7c);
        iVar1 = REG_FLASH;
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
        *(uint *)(iVar1 + 0x10) = *(uint *)(iVar1 + 0x10) | 0x10;
        *DAT_08004ff4 = iVar3;
        iVar2 = FLASH_WaitForLastOperation(0x2000);
        if (iVar2 == 6) {
          FLASH_ClearStatusFlags(0x7c);
          DAT_08004ff4[4] = (param_1 | param_2 | param_3 | param_4) << 0x10 | 0xff00ffff;
          iVar2 = FLASH_WaitForLastOperation(0x2000);
        }
        if (iVar2 != 10) {
          *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fef;
        }
      }
      else if (iVar2 != 10) {
        *(uint *)(REG_FLASH + 0x10) = *(uint *)(REG_FLASH + 0x10) & 0x3fdf;
      }
    }
  }
  else {
    iVar2 = 8;
  }
  return iVar2;
}


