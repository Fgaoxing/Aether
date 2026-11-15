/*
 * syslock.c — flock / fsync / sync
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "shell/shell_internal.h"
#include "syscall_shim.h"
#include "util.h"

/* ---------- flock ---------- */

int tool_flock(int argc, char **argv, char **envp)
{
    (void)envp;
    int lock_type = 2; /* LOCK_EX */
    int i = 1;
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-s")) lock_type = 1;       /* LOCK_SH */
        else if (aether_streq(argv[i], "-x")) lock_type = 2;  /* LOCK_EX */
        else if (aether_streq(argv[i], "-u")) lock_type = 8;  /* LOCK_UN */
        else if (aether_streq(argv[i], "-n")) lock_type |= 4; /* LOCK_NB */
        else break;
    }
    if (i >= argc) { elog_err("flock: usage: flock [-s|-x|-u|-n] <file> [command...]"); return 2; }
    const char *file = argv[i++];

    long fd = sys_open(file, O_RDWR | O_CREAT, 0666);
    if (AETHER_IS_ERR(fd)) { elog_err("flock: cannot open %s", file); return 1; }

    long r = sys_flock((int)fd, lock_type);
    if (AETHER_IS_ERR(r)) { elog_err("flock: %s: lock failed (%d)", file, AETHER_ERR_CODE(r)); sys_close((int)fd); return 1; }

    if (i >= argc) {
        /* 无命令：锁即释放（退出时自动），shell 脚本语义 */
        sys_close((int)fd);
        return 0;
    }

    /* 有命令：在持有锁的进程内执行 */
    int pid = (int)sys_fork();
    if (pid < 0 || AETHER_IS_ERR((long)pid)) { elog_err("flock: fork failed"); sys_close((int)fd); return 1; }
    if (pid == 0) {
        /* 子进程：用与 shell 相同的 PATH 搜索逻辑 exec 命令 */
        char *child_argv[32];
        int n = 0;
        for (int k = i; k < argc && n < 31; k++) child_argv[n++] = argv[k];
        child_argv[n] = 0;
        shell_exec_search(n, child_argv, envp); /* 成功不会返回 */
        aether_exit(127);
    }
    int status = 0;
    sys_wait4(pid, &status, 0, 0);
    sys_close((int)fd);
    return (status >> 8) & 0xff;
}

/* ---------- fsync ---------- */

int tool_fsync(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("fsync: usage: fsync <file>..."); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        long fd = sys_open(argv[i], O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) { elog_err("fsync: cannot open %s", argv[i]); rc = 1; continue; }
        long r = sys_fsync((int)fd);
        sys_close((int)fd);
        if (AETHER_IS_ERR(r)) { elog_err("fsync: %s: %d", argv[i], AETHER_ERR_CODE(r)); rc = 1; }
    }
    return rc;
}

/* ---------- sync ---------- */

int tool_sync(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) {
        sys_sync();
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        long fd = sys_open(argv[i], O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) { elog_err("sync: cannot open %s", argv[i]); rc = 1; continue; }
        long r = sys_fsync((int)fd);
        sys_close((int)fd);
        if (AETHER_IS_ERR(r)) { elog_err("sync: %s: %d", argv[i], AETHER_ERR_CODE(r)); rc = 1; }
    }
    return rc;
}

/* ---------- reboot / halt ---------- */
/* 需要 root/CAP_SYS_BOOT；非特权环境下 syscall 返回 EPERM，如实报错。
 * magic1/magic2 是内核约定（0xfee1dead / 0x28121969），必须原样传递。 */

#define RB_MAGIC    0xfee1dead
#define RB_MAGIC_2  0x28121969
#define RB_AUTOBOOT 0x01234567  /* 正常重启 */
#define RB_HALT_POWEROFF 0x4321fedc /* 关机（掉电） */

static int do_reboot_cmd(const char *who, unsigned int cmd)
{
    /* 先 sync 落盘，减少拔电源式丢数据 */
    sys_sync();
    long r = sys_reboot(RB_MAGIC, RB_MAGIC_2, cmd, 0);
    if (AETHER_IS_ERR(r)) {
        int e = AETHER_ERR_CODE(r);
        if (e == 1) elog_err("%s: 权限不足（需要 root/CAP_SYS_BOOT）", who);
        else elog_err("%s: reboot syscall 失败（%d）", who, e);
        return 1;
    }
    /* 成功则不返回（init/kern 已开始关机/重启流程） */
    return 0;
}

int tool_reboot(int argc, char **argv, char **envp)
{
    (void)envp;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (aether_streq(a, "-h") || aether_streq(a, "--help")) {
            elog_info("usage: reboot   同步磁盘并重启系统（需要 root）");
            return 0;
        } else { elog_err("reboot: unknown option '%s'", a); return 2; }
    }
    elog_info("reboot: 同步并请求系统重启…");
    return do_reboot_cmd("reboot", RB_AUTOBOOT);
}

int tool_halt(int argc, char **argv, char **envp)
{
    (void)envp;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (aether_streq(a, "-h") || aether_streq(a, "--help")) {
            elog_info("usage: halt     同步磁盘并停机/断电（需要 root）");
            return 0;
        } else { elog_err("halt: unknown option '%s'", a); return 2; }
    }
    elog_info("halt: 同步并请求系统停机…");
    return do_reboot_cmd("halt", RB_HALT_POWEROFF);
}
