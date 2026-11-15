/*
 * lineedit.c — 极简行编辑（§7.5）：raw/cbreak 模式切换 + VT100 输出
 * 仅：打印字符插入、退格、左/右/Home/End（含 Ctrl-A/E）、Delete(ESC[3~)、Ctrl-U、Ctrl-C(放弃)、Ctrl-D(EOF)。
 * 不做：历史记录、自动补全、多字节宽字符宽度处理（v1，§7）。
 * 窄字符（单元格 1 宽）场景下重绘才精确；宽字符可能显示错位且 Backspace 可能拆开 UTF-8 序列（已知限制）。
 *
 * TTY 模式：调用方（或本文件）保存/恢复 termios；非 tty（stdin 脚本）时按普通读行处理。
 */
#include <stddef.h>

#include "shell_internal.h"
#include "elog.h"
#include "syscall_shim.h"
#include "ttyutil.h"
#include "util.h"

#define KEY_EOF      (-1)
#define KEY_SIGINT   (-2)
#define KEY_BACKSPACE 0x7f
#define KEY_BACKSPACE2 0x08

/* ---------- 命令历史（仅交互；环形缓冲，与上一条相同则跳过） ---------- */
int aether_telnet_mode = 0; /* telnetd 子进程设 1：IAC 剥离 + 行模式 */

#define HISTORY_MAX 64
#define HISTORY_LINE 1024
static char    history[HISTORY_MAX][HISTORY_LINE];
static int     hist_head = 0;   /* 下一次写入位置 */
static int     hist_count = 0;  /* 有效条数 */

static const char *history_get(int idx)
{
    if (idx < 0 || idx >= hist_count) return 0;
    int pos = (hist_head - 1 - idx) % HISTORY_MAX;
    if (pos < 0) pos += HISTORY_MAX;
    return history[pos];
}

static void history_push(const char *line)
{
    if (!line || !line[0]) return;
    if (hist_count > 0 && aether_streq(history[(hist_head - 1 + HISTORY_MAX) % HISTORY_MAX], line))
        return; /* 与上一条相同 */
    aether_strcpy(history[hist_head], line, HISTORY_LINE);
    hist_head = (hist_head + 1) % HISTORY_MAX;
    if (hist_count < HISTORY_MAX) hist_count++;
}


static size_t xstrlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* —— 非交互/脚本模式：从 fd 0 按字节读一行 —— */
static int read_byte_or_eof(char *out)
{
    long r = sys_read(0, out, 1);
    if (r <= 0) return -1;
    return 0;
}

int shell_read_fd0_line(char *buf, size_t cap)
{
    size_t n = 0;
    if (cap < 2) return -1;
    for (;;) {
        char ch;
        long r = sys_read(0, &ch, 1);
        if (r <= 0) { if (n == 0) return -1; break; } /* EOF（首尾无内容） */
        if (aether_telnet_mode && (unsigned char)ch == 0x03) {
            /* Ctrl-C 从 telnet 客户端过来 → 结束当前救援 shell（等效断开） */
            sys_write(1, "\r\n", 2);
            aether_exit(0);
        }
        if (aether_telnet_mode && (unsigned char)ch == 0xFF) {
            /* IAC：按 telnet 协议跳过（IAC SB … IAC SE 跳过整块；其他跳过一个字节的选项） */
            char opt;
            if (read_byte_or_eof(&opt) != 0) { if (n == 0) return -1; break; }
            if ((unsigned char)opt == 0xF4 || (unsigned char)opt == 0xF5) {
                /* IAC IP / AO → 同 Ctrl-C：终止救援 shell */
                sys_write(1, "\r\n", 2);
                aether_exit(0);
            }
            if ((unsigned char)opt == 0xFA) { /* IAC SB … */
                char prev = 0;
                for (;;) {
                    char c;
                    if (read_byte_or_eof(&c) != 0) { if (n == 0) return -1; break; }
                    if ((unsigned char)prev == 0xFF && (unsigned char)c == 0xF0) break; /* IAC SE */
                    prev = c;
                }
            } else if ((unsigned char)opt != 0xF0 && (unsigned char)opt != 0xF1 && (unsigned char)opt != 0xF2 && (unsigned char)opt != 0xF3 && (unsigned char)opt != 0xF4 && (unsigned char)opt != 0xF5 && (unsigned char)opt != 0xF6 && (unsigned char)opt != 0xF7 && (unsigned char)opt != 0xF8 && (unsigned char)opt != 0xF9) {
                (void)opt; /* 已跳过 */
            }
            continue;
        }
        if (ch == '\n') break;
        if (n + 1 < cap) buf[n++] = ch;
    }
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) n--;
    buf[n] = '\0';
    return (int)n;
}

