    .code32
/* _start — i386 Linux。内核入口：esp 指向 argc，随后 argv、NULL、envp。 */
    .text
    .globl _start
    .type _start, @function
_start:
    xor     %ebp, %ebp          /* 终止回溯 */
    mov     (%esp), %eax        /* argc */
    lea     4(%esp), %ebx       /* argv */
    lea     8(%esp,%eax,4), %ecx   /* envp = esp + 8 + argc*4 */
    and     $-16, %esp          /* 调用点 16 字节对齐 */
    push    $0                  /* dummy（占位，让调用点 esp%16==0） */
    push    %ecx                /* envp */
    push    %ebx                /* argv */
    push    %eax                /* argc */
    call    main
    add     $16, %esp
    push    $0                  /* dummy×3（先压，对齐/占位） */
    push    $0
    push    $0
    push    %eax                /* aether_exit(long code) 唯一参数：最后压，保证入口 [esp+4]=code */
    call    aether_exit
1:  jmp     1b
    .size _start, . - _start
