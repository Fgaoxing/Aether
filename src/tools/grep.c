/*
 * grep.c — 精简 grep（扩展正则；救援环境用）
 *   grep [-i -v -n -c -l -q] [-e PATTERN]... [--] PATTERN [FILE...]
 *   -i 忽略大小写   -v 反相匹配   -n 行号   -c 只输出计数   -l 只输出文件名   -q 安静（首个匹配即退出 0）
 *   -e PATTERN 指定模式（可多次，等价于以 | 连接）；默认读 stdin
 *   多文件时输出带 "file:" 前缀。退出码：0 有匹配 / 1 无匹配 / 2 错误（类 GNU）
 *   正则见 regex.h（. * + ? {n,m} ^ $ [..] (..) |，\d \s \w）
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "regex.h"
#include "syscall_shim.h"
#include "util.h"

static void grep_usage(void)
{
    elog_info("usage: grep [-i -v -n -c -l -q] [-e PATTERN]... [--] PATTERN [FILE...]");
    elog_info("  -i 忽略大小写   -v 反相匹配   -n 行号   -c 计数   -l 文件名   -q 安静");
    elog_info("  正则：. * + ? {n,m} ^ $ [..] [^..] (..) |  \\d \\s \\w  \\. 转义");
}

typedef struct {
    int ci, invert, lineno, count, files_only, quiet;
    const char *pattern;      /* 主模式（可能多个 -e 以 | 连接） */
    char patbuf[512];
    int have_pattern;
    int nerr;
} grep_opts_t;

static int grep_process_file(grep_opts_t *o, const char *name, int fd)
{
    aether_reader_t r;
    aether_reader_init(&r, fd);
    char line[8192];
    long total = 0;
    long lineno = 0;
    int any = 0;
    int print_name = (name != 0);

    for (;;) {
        long n = aether_reader_line(&r, line, sizeof(line));
        if (n < 0) break;
        lineno++;
        int m = re_search(line, (int)n);
        if (o->invert) m = !m;
        if (!m) continue;
        any = 1;
        total++;
        if (o->quiet) return 1;
        if (o->count) continue;
        if (o->files_only) continue;
        if (print_name) {
            if (o->lineno) elog_info("%s:%ld:%s", name, lineno, line);
            else elog_info("%s:%s", name, line);
        } else {
            if (o->lineno) elog_info("%ld:%s", lineno, line);
            else elog_info("%s", line);
        }
    }
    if (o->count) {
        if (print_name) elog_info("%s:%ld", name, total);
        else elog_info("%ld", total);
    } else if (o->files_only && any) {
        elog_info("%s", name ? name : "-");
    }
    return any;
}

int tool_grep(int argc, char **argv, char **envp)
{
    (void)envp;
    grep_opts_t o;
    for (size_t i = 0; i < sizeof(o); i++) ((char *)&o)[i] = 0;
    o.patbuf[0] = '\0';

    int i = 1;
    int end_opts = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!end_opts && a[0] == '-' && a[1]) {
            if (aether_streq(a, "--")) { end_opts = 1; continue; }
            if (aether_streq(a, "-i")) { o.ci = 1; continue; }
            if (aether_streq(a, "-v")) { o.invert = 1; continue; }
            if (aether_streq(a, "-n")) { o.lineno = 1; continue; }
            if (aether_streq(a, "-c")) { o.count = 1; continue; }
            if (aether_streq(a, "-l")) { o.files_only = 1; continue; }
            if (aether_streq(a, "-q")) { o.quiet = 1; continue; }
            if (aether_streq(a, "-E")) { continue; } /* 默认已是扩展正则 */
            if (aether_streq(a, "-e") && i + 1 < argc) {
                const char *p = argv[++i];
                if (o.patbuf[0]) { size_t l = aether_strlen(o.patbuf); if (l + 1 < sizeof(o.patbuf)) o.patbuf[l++] = '|', o.patbuf[l] = '\0'; }
                size_t l = aether_strlen(o.patbuf);
                size_t pl = aether_strlen(p);
                if (l + pl + 2 >= sizeof(o.patbuf)) { elog_err("grep: pattern too long"); return 2; }
                for (size_t k = 0; k <= pl; k++) o.patbuf[l + k] = p[k];
                o.have_pattern = 1;
                continue;
            }
            if (aether_streq(a, "-h") || aether_streq(a, "--help")) { grep_usage(); return 0; }
            elog_err("grep: unknown option '%s'", a);
            grep_usage();
            return 2;
        }
        break;
    }
    if (!o.have_pattern) {
        if (i >= argc) { grep_usage(); return 2; }
        /* 位置参数 = 模式 */
        size_t l = aether_strlen(argv[i]);
        if (l + 1 >= sizeof(o.patbuf)) { elog_err("grep: pattern too long"); return 2; }
        for (size_t k = 0; k <= l; k++) o.patbuf[k] = argv[i][k];
        i++;
        o.have_pattern = 1;
    }
    o.pattern = o.patbuf;

    if (re_compile(o.pattern, o.ci) != 0) { elog_err("grep: invalid regex '%s'", o.pattern); return 2; }

    const char *files[32];
    int nfiles = 0;
    for (; i < argc && nfiles < 32; i++) files[nfiles++] = argv[i];

    int any = 0;
    if (nfiles == 0) {
        int m = grep_process_file(&o, 0, 0);
        if (o.nerr) any = 0;
        return m ? 0 : 1;
    }
    int print_prefix = (nfiles > 1);
    int rc = 1;
    for (int f = 0; f < nfiles; f++) {
        const char *path = files[f];
        /* -l 总是输出文件名；多文件/-c 时也带前缀 */
        const char *display = (o.files_only || print_prefix) ? path : 0;
        if (aether_streq(path, "-")) {
            if (grep_process_file(&o, print_prefix ? "-" : 0, 0)) rc = 0;
            continue;
        }
        long fd = sys_open(path, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) { elog_err("grep: %s: cannot open", path); o.nerr = 1; continue; }
        if (grep_process_file(&o, display, (int)fd)) rc = 0;
        sys_close((int)fd);
    }
    if (o.nerr) return 2;
    return rc;
}
