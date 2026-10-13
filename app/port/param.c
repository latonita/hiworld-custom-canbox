/* ============================================================================
 * param.c -- on-chip flash driver + the parameter (emulated EEPROM) page.
 *
 * Ported from app/src/flash.c (Ghidra output, vendor FLASH driver) plus the
 * three param routines that live in app/src/sys.c and app/src/can.c:
 *
 *     0x080045fc  flash_set_latency            FLASH->AC latency field
 *     0x0800460e  flash_set_prefetch           FLASH->AC prefetch bit
 *     0x080046fc  FLASH_Unlock                 KEYR key sequence
 *     0x08004708  FLASH_Lock                   CTRL.LOCK
 *     0x08004716  FLASH_GetBank1Status         STS -> status code
 *     0x08004762  FLASH_WaitForLastOperation   busy wait with a tick budget
 *     0x08004788  FLASH_ClearStatusFlags       write-1-to-clear on STS
 *     0x08004794  FLASH_ErasePage              page erase (param page only)
 *     0x08004a96  FLASH_ProgramWord            32-bit word program
 *     0x08002dbc  param_page_load_0801F800     flash -> RAM
 *     0x08002d46  param_page_save_0801F800     RAM -> flash, only when dirty
 *     0x08007b6c  can_config_load_defaults     validate / default / prime
 *
 * The _DEAD routines in app/src/flash.c (mass erase, RDP, option bytes, write
 * protection) have no caller in the stock app and are deliberately NOT ported:
 * every one of them can brick or lock the part, and the only reason they exist
 * in the stock image is that the vendor driver was linked whole.
 *
 * ---------------------------------------------------------------------------
 * SAFETY -- READ THIS BEFORE CHANGING ANYTHING HERE
 *
 * The N32L406CBL7 has 128 KB of flash in 2 KB pages:
 *     0x08000000..0x08001FFF   IAP BOOTLOADER. Write protected. It is the ONLY
 *                              way to reflash this box. Erasing it bricks the
 *                              device permanently -- there is no other path in.
 *     0x08002000..0x0801F7FF   the app image we are running from.
 *     0x0801F800..0x0801FFFF   the parameter page. The ONLY page this firmware
 *                              may ever erase or program at runtime.
 *
 * flash_erase_page() accepts exactly one address, PARAM_PAGE_BASE, and
 * flash_program_word() accepts only word addresses inside the param page.
 * Anything else returns FLASH_STS_REFUSED and never touches the controller, so
 * there is no way to reach the erase/program hardware with a bad address
 * through this module's API. Keep it that way.
 * ---------------------------------------------------------------------------
 *
 * Register layout verified against the vendor SDK struct in n32l406_hw.h
 * (FLASH_Module @0x40022000):
 *     AC +0x00   KEYR +0x04   OPTKEY +0x08   STS +0x0C
 *     CTRL +0x10 ADD  +0x14   OB2    +0x18   OB  +0x1C   WRP +0x20
 * This is NOT the STM32 naming (ACR/KEYR/SR/CR/AR); the offsets happen to line
 * up but the field names and the CTRL reset masks do not -- see below.
 * ==========================================================================*/

#include "port.h"
#include "param.h"

/* ---- FLASH->CTRL bits (confirmed by the stock code's reset masks) --------
 * The stock driver clears bits with ABSOLUTE 14-bit masks, not ~bit: 0x3FFE
 * (clear PG), 0x3FFD (clear PER), 0x3FFB (clear MER), 0x3FEF (clear OPTPG),
 * 0x3FDF (clear OPTER). That tells us CTRL is 14 bits wide on this part and
 * fixes the bit assignment below. Reproduced verbatim so the controller sees
 * exactly the writes it saw in the stock image. */
#define FLASH_CTRL_PG           0x00000001U   /* program                     */
#define FLASH_CTRL_PER          0x00000002U   /* page erase                  */
#define FLASH_CTRL_START        0x00000040U   /* start the erase             */
#define FLASH_CTRL_LOCK         0x00000080U   /* re-lock the controller      */
#define FLASH_CTRL_PG_RESET     0x00003FFEU
#define FLASH_CTRL_PER_RESET    0x00003FFDU

