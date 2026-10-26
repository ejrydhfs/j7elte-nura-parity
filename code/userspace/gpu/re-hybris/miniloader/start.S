    .syntax unified
    .arm
    .text
    .global _start
    .type _start, %function
_start:
    ldr     r0, [sp]
    add     r1, sp, #4
    ldr     r2, =main
    blx     r2
    mov     r7, #1
    svc     #0
1:  b       1b
    .size _start, .-_start
    .ltorg
