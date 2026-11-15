/* _start — riscv64 Linux（RV64GC 小端）。内核入口：sp 指向 argc，随后 argv、NULL、envp。 */
    .text
    .globl _start
    .type _start, @function
_start:
    li      x8, 0               /* 终止回溯 */
    ld      a0, 0(sp)           /* argc */
    addi    a1, sp, 8           /* argv */
    slli    t0, a0, 3
    add     t0, t0, sp
    addi    a2, t0, 16          /* envp = sp + argc*8 + 16 */
    andi    sp, sp, -16         /* 调用点 16 字节对齐 */
    call    main
    call    aether_exit         /* a0：返回值即退出码 */
1:  j       1b
    .size _start, . - _start
