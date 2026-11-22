/* _start — loongarch64 Linux（LP64D 小端）。内核入口：sp 指向 argc，随后 argv、NULL、envp。 */
    .text
    .globl _start
    .type _start, @function
_start:
    move        $fp, $zero          /* 终止回溯 */
    ld.d        $a0, $sp, 0         /* argc */
    addi.d      $a1, $sp, 8         /* argv */
    slli.d      $t0, $a0, 3
    add.d       $t0, $t0, $sp
    addi.d      $a2, $t0, 16        /* envp = sp + argc*8 + 16 */
    bstrins.d   $sp, $zero, 3, 0    /* 16 字节对齐 */
    bl          main
    bl          aether_exit         /* a0：返回值即退出码 */
1:  b           1b
    .size _start, . - _start
