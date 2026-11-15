/* _start — aarch64 Linux（小端）。内核入口：sp 指向 argc，随后 argv、NULL、envp。 */
    .text
    .globl _start
    .type _start, @function
_start:
    mov     x29, #0             /* 终止回溯 */
    ldr     w0, [sp]            /* argc（64 位 ABI：低 32 位，写 w 寄存器清零高位） */
    add     x1, sp, #8          /* argv */
    add     x2, sp, #16
    add     x2, x2, x0, lsl #3  /* envp = sp + 16 + argc*8 */
    /* AAPCS64 §5.2.1：公共接口 sp 必须 16 字节对齐，Linux 内核保证进程入口 sp 为 16 对齐；
     * 因此无需（也无法用单条指令）在此再次对齐——直接 bl main 即满足 ABI */
    bl      main
    bl      aether_exit         /* w0 → x0：返回值即退出码 */
1:  b       1b
    .size _start, . - _start
