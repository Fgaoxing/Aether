/* sigreturn trampoline — x86_64。x86_64 内核不为信号返回注入代码，
 * handler 返回后跳到这里，走 rt_sigreturn(15) 回内核恢复现场。 */
    .text
    .globl aether_sig_restorer
    .type aether_sig_restorer, @function
aether_sig_restorer:
    movq    $15, %rax           /* __NR_rt_sigreturn */
    syscall
    /* 不会返回 */
    .size aether_sig_restorer, . - aether_sig_restorer