/* ---- FLASH->STS bits ---------------------------------------------------- */
#define FLASH_STS_BUSY_BIT      0x00000001U   /* bit0                        */
#define FLASH_STS_ERRBIT_2      0x00000004U   /* bit2                        */
#define FLASH_STS_ERRBIT_3      0x00000008U   /* bit3                        */
#define FLASH_STS_ERRBIT_4      0x00000010U   /* bit4                        */
#define FLASH_STS_ERRBIT_6      0x00000040U   /* bit6                        */
/* The stock code always clears this exact set before an operation: bits 2..6,
 * i.e. every error/EOP flag. They are write-1-to-clear. */
#define FLASH_STS_CLEAR_ALL     0x0000007CU

/* ---- unlock keys --------------------------------------------------------
 * Read straight out of the stock image's literal pool rather than assumed:
 *     reference/2429-H1N4JPF20A-220524.app.bin
 *     [0x08004a04] = 0x45670123    [0x08004a08] = 0xCDEF89AB
 * (The option-byte routines use the same pair from 0x08004e28/0x08004e2c.)
 * They happen to equal the familiar ST values, but they were taken from this
 * image, not from an STM32 header. */
#define FLASH_KEY1              0x45670123U
#define FLASH_KEY2              0xCDEF89ABU

/* ---- operation timeouts (loop-iteration budgets, stock values) ----------- */
#define FLASH_ERASE_TIMEOUT     0x000B0000U
#define FLASH_PROGRAM_TIMEOUT   0x00002000U

/* ===========================================================================
 * TEMPORARY SCAFFOLDING -- cross-subsystem dependency
 *
 * RCC_EnableHSI_forFlash @0x080046b4 belongs to the rcc subsystem (hw_rcc.c)
 * and is not declared in port.h yet. The flash controller on this part is fed
 * from HSI, so the stock save path turns HSI on before unlocking; skipping it
 * would make every erase/program fail (or hang in the busy wait).
 *
 * Because that failure mode is silent and this is the one destructive path in
 * the firmware, the weak fallback below is a WORKING implementation rather
 * than a no-op: a strong definition from hw_rcc.c overrides it, and if one
 * never lands the save path still behaves like stock. Delete this stub as soon
 * as the rcc owner exports the real one.
 * ========================================================================= */
/* rcc_enable_hsi_for_flash @0x080046b4 is an RCC function and now lives in
 * hw_rcc.c; it is declared in port.h. */

/* ===========================================================================
 * Bounds checking -- the whole point of this file
 * ========================================================================= */

/* True only for [addr, addr+len) fully inside 0x0801F800..0x0801FFFF. */
static int in_param_page(uint32_t addr, uint32_t len)
{
    if (len == 0U)
        return 0;
    if (addr < PARAM_PAGE_BASE || addr > PARAM_PAGE_LAST)
        return 0;
    if ((PARAM_PAGE_LAST - addr) < (len - 1U))
        return 0;
    return 1;
}

/* ===========================================================================
 * FLASH primitives
 * ========================================================================= */

/* --- 0x080045fc --- AC = (AC & 0xF8) | latency.
 * The stock mask is absolute, not ~0x07; reproduced as written. Only caller is
 * the dead PLL path in rcc.c -- the bootloader sets the latency the app runs
 * with, so nothing live calls this. */
void flash_set_latency(uint32_t latency)
{
    FLASH_R->AC = (FLASH_R->AC & 0xF8U) | latency;
}

/* --- 0x0800460e --- AC prefetch bit (bit 4): clear, then OR the caller's
 * value. Same story: only the dead PLL path in rcc.c calls it. */
void flash_set_prefetch(uint32_t prefetch)
{
    FLASH_R->AC &= 0xFFFFFFEFU;
    FLASH_R->AC |= prefetch;
}

/* --- 0x080046fc --- Two-word key sequence into KEYR (NOT a single write, and
 * NOT the OPTKEY register). Any other value or order leaves the controller
 * locked until the next reset. */
void flash_unlock(void)
{
    FLASH_R->KEYR = FLASH_KEY1;
    FLASH_R->KEYR = FLASH_KEY2;
}

/* --- 0x08004708 --- */
void flash_lock(void)
{
    FLASH_R->CTRL |= FLASH_CTRL_LOCK;
}

/* --- 0x08004716 (FLASH_GetBank1Status) --- STS decoded in the stock order:
 * BUSY first, then the error bits in ascending order, else "complete". */
