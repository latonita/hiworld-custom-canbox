/* ============================================================================
 * app.ld  —  GNU ld linker script for the Hiworld canbox APP
 * Nations N32L406CBL7 (Cortex-M4F, Thumb-2), 128 KB flash, 24 KB SRAM.
 *
 * Rebuilds ONLY the application that lives at 0x08002000. The 8 KB vendor IAP
 * bootloader at 0x08000000 is NEVER rebuilt or overwritten; it owns the real
 * Cortex-M vector table and launches this image. The 2 KB parameter page at
 * 0x0801F800 is runtime EEPROM and is NOT part of the image.
 *
 * THIS IS A STARTING-POINT SCRIPT. It encodes the hard address constraints
 * correctly, but the SECTIONS body (esp. the SRAM layout and the crt0/veneer
 * preamble) will need iteration as the real C is reconstructed. Read the
 * CONSTRAINTS block below in full before changing anything.
 * See app/REBUILD.md, docs/hiworld-startup-sequence.md,
 *     docs/hiworld-bootloader-analysis.md.
 * ==========================================================================*/

/* ----------------------------------------------------------------------------
 * CONSTRAINTS (verified against the reference image + the bootloader dump)
 *
 * 1. NO .isr_vector IN THIS IMAGE.
 *    The application has no vector table of its own. The bootloader owns VTOR
 *    (= 0x08000000) and MSP (= 0x20000600) and keeps them; the app never
 *    relocates VTOR and never sets up a stack pointer. Do NOT place a vector
 *    table (initial-SP word + reset vector + handler table) at 0x08002000.
 *    Bytes 0x08002000.. are Thumb CODE, not vectors.
 *
 * 2. ENTRY VENEER MUST LAND AT EXACTLY 0x08002202 (image offset 0x202).
 *    The bootloader enters the app with `bl 0x08002202` after it has confirmed
 *    the valid-app marker *0x08001800 == 0x19780217 (which the bootloader
 *    itself writes after a good flash). 0x08002202 is an 8-byte reset veneer:
 *        10 b5           push {r4, lr}
 *        00 f0 fc f8     bl   __main          (ARM C-lib: scatterload + rt_entry)
 *        10 bd           pop  {r4, pc}
 *    If the image's byte at offset 0x202 is anything else, the box will branch
 *    into garbage. This is the single most important constraint in the build.
 *
 * 3. 0x08002000 .. 0x08002201 IS A FIXED PREAMBLE (do not let .text grow into
 *    it from the front). It holds, in this exact order:
 *      - 42 bytes of ARM C-lib crt0 fragments at 0x08002000 (nothing is ever
 *        dispatched here -- free space in a rebuild), and
 *      - a dense array of 59 eight-byte IRQ/exception veneers running
 *        0x0800202a .. 0x08002201, each of the form
 *           push {r4,lr}; bl <handler>; pop {r4,pc}.
 *    The array is ADDRESS-INDEXED by the bootloader's immutable vector table.
 *    That mapping is NOT the linear `0x08002072 + 8*IRQn` rule quoted elsewhere
 *    in these docs -- that holds only for IRQ0..10 and IRQ23..41, because the
 *    N32L406 does not use STM32F1 interrupt numbering. Read off the real table
 *    in reference/bootloader_0x08000000_8k.bin:
 *        NMI->slot 0, HardFault->slot 1, MemManage->slot 2, BusFault->slot 3,
 *        UsageFault->slot 4, DebugMon->slot 6, IRQ0..10->slots 9..19,
 *        IRQ19->slot 27, IRQ53..56->slots 28..31, IRQ23..41->slots 32..50,
 *        IRQ50->slot 52, IRQ47->slot 54, IRQ48->slot 55, IRQ51->slot 56,
 *        IRQ52->slot 57.  Full table + evidence: app/startup/crt0.S.
 *    => For interrupts to work, every veneer slot the hardware dispatches to
 *       must sit at its original address. The veneer BODY (the bl target) may
 *       point at a rebuilt handler, but the slot ADDRESS is not negotiable.
 *    Therefore the whole 0x08002000..0x08002201 preamble is best emitted by a
 *    single hand-written crt0/veneer object placed first (section .image_entry
 *    below), laid out to match the original. See docs/hiworld-startup-sequence.md §2.
 *
 * 4. The reference (stock) image is only ~32 KB; the 118 KB region is headroom.
 *    Keep the image packed from 0x08002000 upward; do not pad to region size.
 * --------------------------------------------------------------------------*/

