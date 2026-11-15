/*
 * ttyutil.h — tty 模式切换 + VT100 输出封装（§9）
 * 双分支共用；termios 一律走内核 UAPI 布局 + raw ioctl（TCGETS/TCSETS），layout 对 x86_64/i386/aarch64/riscv64 一致。
 * 交互式工具自持状态：进入时 tty_save+tty_cbreak，退出 tty_restore；禁止全局 tty 状态（§9）。
 */
#ifndef AETHER_TTYUTIL_H
#define AETHER_TTYUTIL_H

#include <stddef.h>

/* 内核 UAPI struct termios（asm-generic，NCCS=19；x86_64 与之集合，含 44B 不含 c_line 对齐差异） */
typedef struct {
    unsigned int  c_iflag;
    unsigned int  c_oflag;
    unsigned int  c_cflag;
    unsigned int  c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
    unsigned int  c_ispeed;
    unsigned int  c_ospeed;
} aether_termios_t; /* 44 字节 */

/* 每工具一份的上下文（禁止全局） */
typedef struct {
    int              saved;      /* 1 = 已保存 termios（restore 前有效） */
    aether_termios_t saved_tios;
} aether_tty_t;

/* ioctl 常数（asm-generic 与 x86 同值） */
#define TTY_IOC_TCGETS   0x5401
#define TTY_IOC_TCSETS   0x5402
#define TTY_IOC_WINSIZE  0x5413 /* TIOCGWINSZ */

/* termios 标志位（asm-generic 与 x86 一致） */
#define TTY_ISIG    0x0001
#define TTY_ICANON  0x0002
#define TTY_ECHO    0x0008
#define TTY_IXON    0x0400
#define TTY_IEXTEN  0x8000
#define TTY_OPOST   0x0001
#define TTY_VMIN    6
#define TTY_VTIME   5

typedef struct { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; } aether_winsize_t;

/* 保存/恢复 */
int  tty_save(int fd, aether_tty_t *t);      /* 0 成功；非 tty 返回 -1（调用方决定是否退出） */
void tty_restore(int fd, const aether_tty_t *t);
/* 切换到 cbreak：VMIN=1 VTIME=0；清 ICANON/ECHO/IEXTEN/IXON；保留 ISIG（Ctrl-C 可被信号化） */
int  tty_cbreak(int fd, const aether_tty_t *t);
void tty_size(int fd, int *rows, int *cols); /* 失败默认 24x80 */

/* 进程组 / 控制终端（作业控制用） */
#define TTY_IOC_TIOCGPGRP 0x540F
#define TTY_IOC_TIOCSPGRP 0x5410
int  tty_getpgrp(int fd);                      /* <0 失败（fd 非 tty） */
int  tty_setpgrp(int fd, int pgrp);            /* 0 成功 */
/* tty 打开回退链：/dev/tty（有 ctty 时）→ fd0（若本身是 tty）→ /dev/console（initramfs PID1 有 ctty 时）→ fd1（若是 tty）
 * 返回可读可写 fd，失败 -1。给 editor/hexedit/less 等需要直接访问终端的工具使用。 */
int  aether_tty_open_rw(void);

/* VT100 输出（写到 fd=1，不构建大串） */
enum { TTY_UP = 0, TTY_DOWN, TTY_LEFT, TTY_RIGHT, TTY_HOME, TTY_CLEAR_LINE, TTY_CLEAR_SCREEN,
       TTY_CURSOR_SAVE, TTY_CURSOR_RESTORE };
void tty_vt(int what, int n);                /* n 用于方向/行数等 */
void tty_vt_str(const char *s);              /* 原样输出到 fd=1 */

#endif /* AETHER_TTYUTIL_H */
