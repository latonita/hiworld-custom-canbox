        .syntax unified
        .cpu cortex-m4
        .thumb
        .section .entry,"ax"
        .global _start
        .thumb_func
_start:
        .rept 128
        nop                      @ 256-byte NOP sled: entry anywhere here slides down
        .endr
        ldr   r0, =0x20003000
        ldr   r1, =0xD00D0001
        str   r1, [r0]
        ldr   r0, =0x20003004
        ldr   r1, =0xD00D0002
        str   r1, [r0]
        ldr   r0, =0x20006000
        mov   sp, r0
1:      b     1b
        .ltorg
