        .syntax unified
        .cpu cortex-m4
        .thumb
        .section .entry,"ax"
        .global _start
        .thumb_func
_start:
        ldr   r0, =0x20003000      @ marker A (write FIRST, no SP needed)
        ldr   r1, =0xC0DE0001
        str   r1, [r0]
        ldr   r0, =0x20005000      @ marker B
        ldr   r1, =0xC0DE0002
        str   r1, [r0]
        ldr   r0, =0x20006000
        mov   sp, r0
1:      b     1b
        .ltorg
