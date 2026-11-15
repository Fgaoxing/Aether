/*
 * hex.c — xxd / od / hexdump / hexedit
 * xxd:   offset: hex  ascii
 * od:    offset (hex): hex字节  ascii（-A x -t x1z 风格，不必标准）
 * hexdump: 偏移 十六进制（8 字节一组）
 * hexedit: 全屏hex editor（^S 保存，^X 退出）
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "syscall_shim.h"
#include "ttyutil.h"
#include "util.h"

/* 通用读全部（hexdump 类工具共用） */
#define HEX_MAX (4 * 1024 * 1024)
static char hex_buf[HEX_MAX];
static size_t hex_len;

static long hex_read_all(int fd)
{
    size_t off = 0;
    for (;;) {
        long n = sys_read(fd, hex_buf + off, sizeof(hex_buf) - off);
        if (n == 0) break;
        if (AETHER_IS_ERR(n)) return -1;
        off += (size_t)n;
        if (off >= sizeof(hex_buf)) break; /* 截断 */
    }
    hex_len = off;
    return (long)off;
}

static int hex_arg_fd(int argc, char **argv, int i, int *fd_out)
{
    if (i >= argc) { *fd_out = 0; return 0; }
    long fd = sys_open(argv[i], O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) { elog_err("%s: cannot open", argv[i]); return -1; }
    *fd_out = (int)fd;
    return 0;
}

static void print_offset(unsigned long long off)
{
    char buf[24];
    size_t k = 0;
    const char *hex = "0123456789abcdef";
    buf[k++] = hex[(off >> 28) & 0xf];
    buf[k++] = hex[(off >> 24) & 0xf];
    buf[k++] = hex[(off >> 20) & 0xf];
    buf[k++] = hex[(off >> 16) & 0xf];
    buf[k++] = hex[(off >> 12) & 0xf];
    buf[k++] = hex[(off >> 8) & 0xf];
    buf[k++] = hex[(off >> 4) & 0xf];
    buf[k++] = hex[off & 0xf];
    buf[k] = '\0';
    sys_write(1, buf, k);
}

static void print_hex_byte(unsigned char b, char *out)
{
    const char *hex = "0123456789abcdef";
    out[0] = hex[b >> 4];
    out[1] = hex[b & 0xf];
}

int tool_xxd(int argc, char **argv, char **envp)
{
    (void)envp;
    int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) { elog_err("xxd: unknown option '%s'", argv[i]); return 2; }
        break;
    }
    int fd = 0;
    if (hex_arg_fd(argc, argv, i, &fd) != 0) return 1;

    long len = hex_read_all(fd);
    if (len < 0) return 1;
    if (fd != 0) sys_close(fd);

    for (size_t off = 0; off < hex_len; off += 16) {
        print_offset(off);
        sys_write(1, ": ", sizeof(": ") - 1);
        /* hex 部分：8 字节一组 */
        for (size_t j = 0; j < 16; j++) {
            if (off + j < hex_len) {
                char hb[3];
                print_hex_byte((unsigned char)hex_buf[off + j], hb);
                sys_write(1, hb, 2);
                sys_write(1, " ", sizeof(" ") - 1);
            } else {
                sys_write(1, "   ", sizeof("   ") - 1);
            }
            if (j == 7) sys_write(1, " ", sizeof(" ") - 1);
        }
        sys_write(1, " ", sizeof(" ") - 1);
        /* ASCII 部分 */
        for (size_t j = 0; j < 16 && off + j < hex_len; j++) {
            unsigned char b = (unsigned char)hex_buf[off + j];
            char c = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
            sys_write(1, &c, 1);
        }
        sys_write(1, "\n", sizeof("\n") - 1);
    }
    return 0;
}

int tool_od(int argc, char **argv, char **envp)
{
    (void)envp;
    int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) { elog_err("od: unknown option '%s'", argv[i]); return 2; }
        break;
    }
    int fd = 0;
    if (hex_arg_fd(argc, argv, i, &fd) != 0) return 1;

    long len = hex_read_all(fd);
    if (len < 0) return 1;
    if (fd != 0) sys_close(fd);

    for (size_t off = 0; off < hex_len; off += 16) {
        print_offset(off);
        sys_write(1, " ", sizeof(" ") - 1);
        for (size_t j = 0; j < 16; j++) {
            if (off + j < hex_len) {
                char hb[3];
                print_hex_byte((unsigned char)hex_buf[off + j], hb);
                sys_write(1, hb, 2);
                sys_write(1, " ", sizeof(" ") - 1);
            } else sys_write(1, "   ", sizeof("   ") - 1);
        }
        sys_write(1, ">", sizeof(">") - 1);
        for (size_t j = 0; j < 16 && off + j < hex_len; j++) {
            unsigned char b = (unsigned char)hex_buf[off + j];
            char c = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
            sys_write(1, &c, 1);
        }
        sys_write(1, "<\n", sizeof("<\n") - 1);
    }
    return 0;
}