int flash_get_status(void)
{
    uint32_t sts = FLASH_R->STS;

    if ((sts & FLASH_STS_BUSY_BIT) != 0U)
        return FLASH_STS_BUSY;
    if ((sts & FLASH_STS_ERRBIT_2) != 0U)
        return FLASH_STS_ERR_PG;
    if ((sts & FLASH_STS_ERRBIT_3) != 0U)
        return FLASH_STS_ERR_PV;
    if ((sts & FLASH_STS_ERRBIT_4) != 0U)
        return FLASH_STS_ERR_WRP;
    if ((sts & FLASH_STS_ERRBIT_6) != 0U)
        return FLASH_STS_ERR_EV;
    return FLASH_STS_COMPL;
}

/* --- 0x08004762 (FLASH_WaitForLastOperation) --- Spin while the controller
 * reports BUSY, counting the caller's budget down; running out is a TIMEOUT,
 * never an implicit success. (Ghidra's output for this one is mangled -- the
 * countdown lives in r2 across the call and comes back as `extraout_r2` -- but
 * the shape is the stock vendor routine and is unambiguous.) */
int flash_wait_for_last_operation(uint32_t timeout)
{
    int status = flash_get_status();

    while (status == FLASH_STS_BUSY && timeout != 0U) {
        status = flash_get_status();
        timeout--;
    }
    if (timeout == 0U)
        status = FLASH_STS_TIMEOUT;

    return status;
}

/* --- 0x08004788 --- STS flags are write-1-to-clear. */
void flash_clear_status_flags(uint32_t flags)
{
    FLASH_R->STS |= flags;
}

/* --- 0x08004794 (FLASH_ErasePage) --- Erase one 2 KB page.
 *
 * BOUNDS CHECK: the only page this firmware is ever allowed to erase is the
 * parameter page at 0x0801F800. Anything else -- the bootloader below
 * 0x08002000, or the running app image -- is refused here, before a single
 * controller register is touched. Do not relax this.
 *
 * Sequence (stock, unchanged): clear status flags, wait for idle, set CTRL.PER,
 * write the page address to ADD, set CTRL.START, wait, then clear PER with the
 * absolute reset mask. */
int flash_erase_page(uint32_t page_addr)
{
    int status;

    if (page_addr != PARAM_PAGE_BASE)
        return FLASH_STS_REFUSED;

    flash_clear_status_flags(FLASH_STS_CLEAR_ALL);
    status = flash_wait_for_last_operation(FLASH_ERASE_TIMEOUT);
    if (status == FLASH_STS_COMPL) {
        FLASH_R->CTRL |= FLASH_CTRL_PER;
        FLASH_R->ADD   = page_addr;
        FLASH_R->CTRL |= FLASH_CTRL_START;
        status = flash_wait_for_last_operation(FLASH_ERASE_TIMEOUT);
        FLASH_R->CTRL &= FLASH_CTRL_PER_RESET;
    }
    return status;
}

/* --- 0x08004a96 (FLASH_ProgramWord) --- Program one 32-bit word.
 *
 * This part programs a full WORD (the stock param page stores one config byte
 * per 32-bit word: the dumped page reads "AA 00 00 00  01 00 00 00 ..."), so
 * this is not an STM32F1-style half-word program.
 *
 * BOUNDS CHECK: only word addresses inside the parameter page are accepted.
 * The alignment test is the stock one and runs first, so the stock
 * FLASH_STS_ERR_ALIGN result is preserved for misaligned addresses. */
int flash_program_word(uint32_t addr, uint32_t data)
{
    int status;

    if ((addr & 3U) != 0U)
        return FLASH_STS_ERR_ALIGN;
    if (!in_param_page(addr, 4U))
        return FLASH_STS_REFUSED;

    flash_clear_status_flags(FLASH_STS_CLEAR_ALL);
    status = flash_wait_for_last_operation(FLASH_PROGRAM_TIMEOUT);
    if (status == FLASH_STS_COMPL) {
        FLASH_R->CTRL |= FLASH_CTRL_PG;
        *(volatile uint32_t *)addr = data;
        status = flash_wait_for_last_operation(FLASH_PROGRAM_TIMEOUT);
        FLASH_R->CTRL &= FLASH_CTRL_PG_RESET;
    }
    return status;
}

