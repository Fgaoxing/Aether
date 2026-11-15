/*
 * builtin.c — shell 内置命令（§2 / §9）
 * in_process=1 的（cd / exit / exec）直接在 shell 进程内执行（继承 §2 原始语义）；
 * 其余在 fork 出的子进程中执行，统一走 (argc, argv, envp) → int 返回码。
 * 注意：in_process 内置忽略重定向（cd/exit/exec 无意义重定向），文档化。
 */
#include <stddef.h>

#include "applets.h"
#include "boot.h"
#include "elog.h"
#include "shell_internal.h"
#include "syscall_shim.h"
#include "util.h"

#define AT_REMOVEDIR 0x200
#define MNT_DETACH   0x2
#define PATH_DEFAULT "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

/* 简单前缀匹配（不构造额外字符串） */
static int has_prefix(const char *s, const char *prefix)
{
    size_t i = 0;
    while (prefix[i] != '\0') {
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

/* ---------- 小型辅助 ---------- */

static size_t str_copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (cap == 0) return 0;
    while (src[i] != '\0' && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static size_t str_cat(char *dst, size_t cap, const char *src)
{
    size_t len = 0;
    while (dst[len] != '\0') len++;
    return len + str_copy(dst + len, cap > len ? cap - len : 0, src);
}

static int parse_num(const char *s, unsigned long long *out)
{
    unsigned long long v = 0;
    int base = 10, i = 0;
    if (!s || !*s) return -1;
    if (s[0] == '-' && s[1]) return -1; /* 暂不支持负数编号 */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
    else if (s[0] == '0' && s[1] >= '0' && s[1] <= '7') base = 8;
    for (; s[i]; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        if (d >= base) return -1;
        v = v * (unsigned)base + (unsigned)d;
    }
    *out = v;
    return 0;
}

/* 目录存在性：用 O_DIRECTORY open 检验 */
static int dir_exists(const char *path)
{
    long fd = sys_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (AETHER_IS_ERR(fd)) return 0;
    sys_close((int)fd);
    return 1;
}

/* 拼接 dir + "/" + sub（去掉 dir 尾部斜杠） */
static int join_path(char *buf, size_t cap, const char *dir, const char *sub)
{
    size_t d = 0;
    if (cap < 2) return -1;
    while (dir[d] && d + 1 < cap) { buf[d] = dir[d]; d++; }
    buf[d] = '\0';
    while (d > 0 && buf[d - 1] == '/') buf[--d] = '\0';
    if (d + 2 > cap) return -1;
    buf[d++] = '/';
    buf[d] = '\0';
    return str_cat(buf, cap, sub) == 0 ? -1 : 0;
}

/* 从环境里找 PATH（没有则 NULL） */
static const char *env_path(char **envp)
{
    int i;
    if (!envp) return 0;
    for (i = 0; envp[i]; i++) {
        const char *e = envp[i];
        if (e[0] == 'P' && e[1] == 'A' && e[2] == 'T' && e[3] == 'H' && e[4] == '=')
            return e + 5;
    }
    return 0;
}

/* PATH 搜索 + execve。成功不返回；失败返回 -1 */
static int execve_search(int argc, char **argv, char **envp)
{
    (void)argc;
    char path[512];
    const char *p = env_path(envp);
    if (!p) p = PATH_DEFAULT;
    size_t d = 0;

    path[0] = '\0';
    if (!argv || !argv[0]) return -1;

    /* 已是路径（含 /）：直接 execve */
    if (aether_base_name(argv[0]) != argv[0]) {
        long r = sys_execve(argv[0], argv, envp);
        return AETHER_IS_ERR(r) ? -1 : 0;
    }

    /* 依次尝试 PATH 的每个冒号分段 */
    for (;;) {
        char ch = *p;
        if (ch == ':' || ch == '\0') {
            path[d] = '\0';
            if (path[0] != '\0') {
                char full[512];
                str_copy(full, sizeof(full), path);
                str_cat(full, sizeof(full), "/");
                str_cat(full, sizeof(full), argv[0]);
                long r = sys_execve(full, argv, envp);
                if (!AETHER_IS_ERR(r)) return 0; /* execve 成功不会返回 */
            }
            if (ch == '\0') break;
            p++;
            d = 0;
            continue;
        }
        if (d + 1 < sizeof(path)) path[d++] = ch;
        p++;
    }
    return -1;
}

/* 供 shell.c 调用：exec 的 PATH 搜索语义（成功不返回） */
int shell_exec_search(int argc, char **argv, char **envp)
{
    return execve_search(argc, argv, envp);
}

/* ---------- 内置命令实现 ---------- */

static int builtin_true(int argc, char **argv, char **envp) { (void)argc; (void)argv; (void)envp; return 0; }
static int builtin_false(int argc, char **argv, char **envp) { (void)argc; (void)argv; (void)envp; return 1; }

static int builtin_pwd(int argc, char **argv, char **envp)
{
    char buf[1024];
    long r = sys_getcwd(buf, sizeof(buf));
    (void)argc; (void)argv; (void)envp;
    if (AETHER_IS_ERR(r)) {
        elog_err("pwd: getcwd failed (%d)", AETHER_ERR_CODE(r));
        return 1;
    }
    elog_info("%s", buf);
    return 0;
}

static int builtin_cd(int argc, char **argv, char **envp)
{
    const char *dir = (argc >= 2) ? argv[1] : 0;
    if (!dir) {
        /* 不存在环境时退到 / */
        if (!envp) { dir = "/"; goto out; }
        for (int i = 0; envp[i]; i++) {
            const char *e = envp[i];
            if (e[0] == 'H' && e[1] == 'O' && e[2] == 'M' && e[3] == 'E' && e[4] == '=') {
                dir = e + 5;
                goto out;
            }
        }
        dir = "/";
    }
out:
    {
        long r = sys_chdir(dir);
        if (AETHER_IS_ERR(r)) {
            elog_err("cd: %s: %d", dir, AETHER_ERR_CODE(r));
            return 1;
        }
        return 0;
    }
}

static int builtin_exit(int argc, char **argv, char **envp)
{
    unsigned long long code = 0;
    (void)envp;
    if (argc >= 2) {
        if (parse_num(argv[1], &code) != 0) {
            elog_err("exit: %s: numeric argument required", argv[1]);
            aether_exit(2);
        }
    }
    aether_exit((int)(code & 0xff));
}

static int builtin_exec(int argc, char **argv, char **envp)
{
    if (argc < 2) {
        elog_info("exec: replace shell with command");
        return 0;
    }
    /* exec 成功不返回；失败退出 127（POSIX 惯例） */
    if (execve_search(argc - 1, argv + 1, envp) != 0) {
        elog_err("exec: %s: command not found", argv[1]);
        aether_exit(127);
    }
    return 127; /* 不可达 */
}

static int builtin_echo(int argc, char **argv, char **envp)
{
    (void)envp;
    for (int i = 1; i < argc; i++) {
        if (i > 1) sys_write(1, " ", sizeof(" ") - 1);
        sys_write(1, argv[i], aether_strlen(argv[i]));
    }
    sys_write(1, "\n", sizeof("\n") - 1);
    return 0;
}

static int copy_fd(int src, int dst)
{
    char buf[4096];
    for (;;) {
        long n = sys_read(src, buf, sizeof(buf));
        if (n == 0) return 0;
        if (AETHER_IS_ERR(n)) return -1;
        size_t off = 0;
        while (off < (size_t)n) {
            long w = sys_write(dst, buf + off, (size_t)n - off);
            if (AETHER_IS_ERR(w) || w == 0) return -1;
            off += (size_t)w;
        }
    }
}

static int builtin_cat(int argc, char **argv, char **envp)
{
    int rc = 0;
    (void)envp;
    if (argc < 2) {
        copy_fd(0, 1);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];
        if (aether_streq(path, "-")) {
            if (copy_fd(0, 1) != 0) rc = 1;
            continue;
        }
        long fd = sys_open(path, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) {
            elog_err("cat: %s: %d", path, AETHER_ERR_CODE(fd));
            rc = 1;
            continue;
        }
        if (copy_fd((int)fd, 1) != 0) {
            elog_err("cat: %s: read failed", path);
            rc = 1;
        }
        sys_close((int)fd);
    }
    return rc;
}

/* ---------- [ / test ---------- */

static int file_test(const char *path, char op)
{
    switch (op) {
    case 'e': return sys_faccessat(AT_FDCWD, path, 0, 0) == 0;
    case 'r': return sys_faccessat(AT_FDCWD, path, 4, 0) == 0;
    case 'w': return sys_faccessat(AT_FDCWD, path, 2, 0) == 0;
    case 'x': return sys_faccessat(AT_FDCWD, path, 1, 0) == 0;
    case 'd': {
        long fd = sys_open(path, O_RDONLY | O_DIRECTORY, 0);
        if (AETHER_IS_ERR(fd)) return 0;
        sys_close((int)fd);
        return 1;
    }
    case 'f': {
        long fd = sys_open(path, O_RDONLY | O_DIRECTORY, 0);
        if (!AETHER_IS_ERR(fd)) { sys_close((int)fd); return 0; } /* 目录不是常规文件 */
        /* 能打开即认为存在（可读性不要求） */
        fd = sys_open(path, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) return 0;
        sys_close((int)fd);
        return 1;
    }
    case 's': {
        long fd = sys_open(path, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) return 0;
        long sz = sys_lseek((int)fd, 0, SEEK_END);
        sys_close((int)fd);
        return (!AETHER_IS_ERR(sz) && sz > 0) ? 1 : 0;
    }
    default: return -1;
    }
}

static int test_eval(int argc, char **argv) /* 不含名字与末尾 ] */
{
    if (argc == 0) return 1;
    if (argc >= 2 && argv[0][0] == '!') return !test_eval(argc - 1, argv + 1);

    if (argc == 1) {
        if (argv[0][0] == '-' && argv[0][1]) { /* 单参数以 - 开头：视为语法错误 */
            elog_err("test: missing argument after '%s'", argv[0]);
            return 2;
        }
        return argv[0][0] != '\0' ? 0 : 1;
    }
    if (argc == 2) {
        /* 一元文件测试：-e -f -d -r -w -x -s file */
        if (argv[0][0] == '-') {
            char op = argv[0][1];
            int r = file_test(argv[1], op);
            if (r >= 0) return r ? 0 : 1;
        }
        elog_err("test: unknown unary operator '%s'", argv[0]);
        return 2;
    }
    if ((argc == 3 && aether_streq(argv[1], "=")) || (argc == 3 && aether_streq(argv[1], "==")))
        return aether_strcmp(argv[0], argv[2]) == 0 ? 0 : 1;
    if (argc == 3 && aether_streq(argv[1], "!="))
        return aether_strcmp(argv[0], argv[2]) != 0 ? 0 : 1;
    if (argc == 3 && argv[1][0] == '-') {
        unsigned long long a, b;
        const char *op = argv[1];
        int cmp;
        if (parse_num(argv[0], &a) != 0 || parse_num(argv[2], &b) != 0) {
            elog_err("test: integer expected");
            return 2;
        }
        if      (aether_streq(op, "-eq")) cmp = (a == b);
        else if (aether_streq(op, "-ne")) cmp = (a != b);
        else if (aether_streq(op, "-lt")) cmp = (a <  b);
        else if (aether_streq(op, "-le")) cmp = (a <= b);
        else if (aether_streq(op, "-gt")) cmp = (a >  b);
        else if (aether_streq(op, "-ge")) cmp = (a >= b);
        else { elog_err("test: unknown operator '%s'", op); return 2; }
        return cmp ? 0 : 1;
    }
    elog_err("test: too many arguments");
    return 2;
}

static int builtin_test(int argc, char **argv, char **envp)
{
    (void)envp;
    return test_eval(argc - 1, argv + 1);
}

static int builtin_bracket(int argc, char **argv, char **envp)
{
    (void)envp;
    /* 末尾必须为 ] */
    if (argc < 2 || !aether_streq(argv[argc - 1], "]")) {
        elog_err("[: missing closing ]");
        return 2;
    }
    return test_eval(argc - 2, argv + 1);
}

/* ---------- mount / umount ---------- */

static void mount_dump(void)
{
    long fd = sys_open("/proc/mounts", O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) {
        elog_err("mount: cannot read /proc/mounts: %d", AETHER_ERR_CODE(fd));
        return;
    }
    copy_fd((int)fd, 1);
    sys_close((int)fd);
}

static int builtin_mount(int argc, char **argv, char **envp)
{
    const char *fstype = 0, *source = 0, *target = 0, *opts = 0;
    unsigned long flags = 0;
    int remount = 0, request_ro = 0;
    (void)envp;

    if (argc < 2) { mount_dump(); return 0; }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-') {
            if (aether_streq(a, "-t")) {
                if (++i >= argc) { elog_err("mount: -t requires an argument"); return 2; }
                fstype = argv[i];
            } else if (aether_streq(a, "-o")) {
                if (++i >= argc) { elog_err("mount: -o requires an argument"); return 2; }
                opts = argv[i];
            } else {
                elog_err("mount: unknown option '%s'", a);
                return 2;
            }
            continue;
        }
        if (!source) source = a;
        else if (!target) target = a;
        else { elog_err("mount: too many arguments"); return 2; }
    }

    if (opts) {
        /* 逗号分隔项 */
        const char *p = opts;
        char item[32];
        for (;;) {
            size_t n = 0;
            while (*p && *p != ',' && n + 1 < sizeof(item)) item[n++] = *p++;
            item[n] = '\0';
            if      (aether_streq(item, "bind"))     flags |= MS_BIND;
            else if (aether_streq(item, "remount"))  remount = 1;
            else if (aether_streq(item, "ro"))       request_ro = 1;
            else if (aether_streq(item, "rw"))       request_ro = 0;
            else if (aether_streq(item, "nosuid"))   flags |= MS_NOSUID;
            else if (aether_streq(item, "noexec"))   flags |= MS_NOEXEC;
            else if (aether_streq(item, "nodev"))    flags |= MS_NODEV;
            else if (aether_streq(item, "noatime"))  flags |= MS_NOATIME;
            else { elog_err("mount: unknown option \"%s\"", item); return 2; }
            if (*p == '\0') break;
            p++; /* 跳过逗号 */
        }
    }

    if (remount || (flags & MS_BIND)) {
        if (!target && source) { target = source; source = 0; }
        if (!target) { elog_err("mount: remount/bind requires a target"); return 2; }
        if (remount) {
            flags |= MS_REMOUNT;
            if (request_ro) flags |= MS_RDONLY;
            else            flags |= 0; /* rw：MS_REMOUNT 无 RDONLY 即 rw */
        }
        long r = sys_mount(source ? source : "none", target, fstype ? fstype : 0, flags, 0);
        if (AETHER_IS_ERR(r)) {
            elog_err("mount: %s: %d", target, AETHER_ERR_CODE(r));
            return 1;
        }
        return 0;
    }

    if (!target) { elog_err("mount: missing target"); return 2; }
    if (!fstype) { elog_err("mount: missing -t fstype (or use -o bind/remount)"); return 2; }
    /* 只读根可能无法挂载 efivarfs 等；给出清晰错误 */
    {
        long r = sys_mount(source ? source : "none", target, fstype, flags, 0);
        if (AETHER_IS_ERR(r)) {
            elog_err("mount: %s -> %s: %d", source ? source : "(none)", target, AETHER_ERR_CODE(r));
            return 1;
        }
        return 0;
    }
}

