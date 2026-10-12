@ Hiworld canbox bootloader dumper — flash-resident, delivered via IAP.
@
@ Runs at 0x08002000 (app base). The bootloader branches here as Thumb code
@ (the stock app image also starts with code, not a vector table). Under RDP-L1,
@ code executing from main flash may read all main flash, so we copy the 8 KB
@ vendor bootloader (0x08000000..0x08001FFF) and the 2 KB parameter page into
@ SRAM, drop two markers, and spin. Nothing is written to flash; nothing uses
@ the UART or the clock. We then read the SRAM copy out over SWD (SRAM is
@ RDP-exempt), after the flash read has already completed in non-debug mode.
@
@ SRAM layout (24 KB, 0x20000000..0x20006000):
@   0x20002000  8 KB  copy of 0x08000000..0x08001FFF  (vendor bootloader)
@   0x20004000  2 KB  copy of 0x0801F800..0x0801FFFF  (param / EEPROM page)
@   0x20005000  u32   started marker 0x53544152 ('STAR')  — entry executed
@   0x20005004  u32   done    marker 0x444F4E45 ('DONE')  — copies completed
@   0x20006000        stack top (barely used)

        .syntax unified
        .cpu    cortex-m4
        .thumb

        .section .entry,"ax"
        .global _start
        .thumb_func
_start:
        ldr     r0, =0x20006000     @ set our own stack (don't trust inherited SP)
        mov     sp, r0

        ldr     r0, =0x20005000     @ started marker
        ldr     r1, =0x53544152
        str     r1, [r0]

        @ --- copy 8 KB: 0x08000000 -> 0x20002000 ---
        ldr     r0, =0x20002000
        ldr     r1, =0x08000000
        ldr     r2, =0x2000
1:      ldr     r3, [r1], #4
        str     r3, [r0], #4
        subs    r2, r2, #4
        bne     1b

        @ --- copy 2 KB: 0x0801F800 -> 0x20004000 ---
        ldr     r0, =0x20004000
        ldr     r1, =0x0801F800
        ldr     r2, =0x800
2:      ldr     r3, [r1], #4
        str     r3, [r0], #4
        subs    r2, r2, #4
        bne     2b

        ldr     r0, =0x20005004     @ done marker
        ldr     r1, =0x444F4E45
        str     r1, [r0]

3:      b       3b                  @ spin forever (no WDG kicked; bootloader leaves it disarmed)
        .ltorg
