#include "util.h"
#include "elog.h"
#include "syscall_shim.h"

/* libc/nolibc 分支都能用的极简 stdio 输入：从 fd 0 读一行，去尾部 \n/\r */
static size_t read_line(char *buf, size_t cap)
{
    size_t n = 0;
    if (cap < 2) return 0;
    for (;;) {
        char ch;
        long r = sys_read(0, &ch, 1);
        if (r <= 0) break;              /* EOF 或错误 */
        if (ch == '\n') break;
        if (n + 1 < cap) buf[n++] = ch;
    }
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) n--;
    buf[n] = '\0';
    return n;
}

int aether_strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int aether_streq(const char *a, const char *b)
{
    return aether_strcmp(a, b) == 0;
}

int aether_strncmp(const char *a, const char *b, size_t n)
{
    while (n-- > 0) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (*a == '\0') return 0;
        a++; b++;
    }
    return 0;
}

size_t aether_strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

char *aether_strcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return dst;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return dst;
}

size_t aether_u64toa(char *dst, size_t cap, unsigned long long v)
{
    char tmp[24];
    size_t i = 0, k = 0;
    if (v == 0) { if (cap > 1) { dst[0] = '0'; dst[1] = '\0'; } return 1; }
    while (v && i < sizeof(tmp)) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0 && k + 1 < cap) dst[k++] = tmp[--i];
    dst[k] = '\0';
    return k;
}