static int builtin_umount(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("umount: usage: umount <target>"); return 2; }
    {
        long r = sys_umount2(argv[1], 0);
        if (AETHER_IS_ERR(r)) {
            elog_err("umount: %s: %d", argv[1], AETHER_ERR_CODE(r));
            return 1;
        }
        return 0;
    }
}

/* ---------- chroot ---------- */

static int bind_into(const char *dir, const char *mp, const char *src_fs, const char *src_path)
{
    char dst[512];
    if (join_path(dst, sizeof(dst), dir, mp) != 0) return 0;
    if (!dir_exists(dst)) return 0;
    sys_mount(src_path ? src_path : src_fs, dst, src_fs, 0, 0); /* 失败静默：源可能未挂载，不值得中止 */
    return 1;
}

static int builtin_chroot(int argc, char **argv, char **envp)
{
    const char *dir;
    if (argc < 2) { elog_err("chroot: usage: chroot <dir> [command...]"); return 2; }
    dir = argv[1];
    if (!dir_exists(dir)) { elog_err("chroot: %s: not a directory", dir); return 1; }

    /* 绑定 /proc /sys /dev（只读救援场景：/dev 用 bind，不带 MS_REC） */
    bind_into(dir, "proc", "proc", 0);
    bind_into(dir, "sys",  "sysfs", 0);
    bind_into(dir, "dev",  "devtmpfs", "/dev");

    long pid = sys_fork();
    if (AETHER_IS_ERR(pid)) { elog_err("chroot: fork failed"); return 1; }
    if (pid == 0) {
        /* 子进程：不会影响父（shell）的 root */
        if (sys_chdir(dir) != 0) { elog_err("chroot: chdir %s failed", dir); aether_exit(1); }
        if (sys_chroot(dir) != 0) { elog_err("chroot: chroot %s failed", dir); aether_exit(1); }
        sys_chdir("/");
        /* 注入 SYSTEMD_OFFLINE=1 */
        {
            char *new_env[64];
            int n = 0, replaced = 0;
            if (envp) {
                for (int i = 0; envp[i] && n < 63; i++) {
                    const char *e = envp[i];
                    if (has_prefix(e, "SYSTEMD_OFFLINE=")) {
                        new_env[n++] = (char *)"SYSTEMD_OFFLINE=1";
                        replaced = 1;
                    } else {
                        new_env[n++] = (char *)e;
                    }
                }
            }
            if (!replaced && n < 63) new_env[n++] = (char *)"SYSTEMD_OFFLINE=1";
            new_env[n] = 0;
            if (argc >= 3) {
                execve_search(argc - 2, argv + 2, new_env);
                elog_err("chroot: %s: not found", argv[2]);
                aether_exit(127);
            }
            {
                static const char *const sh_candidates[] = { "/bin/sh", "/bin/bash", "/sbin/init", 0 };
                for (int i = 0; sh_candidates[i]; i++) {
                    char *a[] = { (char *)"sh", 0 };
                    sys_execve(sh_candidates[i], a, new_env);
                }
                elog_err("chroot: no shell found in %s (tried /bin/sh /bin/bash /sbin/init)", dir);
                aether_exit(127);
            }
        }
    }
    /* 父进程：等孩子死，原样返回其退出码 */
    {
        int status = 0;
        sys_wait4((int)pid, &status, 0, 0);
        return (status >> 8) & 0xff;
    }
}

