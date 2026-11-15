/*
 * text_stream.c — wc head tail strings sort uniq cut paste fold shuf cmp
 * 流式处理；sort/shuf 需要内存，文件上限 2MB。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "syscall_shim.h"
#include "util.h"

/* 大静态缓冲：用于 sort / shuf（救援环境文件一般不大） */
#define TEXT_BUFSZ (2 * 1024 * 1024)
static char text_buf[TEXT_BUFSZ];
static char *text_lines[131072];

static long split_lines(char *buf, long len, char **lines, long max)
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

/* qsort（自己实现，避免 libc 依赖） */
static void qsort_lines(char **arr, long n, int reverse)
{
    if (n < 2) return;
    char *pivot = arr[n / 2];
    long i = 0, j = n - 1;
    while (i <= j) {
        while (aether_strcmp(arr[i], pivot) < 0) i++;
        while (aether_strcmp(arr[j], pivot) > 0) j--;
        if (i <= j) { char *t = arr[i]; arr[i] = arr[j]; arr[j] = t; i++; j--; }
    }
    if (j + 1 < n) qsort_lines(arr, j + 1, reverse);
    if (i < n) qsort_lines(arr + i, n - i, reverse);
    (void)reverse;
}

static int read_arg_file(const char *path, int *fd_out)
{
    long fd = sys_open(path, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) { elog_err("%s: cannot open", path); return -1; }
    *fd_out = (int)fd;
    return 0;
}

/* ---------- wc ---------- */

int tool_wc(int argc, char **argv, char **envp)
{
    (void)envp;
    int show_l = 0, show_w = 0, show_c = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (aether_streq(a, "-l")) show_l = 1;
        else if (aether_streq(a, "-w")) show_w = 1;
        else if (aether_streq(a, "-c")) show_c = 1;
        else break;
    }
    if (!show_l && !show_w && !show_c) { show_l = show_w = show_c = 1; }

    const char *files[8];
    int nfiles = 0, any = 0;
    if (i < argc) { for (; i < argc; i++) files[nfiles++] = argv[i]; }
    else files[nfiles++] = 0;

    unsigned long long tl = 0, tw = 0, tc = 0;
    for (int f = 0; f < nfiles; f++) {
        int fd = 0;
        const char *name = "-";
        if (files[f]) { if (read_arg_file(files[f], &fd) != 0) continue; name = files[f]; any = 1; }
        unsigned long long lines = 0, words = 0, bytes = 0;
        int in_word = 0;
        aether_reader_t r; aether_reader_init(&r, fd);
        for (;;) {
            int ch = aether_reader_getc(&r);
            if (ch == -1) break;
            bytes++;
            if (ch == '\n') lines++;
            int isspace = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v');
            if (isspace) in_word = 0;
            else if (!in_word) { in_word = 1; words++; }
        }
        if (files[f]) sys_close(fd);
        tl += lines; tw += words; tc += bytes;

        if (show_l && show_w && show_c) elog_info("%llu %llu %llu %s", lines, words, bytes, name);
        else if (show_l && show_w) elog_info("%llu %llu %s", lines, words, name);
        else if (show_l && show_c) elog_info("%llu %llu %s", lines, bytes, name);
        else if (show_w && show_c) elog_info("%llu %llu %s", words, bytes, name);
        else if (show_l) elog_info("%llu %s", lines, name);
        else if (show_w) elog_info("%llu %s", words, name);
        else if (show_c) elog_info("%llu %s", bytes, name);
    }
    if (nfiles > 1 && any) {
        if (show_l && show_w && show_c) elog_info("%llu %llu %llu total", tl, tw, tc);
        else if (show_l && show_w) elog_info("%llu %llu total", tl, tw);
        else if (show_l && show_c) elog_info("%llu %llu total", tl, tc);
        else if (show_w && show_c) elog_info("%llu %llu total", tw, tc);
        else if (show_l) elog_info("%llu total", tl);
        else if (show_w) elog_info("%llu total", tw);
        else if (show_c) elog_info("%llu total", tc);
    }
    return 0;
}

/* ---------- head ---------- */