int tool_hexdump(int argc, char **argv, char **envp)
{
    (void)envp;
    int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) { elog_err("hexdump: unknown option '%s'", argv[i]); return 2; }
        break;
    }
    int fd = 0;
    if (hex_arg_fd(argc, argv, i, &fd) != 0) return 1;

    long len = hex_read_all(fd);
    if (len < 0) return 1;
    if (fd != 0) sys_close(fd);

    for (size_t off = 0; off < hex_len; off += 16) {
        print_offset(off);
        sys_write(1, "  ", sizeof("  ") - 1);
        for (size_t j = 0; j < 16; j++) {
            if (off + j < hex_len) {
                char hb[3];
                print_hex_byte((unsigned char)hex_buf[off + j], hb);
                sys_write(1, hb, 2);
            } else sys_write(1, "  ", sizeof("  ") - 1);
            if (j != 15) sys_write(1, " ", sizeof(" ") - 1);
        }
        sys_write(1, "  |", sizeof("  |") - 1);
        for (size_t j = 0; j < 16 && off + j < hex_len; j++) {
            unsigned char b = (unsigned char)hex_buf[off + j];
            char c = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
            sys_write(1, &c, 1);
        }
        sys_write(1, "|\n", sizeof("|\n") - 1);
    }
    return 0;
}

/* ---------- hexedit ---------- */

static void hexedit_status(size_t off, const char *msg)
{
    /* 简单状态行：offset + msg */
    char buf[96];
    size_t k = 0;
    const char *p = "offset 0x";
    for (size_t i = 0; p[i]; i++) buf[k++] = p[i];
    const char *hex = "0123456789abcdef";
    buf[k++] = hex[(off >> 28) & 0xf];
    buf[k++] = hex[(off >> 24) & 0xf];
    buf[k++] = hex[(off >> 20) & 0xf];
    buf[k++] = hex[(off >> 16) & 0xf];
    buf[k++] = hex[(off >> 12) & 0xf];
    buf[k++] = hex[(off >> 8) & 0xf];
    buf[k++] = hex[(off >> 4) & 0xf];
    buf[k++] = hex[off & 0xf];
    buf[k++] = ' ';
    if (msg) for (size_t i = 0; msg[i] && k + 1 < sizeof(buf); i++) buf[k++] = msg[i];
    buf[k] = '\0';
    sys_write(1, "\x1b[7m", sizeof("\x1b[7m") - 1);
    sys_write(1, buf, k);
    int written = (int)k;
    for (int i = written; i < 78; i++) sys_write(1, " ", sizeof(" ") - 1);
    sys_write(1, "\x1b[27m\r\n", sizeof("\x1b[27m\r\n") - 1);
}

