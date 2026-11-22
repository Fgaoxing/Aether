/* _start — ARM 32-bit Linux (EABI)。内核入口：sp 指向 argc，随后 argv、NULL、envp。 */
    .text
    .globl _start
    .type _start, %function
_start:
    mov     fp, #0              /* 终止回溯 */
    ldr     r0, [sp]            /* argc */
    add     r1, sp, #4          /* argv */
    lsl     r2, r0, #2
    add     r2, r2, sp
    add     r2, r2, #8          /* envp = sp + argc*4 + 8 */
    bic     sp, sp, #7          /* 8 字节对齐（AAPCS） */
    bl      main
    bl      aether_exit         /* r0：返回值即退出码 */
1:  b       1b
    .size _start, . - _start
