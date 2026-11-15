/*
 * cmp_diff.c — diff / join / split
 * diff: 行级 diff（公共前缀/后缀修剪 + 中段 LCS），输出 < / > 行
 * join: 按第一个字段合并（两个已按第一个字段sort的文件）
 * split: 按行数切分
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "syscall_shim.h"
#include "util.h"

#define DIFF_BUFSZ (1024 * 1024)
static char diff_a[DIFF_BUFSZ], diff_b[DIFF_BUFSZ];
static char *a_lines[16384], *b_lines[16384];

static int read_into(const char *path, char *buf, size_t cap)
{
    long fd = sys_open(path, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) { elog_err("%s: cannot open", path); return -1; }
    size_t off = 0;
    for (;;) {
        long n = sys_read((int)fd, buf + off, cap - off);
        if (n == 0) break;
        if (AETHER_IS_ERR(n)) return -1;
        off += (size_t)n;
        if (off >= cap) break;
    }
    sys_close((int)fd);
    buf[off] = '\0';
    return (int)off;
}

static long split_lines2(char *buf, long len, char **lines, long max)
{
    long n = 0;
    char *p = buf, *end = buf + len;
    while (p < end && n < max) {
        lines[n++] = p;
        while (p < end && *p != '\n') p++;
        if (p < end) { *p = '\0'; p++; }
    }
    return n;
}

/* LCS 长度（只用于小段 ≤64 行） */
static int lcs_len(char **a, long na, char **b, long nb)
{
    static int dp[65][65];
    for (long i = 0; i <= na; i++) dp[i][0] = 0;
    for (long j = 0; j <= nb; j++) dp[0][j] = 0;
    for (long i = 1; i <= na; i++)
        for (long j = 1; j <= nb; j++) {
            if (aether_streq(a[i - 1], b[j - 1])) dp[i][j] = dp[i - 1][j - 1] + 1;
            else dp[i][j] = dp[i - 1][j] > dp[i][j - 1] ? dp[i - 1][j] : dp[i][j - 1];
        }
    return dp[na][nb];
}

/* 简单 diff：找公共前缀/后缀，中段 LCS，输出 */
int tool_diff(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 3) { elog_err("diff: usage: diff <file1> <file2>"); return 2; }

    int alen = read_into(argv[1], diff_a, DIFF_BUFSZ);
    if (alen < 0) return 1;
    int blen = read_into(argv[2], diff_b, DIFF_BUFSZ);
    if (blen < 0) return 1;

    long na = split_lines2(diff_a, (long)alen, a_lines, (long)(sizeof(a_lines) / sizeof(a_lines[0])));
    long nb = split_lines2(diff_b, (long)blen, b_lines, (long)(sizeof(b_lines) / sizeof(b_lines[0])));

    long pre = 0;
    while (pre < na && pre < nb && aether_streq(a_lines[pre], b_lines[pre])) pre++;
    long suf_a = na, suf_b = nb;
    while (suf_a > pre && suf_b > pre && aether_streq(a_lines[suf_a - 1], b_lines[suf_b - 1])) { suf_a--; suf_b--; }

    long amid = suf_a - pre, bmid = suf_b - pre;
    /* LCS 只对小段（≤64 行） */
    int common = 0;
    if (amid > 0 && bmid > 0 && amid <= 64 && bmid <= 64)
        common = lcs_len(a_lines + pre, amid, b_lines + pre, bmid);

    /* 输出 */
    int rc = 0;
    if (amid || bmid) rc = 1;
    /* 公共前缀（不打印，仅定位） */
    if (rc == 0) { return 0; }

    elog_info("--- %s", argv[1]);
    elog_info("+++ %s", argv[2]);
    elog_info("@@ -%ld +%ld @@", pre + 1, pre + 1);
    for (long k = pre; k < suf_a; k++) elog_info("-%s", a_lines[k]);
    for (long k = pre; k < suf_b; k++) elog_info("+%s", b_lines[k]);
    (void)common;
    return rc;
}

/* ---------- join ---------- */

