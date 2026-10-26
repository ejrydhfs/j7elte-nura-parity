/* Minimal Android/ARM entry point: hand the raw stack to bionic libc. */
    .syntax unified
    .arm
    .text
    .global _start
    .type _start, %function
_start:
    mov     r0, sp          @ raw_args: argc at [sp], argv, NULL, envp
    mov     r1, #0          @ onexit
    ldr     r2, =main       @ slingshot
    ldr     r3, =g_structors   @ zeroed structors (bionic tolerates NULL on API 23?)
    bl      __libc_init
1:  b       1b
    .size _start, .-_start
    .ltorg