long aether_atol(const char *s)
{
    long v = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

const char *aether_base_name(const char *path)
{
    const char *base = path;
    while (*path) {
        if (*path == '/') base = path + 1;
        path++;
    }
    return base;
}

int aether_ask(const char *prompt, const char *accept)
{
    char buf[64];
    elog_write(2, "%s", prompt); /* 提示写 stderr：stdout 可能被重定向 */
    read_line(buf, sizeof(buf));
    return aether_strcmp(buf, accept) == 0;
}

/* East Asian Wide / 全角区间表（宽 = 2 列） */
static int utf8_is_wide(unsigned cp)
{
    if (cp >= 0x1100 && cp <= 0x115F) return 1;   /* Hangul Jamo */
    if (cp >= 0x2E80 && cp <= 0xA4CF) return 1;   /* CJK */
    if (cp >= 0xAC00 && cp <= 0xD7AF) return 1;   /* Hangul Syllables */
    if (cp >= 0xF900 && cp <= 0xFAFF) return 1;   /* CJK Compat Ideographs */
    if (cp >= 0xFE30 && cp <= 0xFE4F) return 1;   /* CJK Compat Forms */
    if (cp >= 0xFF00 && cp <= 0xFF60) return 1;   /* Fullwidth Forms */
    if (cp >= 0x20000 && cp <= 0x2FFFD) return 1; /* CJK Ext B.. */
    if (cp >= 0x30000 && cp <= 0x3FFFD) return 1;
    return 0;
}

void aether_human_size(unsigned long long kb, char *out, size_t cap)
{
    aether_human_bytes(kb * 1024ULL, out, cap);
}

void aether_human_bytes(unsigned long long bytes, char *out, size_t cap)
{
    const char *units[] = { "B", "K", "M", "G", "T", "P" };
    int u = 0;
    unsigned long long v = bytes;
    while (v >= 1024 && u < 5) { v /= 1024; u++; }
    unsigned long long div = 1ULL;
    for (int i = 0; i < u; i++) div *= 1024;
    unsigned long long whole = bytes / div;
    unsigned long long frac = ((bytes % div) * 10 + div / 2) / div;
    if (frac >= 10) { whole += 1; frac = 0; }
    char num[24];
    size_t m = 0, k = 0;
    unsigned long long t = whole;
    if (t == 0) num[m++] = '0';
    while (t && m < sizeof(num)) { num[m++] = (char)('0' + (t % 10)); t /= 10; }
    while (m && k + 2 < cap) out[k++] = num[--m];
    if (u > 0 && k + 3 < cap) { out[k++] = '.'; out[k++] = (char)('0' + frac); }
    const char *un = units[u];
    for (size_t i = 0; un[i] && k + 2 < cap; i++) out[k++] = un[i];
    out[k] = '\0';
}

/* civil_from_days（Howard Hinnant 算法）：days since 1970-01-01 → 年月日 */
static void aether_civil_from_days(long long z, long long *y, long long *mo, long long *d)
{
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long long doe = (unsigned long long)(z - era * 146097);
    unsigned long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = (long long)yoe + era * 400;
    unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long long mp = (5 * doy + 2) / 153;
    long long dd = (long long)(doy - (153 * mp + 2) / 5 + 1);
    long long mm = (long long)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (mm <= 2);
    *mo = mm;
    *d = dd;
}

static void u2(char *out, size_t *k, unsigned long long v)
{
    /* 两位零填充 */
    out[(*k)++] = (char)('0' + (v / 10) % 10);
    out[(*k)++] = (char)('0' + v % 10);
}

size_t aether_iso_time(long long sec, char *out, size_t cap)
{
    if (cap < 20) { if (cap) out[0] = '\0'; return 0; }
    long long days = sec / 86400;
    long long rem = sec - days * 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    long long y, mo, d;
    aether_civil_from_days(days, &y, &mo, &d);
    unsigned long long hh = (unsigned long long)(rem / 3600);
    unsigned long long mm = (unsigned long long)((rem % 3600) / 60);
    unsigned long long ss = (unsigned long long)(rem % 60);
    /* "YYYY-MM-DD HH:MM:SS\0" = 19 + 1 = 20 字节，year 超过 9999 也要保证有空间 */
    char buf[32];
    size_t k = 0;
    /* 年 */
    long long yy = y;
    char ybuf[8];
    size_t m = 0;
    if (yy == 0) ybuf[m++] = '0';
    while (yy) { ybuf[m++] = (char)('0' + (yy % 10)); yy /= 10; }
    while (m) buf[k++] = ybuf[--m];
    buf[k++] = '-'; u2(buf, &k, (unsigned long long)mo);
    buf[k++] = '-'; u2(buf, &k, (unsigned long long)d);
    buf[k++] = ' '; u2(buf, &k, hh);
    buf[k++] = ':'; u2(buf, &k, mm);
    buf[k++] = ':'; u2(buf, &k, ss);
    buf[k] = '\0';
    for (size_t i = 0; i <= k; i++) out[i] = buf[i];
    return k;
}

void aether_mode_str(unsigned int mode, char out[11])
{
    unsigned type = mode & 0170000;
    char t;
    switch (type) {
    case 0040000: t = 'd'; break;
    case 0100000: t = '-'; break;
    case 0120000: t = 'l'; break;
    case 0060000: t = 'b'; break;
    case 0020000: t = 'c'; break;
    case 0010000: t = 'p'; break;
    case 0140000: t = 's'; break;
    default: t = '?'; break;
    }
    out[0] = t;
    /* owner */
    out[1] = (mode & 0400) ? 'r' : '-';
    out[2] = (mode & 0200) ? 'w' : '-';
    out[3] = (mode & 0100) ? ((mode & 04000) ? 's' : 'x') : ((mode & 04000) ? 'S' : '-');
    /* group */
    out[4] = (mode & 0040) ? 'r' : '-';
    out[5] = (mode & 0020) ? 'w' : '-';
    out[6] = (mode & 0010) ? ((mode & 02000) ? 's' : 'x') : ((mode & 02000) ? 'S' : '-');
    /* other */
    out[7] = (mode & 0004) ? 'r' : '-';
    out[8] = (mode & 0002) ? 'w' : '-';
    out[9] = (mode & 0001) ? ((mode & 01000) ? 't' : 'x') : ((mode & 01000) ? 'T' : '-');
    out[10] = '\0';
}

void aether_classify_part(const char *name, char *parent, size_t pcap, int *is_part)
{
    size_t n = aether_strlen(name);
    *is_part = 0;
    parent[0] = '\0';

    /* nvme0n1 / nvme0n1p3, mmcblk0 / mmcblk0p2, loop0 / loop0p1：整盘与 pN 分区 */
    if (aether_strncmp(name, "nvme", 4) == 0 || aether_strncmp(name, "mmcblk", 6) == 0 ||
        aether_strncmp(name, "loop", 4) == 0) {
        size_t d_end = n;
        while (d_end > 0 && name[d_end - 1] >= '0' && name[d_end - 1] <= '9') d_end--;
        if (d_end < n && d_end > 1 && name[d_end - 1] == 'p') {
            size_t keep = d_end - 1; /* 去掉 pN */
            if (keep >= pcap) keep = pcap - 1;
            for (size_t k = 0; k < keep; k++) parent[k] = name[k];
            parent[keep] = '\0';
            *is_part = 1;
            return;
        }
        return; /* 整盘 */
    }

    /* 通用：字母前缀 + 全数字结尾（sda1 / vda2 / hda5 / sr0）：前缀必须全字母 */
    size_t i = n;
    while (i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9') i--;
    if (i > 0 && i < n) {
        int alpha = 1;
        for (size_t k = 0; k < i; k++) {
            char c = name[k];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { alpha = 0; break; }
        }
        if (alpha) {
            if (i >= pcap) i = pcap - 1;
            for (size_t k = 0; k < i; k++) parent[k] = name[k];
            parent[i] = '\0';
            *is_part = 1;
            return;
        }
    }
}

/* ---------- 缓冲读取器 ---------- */
void aether_reader_init(aether_reader_t *r, int fd)
{
    r->fd = fd;
    r->start = 0;
    r->len = 0;
}

static int reader_fill(aether_reader_t *r)
{
    if (r->start < r->len) return 0; /* 还有数据 */
    for (;;) {
        long n = sys_read(r->fd, r->buf, sizeof(r->buf));
        if (n == 0) { r->start = 0; r->len = 0; return -1; } /* EOF：清空水位，防止陈旧数据被反复读出 */
        if (AETHER_IS_ERR(n)) {
            if (AETHER_ERR_CODE(n) == 4) continue; /* EINTR 重试 */
            r->start = 0; r->len = 0;
            return -1;
        }
        r->start = 0;
        r->len = (size_t)n;
        return 0;
    }
}

int aether_reader_getc(aether_reader_t *r)
{
    if (reader_fill(r) != 0) return -1;
    return (unsigned char)r->buf[r->start++];
}

long aether_reader_line(aether_reader_t *r, char *out, size_t cap)
{
    size_t n = 0;
    if (cap < 2) return -1;
    for (;;) {
        int ch = aether_reader_getc(r);
        if (ch == -1) { /* EOF */
            if (n == 0) return -1;
            break;
        }
        if (ch == '\n') break;
        if (n + 1 < cap) out[n++] = (char)ch;
    }
    while (n > 0 && out[n - 1] == '\r') n--; /* 去 \r */
    out[n] = '\0';
    return (long)n;
}

long aether_read_all(int fd, char *buf, size_t cap)
{
    size_t off = 0;
    for (;;) {
        long n = sys_read(fd, buf + off, cap - off);
        if (n == 0) break;
        if (AETHER_IS_ERR(n)) {
            if (AETHER_ERR_CODE(n) == 4) continue;
            return -1;
        }
        off += (size_t)n;
        if (off >= cap) break; /* 截断 */
    }
    return (long)off;
}

void aether_prints(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
    sys_write(1, "\n", sizeof("\n") - 1);
}

size_t aether_utf8_cells(const char *s)
{
    size_t cells = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned cp;
        size_t len;
        if (*p < 0x20 || (*p >= 0x7F && *p < 0xA0)) { p++; continue; } /* 控制字符不计 */
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
        else { cp = 0xFFFD; len = 1; } /* 非法字节：替换符，按 1 列计 */
        for (size_t i = 1; i < len && p[i]; i++) cp = (cp << 6) | (p[i] & 0x3F);
        cells += utf8_is_wide(cp) ? 2 : 1;
        p += len;
    }
    return cells;
}

size_t aether_str_pad(char *dst, size_t cap, const char *src, size_t width, int align)
{
    size_t slen = aether_strlen(src);
    size_t cells = aether_utf8_cells(src);
    size_t pad = cells < width ? width - cells : 0;
    size_t d = 0;
    if (cap < 1) return 0;
    if (align == 1) { /* 右对齐：先填空格 */
        for (size_t i = 0; i < pad && d + 1 < cap; i++) dst[d++] = ' ';
    }
    for (size_t i = 0; i < slen && d + 1 < cap; i++) dst[d++] = src[i];
    if (align == 0) { /* 左对齐：后补空格 */
        for (size_t i = 0; i < pad && d + 1 < cap; i++) dst[d++] = ' ';
    }
    dst[d] = '\0';
    return d;
}
