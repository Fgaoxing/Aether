/*
 * ps.c — process list（§12.5）
 * 读 /proc/[pid]/stat + /proc/[pid]/comm；/proc 未挂载时给出 mount 提示。
 * 顺序按 getdents64 返回顺序（v1 不sort）。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "syscall_shim.h"
#include "util.h"

/* linux_dirent64（UAPI 布局，三架构一致） */
typedef struct {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
} ps_dirent_t;

static int all_digits(const char *s)
{
    if (!*s) return 0;
    for (int i = 0; s[i]; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

static void print_proc_stat(const char *name)
{
    char path[64];
    size_t d = 0;
    const char *pre = "/proc/";
    for (size_t i = 0; pre[i]; i++) path[d++] = pre[i];
    for (size_t i = 0; name[i]; i++) path[d++] = name[i];
    const char *post = "/stat";
    for (size_t i = 0; post[i]; i++) path[d++] = post[i];
    path[d] = '\0';

    long fd = sys_open(path, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) return;

    char buf[512];
    long n = sys_read((int)fd, buf, sizeof(buf) - 1);
    sys_close((int)fd);
    if (n <= 0) return;
    buf[n] = '\0';

    /* 123 (comm with spaces) S ... → comm 可能含空格/括号，按最后一个 ')' 切 */
    const char *lp = 0, *rp = 0;
    for (long i = 0; i < n; i++) {
        if (buf[i] == '(') lp = buf + i + 1;
        if (buf[i] == ')') rp = buf + i; /* 保留最后一次 */
    }
    if (!lp || !rp || rp < lp) return;

    /* state：rp 之后跳过 ') ' */
    const char *q = rp + 1;
    while (*q == ' ') q++;
    char state = *q ? *q : '?';

    /* pid：( 之前 */
    const char *sp = buf;
    while (*sp == ' ' || *sp == '\t') sp++;
    int pid = 0;
    while (*sp >= '0' && *sp <= '9') { pid = pid * 10 + (*sp - '0'); sp++; }

    /* comm：lp..rp-1，去掉换行 */
    size_t clen = (size_t)(rp - lp);
    char comm[128];
    if (clen >= sizeof(comm)) clen = sizeof(comm) - 1;
    for (size_t i = 0; i < clen; i++) comm[i] = lp[i];
    comm[clen] = '\0';
    for (size_t i = 0; comm[i]; i++) if (comm[i] == '\n') comm[i] = '\0';

    {
        char num[12];
        unsigned v = (unsigned)pid;
        size_t m = 0;
        if (v == 0) num[m++] = '0';
        while (v) { num[m++] = (char)('0' + (v % 10)); v /= 10; }
        for (size_t i = 0, j = m - 1; i < j; i++, j--) { char t = num[i]; num[i] = num[j]; num[j] = t; }
        num[m] = '\0';
        char sc[2] = { state, '\0' };
        char c1[10], c2[4];
        aether_str_pad(c1, sizeof(c1), num, 6, 1);
        aether_str_pad(c2, sizeof(c2), sc, 2, 0);
        elog_info("%s %s %s", c1, c2, comm);
    }
}

int tool_ps(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;
    if (argc > 1) { elog_err("ps: usage: ps"); return 2; }

    long fd = sys_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (AETHER_IS_ERR(fd)) {
        elog_err("ps: cannot read /proc (%d). Try: mount -t proc proc /proc", AETHER_ERR_CODE(fd));
        return 1;
    }
    {
        char h1[10], h2[4];
        aether_str_pad(h1, sizeof(h1), "PID", 6, 1);
        aether_str_pad(h2, sizeof(h2), "S", 2, 0);
        elog_info("%s %s COMMAND", h1, h2);
    }
    char buf[4096];
    long n;
    while ((n = sys_getdents64((int)fd, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            ps_dirent_t *d = (ps_dirent_t *)(buf + off);
            off += d->d_reclen;
            if (d->d_reclen == 0) break; /* 防御 */
            if (all_digits(d->d_name))
                print_proc_stat(d->d_name);
        }
    }
    sys_close((int)fd);
    return 0;
}
