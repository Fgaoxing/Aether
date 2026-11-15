/*
 * less.c — mini pager（§12.5）
 * less <file> 或 stdin：q 退出；↑↓ 逐行；空格/PgDn 下翻页；b/PgUp 上翻；g/G 首尾。
 * 不支持搜索/跳行（v1，见 §13 非目标）。
 * 内容全部读入后才进入交互；键盘从 /dev/tty 读（stdin 可能是管道）。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "syscall_shim.h"
#include "ttyutil.h"
#include "util.h"

#define LESS_BUFSZ   (256 * 1024)
#define LESS_MAXLINE 16384

static char    less_buf[LESS_BUFSZ];
static size_t  less_len;
static size_t  less_off[LESS_MAXLINE + 1]; /* less_off[i] = 第 i 行的偏移 */
static int     less_nlines;

static volatile int less_sigint;
static void less_sigint_handler(int sig) { (void)sig; less_sigint = 1; }

static void less_write_line(size_t off, size_t len, int max_cols)
{
    if (len > 0 && less_buf[off + len - 1] == '\n') len--;
    if ((int)len > max_cols) len = (size_t)max_cols;
    if (len) sys_write(1, less_buf + off, len);
    sys_write(1, "\x1b[K\r\n", sizeof("\x1b[K\r\n") - 1); /* 清行尾 + 换行 */
}

static void less_render(int top, int rows, int cols)
{
    sys_write(1, "\x1b[2J\x1b[H", sizeof("\x1b[2J\x1b[H") - 1);
    int last_line = top + rows - 2 < less_nlines ? top + rows - 2 : less_nlines;
    for (int i = top; i < last_line; i++)
        less_write_line(less_off[i], less_off[i + 1] - less_off[i], cols);
    for (int i = top + (last_line - top); i < rows - 1; i++)
        sys_write(1, "\r\x1b[K\n", sizeof("\r\x1b[K\n") - 1);
    /* 状态栏（反白） */
    char st[80];
    size_t d = 0;
    const char *pre = "\x1b[7m aether less ";
    for (size_t k = 0; pre[k]; k++) st[d++] = pre[k];
    /* "行 %d/%d" 用极简十进制 */
    char num[24];
    size_t m = 0;
    unsigned v;
    v = (unsigned)(top + 1);
    if (v == 0) num[m++] = '0';
    while (v) { num[m++] = (char)('0' + (v % 10)); v /= 10; }
    while (m) st[d++] = num[--m];
    st[d++] = '/';
    v = (unsigned)less_nlines;
    m = 0;
    if (v == 0) num[m++] = '0';
    while (v) { num[m++] = (char)('0' + (v % 10)); v /= 10; }
    while (m) st[d++] = num[--m];
    const char *suf = "  (q quit) \x1b[27m";
    for (size_t k = 0; suf[k]; k++) st[d++] = suf[k];
    st[d] = '\0';
    sys_write(1, st, d);
}

int tool_less(int argc, char **argv, char **envp)
{
    (void)envp;
    const char *path = (argc >= 2) ? argv[1] : 0;
    int from_stdin = (path == 0) || aether_streq(path, "-");

    /* ---- 读内容 ---- */
    long fd = 0;
    if (!from_stdin) {
        fd = sys_open(path, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) { elog_err("less: %s: %d", path, AETHER_ERR_CODE(fd)); return 1; }
    }
    less_len = 0;
    for (;;) {
        long n = sys_read((int)fd, less_buf + less_len, sizeof(less_buf) - less_len);
        if (n == 0) break;
        if (AETHER_IS_ERR(n)) { elog_err("less: read failed"); if (!from_stdin) sys_close((int)fd); return 1; }
        less_len += (size_t)n;
        if (less_len == sizeof(less_buf)) break; /* 大文件截断 */
    }
    if (!from_stdin) sys_close((int)fd);

    /* ---- 索引行 ---- */
    less_nlines = 0;
    less_off[0] = 0;
    {
        int lines = 0;
        size_t i = 0;
        while (i < less_len && lines < LESS_MAXLINE) {
            if (less_buf[i] == '\n') { less_off[++lines] = i + 1; }
            i++;
        }
        if (lines == 0 || less_off[lines] != less_len) less_off[++lines] = less_len;
        less_nlines = lines;
    }

    /* ---- 键盘（/dev/tty 优先；stdin 是管道时也能读键盘） ---- */
    int kbd = aether_tty_open_rw(); /* 回退链：/dev/tty → fd0(tty) → /dev/console → fd1 */
    if (kbd < 0) kbd = 0; /* 绝对兜底：stdin */

    aether_tty_t tty;
    aether_signal(SIGINT, less_sigint_handler);
    if (tty_save(kbd, &tty) != 0 || tty_cbreak(kbd, &tty) != 0) {
        elog_err("less: needs a tty");
        return 1;
    }

    int rows = 24, cols = 80;
    tty_size(kbd, &rows, &cols);
    int top = 0;
    int page = rows - 1 < 1 ? 1 : rows - 1;

    for (;;) {
        if (less_sigint) break;
        int max_top = less_nlines > rows - 1 ? less_nlines - (rows - 1) : 0;
        if (top > max_top) top = max_top;
        if (top < 0) top = 0;
        less_render(top, rows, cols);

        char ch;
        long r = sys_read(kbd, &ch, 1);
        if (r <= 0 || less_sigint) break;

        if (ch == 'q' || ch == 'Q') break;
        else if (ch == '\x1b') { /* ESC 序列：↑↓ PgUp PgDn Home End */
            char s[6];
            int n = 0;
            long rr = sys_read(kbd, &s[n], 1);
            if (rr <= 0) continue;
            if (s[n] != '[' && s[n] != 'O') continue;
            n++;
            rr = sys_read(kbd, &s[n], 1);
            if (rr <= 0) continue;
            n++;
            char k = s[1];
            if (k == 'A' || k == 'B') { /* 上下 */
                top += (k == 'A') ? -1 : 1;
            } else if (k == '5' || k == '6') { /* PgUp/PgDn：ESC [ 5 ~ / 6 ~ */
                char t;
                rr = sys_read(kbd, &t, 1);
                top += (k == '5') ? -page : page;
            } else if (k == 'H') top = 0;
            else if (k == 'F') top = max_top;
            continue;
        }
        else if (ch == ' ' || ch == 'f') top += page;
        else if (ch == 'b') top -= page;
        else if (ch == 'g') top = 0;
        else if (ch == 'G') top = less_nlines > rows - 1 ? less_nlines - (rows - 1) : 0;
        else if (ch == 'j') top += 1;
        else if (ch == 'k') top -= 1;
        else if (ch == '0') top = 0;
    }

    tty_restore(kbd, &tty);
    if (kbd > 2) sys_close(kbd);
    sys_write(1, "\x1b[2J\x1b[H", sizeof("\x1b[2J\x1b[H") - 1); /* 退出清屏 */
    return 0;
}
