    .syntax unified
    .arm
/* sigreturn trampoline — ARM EABI。内核不为信号返回注入代码，
 * handler 返回后跳到这里，走 svc 0 + __NR_rt_sigreturn(173) 回内核恢复现场。 */
    .text
    .globl aether_sig_restorer
    .type aether_sig_restorer, %function
aether_sig_restorer:
    mov     r7, #173            /* __NR_rt_sigreturn */
    svc     #0
    /* 不会返回 */
    .size aether_sig_restorer, . - aether_sig_restorer
