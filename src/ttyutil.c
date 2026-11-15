#include "ttyutil.h"
#include "syscall_shim.h"
#include <stddef.h>

static void vt_out(const char *s, size_t n)
{
    sys_write(1, s, n);
}

/* 极简 uint→dec（仅正数；VT100 参数永远非负） */
static size_t u32toa(char *buf, size_t cap, unsigned v)
{
    char tmp[10];
    size_t i = 0, k = 0;
    if (v == 0) tmp[i++] = '0';
    while (v && i < sizeof(tmp)) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0 && k + 1 < cap) buf[k++] = tmp[--i];
    buf[k] = '\0';
    return k;
}

int tty_save(int fd, aether_tty_t *t)
{
    long r = sys_ioctl(fd, TTY_IOC_TCGETS, (unsigned long)&t->saved_tios);
    t->saved = (r == 0);
    return t->saved ? 0 : -1;
}

void tty_restore(int fd, const aether_tty_t *t)
{
    if (t && t->saved)
        sys_ioctl(fd, TTY_IOC_TCSETS, (unsigned long)&t->saved_tios);
}

int tty_cbreak(int fd, const aether_tty_t *t)
{
    aether_termios_t tios = t->saved_tios;
    tios.c_lflag &= ~(unsigned)(TTY_ICANON | TTY_ECHO | TTY_IEXTEN); /* ISIG 保留 */
    tios.c_iflag &= ~(unsigned)(TTY_IXON);
    tios.c_oflag &= ~(unsigned)(TTY_OPOST);
    tios.c_cc[TTY_VMIN]  = 1;
    tios.c_cc[TTY_VTIME] = 0;
    return sys_ioctl(fd, TTY_IOC_TCSETS, (unsigned long)&tios) == 0 ? 0 : -1;
}

void tty_size(int fd, int *rows, int *cols)
{
    aether_winsize_t ws = { 0, 0, 0, 0 };
    if (sys_ioctl(fd, TTY_IOC_WINSIZE, (unsigned long)&ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        if (rows) *rows = ws.ws_row;
        if (cols) *cols = ws.ws_col;
        return;
    }
    if (rows) *rows = 24;
    if (cols) *cols = 80;
}

void tty_vt(int what, int n)
{
    char buf[32];
    size_t len = 0, pos = 0;

    if (what == TTY_UP || what == TTY_DOWN || what == TTY_LEFT || what == TTY_RIGHT) {
        buf[pos++] = '\x1b'; buf[pos++] = '[';
        pos += u32toa(buf + pos, sizeof(buf) - pos, (unsigned)(n < 0 ? 0 : n));
        buf[pos++] = (what == TTY_UP) ? 'A' : (what == TTY_DOWN) ? 'B' : (what == TTY_RIGHT) ? 'C' : 'D';
        len = pos;
    } else {
        const char *seq = 0;
        size_t seq_len = 0;
        switch (what) {
        case TTY_HOME:        seq = "\x1b[H";       seq_len = 3;  break;
        case TTY_CLEAR_LINE:  seq = "\x1b[2K\r";    seq_len = 5;  break;
        case TTY_CLEAR_SCREEN:seq = "\x1b[2J\x1b[H";seq_len = 7;  break;
        case TTY_CURSOR_SAVE:    seq = "\x1b[s";    seq_len = 3;  break;
        case TTY_CURSOR_RESTORE: seq = "\x1b[u";    seq_len = 3;  break;
        default: return;
        }
        vt_out(seq, seq_len);
        return;
    }
    vt_out(buf, len);
}

void tty_vt_str(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') n++;
    vt_out(s, n);
}

int tty_getpgrp(int fd)
{
    int pgrp = -1;
    if (sys_ioctl(fd, TTY_IOC_TIOCGPGRP, (unsigned long)&pgrp) != 0) return -1;
    return pgrp;
}

int tty_setpgrp(int fd, int pgrp)
{
    long r = sys_ioctl(fd, TTY_IOC_TIOCSPGRP, (unsigned long)&pgrp);
    return r == 0 ? 0 : -1;
}

int aether_tty_open_rw(void)
{
    aether_termios_t scratch;
    long fd;

    /* 1) 控制终端 */
    fd = sys_open("/dev/tty", O_RDWR, 0);
    if (!AETHER_IS_ERR(fd)) return (int)fd;
    /* 2) fd0 已是 tty */
    if (sys_ioctl(0, TTY_IOC_TCGETS, (unsigned long)&scratch) == 0) return 0;
    /* 3) /dev/console（PID 1 / initramfs 场景） */
    fd = sys_open("/dev/console", O_RDWR, 0);
    if (!AETHER_IS_ERR(fd)) return (int)fd;
    /* 4) fd1 是 tty */
    if (sys_ioctl(1, TTY_IOC_TCGETS, (unsigned long)&scratch) == 0) return 1;
    return -1;
}