/* ---------- syscall 调试内置（§5.2）---------- */

static int builtin_syscall(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) {
        elog_info("usage: syscall <nr> [arg0 ... arg5]");
        elog_info("raw syscall for debugging; numbers are arch-specific. 日常修复不要用它");
        return 0;
    }
    unsigned long long nr, a[6] = {0, 0, 0, 0, 0, 0};
    if (parse_num(argv[1], &nr) != 0) { elog_err("syscall: bad number '%s'", argv[1]); return 2; }
    for (int i = 2; i < argc && i < 8; i++) {
        if (parse_num(argv[i], &a[i - 2]) != 0) { elog_err("syscall: bad argument '%s'", argv[i]); return 2; }
    }
    long r = syscall_nr((long)nr, (long)a[0], (long)a[1], (long)a[2], (long)a[3], (long)a[4], (long)a[5]);
    if (AETHER_IS_ERR(r)) elog_info("%ld (-errno %d)", r, AETHER_ERR_CODE(r));
    else                  elog_info("%ld", r);
    return 0;
}

/* ---------- switch_root ---------- */

static int builtin_switch_root(int argc, char **argv, char **envp)
{
    const char *newroot, *init;
    (void)envp;
    if (argc < 3) { elog_err("switch_root: usage: switch_root <newroot> <init>"); return 2; }
    newroot = argv[1];
    init    = argv[2];
    if (aether_streq(newroot, "/")) { elog_err("switch_root: refusing to switch_root onto /"); return 2; }
    if (!dir_exists(newroot)) { elog_err("switch_root: %s: not a directory", newroot); return 1; }

    /* init 必须在新根里存在 */
    {
        char p[512];
        size_t d = str_copy(p, sizeof(p), newroot);
        if (d > 0 && p[d - 1] == '/') p[--d] = '\0';
        str_cat(p, sizeof(p), "/");
        str_cat(p, sizeof(p), init[0] == '/' ? init + 1 : init);
        if (sys_faccessat(AT_FDCWD, p, 0, 0) != 0) {
            elog_err("switch_root: %s: init not found in new root", p);
            return 1;
        }
    }

    elog_info("switch_root 是不可逆操作：这将切换到 %s 并执行 %s，当前救援环境将丢失。",
              newroot, init);
    if (!aether_ask("Type YES to continue: ", "YES")) {
        elog_err("switch_root: aborted");
        return 1;
    }

    /* 把 /proc /sys /dev 绑进新根（若本身已挂载） */
    bind_into(newroot, "proc", "proc", 0);
    bind_into(newroot, "sys",  "sysfs", 0);
    bind_into(newroot, "dev",  "devtmpfs", "/dev");

    /* 绑定自身为可 pivot 的挂载点 */
    if (sys_mount(newroot, newroot, 0, MS_BIND | MS_REC, 0) != 0) {
        elog_err("switch_root: bind %s failed", newroot);
        return 1;
    }
    if (sys_chdir(newroot) != 0) { elog_err("switch_root: chdir %s failed", newroot); return 1; }

    /* 建暂存目录并 pivot_root */
    sys_mkdirat(AT_FDCWD, ".aether_stash", 0700);
    {
        long r = sys_pivot_root(".", ".aether_stash");
        if (AETHER_IS_ERR(r)) {
            elog_err("switch_root: pivot_root failed: %d", AETHER_ERR_CODE(r));
            return 1;
        }
    }
    /* 现在 old root 在 /.aether_stash；卸掉它 */
    sys_chdir("/");
    sys_umount2("/.aether_stash", MNT_DETACH);
    sys_unlinkat(AT_FDCWD, ".aether_stash", AT_REMOVEDIR);

    /* 执行 init */
    {
        char *child_env[] = { (char *)"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
                              (char *)"TERM=linux",
                              (char *)"SYSTEMD_OFFLINE=1",
                              0 };
        char *a[] = { (char *)init, 0 };
        sys_execve(init, a, child_env);
        elog_err("switch_root: exec %s failed", init);
        return 1;
    }
}


