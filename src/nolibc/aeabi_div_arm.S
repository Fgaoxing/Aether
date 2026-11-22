    .syntax unified
    .arm
    .text

/* ARM EABI 32-bit unsigned divmod: r0=n, r1=d -> r0=q, r1=r */
    .globl __aeabi_uidivmod
    .type __aeabi_uidivmod, %function
__aeabi_uidivmod:
    cmp     r1, #0
    beq     .Luidiv_zero
    push    {r4, r5, lr}
    mov     r4, r1              /* den */
    mov     r5, r0              /* num */
    mov     r0, #0              /* quot */
    mov     r1, #1              /* bit */
    /* align denominator */
1:  cmp     r4, r5, lsr #1
    bhi     2f
    lsls    r2, r4, #1          /* test overflow */
    bcs     2f
    lsl     r4, r4, #1
    lsl     r1, r1, #1
    b       1b
2:  cmp     r5, r4
    blo     3f
    sub     r5, r5, r4
    add     r0, r0, r1
3:  lsr     r1, r1, #1
    lsr     r4, r4, #1
    cmp     r1, #0
    bne     2b
    mov     r1, r5              /* rem */
    pop     {r4, r5, pc}
.Luidiv_zero:
    mov     r0, #0
    mov     r1, #0
    bx      lr
    .size __aeabi_uidivmod, . - __aeabi_uidivmod

    .globl __aeabi_uidiv
    .type __aeabi_uidiv, %function
__aeabi_uidiv:
    push    {lr}
    bl      __aeabi_uidivmod
    pop     {pc}
    .size __aeabi_uidiv, . - __aeabi_uidiv

/* ARM EABI 32-bit signed divmod: r0=n, r1=d -> r0=q, r1=r */
    .globl __aeabi_idivmod
    .type __aeabi_idivmod, %function
__aeabi_idivmod:
    push    {r4-r6, lr}
    mov     r4, r0              /* n */
    mov     r5, r1              /* d */
    eor     r6, r4, r5          /* sign of q */
    cmp     r4, #0
    rsblt   r0, r4, #0
    movge   r0, r4
    cmp     r5, #0
    rsblt   r1, r5, #0
    movge   r1, r5
    bl      __aeabi_uidivmod
    cmp     r6, #0
    rsblt   r0, r0, #0          /* negate q if needed */
    cmp     r4, #0
    rsblt   r1, r1, #0          /* rem sign follows numerator */
    pop     {r4-r6, pc}
    .size __aeabi_idivmod, . - __aeabi_idivmod

    .globl __aeabi_idiv
    .type __aeabi_idiv, %function
__aeabi_idiv:
    push    {lr}
    bl      __aeabi_idivmod
    pop     {pc}
    .size __aeabi_idiv, . - __aeabi_idiv

/* 64-bit unsigned divmod: (r0:r1=n, r2:r3=d) -> (r0:r1=q, r2:r3=r) */
    .globl __aeabi_uldivmod
    .type __aeabi_uldivmod, %function
__aeabi_uldivmod:
    push    {r4-r7, lr}
    sub     sp, sp, #16
    /* r0-r1=n, r2-r3=d, 5th arg = &rem */
    add     r12, sp, #8
    str     r12, [sp]
    bl      __udivmoddi4
    ldr     r2, [sp, #8]
    ldr     r3, [sp, #12]
    add     sp, sp, #16
    pop     {r4-r7, pc}
    .size __aeabi_uldivmod, . - __aeabi_uldivmod

/* 64-bit signed divmod: (r0:r1=n, r2:r3=d) -> (r0:r1=q, r2:r3=r) */
    .globl __aeabi_ldivmod
    .type __aeabi_ldivmod, %function
__aeabi_ldivmod:
    push    {r4-r7, lr}
    sub     sp, sp, #16
    add     r12, sp, #8
    str     r12, [sp]
    bl      __divmoddi4
    ldr     r2, [sp, #8]
    ldr     r3, [sp, #12]
    add     sp, sp, #16
    pop     {r4-r7, pc}
    .size __aeabi_ldivmod, . - __aeabi_ldivmod
