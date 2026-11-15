/*
 * editor.c — nano 风格极简全屏编辑器（§12 第 7 步）
 * nano 键位：^S 保存，^X 退出（有未保存修改时询问 y/n），↑↓←→ 移动，PgUp/PgDn，
 *           Home/End，Delete，Backspace。
 * 无 ncurses / readline：直接操作 tty + VT100。文件 ≤256KB；无语法高亮/撤销/搜索（v1）。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "syscall_shim.h"
#include "ttyutil.h"
#include "util.h"

#define EDITOR_BUFSZ   (256 * 1024)
#define EDITOR_MAXLINE 8192
#define TAB_WIDTH      4

static char    ed_buf[EDITOR_BUFSZ];
static size_t  ed_len;
static size_t  ed_lineoff[EDITOR_MAXLINE + 1];
static int     ed_nlines;

static const char *ed_path;
static const char *ed_status;   /* 状态栏左侧临时消息（可为空） */
static int  ed_modified;
static int  ed_read_only;
static int  ed_cur_line, ed_cur_col;   /* 字节偏移（在当前行内） */
static int  ed_goal_col = -1;          /* 垂直移动的目标显示列 */
static int  ed_top_line;               /* 屏幕首行 */
static int  ed_prompt;                 /* 0=无，1=退出前保存确认 */

static volatile int ed_sigint;
static void ed_sigint_handler(int sig) { (void)sig; ed_sigint = 1; }