/* ---------- 作业控制内置：jobs / fg / bg（必须 in_process，直接操作 shell 的作业表） ---------- */

static int builtin_jobs(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    if (!shell_jobs_interactive()) { elog_err("jobs: 仅交互模式"); return 1; }
    shell_job_list_print();
    return 0;
}

static int builtin_fg(int argc, char **argv, char **envp)
{
    (void)envp;
    if (!shell_jobs_interactive()) { elog_err("fg: 仅交互模式"); return 1; }
    shell_job_t *j;
    if (argc > 1) {
        int id = (int)aether_atol(argv[1]);
        j = shell_job_find(id);
        if (!j) { elog_err("fg: 没有作业 %d", id); return 1; }
    } else {
        j = shell_job_latest();
        if (!j) { elog_err("fg: 没有作业"); return 1; }
    }
    return shell_job_continue_fg(j) == -2 ? 0 : 0;
}

static int builtin_bg(int argc, char **argv, char **envp)
{
    (void)envp;
    if (!shell_jobs_interactive()) { elog_err("bg: 仅交互模式"); return 1; }
    shell_job_t *j;
    if (argc > 1) {
        int id = (int)aether_atol(argv[1]);
        j = shell_job_find(id);
        if (!j) { elog_err("bg: 没有作业 %d", id); return 1; }
    } else {
        j = shell_job_latest();
        if (!j) { elog_err("bg: 没有作业"); return 1; }
    }
    shell_job_continue_bg(j);
    elog_info("[%d] %s &", j->id, j->cmdline);
    return 0;
}

