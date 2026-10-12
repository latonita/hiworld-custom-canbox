// app subsystem: wwdg  —  decompiled from Ghidra (named).
// Nations N32L406 canbox. Addresses are original flash addresses.
#include "n32l406.h"

// ================= wwdg_kick @ 0x08002d3e =================

void wwdg_kick(void)

{
  *REG_WWDG = 0xff;
  return;
}


// ================= wwdg_force_reset_thunk @ 0x08002e26 =================

void wwdg_force_reset_thunk(void)

{
  do {
    *REG_WWDG = 0xbf;
  } while( true );
}


// ================= wwdg_force_reset @ 0x08002e2e =================

void wwdg_force_reset(void)

{
  do {
    *REG_WWDG = 0xbf;
  } while( true );
}


// ================= wwdg_set_prescaler @ 0x080073be =================

void wwdg_set_prescaler(uint param_1)

{
  *DAT_08007428 = *DAT_08007428 & 0xfffffe7f | param_1;
  return;
}


// ================= wwdg_set_window @ 0x080073d0 =================

void wwdg_set_window(uint param_1)

{
  *DAT_08007428 = param_1 & 0x7f | *DAT_08007428 & 0xffffff80;
  return;
}


// ================= wwdg_enable_set_counter @ 0x08007406 =================

void wwdg_enable_set_counter(uint param_1)

{
  *(uint *)(DAT_08007428 + -4) = param_1 | 0x80;
  return;
}


