/*
 * free.c — memory status（§12.5）
 * 读 /proc/meminfo，打印 Mem/Swap 总量与可用量（KiB）。表头与数据行同列宽，按显示宽度对齐。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "syscall_shim.h"
#include "util.h"

static void u64toa_dec(char *dst, size_t cap, unsigned long long v)
{
    char tmp[32];
    size_t i = 0, k = 0;
    if (v == 0) { if (cap > 1) { dst[0] = '0'; dst[1] = '\0'; } return; }
    while (v && i < sizeof(tmp)) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0 && k + 1 < cap) dst[k++] = tmp[--i];
    dst[k] = '\0';
}

static void cell(char *dst, size_t cap, unsigned long long v, size_t width)
{
    char tmp[32];
    u64toa_dec(tmp, sizeof(tmp), v);
    aether_str_pad(dst, cap, tmp, width, 1);
}

static long long get_kib(const char *buf, const char *end, const char *key)
{
    size_t klen = aether_strlen(key);
    const char *p = buf;
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        size_t llen = (size_t)(p - ls);
        if (p < end) p++; /* 跳过 \n */
        if (llen > klen + 1 && ls[klen] == ':' &&
            aether_strncmp(ls, key, klen) == 0) {
            const char *q = ls + klen + 1;
            const char *lend = ls + llen;
            while (q < lend && (*q == ' ' || *q == '\t')) q++;
            long long v = 0;
            while (q < lend && *q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
            return v;
        }
    }
    return -1;
}

int tool_free(int argc, char **argv, char **envp)
{
    static char buf[16384];
    (void)argv; (void)envp;
    if (argc > 1) { elog_err("free: usage: free"); return 2; }
    long fd = sys_open("/proc/meminfo", O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) {
        elog_err("free: cannot read /proc/meminfo: %d (/proc not mounted?)", AETHER_ERR_CODE(fd));
        return 1;
    }
    long n = sys_read((int)fd, buf, sizeof(buf) - 1);
    sys_close((int)fd);
    if (n <= 0) { elog_err("free: /proc/meminfo empty"); return 1; }
    buf[n] = '\0';
    const char *end = buf + n;

    long long total     = get_kib(buf, end, "MemTotal");
    long long free_     = get_kib(buf, end, "MemFree");
    long long avail     = get_kib(buf, end, "MemAvailable");
    long long buffers   = get_kib(buf, end, "Buffers");
    long long cached    = get_kib(buf, end, "Cached");
    long long swap_tot  = get_kib(buf, end, "SwapTotal");
    long long swap_free = get_kib(buf, end, "SwapFree");

    if (avail < 0) avail = free_; /* 老内核无 MemAvailable */

    char h1[16], h2[16], h3[16], h4[16], h5[16], h6[16];
    aether_str_pad(h1, sizeof(h1), "", 6, 0);
    aether_str_pad(h2, sizeof(h2), "total", 12, 1);
    aether_str_pad(h3, sizeof(h3), "used", 12, 1);
    aether_str_pad(h4, sizeof(h4), "free", 12, 1);
    aether_str_pad(h5, sizeof(h5), "buffers/cache", 16, 1);
    aether_str_pad(h6, sizeof(h6), "available", 12, 1);
    elog_info("%s%s%s%s%s%s", h1, h2, h3, h4, h5, h6);

    if (total >= 0 && free_ >= 0) {
        long long cache = (buffers > 0 ? buffers : 0) + (cached > 0 ? cached : 0);
        long long used = total - free_ - cache;
        char c1[16], c2[16], c3[16], c4[16], c5[16], c6[16];
        aether_str_pad(c1, sizeof(c1), "Mem:", 6, 0);
        cell(c2, sizeof(c2), (unsigned long long)total, 12);
        cell(c3, sizeof(c3), (unsigned long long)used, 12);
        cell(c4, sizeof(c4), (unsigned long long)free_, 12);
        cell(c5, sizeof(c5), (unsigned long long)cache, 16);
        cell(c6, sizeof(c6), (unsigned long long)avail, 12);
        elog_info("%s%s%s%s%s%s", c1, c2, c3, c4, c5, c6);
    } else {
        elog_info("Mem: (unparseable)");
    }
    if (swap_tot >= 0 && swap_free >= 0) {
        char c1[16], c2[16], c3[16], c4[16];
        aether_str_pad(c1, sizeof(c1), "Swap:", 6, 0);
        cell(c2, sizeof(c2), (unsigned long long)swap_tot, 12);
        cell(c3, sizeof(c3), (unsigned long long)(swap_tot - swap_free), 12);
        cell(c4, sizeof(c4), (unsigned long long)swap_free, 12);
        elog_info("%s%s%s%s", c1, c2, c3, c4);
    }
    return 0;
}