/* ---------- 内置表（shell_builtin_t 定义见 shell_internal.h） ---------- */

static const shell_builtin_t builtins[] = {
    { ":",           builtin_true,    1, "true" },
    { "true",        builtin_true,    1, "true" },
    { "false",       builtin_false,   1, "false" },
    { "cd",          builtin_cd,      1, "切换工作目录" },
    { "exit",        builtin_exit,    1, "退出 shell" },
    { "exec",        builtin_exec,    1, "以命令替换当前进程" },
    { "echo",        builtin_echo,    0, "打印参数" },
    { "cat",         builtin_cat,     0, "输出文件内容" },
    { "pwd",         builtin_pwd,     0, "打印工作目录" },
    { "[",           builtin_bracket, 0, "test 的括号形式" },
    { "test",        builtin_test,    0, "条件测试（-e -f -d -r -w -x -s, =, !=, -eq...）" },
    { "mount",       builtin_mount,   0, "挂载（-t -o bind/remount/ro/rw...）/ 不带参数列出 /proc/mounts" },
    { "umount",      builtin_umount,  0, "卸载" },
    { "chroot",      builtin_chroot,  0, "chroot 进目标根（自动绑定 /proc /sys /dev，SYSTEMD_OFFLINE=1）" },
    { "switch_root", builtin_switch_root, 0, "切换到新根（不可逆，需 YES 确认）" },
    { "syscall",     builtin_syscall, 0, "调试：syscall <nr> [args...]（编号随架构变化）" },
    { "cmdline",     aether_cmd_cmdline, 0, "查看 /proc/cmdline" },
    { "rescue-boot", aether_cmd_rescue_boot, 0, "重试启动（可指定覆盖 cmdline，如 root=/dev/x rw）" },
    { "efi-cmdline", aether_cmd_efi_cmdline, 0, "查看/编辑 EFI 启动项的 cmdline（edit 需 root）" },
    { "jobs",        builtin_jobs,    1, "列出停止/后台作业" },
    { "fg",          builtin_fg,      1, "把作业调回前台：fg [id]" },
    { "bg",          builtin_bg,      1, "继续停止的作业在后台：bg [id]" },
};
#define N_BUILTINS (sizeof(builtins) / sizeof(builtins[0]))

const shell_builtin_t *shell_builtin_find(const char *name)
{
    size_t i;
    if (!name) return 0;
    for (i = 0; i < N_BUILTINS; i++)
        if (aether_streq(name, builtins[i].name))
            return &builtins[i];
    return 0;
}

size_t shell_builtin_count(void) { return N_BUILTINS; }
const shell_builtin_t *shell_builtin_at(size_t i) { return i < N_BUILTINS ? &builtins[i] : 0; }