/* ===========================================================================
 * The parameter page (emulated EEPROM)
 *
 * 32 config bytes live in the first 128 bytes of page 0x0801F800, ONE BYTE PER
 * 32-BIT WORD (byte i at 0x0801F800 + 4*i, the other three bytes left erased).
 * They are mirrored to RAM:
 *     g_param_struct  0x200011d0   the live copy the firmware reads/writes
 *     g_param_shadow  0x20001220   what is believed to be in flash; the save
 *                                  path compares the two and only burns a page
 *                                  when they differ.
 *
 * Layout, from can_config_load_defaults @0x08007b6c cross-checked against the
 * real dumped page (reference/parampage_0x0801F800_2k.bin):
 *
 *   [0x00]  validity magic, must be 0xAA
 *   [0x01]  CAN profile / bit rate selector: 1 = 125 kbit/s profile 1,
 *           0 = 83.333 kbit/s profile 0. Clamped to <= 1. Toggled by the
 *           phase-1 autobaud retry, which is why this page gets rewritten.
 *   [0x02..0x0F]  unused, erased (0xFF in the real dump)
 *   [0x10]  stored amplifier volume (0..38)
 *   [0x11]  stored BAL    } all 1..19, centre 10; defaulted to 10 ("\n")
 *   [0x12]  stored FAD    }
 *   [0x13]  stored BASS   }
 *   [0x14]  stored MID    }
 *   [0x15]  stored TRE    }
 *   [0x16]  0 -- the NUL of the 6-byte strncpy that defaults [0x11..0x15]
 *   [0x17..0x1F]  unused, erased
 * ========================================================================= */

/* --- 0x08002dbc (param_page_load_0801F800) --- Boot-time load: zero the RAM
 * copy, then read the 32 bytes out of the page (one byte per word) into
 * g_param_struct and mirror them into g_param_shadow, so the first save sees a
 * clean (non-dirty) block. */
void param_page_load(void)
{
    const volatile uint8_t *page = (const volatile uint8_t *)PARAM_PAGE_BASE;
    uint32_t i;

    for (i = 0; i < PARAM_COUNT; i++)
        g_param_struct[i] = 0;

    for (i = 0; i < PARAM_COUNT; i++) {
        g_param_struct[i] = page[i * 4U];
        g_param_shadow[i] = g_param_struct[i];
    }
}

/* --- 0x08002d46 (param_page_save_0801F800) --- THE ONLY LIVE FLASH WRITER.
 *
 * Compares the live block against the shadow; if nothing changed it returns
 * without touching flash (this runs every 1024 ms from the main loop, so the
 * dirty check is what stops the page being burnt to death).
 *
 * When dirty: mask interrupts for the whole operation (a flash erase stalls
 * the bus, and the CAN/UART ISRs must not be fetched from a flash that is
 * mid-erase), bring HSI up for the flash controller, unlock, erase the page,
 * then program the 32 bytes one word at a time, updating the shadow as it
 * goes. The watchdog is kicked before every step exactly as stock does -- an
 * erase takes far longer than the ~65.5 ms WWDG window.
 *
 * Address arithmetic: PARAM_PAGE_BASE + i*4 for i < 32 spans
 * 0x0801F800..0x0801F87C, inside the page by construction; the bounds check in
 * flash_program_word is belt and braces. */
void param_page_save(void)
{
    uint32_t i;
    int dirty = 0;

    for (i = 0; i < PARAM_COUNT; i++) {
        if (g_param_struct[i] != g_param_shadow[i]) {
            dirty = 1;
            break;
        }
    }
    if (!dirty)
        return;

    irq_disable();
    wwdg_kick();

    (void)rcc_enable_hsi_for_flash();
    flash_unlock();
    wwdg_kick();

    (void)flash_erase_page(PARAM_PAGE_BASE);
    wwdg_kick();

    for (i = 0; i < PARAM_COUNT; i++) {
        uint8_t v = g_param_struct[i];
        g_param_shadow[i] = v;
        (void)flash_program_word(PARAM_PAGE_BASE + i * 4U, (uint32_t)v);
        wwdg_kick();
    }

    flash_lock();
    irq_enable();
}

/* --- 0x08007b6c (can_config_load_defaults) --- Validate the loaded block,
 * apply defaults to a virgin/corrupt page, then prime every RAM block that is
 * derived from it.
 *
 * Note it does NOT write flash: the defaults land in g_param_struct only, and
 * the next param_page_save() sees the block as dirty and burns them in. */
