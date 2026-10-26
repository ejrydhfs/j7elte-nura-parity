# 1 "start.S"
# 1 "<built-in>" 1
# 1 "<built-in>" 3
# 425 "<built-in>" 3
# 1 "<command line>" 1
# 1 "<built-in>" 2
# 1 "start.S" 2

    .syntax unified
    .arm
    .text
    .global _start
    .type _start, %function
_start:
    mov r0, sp @ raw_args: argc at [sp], argv, NULL, envp
    mov r1, #0 @ onexit
    ldr r2, =main @ slingshot
    ldr r3, =g_structors @ zeroed structors (bionic tolerates NULL on API 23?)
    bl __libc_init
1: b 1b
    .size _start, .-_start
    .ltorg
