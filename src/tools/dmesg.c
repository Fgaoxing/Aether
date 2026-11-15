/*
 * dmesg.c — 内核日志（§5/§12.5）
 * 优先 sys_syslog(READ_ALL)；EPERM/ENOSYS 时提示权限/平台问题。v1 不分页（| less 自行接续）。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "syscall_shim.h"
#include "util.h"

#define SYSLOG_READ_ALL 3
#define DMESG_BUFSZ     (128 * 1024)

static char dmesg_buf[DMESG_BUFSZ];

static int strip_prefix_level(const char **pline, size_t *plen)
{
    const char *p = *pline;
    if (p[0] == '<' && p[1] >= '0' && p[1] <= '9' && p[2] == '>') {
        *pline = p + 3;
        *plen -= 3;
        return 1;
    }
    return 0;
}

int tool_dmesg(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;
    if (argc > 1) {
        elog_err("dmesg: usage: dmesg");
        return 2;
    }
    long n = sys_syslog(SYSLOG_READ_ALL, dmesg_buf, sizeof(dmesg_buf));
    if (AETHER_IS_ERR(n)) {
        int e = AETHER_ERR_CODE(n);
        if (e == 1) /* EPERM */
            elog_err("dmesg: 权限不足（需要 root / CAP_SYSLOG，或内核 kernel.dmesg_restrict=0）");
        else
            elog_err("dmesg: syslog 失败（%d）——此内核可能不支持 syslog 接口", e);
        return 1;
    }
    /* 输出：逐行剥掉 printk 级别前缀 "<n>" */
    const char *p = dmesg_buf, *end = dmesg_buf + n;
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        size_t len = (size_t)(nl - p);
        const char *line = p;
        strip_prefix_level(&line, &len);
        if (len) sys_write(1, line, len);
        sys_write(1, "\n", sizeof("\n") - 1);
        p = (nl < end) ? nl + 1 : end;
    }
    return 0;
}