void param_validate_defaults(void)
{
    uint32_t i;
    uint8_t v;

    /* Virgin or corrupt page -> stock defaults. (-0x56 in the decompile is
     * 0xAA read as a signed char.) */
    if (g_param_struct[PARAM_MAGIC] != PARAM_MAGIC_VALUE) {
        g_param_struct[PARAM_MAGIC]   = PARAM_MAGIC_VALUE;
        g_param_struct[PARAM_PROFILE] = 1;     /* default: 125k / profile 1  */
        g_param_struct[PARAM_VOLUME]  = 0;
        /* stock: strncpy(param+0x11, "\n\n\n\n\n", 6) -- five 10s plus the NUL
         * that lands in [0x16]. 10 is the centre of the 1..19 tone range. */
        for (i = 0; i < PARAM_TONE_COUNT; i++)
            g_param_struct[PARAM_TONE_FIRST + i] = 10;
        g_param_struct[PARAM_TONE_FIRST + PARAM_TONE_COUNT] = 0;
    }

    /* Only profiles 0 and 1 exist; anything else means a half-written page. */
    if (g_param_struct[PARAM_PROFILE] > 1U)
        g_param_struct[PARAM_PROFILE] = 1;

    g_profile_active = g_param_struct[PARAM_PROFILE];

    /* CAN 0x416 payload -- constant: FE 16 3F FF FF FF FF FF */
    g_swc_tx_buf[0] = 0xFE;
    g_swc_tx_buf[1] = 0x16;
    g_swc_tx_buf[2] = 0x3F;
    g_swc_tx_buf[3] = 0xFF;
    g_swc_tx_buf[4] = 0xFF;
    g_swc_tx_buf[5] = 0xFF;
    g_swc_tx_buf[6] = 0xFF;
    g_swc_tx_buf[7] = 0xFF;

    /* Stored tone values -> the amplifier CAN frame and the UART state block.
     * Indices 1..5 in both, i.e. BAL/FAD/BASS/MID/TRE; volume goes to [0]. */
    for (i = 1; i < 6U; i++) {
        v = g_param_struct[PARAM_VOLUME + i];
        g_tx_frame_amp[i] = v;
        g_vstate_mid[i]   = v;
    }
    g_vstate_mid[0] = g_param_struct[PARAM_VOLUME];

    /* The same five values are aliased a second time further into the block --
     * this is the documented "0A 0A 0D 0A 0A tail" quirk in UART cmd 0x12. */
    g_vstate_mid[0x16] = g_tx_frame_amp[1];
    g_vstate_mid[0x17] = g_tx_frame_amp[2];
    g_vstate_mid[0x18] = g_tx_frame_amp[3];
    g_vstate_mid[0x19] = g_tx_frame_amp[4];
    g_vstate_mid[0x1A] = g_tx_frame_amp[5];

    /* g_vstate_mid[0x15] is g_amp_level @0x20001b75: 30 on profile 1, 20 on 0 */
    g_vstate_mid[0x15] = (g_profile_active == 1U) ? 0x1EU : 0x14U;

    /* --- periodic CAN TX frame templates, profile dependent --------------- */
    g_tx_frame_f0[0] = 0;
    g_tx_frame_f0[1] = 0;
    g_tx_frame_f0[2] = 0;
    g_tx_frame_f0[3] = 0;

    if (g_profile_active == 1U) {
        g_tx_frame_326[0] = 0;
        g_tx_frame_326[1] = 0;
        g_tx_frame_326[2] = 0;
        g_tx_frame_326[3] = 0;
    } else {
        g_tx_frame_326[0] = 0;
        g_tx_frame_326[1] = 7;
        g_tx_frame_326[2] = 0x0F;
        g_tx_frame_326[3] = 7;
    }
    g_tx_frame_326[4] = 0;
    g_tx_frame_326[5] = 0;
    g_tx_frame_326[6] = 0;

    g_tx_frame_190[0] = 1;
    g_tx_frame_190[1] = 0x0D;
    g_tx_frame_190[2] = 0x65;
    g_tx_frame_190[3] = 0;
    g_tx_frame_190[4] = 0x10;
    g_tx_frame_190[5] = 0;
    g_tx_frame_190[6] = 7;

    g_tx_frame_18c[0] = 0x50;
    g_tx_frame_18c[1] = 0;
    g_tx_frame_18c[2] = 0;
    g_tx_frame_18c[3] = 0x20;
    g_tx_frame_18c[4] = 0;
    g_tx_frame_18c[5] = 0;
    g_tx_frame_18c[6] = 0;
    g_tx_frame_18c[7] = 0;

    g_tx_frame_9f[0] = 1;
    g_tx_frame_9f[1] = 0x23;
    g_tx_frame_9f[2] = 0x6B;
    g_tx_frame_9f[3] = 0;
    g_tx_frame_9f[4] = 0;
    g_tx_frame_9f[5] = 0;
    g_tx_frame_9f[6] = 0;
    g_tx_frame_9f[7] = 0x1F;
}