/* The bootloader branches here; ENTRY() is informational for GNU ld (this image
 * is called as a subroutine, not reset-vectored). Keep the symbol pinned at 0x202. */
ENTRY(image_entry)

MEMORY
{
    /* Application region only. 0x0801F7FF is the last app byte; 0x0801F800 is
     * the param/EEPROM page and must NOT be written by the image. */
    FLASH (rx)  : ORIGIN = 0x08002000, LENGTH = 0x1D800   /* 118 KB  (0x08002000..0x0801F7FF) */

    /* Full 24 KB SRAM. NOTE the two reserved sub-ranges handled in SECTIONS:
     *   0x20000000..0x20000600  system stack (MSP top = 0x20000600, grows down,
     *                           set by the bootloader — this image must not use it)
     *   0x20001000..~0x20001C24 the app's fixed absolute globals (see globals.h /
     *                           MEMORY_MAP.md) are addressed by hard pointers, NOT
     *                           allocated by this script. Linker-managed .data/.bss
     *                           must be placed so it does not overlap them. */
    SRAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 0x6000    /* 24 KB   (0x20000000..0x20005FFF) */
}

/* MSP is provided by the bootloader (0x20000600). _estack is defined only for
 * completeness / any crt0 that wants to re-init SP; the stock app does not. */
_estack      = 0x20000600;
PROVIDE(_Min_Heap_Size  = 0);
PROVIDE(_Min_Stack_Size = 0x400);

SECTIONS
{
    /* ---- Fixed preamble: crt0 + entry veneer + IRQ veneer array ----------
     * MUST be the first thing in FLASH and MUST reproduce the original layout
     * so that (a) image_entry lands at 0x08002202 and (b) each hardware-
     * dispatched veneer keeps its original address (constraint 3).
     * Provide these in a dedicated object (e.g. startup/crt0.S) exporting the
     * section .image_entry, with `image_entry` as the label at offset 0x202. */
    .image_entry 0x08002000 :
    {
        KEEP(*(.image_entry))       /* crt0 fragments + 59 veneers + entry veneer */
        KEEP(*(.image_entry.*))
    } > FLASH

    /* ---- Code ------------------------------------------------------------ */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)                  /* ARM/Thumb interworking veneers */
        *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* ---- Read-only data -------------------------------------------------- */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* ARM exception-unwind tables (usually empty with -fno-exceptions). */
    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    /* _sidata = flash load address of the .data initialisers (copied by crt0). */
    . = ALIGN(4);
    _sidata = LOADADDR(.data);

    /* ---- Initialised data (VMA in SRAM, LMA in FLASH) --------------------
     * NOTE: the stock firmware's named globals live at FIXED absolute addresses
     * (0x20001000 .. 0x20001C24, via globals.h) and are therefore NOT emitted
     * here. This region only holds compiler statics the rebuilt C introduces.
     *
     * VMA = 0x20001C40: immediately ABOVE the fixed global block, 8-byte
     * aligned, leaving 0x20001C40..0x20006000 (~17 KB) for linker-managed
     * statics. Chosen over the 0x20000600..0x20000FFF gap because that gap is
     * only 2.5 KB and sits directly on top of the bootloader's 1.5 KB stack
     * (MSP = 0x20000600, grows down) -- a stack overflow there would silently
     * corrupt .data instead of faulting. The ASSERT at the bottom enforces
     * that .data/.bss stay clear of the fixed block either way. */
    .data 0x20001C40 : AT ( _sidata )
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM

    /* ---- Zero-initialised data (BSS) ------------------------------------- */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > SRAM

    /* Optional heap/stack guard region (unused by the stock app). */
    ._user_heap_stack (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE ( end = . );
        PROVIDE ( _end = . );
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > SRAM

    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) }
}

/* ---- Build-time guards ------------------------------------------------------
 * The entry veneer MUST be at 0x08002202. Keep these ASSERTs enabled; they are
 * the cheapest possible catch for a broken preamble. */
ASSERT(DEFINED(image_entry), "image_entry (reset veneer label) is undefined — provide startup/crt0.S")
ASSERT(image_entry == 0x08002202,
       "ENTRY VENEER NOT AT 0x08002202 — the bootloader `bl 0x08002202` would miss it")
ASSERT((_ebss <= 0x20001000) || (_sbss >= 0x20001C40),
       "linker-managed .bss overlaps the fixed absolute global block at 0x20001000 (see globals.h)")
