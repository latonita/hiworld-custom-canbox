/* ============================================================================
 * app/hal/hal_flash.c -- on-chip flash controller driver (HAL layer).
 *
 * Ported from the FLASH primitives in the retired app/port/param.c, which came from
 * reference/decompile/app/flash.c (Ghidra output, vendor FLASH driver):
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
 *
 * The _DEAD routines in reference/decompile/app/flash.c (mass erase, RDP, option bytes, write
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
 *
 * NOTE ON HSI -- a deliberate NON-dependency. The flash controller on this
 * part is fed from HSI, so the stock save path brings HSI up
 * (rcc_enable_hsi_for_flash @0x080046b4) BEFORE unlocking. That call sits in
 * param_page_save @0x08002d46 -- i.e. in the caller, which is services-layer
 * code -- and in none of the primitives below, so this unit has no dependency
 * on hal_rcc and must not grow one. The HSI must still be brought up by
 * whoever drives a save; skipping it makes every erase/program fail silently
 * or hang in the busy wait.
 *
 * LAYER CONTRACT: pure C11, MMIO only. No RAM state, no globals.h.
 * ==========================================================================*/

#include "hal_flash.h"

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
 * Bounds checking -- the whole point of this file
 * ========================================================================= */

/* True only for [addr, addr+len) fully inside 0x0801F800..0x0801FFFF.
 *
 * Written so that no sum can overflow: the span test is phrased as
 * (PARAM_PAGE_LAST - addr) < (len - 1), with addr already proved to be inside
 * the page and len already proved non-zero, instead of comparing addr + len
 * against the end. A wrapping addr + len is exactly how a guard like this
 * gets talked into erasing the bootloader. */
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

/* --- no stock counterpart --- flash_read_word
 * The single read path into the parameter page; see the note in hal_flash.h.
 * volatile because the page changes under us when the erase/program path runs. */
uint32_t flash_read_word(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}