static int tty_is(int fd)
{
    aether_termios_t t;
    return sys_ioctl(fd, TTY_IOC_TCGETS, (unsigned long)&t) == 0;
}

/* 重绘整行（窄字符精确；宽字符可能错位）：\r + prompt + buf + 右侧清空 + \r + prompt + head(pos) */
static void redraw(const char *prompt, const char *buf, size_t len, size_t pos, size_t old_len)
{
    size_t plen = xstrlen(prompt);
    sys_write(1, "\r", sizeof("\r") - 1);
    sys_write(1, prompt, plen);
    if (len) sys_write(1, buf, len);
    /* 擦掉旧行比新行长出的部分 */
    for (size_t i = len; i < old_len; i++) sys_write(1, " ", sizeof(" ") - 1);
    /* 光标回到 pos */
    sys_write(1, "\r", sizeof("\r") - 1);
    sys_write(1, prompt, plen);
    if (pos) sys_write(1, buf, pos);
}

static int read_escape_sequence(void)
{
    char seq[8];
    int n = 0;
    (void)n;
    /* 读 ESC 之后的部分：\x1b [ <A-Za-z0-9~> */
    long r = sys_read(0, &seq[0], 1);
    if (r <= 0 || (seq[0] != '[' && seq[0] != 'O')) return -1; /* CSI 或 SS3 */
    n = 1;
    for (;;) {
        r = sys_read(0, &seq[n], 1);
        if (r <= 0) return -1;
        char ch = seq[n];
        n++;
        if (n >= (int)sizeof(seq)) return -1;
        if (ch >= 'A' && ch <= 'Z') return ch;       /* C D H F 等 */
        if (ch >= 'a' && ch <= 'z') return ch;
        if (ch == '~') { /* ESC [ 3 ~ → Delete */
            if (seq[1] == '3') return '\x7f';
            if (seq[1] == '1') return 'H'; /* ESC [ 1 ~ Home */
            if (seq[1] == '4') return 'F'; /* ESC [ 4 ~ End */
            return -1;
        }
        if (ch >= '0' && ch <= '9') continue;        /* 数字参数继续 */
        return -1;
    }
}

