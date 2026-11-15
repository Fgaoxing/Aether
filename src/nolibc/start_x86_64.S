/* _start — x86_64 Linux。内核入口：rsp 指向 argc，随后 argv、NULL、envp。 */
    .text
    .globl _start
    .type _start, @function
_start:
    xor     %rbp, %rbp          /* 终止回溯 */
    mov     (%rsp), %rdi        /* argc */
    lea     8(%rsp), %rsi       /* argv */
    lea     16(%rsp,%rdi,8), %rdx  /* envp = sp + 16 + argc*8 */
    and     $-16, %rsp          /* 调用点 16 字节对齐 */
    call    main
    mov     %eax, %edi          /* aether_exit(long code) */
    call    aether_exit
1:  jmp     1b
    .size _start, . - _start
