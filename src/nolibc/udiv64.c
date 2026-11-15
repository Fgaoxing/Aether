/*
 * udiv64.c — 32 位 freestanding 环境的 64 位*有符号*除法/取模
 * nolibc_mini.c 已提供 __udivdi3 / __umoddi3（无符号），这里补齐有符号两个：
 * 编译器会对全部 long long 除法生成 __divdi3 / __moddi3 调用。
 * 语义与 libgcc 一致：__divdi3 截断向零，__moddi3 余数符号跟被除数（C 的 %）。
 * 只使用位运算，不依赖 64 位 / 与 %。
 */

#if !defined(__LP64__)

unsigned long long __udivdi3(unsigned long long n, unsigned long long d);
unsigned long long __umoddi3(unsigned long long n, unsigned long long d);

long long __divdi3(long long n, long long d)
{
    int neg = 0;
    unsigned long long un = (unsigned long long)n;
    unsigned long long ud = (unsigned long long)d;
    unsigned long long uq;

    if (n < 0) { un = (unsigned long long)(-n); neg ^= 1; }
    if (d < 0) { ud = (unsigned long long)(-d); neg ^= 1; }
    uq = __udivdi3(un, ud);
    return neg ? -(long long)uq : (long long)uq;
}

long long __moddi3(long long n, long long d)
{
    int neg = 0;
    unsigned long long un = (unsigned long long)n;
    unsigned long long ud = (unsigned long long)d;
    unsigned long long r;

    if (n < 0) { un = (unsigned long long)(-n); neg = 1; }
    if (d < 0) { ud = (unsigned long long)(-d); }
    r = __umoddi3(un, ud);
    return neg ? -(long long)r : (long long)r;
}

#endif /* !__LP64__ */