/* 主入口。buf 容量 cap；返回值：-2 Ctrl-C；-1 EOF；>=0 行长度 */
int lineedit_read(const char *prompt, char *buf, size_t cap)
{
    size_t len = 0, pos = 0;
    int hist_idx = -1;                    /* -1 = 当前编辑行；>=0 = 历史第几条（0=最新） */
    char hist_save[SHELL_MAX_LINE];       /* 进入历史前保存当前未提交行 */
    aether_tty_t saved;

    /* 非 tty：脚本式读行 */
    if (!tty_is(0))
        return shell_read_fd0_line(buf, cap);

    sys_write(1, prompt, xstrlen(prompt));

    if (tty_save(0, &saved) != 0)
        return shell_read_fd0_line(buf, cap);
    if (tty_cbreak(0, &saved) != 0) {
        tty_restore(0, &saved);
        return -1;
    }
    for (;;) {
        char ch;
        long r = sys_read(0, &ch, 1);
        if (r == 0) { tty_restore(0, &saved); return -1; }
        if (AETHER_IS_ERR(r)) {
            if (AETHER_ERR_CODE(r) == 4) { /* EINTR：可能是 SIGINT */
                tty_restore(0, &saved);   /* 先恢复 OPOST */
                sys_write(1, "^C\n", sizeof("^C\n") - 1);
                return KEY_SIGINT;
            }
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            tty_restore(0, &saved);   /* 先恢复 OPOST（ONLCR），\n 才会 CR+LF */
            sys_write(1, "\n", sizeof("\n") - 1);
            buf[len] = '\0';
            history_push(buf);
            return (int)len;
        }
        if (ch == KEY_BACKSPACE || ch == KEY_BACKSPACE2) {
            if (pos > 0) {
                size_t old_len = len;
                if (pos < len) {
                    for (size_t i = pos - 1; i + 1 < len; i++) buf[i] = buf[i + 1];
                }
                len--;
                pos--;
                redraw(prompt, buf, len, pos, old_len);
            }
            continue;
        }
        if (ch == 0x03) { /* Ctrl-C（ISIG 未拦时的字节形态） */
            tty_restore(0, &saved);   /* 先恢复 OPOST */
            sys_write(1, "^C\n", sizeof("^C\n") - 1);
            return KEY_SIGINT;
        }
        if (ch == 0x04) { /* Ctrl-D */
            if (len == 0) {
                tty_restore(0, &saved);   /* 先恢复 OPOST */
                sys_write(1, "\n", sizeof("\n") - 1);
                return -1;
            }
            continue;
        }
        if (ch == 0x15) { /* Ctrl-U 清行 */
            redraw(prompt, "", 0, 0, len);
            len = 0; pos = 0;
            continue;
        }
        if (ch == 0x01) { pos = 0; redraw(prompt, buf, len, pos, len); continue; } /* Ctrl-A */
        if (ch == 0x05) { pos = len; redraw(prompt, buf, len, pos, len); continue; } /* Ctrl-E */
        if (ch == 0x1b) { /* ESC 序列 */
            int k = read_escape_sequence();
            switch (k) {
            case 'A': {  /* ↑ 上一条历史 */
                if (hist_count == 0) break;
                size_t old_len = len;
                if (hist_idx == -1) {
                    size_t i = 0;
                    while (i <= len && i < sizeof(hist_save)) { hist_save[i] = buf[i]; i++; }
                }
                if (hist_idx + 1 >= hist_count) break; /* 已经最早一条 */
                hist_idx++;
                aether_strcpy(buf, history_get(hist_idx), cap);
                len = aether_strlen(buf);
                pos = len;
                redraw(prompt, buf, len, pos, old_len);
                break;
            }
            case 'B': {  /* ↓ 下一条历史（直至回到当前行） */
                if (hist_idx == -1) break;
                size_t old_len = len;
                hist_idx--;
                if (hist_idx < 0) aether_strcpy(buf, hist_save, cap);
                else              aether_strcpy(buf, history_get(hist_idx), cap);
                len = aether_strlen(buf);
                pos = len;
                redraw(prompt, buf, len, pos, old_len);
                break;
            }
            case 'D': if (pos > 0) { pos--; redraw(prompt, buf, len, pos, len); } break;   /* ← */
            case 'C': if (pos < len) { pos++; redraw(prompt, buf, len, pos, len); } break;  /* → */
            case 'H': pos = 0; redraw(prompt, buf, len, pos, len); break;                   /* Home */
            case 'F': pos = len; redraw(prompt, buf, len, pos, len); break;                 /* End */
            case '\x7f': /* Delete（前向删） */
                if (pos < len) {
                    size_t old_len = len;
                    for (size_t i = pos; i + 1 < len; i++) buf[i] = buf[i + 1];
                    len--;
                    redraw(prompt, buf, len, pos, old_len);
                }
                break;
            default: break;
            }
            continue;
        }
        if (ch >= 0x20) { /* 可打印 / 多字节字节（0x7F 已在上面截获） */
            if (len + 1 >= cap) continue; /* 满了：丢弃，不崩 */
            if (pos < len) {
                size_t old_len = len;
                /* 插入到中部：腾位 */
                for (size_t i = len; i > pos; i--) buf[i] = buf[i - 1];
                buf[pos] = ch;
                len++;
                pos++;
                redraw(prompt, buf, len, pos, old_len);
            } else {
                buf[len++] = ch;
                sys_write(1, &ch, 1);
                pos++;
            }
            continue;
        }
        /* 其他控制字节：忽略 */
    }
}
