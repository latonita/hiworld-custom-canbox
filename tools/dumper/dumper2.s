        .syntax unified
        .cpu cortex-m4
        .thumb
        .section .entry,"ax"
        .global _start
        .thumb_func
_start:
        .rept 128
        nop                      @ 256-byte sled: bootloader enters at some offset, slides here
        .endr
        ldr   r0, =0x20006000
        mov   sp, r0
        ldr   r0, =0x20005000     @ started marker
        ldr   r1, =0x53544152
        str   r1, [r0]
        ldr   r0, =0x20002000     @ copy 8 KB: 0x08000000 -> 0x20002000
        ldr   r1, =0x08000000
        ldr   r2, =0x2000
1:      ldr   r3, [r1], #4
        str   r3, [r0], #4
        subs  r2, r2, #4
        bne   1b
        ldr   r0, =0x20004000     @ copy 2 KB param page -> 0x20004000
        ldr   r1, =0x0801F800
        ldr   r2, =0x800
2:      ldr   r3, [r1], #4
        str   r3, [r0], #4
        subs  r2, r2, #4
        bne   2b
        ldr   r0, =0x20005004     @ done marker
        ldr   r1, =0x444F4E45
        str   r1, [r0]
3:      b     3b
        .ltorg