int tool_hexedit(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("hexedit: usage: hexedit <file>"); return 2; }
    const char *path = argv[1];

    int fd = (int)sys_open(path, O_RDWR, 0);
    if (AETHER_IS_ERR((long)fd)) {
        /* 尝试只读（显示用） */
        fd = (int)sys_open(path, O_RDONLY, 0);
        if (AETHER_IS_ERR((long)fd)) { elog_err("hexedit: cannot open %s", path); return 1; }
    }
    long len = hex_read_all(fd);
    sys_close(fd);
    if (len < 0) return 1;

    aether_tty_t tty;
    int kbd = aether_tty_open_rw(); /* 回退链：/dev/tty → fd0(tty) → /dev/console → fd1 */
    if (kbd < 0) { elog_err("hexedit: needs a tty"); return 1; }
    if (tty_save(kbd, &tty) != 0 || tty_cbreak(kbd, &tty) != 0) {
        elog_err("hexedit: needs a tty");
        if (kbd > 2) sys_close(kbd);
        return 1;
    }

    size_t off = 0;         /* 当前字节偏移 */
    int high_nibble = 1;    /* 1 = 编辑高位，0 = 低位 */
    int modified = 0;

    for (;;) {
        int rows, cols;
        tty_size(kbd, &rows, &cols);
        int view_rows = (rows - 2) > 0 ? rows - 2 : 1;
        /* 重绘 */
        sys_write(1, "\x1b[2J\x1b[H", sizeof("\x1b[2J\x1b[H") - 1);
        size_t page_start = (off / (size_t)(view_rows * 16)) * (size_t)(view_rows * 16);
        for (size_t row = 0; row < (size_t)view_rows; row++) {
            size_t base = page_start + row * 16;
            if (base >= hex_len) { sys_write(1, "\x1b[K\r\n", sizeof("\x1b[K\r\n") - 1); continue; }
            print_offset(base);
            sys_write(1, "  ", sizeof("  ") - 1);
            for (size_t j = 0; j < 16; j++) {
                size_t pos = base + j;
                if (pos < hex_len) {
                    char hb[3];
                    if (pos == off) { sys_write(1, "\x1b[7m", sizeof("\x1b[7m") - 1); }
                    print_hex_byte((unsigned char)hex_buf[pos], hb);
                    sys_write(1, hb, 2);
                    if (pos == off) sys_write(1, "\x1b[27m", sizeof("\x1b[27m") - 1);
                } else sys_write(1, "  ", sizeof("  ") - 1);
                sys_write(1, " ", sizeof(" ") - 1);
            }
            sys_write(1, " ", sizeof(" ") - 1);
            for (size_t j = 0; j < 16 && base + j < hex_len; j++) {
                unsigned char b = (unsigned char)hex_buf[base + j];
                char c = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
                if (base + j == off) sys_write(1, "\x1b[7m", sizeof("\x1b[7m") - 1);
                sys_write(1, &c, 1);
                if (base + j == off) sys_write(1, "\x1b[27m", sizeof("\x1b[27m") - 1);
            }
            sys_write(1, "\x1b[K\r\n", sizeof("\x1b[K\r\n") - 1);
        }
        char st[64];
        size_t k = 0;
        const char *msg = modified ? "modified  ^S save  ^X quit" : "^S save  ^X quit";
        for (size_t i = 0; msg[i] && k + 1 < sizeof(st); i++) st[k++] = msg[i];
        st[k] = '\0';
        hexedit_status(off, st);

        char ch;
        long r = sys_read(kbd, &ch, 1);
        if (r <= 0) break;

        if (ch == 0x13) { /* Ctrl-S 保存 */
            /* 写回文件 */
            int wfd = (int)sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (!AETHER_IS_ERR((long)wfd)) {
                long off2 = 0;
                while (off2 < (long)hex_len) {
                    long w = sys_write(wfd, hex_buf + off2, hex_len - (size_t)off2);
                    if (AETHER_IS_ERR(w) || w == 0) break;
                    off2 += w;
                }
                sys_close(wfd);
                modified = 0;
            }
            continue;
        }
        if (ch == 0x18) { /* Ctrl-X 退出 */
            break;
        }
        if (ch == 0x1b) { /* ESC 序列：方向键 */
            char s[8];
            int n = 0;
            long rr = sys_read(kbd, &s[n], 1);
            if (rr <= 0) continue;
            if (s[n] != '[' && s[n] != 'O') continue;
            n++;
            rr = sys_read(kbd, &s[n], 1);
            if (rr <= 0) continue;
            char k2 = s[1];
            if (k2 == 'A' && off >= 16) off -= 16;
            else if (k2 == 'B' && off + 16 < hex_len) off += 16;
            else if (k2 == 'C' && off + 1 < hex_len) off += 1;
            else if (k2 == 'D' && off > 0) off -= 1;
            else if (k2 == 'H') off = 0;
            else if (k2 == 'F') off = hex_len - 1;
            continue;
        }
        /* 十六进制输入 */
        int nib = -1;
        if (ch >= '0' && ch <= '9') nib = ch - '0';
        else if (ch >= 'a' && ch <= 'f') nib = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') nib = ch - 'A' + 10;
        if (nib < 0) continue;
        unsigned char *byte = (unsigned char *)&hex_buf[off];
        if (high_nibble) {
            *byte = (unsigned char)((*byte & 0x0f) | (unsigned char)(nib << 4));
            high_nibble = 0;
        } else {
            *byte = (unsigned char)((*byte & 0xf0) | (unsigned char)nib);
            high_nibble = 1;
            /* 自动移到下一字节 */
            if (off + 1 < hex_len) off++;
        }
        modified = 1;
    }

    tty_restore(kbd, &tty);
    if (kbd > 2) sys_close(kbd);
    sys_write(1, "\x1b[2J\x1b[H", sizeof("\x1b[2J\x1b[H") - 1);
    return 0;
}