int tool_join(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 3) { elog_err("join: usage: join <file1> <file2> [field]"); return 2; }
    const char *path1 = argv[1], *path2 = argv[2];
    int field = (argc >= 4) ? (int)aether_atol(argv[3]) : 1;
    if (field < 1) field = 1;

    long a_len = read_into(path1, diff_a, DIFF_BUFSZ);
    if (a_len < 0) return 1;
    long b_len = read_into(path2, diff_b, DIFF_BUFSZ);
    if (b_len < 0) return 1;

    long na = split_lines2(diff_a, a_len, a_lines, (long)(sizeof(a_lines) / sizeof(a_lines[0])));
    long nb = split_lines2(diff_b, b_len, b_lines, (long)(sizeof(b_lines) / sizeof(b_lines[0])));

    long ia = 0, ib = 0;
    while (ia < na && ib < nb) {
        const char *ka = a_lines[ia], *kb = b_lines[ib];
        /* 取第 field 个字段 */
        const char *fa = ka, *fb = kb;
        for (int k = 1; k < field; k++) { while (*fa && *fa != ' ' && *fa != '\t') fa++; while (*fa == ' ' || *fa == '\t') fa++; }
        for (int k = 1; k < field; k++) { while (*fb && *fb != ' ' && *fb != '\t') fb++; while (*fb == ' ' || *fb == '\t') fb++; }
        size_t la = 0, lb = 0;
        while (fa[la] && fa[la] != ' ' && fa[la] != '\t') la++;
        while (fb[lb] && fb[lb] != ' ' && fb[lb] != '\t') lb++;
        int cmp = 0;
        for (size_t k = 0; k < (la > lb ? la : lb); k++) {
            char ca = k < la ? fa[k] : '\0', cb = k < lb ? fb[k] : '\0';
            if (ca != cb) { cmp = (unsigned char)ca - (unsigned char)cb; break; }
        }
        if (cmp == 0) {
            elog_info("%s %s", ka, kb);
            ia++; ib++;
        } else if (cmp < 0) ia++;
        else ib++;
    }
    return 0;
}

/* ---------- split ---------- */

int tool_split(int argc, char **argv, char **envp)
{
    (void)envp;
    long lines_per = 1000;
    int i = 1;
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-l") && i + 1 < argc) lines_per = aether_atol(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1]) { elog_err("split: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    const char *path = (i < argc) ? argv[i] : 0;
    const char *prefix = (i + 1 < argc) ? argv[i + 1] : "x";

    int fd = 0;
    if (path) {
        long f = sys_open(path, O_RDONLY, 0);
        if (AETHER_IS_ERR(f)) { elog_err("split: cannot open %s", path); return 1; }
        fd = (int)f;
    }
    static char buf[DIFF_BUFSZ];
    long len = aether_read_all(fd, buf, sizeof(buf) - 1);
    if (path) sys_close(fd);
    if (len < 0) return 1;
    buf[len] = '\0';

    int part = 0;
    long written = 0;
    char name[64];
    int outfd = -1;
    char *p = buf, *end = buf + len;
    while (p < end) {
        if (outfd < 0) {
            /* 创建新分卷文件 */
            char suffix[4] = { (char)('a' + (part / 26)), (char)('a' + (part % 26)), '\0' };
            size_t d = 0;
            for (size_t k = 0; prefix[k] && d + 1 < sizeof(name); k++) name[d++] = prefix[k];
            for (size_t k = 0; suffix[k] && d + 1 < sizeof(name); k++) name[d++] = suffix[k];
            name[d] = '\0';
            long f = sys_open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (AETHER_IS_ERR(f)) { elog_err("split: cannot create %s", name); return 1; }
            outfd = (int)f;
            written = 0;
        }
        /* 写一行 */
        char *line_start = p;
        while (p < end && *p != '\n') p++;
        size_t llen = (size_t)(p - line_start) + 1;
        if (p < end) p++;
        sys_write(outfd, line_start, llen > (size_t)(end - line_start) ? (size_t)(end - line_start) : llen);
        written++;
        if (written >= lines_per) { sys_close(outfd); outfd = -1; part++; }
    }
    if (outfd >= 0) sys_close(outfd);
    return 0;
}