static int is_wide_cp(unsigned cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

/* 解码 i 处的 UTF-8 序列，返回字节长度与码位 */
static size_t seq_decode(size_t i, unsigned *cp_out)
{
    unsigned char ch = (unsigned char)ed_buf[i];
    unsigned cp;
    size_t seqlen;
    if (ch < 0x80) { *cp_out = ch; return 1; }
    if ((ch & 0xE0) == 0xC0) { cp = ch & 0x1F; seqlen = 2; }
    else if ((ch & 0xF0) == 0xE0) { cp = ch & 0x0F; seqlen = 3; }
    else if ((ch & 0xF8) == 0xF0) { cp = ch & 0x07; seqlen = 4; }
    else { *cp_out = 0xFFFD; return 1; }
    for (size_t k = 1; k < seqlen && i + k < ed_len; k++) cp = (cp << 6) | ((unsigned char)ed_buf[i + k] & 0x3F);
    *cp_out = cp;
    return seqlen;
}

static void ed_rebuild_lines(void)
{
    int lines = 0;
    size_t i = 0;
    ed_lineoff[0] = 0;
    while (i < ed_len && lines < EDITOR_MAXLINE) {
        if (ed_buf[i] == '\n') ed_lineoff[++lines] = i + 1;
        i++;
    }
    if (ed_lineoff[lines] != ed_len) ed_lineoff[++lines] = ed_len;
    ed_nlines = lines;
}

static int line_len_bytes(int l)
{
    if (l < 0 || l >= ed_nlines) return 0;
    return (int)(ed_lineoff[l + 1] - ed_lineoff[l]);
}

static int ed_byte_col_to_cell(int l, int col_bytes)
{
    size_t start = ed_lineoff[l], i = start;
    size_t end = start + (size_t)col_bytes;
    int cells = 0;
    while (i < end && i < ed_len) {
        unsigned char ch = (unsigned char)ed_buf[i];
        if (ch == '\r') { i++; continue; }
        if (ch == '\t') { cells += TAB_WIDTH - (cells % TAB_WIDTH); i++; continue; }
        if (ch < 0x20 || ch == 0x7F) { i++; continue; }
        unsigned cp;
        size_t seqlen = seq_decode(i, &cp);
        cells += is_wide_cp(cp) ? 2 : 1;
        i += seqlen;
    }
    return cells;
}

/* 显示列 → 字节列（取该列最接近的序列起点） */
static int ed_cell_to_byte_col(int l, int want_cells)
{
    size_t start = ed_lineoff[l], end = ed_lineoff[l + 1], i = start;
    int cells = 0;
    while (i < end) {
        unsigned char ch = (unsigned char)ed_buf[i];
        if (ch == '\n' || ch == '\r') { i++; continue; }
        if (ch == '\t') { cells += TAB_WIDTH - (cells % TAB_WIDTH); i++; continue; }
        if (ch < 0x20 || ch == 0x7F) { i++; continue; }
        unsigned cp;
        size_t seqlen = seq_decode(i, &cp);
        int wide = is_wide_cp(cp);
        int w = wide ? 2 : 1;
        if (cells + w > want_cells) break;
        cells += w;
        i += seqlen;
    }
    return (int)(i - start);
}

static void ed_insert_bytes(const char *bytes, size_t n)
{
    size_t pos = ed_lineoff[ed_cur_line] + (size_t)ed_cur_col;
    if (ed_len + n > EDITOR_BUFSZ) return;
    for (size_t i = ed_len; i-- > pos; ) ed_buf[i + n] = ed_buf[i];
    for (size_t i = 0; i < n; i++) ed_buf[pos + i] = bytes[i];
    ed_len += n;
    ed_modified = 1;
    ed_cur_col += (int)n;
    ed_rebuild_lines();
}

static void ed_backspace(void)
{
    size_t line_start = ed_lineoff[ed_cur_line];
    size_t pos = line_start + (size_t)ed_cur_col;
    if (pos == 0) return;
    if (ed_cur_col == 0) {
        /* 合并上一行：删掉上一行行尾的 '\n' */
        for (size_t i = pos; i < ed_len; i++) ed_buf[i - 1] = ed_buf[i];
        ed_len--;
        ed_cur_line--;
        ed_rebuild_lines();
        ed_cur_col = (int)(ed_lineoff[ed_cur_line + 1] - ed_lineoff[ed_cur_line]) - 1; /* 上一行末尾换行处 */
        ed_modified = 1;
        return;
    }
    /* 删除 cursor 前一个 UTF-8 序列 */
    size_t seq_start = pos - 1;
    while (seq_start > line_start && ((unsigned char)ed_buf[seq_start] & 0xC0) == 0x80) seq_start--;
    size_t n = pos - seq_start;
    for (size_t i = pos; i < ed_len; i++) ed_buf[i - n] = ed_buf[i];
    ed_len -= n;
    ed_cur_col -= (int)n;
    ed_modified = 1;
    ed_rebuild_lines();
}

static void ed_delete_forward(void)
{
    size_t pos = ed_lineoff[ed_cur_line] + (size_t)ed_cur_col;
    if (pos >= ed_len) return;
    unsigned cp;
    size_t n = seq_decode(pos, &cp);
    if (ed_buf[pos] == '\n') n = 1; /* 换行符就是一个字节 */
    for (size_t i = pos + n; i < ed_len; i++) ed_buf[i - n] = ed_buf[i];
    ed_len -= n;
    ed_modified = 1;
    ed_rebuild_lines();
}

static void ed_newline(void)
{
    ed_insert_bytes("\n", 1);
    ed_cur_line++;
    ed_cur_col = 0;
}

/* ---------- 渲染 ---------- */

static void ed_putn(int v)
{
    char num[12];
    size_t m = 0;
    if (v == 0) num[m++] = '0';
    while (v) { num[m++] = (char)('0' + v % 10); v /= 10; }
    while (m) { char c = num[--m]; sys_write(1, &c, 1); }
}

static void ed_render(int rows, int cols)
{
    int view_rows = rows - 1;
    sys_write(1, "\x1b[2J\x1b[H", sizeof("\x1b[2J\x1b[H") - 1);
    for (int r = 0; r < view_rows; r++) {
        int l = ed_top_line + r;
        if (l >= ed_nlines) { sys_write(1, "~\x1b[K\r\n", sizeof("~\x1b[K\r\n") - 1); continue; }
        size_t s = ed_lineoff[l], e = ed_lineoff[l + 1], i = s;
        int written = 0;
        while (i < e && written < cols) {
            unsigned char ch = (unsigned char)ed_buf[i];
            if (ch == '\n') break;
            if (ch == '\r') { i++; continue; }
            if (ch == '\t') {
                int spaces = TAB_WIDTH - (written % TAB_WIDTH);
                for (int k = 0; k < spaces && written < cols; k++, written++) sys_write(1, " ", sizeof(" ") - 1);
                i++;
                continue;
            }
            if (ch < 0x20 || ch == 0x7F) { sys_write(1, "?", sizeof("?") - 1); written++; i++; continue; }
            unsigned cp;
            size_t seqlen = seq_decode(i, &cp);
            int w = is_wide_cp(cp) ? 2 : 1;
            if (written + w > cols) break;
            for (size_t k = 0; k < seqlen; k++) sys_write(1, ed_buf + i + k, 1);
            written += w;
            i += seqlen;
        }
        sys_write(1, "\x1b[K\r\n", sizeof("\x1b[K\r\n") - 1);
    }
    /* 状态栏 */
    sys_write(1, "\x1b[7m", sizeof("\x1b[7m") - 1);
    char status[512];
    size_t d = 0;
    const char *pre = ed_prompt ? " Save modified buffer? [y/n] " : " ^S save  ^X quit  ";
    for (size_t k = 0; pre[k] && d + 1 < sizeof(status); k++) status[d++] = pre[k];
    for (size_t k = 0; ed_path[k] && d + 1 < sizeof(status); k++) {
        unsigned char ch = (unsigned char)ed_path[k];
        status[d++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
    }
    if (ed_modified && d + 11 < sizeof(status)) { 
        const char *m = " [MODIFIED]";
        for (size_t k = 0; m[k]; k++) status[d++] = m[k];
    }
    if (ed_read_only && d + 11 < sizeof(status)) {
        const char *m = " [READ-ONLY]";
        for (size_t k = 0; m[k]; k++) status[d++] = m[k];
    }
    if (ed_status && d + 40 < sizeof(status)) {
        status[d++] = ' ';
        for (size_t k = 0; ed_status[k]; k++) status[d++] = ed_status[k];
    }
    int written = 0;
    for (size_t k = 0; k < d && written < cols; k++, written++) sys_write(1, &status[k], 1);
    for (int i = written; i < cols; i++) sys_write(1, " ", sizeof(" ") - 1);
    sys_write(1, "\x1b[27m", sizeof("\x1b[27m") - 1); /* 不再多余换行，避免 24 行屏幕滚动 */
    /* 光标定位：display row/col */
    sys_write(1, "\x1b[", sizeof("\x1b[") - 1);
    ed_putn(ed_cur_line - ed_top_line + 1);
    sys_write(1, ";", sizeof(";") - 1);
    ed_putn(ed_byte_col_to_cell(ed_cur_line, ed_cur_col) + 1);
    sys_write(1, "H", sizeof("H") - 1);
}

static void ed_clamp_view(int rows)
{
    int view_rows = rows - 1;
    if (ed_cur_line < ed_top_line) ed_top_line = ed_cur_line;
    if (ed_cur_line >= ed_top_line + view_rows) ed_top_line = ed_cur_line - view_rows + 1;
    if (ed_top_line < 0) ed_top_line = 0;
}

static int ed_save(void)
{
    if (ed_read_only) return -1;
    long fd = sys_open(ed_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (AETHER_IS_ERR(fd)) return -2;
    size_t off = 0;
    while (off < ed_len) {
        long w = sys_write((int)fd, ed_buf + off, ed_len - off);
        if (AETHER_IS_ERR(w) || w == 0) { sys_close((int)fd); return -3; }
        off += (size_t)w;
    }
    sys_close((int)fd);
    ed_modified = 0;
    return 0;
}

/* ---------- 交互主体 ---------- */

static int ed_run(void)
{
    aether_tty_t tty;
    aether_signal(SIGINT, ed_sigint_handler);
    int kbd = aether_tty_open_rw(); /* 回退链：/dev/tty → fd0(tty) → /dev/console → fd1 */
    if (kbd < 0) { elog_err("editor: needs a tty"); return 1; }
    if (tty_save(kbd, &tty) != 0 || tty_cbreak(kbd, &tty) != 0) {
        elog_err("editor: needs a tty");
        return 1;
    }
    int rows = 24, cols = 80;
    tty_size(kbd, &rows, &cols);

    for (;;) {
        if (ed_sigint) break;
        ed_clamp_view(rows);
        ed_render(rows, cols);

        char ch;
        long r = sys_read(kbd, &ch, 1);
        if (r <= 0 || ed_sigint) break;

        if (ed_prompt) {
            if (ch == 'y' || ch == 'Y') { if (ed_save() == 0) ed_sigint = 1; else ed_status = "save failed"; ed_prompt = 0; continue; }
            if (ch == 'n' || ch == 'N') { ed_sigint = 1; break; }
            ed_prompt = 0;
            ed_status = ed_modified ? "not saved (^X again to exit)" : 0;
            continue;
        }

        if (ch == 0x13) { /* Ctrl-S */
            int rc = ed_save();
            ed_status = rc == 0 ? "saved" : "save failed";
            continue;
        }
        if (ch == 0x18) { /* Ctrl-X */
            if (ed_modified && !ed_read_only) { ed_prompt = 1; ed_status = 0; continue; }
            break;
        }
        if (ch == '\r' || ch == '\n') { ed_newline(); continue; }
        if (ch == 0x7F || ch == 0x08) { ed_backspace(); continue; }

        if (ch == 0x1B) { /* ESC 序列 */
            char s[8];
            int n = 0;
            long rr = sys_read(kbd, &s[n], 1);
            if (rr <= 0) continue;
            if (s[n] != '[' && s[n] != 'O') continue;
            n++;
            rr = sys_read(kbd, &s[n], 1);
            if (rr <= 0) continue;
            n++;
            char k = s[1];
            int view_rows = rows - 1;
            if (k == 'A') { if (ed_cur_line > 0) ed_cur_line--; }
            else if (k == 'B') { if (ed_cur_line + 1 < ed_nlines) ed_cur_line++; }
            else if (k == 'C') {
                /* 右移一个 UTF-8 序列 */
                int l = ed_cur_line;
                size_t pos = ed_lineoff[l] + (size_t)ed_cur_col;
                size_t e = ed_lineoff[l + 1];
                size_t seq_end = pos;
                if (pos < e && ed_buf[pos] != '\n') {
                    unsigned cp;
                    seq_end = pos + seq_decode(pos, &cp);
                }
                ed_cur_col = (int)(seq_end - ed_lineoff[l]);
                ed_goal_col = -1;
            }
            else if (k == 'D') {
                if (ed_cur_col > 0) {
                    int l = ed_cur_line;
                    size_t line_start = ed_lineoff[l];
                    size_t pos = line_start + (size_t)ed_cur_col;
                    size_t seq_start = pos - 1;
                    while (seq_start > line_start && ((unsigned char)ed_buf[seq_start] & 0xC0) == 0x80) seq_start--;
                    ed_cur_col = (int)(seq_start - line_start);
                }
                ed_goal_col = -1;
            }
            else if (k == 'H') { ed_cur_col = 0; ed_goal_col = -1; }
            else if (k == 'F') {
                int l = ed_cur_line;
                int len = line_len_bytes(l);
                ed_cur_col = len > 0 ? len - 1 : 0; /* 落在换行前 */
                ed_goal_col = -1;
            }
            else if (k == '5' || k == '6') { /* PgUp/PgDn */
                char t;
                sys_read(kbd, &t, 1); /* '~' */
                int delta = (k == '5') ? -view_rows : view_rows;
                ed_cur_line += delta;
                if (ed_cur_line < 0) ed_cur_line = 0;
                if (ed_cur_line >= ed_nlines) ed_cur_line = ed_nlines - 1;
            }
            else if (k == '3') { char t; sys_read(kbd, &t, 1); ed_delete_forward(); }
            /* A/B 垂直移动时保持显示列 */
            {
                int target = ed_goal_col;
                if (target < 0) target = ed_byte_col_to_cell(ed_cur_line, ed_cur_col);
                if (target >= 0 && ed_cur_col >= 0) {
                    int want = target;
                    int got = ed_cell_to_byte_col(ed_cur_line, want);
                    int cur_len = line_len_bytes(ed_cur_line);
                    ed_cur_col = got > cur_len ? (cur_len > 0 ? cur_len - 1 : 0) : got;
                }
                ed_goal_col = target;
            }
            continue;
        }

        if ((unsigned char)ch >= 0x20) {
            char bytes[8];
            size_t n = 0;
            bytes[n++] = ch;
            if ((unsigned char)ch >= 0xC0) { /* UTF-8 续字节 */
                size_t seqlen = ((unsigned char)ch & 0xE0) == 0xC0 ? 2 : ((unsigned char)ch & 0xF0) == 0xE0 ? 3 : 4;
                while (n < seqlen) {
                    long rr = sys_read(kbd, &bytes[n], 1);
                    if (rr <= 0) break;
                    n++;
                }
            }
            ed_insert_bytes(bytes, n);
            continue;
        }
        /* 其他控制字节：忽略 */
    }

    tty_restore(kbd, &tty);
    if (kbd > 2) sys_close(kbd);
    sys_write(1, "\x1b[2J\x1b[H", sizeof("\x1b[2J\x1b[H") - 1); /* 退出清屏 */
    return 0;
}

int tool_editor(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("editor: usage: editor <file>"); return 2; }
    ed_path = argv[1];
    ed_len = 0;
    ed_modified = 0;
    ed_read_only = 0;
    ed_cur_line = 0;
    ed_cur_col = 0;
    ed_top_line = 0;
    ed_goal_col = -1;
    ed_status = 0;
    ed_prompt = 0;
    ed_sigint = 0;

    /* 读文件（不存在视为空文件） */
    {
        long fd = sys_open(ed_path, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) {
            if (AETHER_ERR_CODE(fd) != 2) { /* 非 ENOENT */
                elog_err("editor: %s: %d", ed_path, AETHER_ERR_CODE(fd));
                return 1;
            }
            /* 新文件 */
            ed_lineoff[0] = 0;
            ed_lineoff[1] = 0;
            ed_nlines = 1;
            return ed_run();
        }
        for (;;) {
            long n = sys_read((int)fd, ed_buf + ed_len, sizeof(ed_buf) - ed_len);
            if (n == 0) break;
            if (AETHER_IS_ERR(n)) { elog_err("editor: read failed"); return 1; }
            ed_len += (size_t)n;
            if (ed_len == sizeof(ed_buf)) { elog_err("editor: file too large (>256KB)"); return 1; }
        }
        sys_close((int)fd);
    }
    /* 可写性检查 */
    {
        long fd = sys_open(ed_path, O_RDWR, 0);
        if (AETHER_IS_ERR(fd)) ed_read_only = 1;
        else sys_close((int)fd);
    }
    ed_rebuild_lines();
    return ed_run();
}
