/*
 * nolibc_mini.c — nolibc 分支专用的极简运行时支持（§3：不引 libc / libgcc）
 * GCC/Clang 即使 -nostdlib -ffreestanding 也可能内联展开出对这些的调用，必须提供。
 * 本文件只在 nolibc 分支编译（CMakeLists 中按分支加入源码，libc 分支不编译）。
 */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') { }
    return dst;
}

/* 32 位架构上 64 位除法没有硬件指令，编译器会外调 __udivdi3 / __umoddi3。
 * nolibc 不链接任何运行时，这里自带（i386 / 将来可能的 rv32 等 32 位目标）。 */
#if !defined(__LP64__)
unsigned long long __udivdi3(unsigned long long n, unsigned long long d)
{
    unsigned long long q = 0, r = 0;
    int i;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) { r -= d; q |= 1ULL << i; }
    }
    return q;
}

unsigned long long __umoddi3(unsigned long long n, unsigned long long d)
{
    unsigned long long q = __udivdi3(n, d);
    return n - q * d;
}
#endif