int tool_head(int argc, char **argv, char **envp)
{
    (void)envp;
    long nlines = 10;
    int i = 1;
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-n") && i + 1 < argc) {
            nlines = aether_atol(argv[++i]);
        } else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
            nlines = aether_atol(argv[i] + 1);
        } else if (argv[i][0] == '-' && argv[i][1]) { elog_err("head: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    const char *files[8];
    int nfiles = 0;
    if (i < argc) { for (; i < argc; i++) files[nfiles++] = argv[i]; }
    else files[nfiles++] = 0;

    for (int f = 0; f < nfiles; f++) {
        int fd = 0;
        const char *name = "-";
        if (files[f]) { if (read_arg_file(files[f], &fd) != 0) continue; name = files[f]; }
        if (nfiles > 1) elog_info("==> %s <==", name);
        aether_reader_t r; aether_reader_init(&r, fd);
        char line[8192];
        for (long k = 0; k < nlines; k++) {
            long n = aether_reader_line(&r, line, sizeof(line));
            if (n < 0) break;
            elog_info("%s", line);
        }
        if (files[f]) sys_close(fd);
    }
    return 0;
}

/* ---------- tail ---------- */

int tool_tail(int argc, char **argv, char **envp)
{
    (void)envp;
    long nlines = 10;
    int follow = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-n") && i + 1 < argc) nlines = aether_atol(argv[++i]);
        else if (aether_streq(argv[i], "-f") || aether_streq(argv[i], "--follow")) follow = 1;
        else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') nlines = aether_atol(argv[i] + 1);
        else if (argv[i][0] == '-' && argv[i][1]) { elog_err("tail: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    const char *file = (i < argc) ? argv[i] : 0;
    int fd = 0;
    if (file) { if (read_arg_file(file, &fd) != 0) return 1; }

    long len = aether_read_all(fd, text_buf, TEXT_BUFSZ - 1);
    if (len < 0) { elog_err("tail: read error"); if (file) sys_close(fd); return 1; }

    /* 找最后 nlines 行的起始位置 */
    char *start = text_buf;
    {
        long count = 0;
        for (long p = len - 1; p >= 0; p--) {
            if (text_buf[p] == '\n' && p != len - 1) count++;
            if (count == nlines) { start = text_buf + p + 1; break; }
        }
        if (count < nlines) start = text_buf; /* 文件不足 nlines 行 */
    }
    sys_write(1, start, (size_t)(len - (start - text_buf)));

    if (follow) {
        long pos = len;
        for (;;) {
            struct { long sec; long nsec; } ts = { 0, 200UL * 1000UL * 1000UL };
            sys_nanosleep(&ts, 0);
            char tmp[65536];
            sys_lseek(fd, pos, SEEK_SET);
            long n = sys_read(fd, tmp, sizeof(tmp));
            if (n > 0) { sys_write(1, tmp, (size_t)n); pos += n; }
        }
    }
    if (file) sys_close(fd);
    return 0;
}

/* ---------- strings ---------- */

int tool_strings(int argc, char **argv, char **envp)
{
    (void)envp;
    long min = 4;
    int i = 1;
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-n") && i + 1 < argc) min = aether_atol(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') min = aether_atol(argv[i] + 1);
        else if (argv[i][0] == '-' && argv[i][1]) { elog_err("strings: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    const char *path = (i < argc) ? argv[i] : 0;
    int fd = 0;
    if (path) { if (read_arg_file(path, &fd) != 0) return 1; }

    aether_reader_t r; aether_reader_init(&r, fd);
    char run[4096];
    size_t rl = 0;
    for (;;) {
        int ch = aether_reader_getc(&r);
        if (ch == -1) break;
        if (ch >= 0x20 && ch < 0x7f) {
            if (rl < sizeof(run) - 1) run[rl++] = (char)ch;
        } else {
            if ((long)rl >= min) { run[rl] = '\0'; elog_info("%s", run); }
            rl = 0;
        }
    }
    if ((long)rl >= min) { run[rl] = '\0'; elog_info("%s", run); }
    if (path) sys_close(fd);
    return 0;
}

/* ---------- sort ---------- */

int tool_sort(int argc, char **argv, char **envp)
{
    (void)envp;
    int reverse = 0, numeric = 0, uniq = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-r")) reverse = 1;
        else if (aether_streq(argv[i], "-n")) numeric = 1;
        else if (aether_streq(argv[i], "-u")) uniq = 1;
        else if (argv[i][0] == '-' && argv[i][1]) { elog_err("sort: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    const char *path = (i < argc) ? argv[i] : 0;
    int fd = 0;
    if (path) { if (read_arg_file(path, &fd) != 0) return 1; }

    long len = aether_read_all(fd, text_buf, TEXT_BUFSZ - 1);
    if (path) sys_close(fd);
    if (len < 0) { elog_err("sort: read error"); return 1; }
    text_buf[len] = '\0';

    long nlines = split_lines(text_buf, len, text_lines, (long)(sizeof(text_lines) / sizeof(text_lines[0])));

    if (numeric) {
        static long keys[131072];
        for (long k = 0; k < nlines; k++) keys[k] = aether_atol(text_lines[k]);
        for (long k = 1; k < nlines; k++) {
            long keyk = keys[k]; char *pline = text_lines[k];
            long j = k - 1;
            while (j >= 0 && keys[j] > keyk) { keys[j + 1] = keys[j]; text_lines[j + 1] = text_lines[j]; j--; }
            keys[j + 1] = keyk; text_lines[j + 1] = pline;
        }
    } else {
        qsort_lines(text_lines, nlines, reverse);
    }

    if (uniq) {
        long out = 0;
        for (long k = 0; k < nlines; k++) {
            if (out == 0 || !aether_streq(text_lines[k], text_lines[out - 1])) text_lines[out++] = text_lines[k];
        }
        nlines = out;
    }
    for (long k = 0; k < nlines; k++) {
        long idx = reverse ? (nlines - 1 - k) : k;
        elog_info("%s", text_lines[idx]);
    }
    return 0;
}

/* ---------- uniq ---------- */

int tool_uniq(int argc, char **argv, char **envp)
{
    (void)envp;
    int count = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-c")) count = 1;
        else if (argv[i][0] == '-' && argv[i][1]) { elog_err("uniq: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    const char *path = (i < argc) ? argv[i] : 0;
    int fd = 0;
    if (path) { if (read_arg_file(path, &fd) != 0) return 1; }

    aether_reader_t r; aether_reader_init(&r, fd);
    char line[8192], prev[8192];
    long linecount = 0;
    int have_prev = 0;
    for (;;) {
        long n = aether_reader_line(&r, line, sizeof(line));
        if (n < 0) break;
        if (have_prev && aether_streq(line, prev)) { linecount++; continue; }
        if (have_prev) {
            if (count) { char nbuf[32]; aether_u64toa(nbuf, sizeof(nbuf), (unsigned long long)linecount); elog_info("%s %s", nbuf, prev); }
            else elog_info("%s", prev);
        }
        aether_strcpy(prev, line, sizeof(prev));
        linecount = 1;
        have_prev = 1;
    }
    if (have_prev) {
        if (count) { char nbuf[32]; aether_u64toa(nbuf, sizeof(nbuf), (unsigned long long)linecount); elog_info("%s %s", nbuf, prev); }
        else elog_info("%s", prev);
    }
    if (path) sys_close(fd);
    return 0;
}

/* ---------- cut ---------- */

static long parse_list_spec(const char *s, long *out, int max)
{
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        long a = 0, b = -1;
        while (*p >= '0' && *p <= '9') { a = a * 10 + (*p - '0'); p++; }
        if (*p == '-') {
            p++;
            if (*p >= '0' && *p <= '9') { b = 0; while (*p >= '0' && *p <= '9') { b = b * 10 + (*p - '0'); p++; } }
            else b = 1000000;
        } else b = a;
        out[n++] = a;
        if (b != a) { out[n++] = -b; } /* 负数编码闭区间终点 */
        if (*p == ',') p++;
    }
    return n;
}

int tool_cut(int argc, char **argv, char **envp)
{
    (void)envp;
    int i = 1;
    char delim = '\t';
    int mode = 0; /* 1=bytes/chars, 2=fields */
    char spec_str[256] = "";
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-d") && i + 1 < argc) { delim = argv[++i][0]; }
        else if (argv[i][0] == '-' && argv[i][1] == 'd' && argv[i][2]) { delim = argv[i][2]; }
        else if (aether_streq(argv[i], "-f") && i + 1 < argc) { mode = 2; aether_strcpy(spec_str, argv[++i], sizeof(spec_str)); }
        else if (argv[i][0] == '-' && argv[i][1] == 'f' && argv[i][2]) { mode = 2; aether_strcpy(spec_str, argv[i] + 2, sizeof(spec_str)); }
        else if (aether_streq(argv[i], "-c") && i + 1 < argc) { mode = 1; aether_strcpy(spec_str, argv[++i], sizeof(spec_str)); }
        else if (argv[i][0] == '-' && argv[i][1] == 'c' && argv[i][2]) { mode = 1; aether_strcpy(spec_str, argv[i] + 2, sizeof(spec_str)); }
        else if (aether_streq(argv[i], "-b") && i + 1 < argc) { mode = 1; aether_strcpy(spec_str, argv[++i], sizeof(spec_str)); }
        else if (argv[i][0] == '-' && argv[i][1] == 'b' && argv[i][2]) { mode = 1; aether_strcpy(spec_str, argv[i] + 2, sizeof(spec_str)); }
        else if (argv[i][0] == '-' && argv[i][1]) { elog_err("cut: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    if (mode == 0) { elog_err("cut: -f or -c required"); return 2; }

    long spec[64];
    int nspec = (int)parse_list_spec(spec_str, spec, 64);

    const char *path = (i < argc) ? argv[i] : 0;
    int fd = 0;
    if (path) { if (read_arg_file(path, &fd) != 0) return 1; }

    aether_reader_t r; aether_reader_init(&r, fd);
    char line[8192];
    for (;;) {
        long n = aether_reader_line(&r, line, sizeof(line));
        if (n < 0) break;
        if (mode == 2) {
            /* 按 delim 分字段，输出选中字段，用 delim 连接 */
            long pos = 0;
            int field_no = 0;
            int first_out = 1;
            while (pos <= n) {
                const char *start = line + pos;
                const char *end = start;
                while (*end && *end != delim) end++;
                field_no++;
                size_t flen = (size_t)(end - start);
                int wanted = 0;
                for (int k = 0; k < nspec; k += 2) {
                    long a = spec[k];
                    long b = (k + 1 < nspec && spec[k + 1] < 0) ? -spec[k + 1] : spec[k];
                    if (field_no >= a && field_no <= b) { wanted = 1; break; }
                }
                if (wanted) {
                    if (!first_out) { char d[2] = { delim, '\0' }; sys_write(1, d, 1); }
                    sys_write(1, start, flen);
                    first_out = 0;
                }
                if (*end == '\0') break;
                pos = (end - line) + 1;
            }
            sys_write(1, "\n", sizeof("\n") - 1);
        } else {
            /* 按字符位置输出 */
            for (long p2 = 0; p2 < n; p2++) {
                long col = p2 + 1; /* 1-based */
                int wanted = 0;
                for (int k = 0; k < nspec; k += 2) {
                    long a = spec[k];
                    long b = (k + 1 < nspec && spec[k + 1] < 0) ? -spec[k + 1] : spec[k];
                    if (col >= a && col <= b) { wanted = 1; break; }
                }
                if (wanted) sys_write(1, line + p2, 1);
            }
            sys_write(1, "\n", sizeof("\n") - 1);
        }
    }
    if (path) sys_close(fd);
    return 0;
}

/* ---------- paste ---------- */

int tool_paste(int argc, char **argv, char **envp)
{
    (void)envp;
    int i = 1;
    char delim = '\t';
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-d") && i + 1 < argc) delim = argv[++i][0];
        else if (argv[i][0] == '-' && argv[i][1] == 'd' && argv[i][2]) delim = argv[i][2];
        else if (argv[i][0] == '-' && argv[i][1]) { elog_err("paste: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    if (i >= argc) { elog_err("paste: missing operand"); return 2; }

    aether_reader_t readers[8];
    int nfd = 0;
    for (int k = i; k < argc && nfd < 8; k++) {
        int fd = 0;
        if (read_arg_file(argv[k], &fd) != 0) return 1;
        aether_reader_init(&readers[nfd], fd);
        nfd++;
    }

    char lines[8][8192];
    for (;;) {
        int got_any = 0;
        for (int f = 0; f < nfd; f++) {
            long n = aether_reader_line(&readers[f], lines[f], sizeof(lines[f]));
            if (n < 0) { lines[f][0] = '\0'; lines[f][1] = '\0'; /* 该文件已到 EOF，按空行占位 */ }
            else got_any = 1;
        }
        if (!got_any) break; /* 所有文件都已到 EOF */
        for (int f = 0; f < nfd; f++) {
            if (f > 0) sys_write(1, &delim, 1);
            sys_write(1, lines[f], aether_strlen(lines[f]));
        }
        sys_write(1, "\n", sizeof("\n") - 1);
    }
    for (int f = 0; f < nfd; f++) sys_close(readers[f].fd);
    return 0;
}

/* ---------- fold ---------- */

int tool_fold(int argc, char **argv, char **envp)
{
    (void)envp;
    long width = 80;
    int break_spaces = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-w") && i + 1 < argc) width = aether_atol(argv[++i]);
        else if (aether_streq(argv[i], "-s")) break_spaces = 1;
        else if (argv[i][0] == '-' && argv[i][1]) { elog_err("fold: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    const char *path = (i < argc) ? argv[i] : 0;
    int fd = 0;
    if (path) { if (read_arg_file(path, &fd) != 0) return 1; }

    aether_reader_t r; aether_reader_init(&r, fd);
    char line[8192];
    for (;;) {
        long n = aether_reader_line(&r, line, sizeof(line));
        if (n < 0) break;
        if (n <= width) { elog_info("%s", line); continue; }
        /* 按 width 折行 */
        long pos = 0;
        while (pos < n) {
            long end = pos + width;
            if (end >= n) { sys_write(1, line + pos, (size_t)(n - pos)); sys_write(1, "\n", sizeof("\n") - 1); break; }
            if (break_spaces) {
                /* 在 width 内找最后一个空格 */
                long cut = -1;
                for (long k = pos + width - 1; k >= pos; k--) {
                    if (line[k] == ' ') { cut = k; break; }
                }
                if (cut > pos) { sys_write(1, line + pos, (size_t)(cut - pos)); sys_write(1, "\n", sizeof("\n") - 1); pos = cut + 1; continue; }
            }
            sys_write(1, line + pos, (size_t)width);
            sys_write(1, "\n", sizeof("\n") - 1);
            pos = end;
        }
    }
    if (path) sys_close(fd);
    return 0;
}

/* ---------- shuf ---------- */

int tool_shuf(int argc, char **argv, char **envp)
{
    (void)envp;
    long n_out = -1;
    int i = 1;
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-n") && i + 1 < argc) n_out = aether_atol(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1]) { elog_err("shuf: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    const char *path = (i < argc) ? argv[i] : 0;
    int fd = 0;
    if (path) { if (read_arg_file(path, &fd) != 0) return 1; }

    long len = aether_read_all(fd, text_buf, TEXT_BUFSZ - 1);
    if (path) sys_close(fd);
    if (len < 0) return 1;
    text_buf[len] = '\0';

    long nlines = split_lines(text_buf, len, text_lines, (long)(sizeof(text_lines) / sizeof(text_lines[0])));

    /* 读 /dev/urandom 做种子 */
    unsigned long long seed = 12345;
    {
        int ufd = (int)sys_open("/dev/urandom", O_RDONLY, 0);
        if (!AETHER_IS_ERR((long)ufd)) {
            unsigned char b[8];
            long rn = sys_read(ufd, b, sizeof(b));
            if (rn == (long)sizeof(b)) {
                seed = 0;
                for (int k = 0; k < 8; k++) seed = (seed << 8) | b[k];
            }
            sys_close(ufd);
        }
    }
    /* Fisher-Yates */
    for (long k = nlines - 1; k > 0; k--) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        long j = (long)((seed >> 33) % (unsigned long long)(k + 1));
        char *t = text_lines[k]; text_lines[k] = text_lines[j]; text_lines[j] = t;
    }
    long emit = (n_out >= 0 && n_out < nlines) ? n_out : nlines;
    for (long k = 0; k < emit; k++) elog_info("%s", text_lines[k]);
    return 0;
}

/* ---------- cmp ---------- */

int tool_cmp(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 3) { elog_err("cmp: usage: cmp <file1> <file2>"); return 2; }
    int fd1 = 0, fd2 = 0;
    if (read_arg_file(argv[1], &fd1) != 0) return 1;
    if (read_arg_file(argv[2], &fd2) != 0) { sys_close(fd1); return 1; }

    static char b1[65536], b2[65536];
    unsigned long long offset = 0;
    for (;;) {
        long n1 = sys_read(fd1, b1, sizeof(b1));
        long n2 = sys_read(fd2, b2, sizeof(b2));
        if (n1 < 0 || n2 < 0) { elog_err("cmp: read error"); break; }
        long m = n1 < n2 ? n1 : n2;
        for (long i = 0; i < m; i++) {
            if (b1[i] != b2[i]) {
                elog_info("%s %s differ at byte %llu (0x%02x vs 0x%02x)", argv[1], argv[2], offset + (unsigned long long)i + 1,
                          (unsigned char)b1[i], (unsigned char)b2[i]);
                sys_close(fd1); sys_close(fd2);
                return 1;
            }
        }
        offset += (unsigned long long)m;
        if (n1 != n2) {
            elog_info("%s %s differ at byte %llu (size differs)", argv[1], argv[2], offset + 1);
            sys_close(fd1); sys_close(fd2);
            return 1;
        }
        if (n1 == 0 && n2 == 0) { sys_close(fd1); sys_close(fd2); return 0; }
    }
    sys_close(fd1); sys_close(fd2);
    return 1;
}
