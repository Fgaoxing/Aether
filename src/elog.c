#include "elog.h"
#include "syscall_shim.h"

#include <stdarg.h>
#include <stddef.h> /* ptrdiff_t */

/* 单个转换最多 33 字节（64 位十六进制 + 符号 + '%' 之类） */
#define ELOG_NUM_BUF 40

static size_t ustrlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        long n = sys_write(fd, buf, len);
        if (n <= 0) return; /* 输出失败就算了，救援环境不引回滚语义 */
        buf += n;
        len -= (size_t)n;
    }
}

static const char *fmt_ull(unsigned long long v, unsigned base, int upper, char *out, size_t cap)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[ELOG_NUM_BUF];
    size_t i = 0, k;

    if (v == 0) {
        tmp[i++] = '0';
    } else {
        while (v > 0 && i < sizeof(tmp)) {
            tmp[i++] = digits[v % base];
            v /= base;
        }
    }
    if (i >= cap) i = cap - 1;
    for (k = 0; k < i; k++)
        out[k] = tmp[i - 1 - k];
    out[i] = '\0';
    return out;
}

static void elog_vwrite(int fd, const char *fmt, va_list ap)
{
    char c;

    while ((c = *fmt++) != '\0') {
        if (c != '%') {
            write_all(fd, &c, 1);
            continue;
        }
        c = *fmt++;
        if (c == '\0') break;
        if (c == '%') {
            write_all(fd, "%", 1);
            continue;
        }

        /* 宽度：-（左对齐） + 数字 + 可选 0（0 填充，仅数字/右对齐） */
        int left_align = 0, zero_pad = 0;
        size_t width = 0;
        if (c == '-') { left_align = 1; c = *fmt++; }
        if (c == '0') { zero_pad = 1; c = *fmt++; }
        while (c >= '0' && c <= '9') { width = width * 10 + (size_t)(c - '0'); c = *fmt++; }
        if (left_align) zero_pad = 0; /* 左对齐无视 0 填充 */

        /* 长度前缀 */
        int is_long = 0, is_longlong = 0, is_size = 0;
        while (c == 'l' || c == 'z') {
            if (c == 'z') is_size = 1;
            else if (is_long) is_longlong = 1;
            else is_long = 1;
            c = *fmt++;
            if (c == '\0') return;
        }

        /* 统一把结果格式化进 out，再按宽度输出 */
        char out[ELOG_NUM_BUF + 1];
        size_t out_len = 0;
        int is_str = 0;
        const char *str_val = 0;

        if (c == 'd' || c == 'i') {
            long long v;
            if (is_size)      v = (long long)va_arg(ap, ptrdiff_t);
            else if (is_longlong) v = va_arg(ap, long long);
            else if (is_long) v = (long long)va_arg(ap, long);
            else              v = (long long)va_arg(ap, int);
            unsigned long long uv;
            if (v < 0) {
                out[out_len++] = '-';
                uv = (unsigned long long)(-(v + 1)) + 1ULL;
            } else {
                uv = (unsigned long long)v;
            }
            out_len += ustrlen(fmt_ull(uv, 10, 0, out + out_len, sizeof(out) - out_len));
        } else if (c == 'u' || c == 'x') {
            unsigned long long v;
            unsigned base = (c == 'u') ? 10u : 16u;
            if (is_size)      v = (unsigned long long)va_arg(ap, size_t);
            else if (is_longlong) v = va_arg(ap, unsigned long long);
            else if (is_long) v = (unsigned long long)va_arg(ap, unsigned long);
            else              v = (unsigned long long)va_arg(ap, unsigned int);
            out_len = ustrlen(fmt_ull(v, base, 0, out, sizeof(out)));
        } else if (c == 'c') {
            out[out_len++] = (char)va_arg(ap, int);
            out[out_len] = '\0';
        } else if (c == 's') {
            str_val = va_arg(ap, const char *);
            if (str_val == 0) str_val = "(null)";
            is_str = 1;
            out_len = ustrlen(str_val);
        } else {
            /* 未知格式符：原样输出 */
            write_all(fd, "%", 1);
            if (left_align) write_all(fd, "-", 1);
            if (width) {
                char wb[12];
                size_t l = 0, m = 0;
                unsigned wv = (unsigned)width;
                char tmp[12];
                if (wv == 0) {
                    tmp[m++] = '0';
                } else {
                    while (wv) { tmp[m++] = (char)('0' + wv % 10); wv /= 10; }
                }
                while (m && l < sizeof(wb) - 1) wb[l++] = tmp[--m];
                wb[l] = '\0';
                write_all(fd, wb, l);
            }
            if (is_longlong) write_all(fd, "ll", 2);
            else if (is_long) write_all(fd, "l", 1);
            else if (is_size) write_all(fd, "z", 1);
            write_all(fd, &c, 1);
            continue;
        }

        size_t shown = out_len;
        if (is_str) {
            if (left_align) {
                write_all(fd, str_val, out_len);
                for (size_t i = out_len; i < width; i++) write_all(fd, " ", 1);
            } else {
                for (size_t i = out_len; i < width; i++) write_all(fd, " ", 1);
                write_all(fd, str_val, out_len);
            }
        } else if (zero_pad && !left_align) {
            char pc = '0';
            if (shown > 0 && out[0] == '-') {
                write_all(fd, "-", 1);
                for (size_t i = shown; i < width; i++) write_all(fd, &pc, 1);
                write_all(fd, out + 1, shown - 1);
            } else {
                for (size_t i = shown; i < width; i++) write_all(fd, &pc, 1);
                write_all(fd, out, shown);
            }
        } else {
            if (left_align) {
                write_all(fd, out, out_len);
                for (size_t i = shown; i < width; i++) write_all(fd, " ", 1);
            } else {
                for (size_t i = shown; i < width; i++) write_all(fd, " ", 1);
                write_all(fd, out, out_len);
            }
        }
    }
}

void elog_write(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    elog_vwrite(fd, fmt, ap);
    va_end(ap);
}

void elog_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    elog_vwrite(1, fmt, ap);
    write_all(1, "\n", 1);
    va_end(ap);
}

void elog_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    elog_vwrite(2, fmt, ap);
    write_all(2, "\n", 1);
    va_end(ap);
}

void elog_die(int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    elog_vwrite(2, fmt, ap);
    write_all(2, "\n", 1);
    va_end(ap);
    aether_exit(code);
}
