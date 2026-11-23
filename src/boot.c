/*
 * boot.c — aether 的 PID 1 / 救援 init 实现（§1 启动链兜底，§5.3 switch_root 语义）
 *
 * PID 1 流程：挂载伪文件系统 → uevent 补齐 /dev → 读 /proc/cmdline → 解析 root= →
 * resolve 根设备 → 挂载 /newroot → 绑定 proc/sys/dev → pivot_root → execve(init)。
 * 任一步失败：打印明确的 "rescue mode" 信息并进入交互拯救 shell。
 *
 * 拯救 shell builtins:cmdline / rescue-boot [override] / efi-cmdline。
 */
#include <limits.h>
#include <stddef.h>

#include "boot.h"
#include "applets.h"
#include "elog.h"
#include "mem.h"
#include "shell/shell_internal.h"
#include "syscall_shim.h"
#include "sysfsutil.h"
#include "statx.h"
#include "uevent.h"
#include "modload.h"
#include "ttyutil.h"
#include "util.h"

#define NEWROOT "/newroot"

/* 前向声明：DELETE 后台监听的收尾函数（定义在本文件下方） */
static void aether_del_watch_stop(void);
static int  aether_del_pressed(void);
static int switch_mounted_root(const char *newroot, const char *init);
static int exec_real_init(const char *init_path);
static void prepare_rescue_boot(void);
static size_t copy_str(char *dst, size_t cap, const char *src);
static volatile int g_del_rescue;
static int g_modules_loaded;

/* =============== 灾难回退：失败时自动尝试全部可用终端/串口 ===============
 * PID 1 挂载/解析/boot 任一环节失败，不能直接退（内核 panic）；
 * 也不应只依赖 /dev/console（可能缺失/被占用）。枚举常见控制台设备，
 * 对每个可打开的 O_RDWR（且能 TCGETS）的终端/串口 fork 一个救援 shell。
 * 全部都打不开则退回当前 fd（PID 1 兜底，绝不退出）。 */
#define CONSOLE_CANDIDATES_MAX 16
static const char *const console_candidates[] = {
    "/dev/console", "/dev/tty0", "/dev/tty1", "/dev/tty2", "/dev/tty3",
    "/dev/ttyS0", "/dev/ttyS1", "/dev/ttyS2", "/dev/ttyS3",
    "/dev/ttyAMA0", "/dev/ttyAMA1", "/dev/ttyUSB0", "/dev/ttyUSB1",
    "/dev/hvc0", "/dev/hvc1", "/dev/ttymxc0", 0
};

/* 尝试打开一个控制台设备；成功返回 O_RDWR fd，失败 -1 */
static int try_open_console(const char *path)
{
    long fd = sys_open(path, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
    if (AETHER_IS_ERR(fd)) return -1;
    aether_termios_t t;
    long flags = sys_fcntl((int)fd, F_GETFL, 0);
    /* 非阻塞只用于打开串口。shell 及其确认提示需要阻塞读。 */
    if (sys_ioctl((int)fd, TTY_IOC_TCGETS, (unsigned long)&t) != 0 ||
        AETHER_IS_ERR(flags) || sys_fcntl((int)fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        sys_close((int)fd);
        return -1;
    }
    return (int)fd;
}

/* init 失败回退：在全部可用终端/串口上各起一个救援 shell（PID 1 等所有子进程） */
/* 收掉 rescue 环境的全部子进程（shell 和后台守护）：SIGTERM 后逐个收割 */
static void rescue_kill_all(int *pids, int n)
{
    for (int i = 0; i < n; i++)
        if (pids[i] > 0) sys_kill(pids[i], 15 /*SIGTERM*/);
    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0) continue;
        int status = 0;
        /* ECHILD（已被等待循环回收）直接跳过 */
        (void)sys_wait4(pids[i], &status, 0, 0);
    }
}

/* AETHER_RESCUE_EXIT_BOOT（子进程 exit/rescue-boot 请求）→ PID 1 本人执行启动尝试。
 * 成功不返回（已 exec 真正的 init，init 成为 PID 1）；失败返回 -1，调用方重新进入救援。 */
static int rescue_retry_boot(void)
{
    prepare_rescue_boot();
    aether_cmdline_t opts;
    char ov[1800];
    size_t olen = 0;
    /* 1) rescue-boot 覆盖参数（init 救援 shell 里 rescue-boot args 写入此文件） */
    if (aether_read_file("/etc/.rescue-boot-override", ov, sizeof(ov) - 1, &olen) == 0 && olen > 0) {
        ov[olen] = '\0';
        sys_unlinkat(AT_FDCWD, "/etc/.rescue-boot-override", 0);
        if (aether_parse_cmdline(ov, &opts) == 0) {
            elog_info("rescue: retrying boot with override cmdline: %s", ov);
            return aether_boot_attempt(&opts);
        }
        elog_err("rescue: override cmdline parse failed, falling back to /proc/cmdline");
    }
    /* 2) /proc/cmdline，剔除 aether_rescue */
    if (aether_cmdline_file("/proc/cmdline", &opts) != 0) {
        elog_err("rescue: cannot read /proc/cmdline");
        return -1;
    }
    if (opts.aether_rescue) opts.aether_rescue = 0;
    elog_info("rescue: retrying boot (root=%s)...", opts.root[0] ? opts.root : "(not set)");
    return aether_boot_attempt(&opts);
}

typedef struct {
    char root[512];
    char init[512];
} switch_root_request_t;

static void switch_root_request_path(long pid, char path[64])
{
    char suffix[24];
    aether_u64toa(suffix, sizeof(suffix), (unsigned long)pid);
    size_t len = copy_str(path, 64, "/etc/.rescue-switch-root-");
    copy_str(path + len, 64 - len, suffix);
}

int aether_request_switch_root(const char *newroot, const char *init)
{
    switch_root_request_t req = { 0 };
    if (aether_strlen(init) >= sizeof(req.init) || aether_strlen(newroot) >= sizeof(req.root)) {
        elog_err("switch_root: path too long");
        return 1;
    }
    long pid = sys_getpid();
    if (pid == 1) return switch_mounted_root(newroot, init) != 0;
    if (newroot[0] == '/') {
        copy_str(req.root, sizeof(req.root), newroot);
    } else {
        char cwd[sizeof(req.root)];
        if (sys_getcwd(cwd, sizeof(cwd)) < 0 ||
            aether_strlen(cwd) + aether_strlen(newroot) + 2 > sizeof(req.root) ||
            aether_path_join(req.root, sizeof(req.root), cwd, newroot) != 0) {
            elog_err("switch_root: cannot resolve %s", newroot);
            return 1;
        }
    }
    copy_str(req.init, sizeof(req.init), init);
    char path[64];
    switch_root_request_path(pid, path);
    long fd = sys_open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (AETHER_IS_ERR(fd)) {
        elog_err("switch_root: cannot submit request (%d)", AETHER_ERR_CODE(fd));
        return 1;
    }
    long n = sys_write((int)fd, &req, sizeof(req));
    sys_close((int)fd);
    if (n != (long)sizeof(req)) {
        sys_unlink(path);
        elog_err("switch_root: cannot write request");
        return 1;
    }
    aether_exit(AETHER_RESCUE_EXIT_SWITCH);
}

static int rescue_switch_root(long pid)
{
    switch_root_request_t req;
    char path[64];
    size_t len = 0;
    switch_root_request_path(pid, path);
    int r = aether_read_file(path, &req, sizeof(req), &len);
    sys_unlink(path);
    if (r != 0 || len != sizeof(req) || req.root[0] != '/' ||
        req.root[sizeof(req.root) - 1] || req.init[sizeof(req.init) - 1] || !req.init[0]) {
        elog_err("switch_root: invalid request");
        return -1;
    }
    return switch_mounted_root(req.root, req.init);
}

static void rescue_consoles(void)
{
    /* 常驻救援：shell 退出码携带启动请求，由 PID 1 本人切根并继续启动；
     * 失败（或其他退出）→ 重新拉起整套救援环境。PID 1 永不退出。 */
    for (;;) {
    /* 先停掉后台 DELETE 监听并恢复控制台（rescue shell 自带 cbreak/restore） */
    aether_del_watch_stop();
    g_del_rescue = 0;
    int pids[CONSOLE_CANDIDATES_MAX * 3 + 8];
    /* 去重：/dev/console 是内核 console= 列表的别名，
     * console=ttyS0 时 /dev/console 与 /dev/ttyS0 是同一设备，
     * 都打开会导致两个 shell 抢同一输入。读内核 console 列表，只为真实控制台起 shell。 */
    const char *active_list[CONSOLE_CANDIDATES_MAX];
    char active_storage[CONSOLE_CANDIDATES_MAX][48];
    int nactive = 0;
    char active[256];
    size_t alen = 0;
    if (aether_read_file("/sys/devices/virtual/tty/console/active", active, sizeof(active) - 1, &alen) == 0) {
        active[alen] = '\0';
        const char *p = active;
        char buf[32];
        while (*p && nactive < CONSOLE_CANDIDATES_MAX) {
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            size_t t = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && t + 1 < sizeof(buf)) buf[t++] = *p++;
            if (t == 0) break;
            buf[t] = '\0';
            /* "/dev/" + token */
            char *path = active_storage[nactive];
            size_t d = 0;
            const char *pre = "/dev/";
            for (size_t k = 0; pre[k]; k++) path[d++] = pre[k];
            for (size_t k = 0; buf[k] && d + 1 < sizeof(active_storage[nactive]); k++) path[d++] = buf[k];
            path[d] = '\0';
            active_list[nactive] = path;
            nactive++;
        }
    }
    const char *const *list = (nactive > 0) ? active_list : console_candidates;
    int list_n = (nactive > 0) ? nactive : (int)(sizeof(console_candidates) / sizeof(console_candidates[0]) - 1);

    int nch = 0;
    int opened = 0;
    for (int pass = 0; pass < 2; pass++) {
        /* 第一遍：受限于 active 列表；若全部打不开，第二遍退回全列表 */
        if (pass == 1 && (nactive == 0 || opened > 0)) break;
        const char *const *pl = (pass == 1) ? console_candidates : list;
        int pn = (pass == 1) ? (int)(sizeof(console_candidates) / sizeof(console_candidates[0]) - 1) : list_n;
        for (int i = 0; i < pn; i++) {
            int fd = try_open_console(pl[i]);
            if (fd < 0) continue;
            int pid = (int)sys_fork();
            if (pid == 0) {
                /* 子进程：三个 fd 重绑到该控制台 */
                sys_dup2(fd, 0);
                sys_dup2(fd, 1);
                sys_dup2(fd, 2);
                if (fd > 2) sys_close(fd);
                elog_info("=========================================");
                elog_info(" aether rescue shell (%s)", pl[i]);
                elog_info(" possible causes: root= wrong / root device missing / mount failed");
                elog_info(" commands: rescue-boot [root=/dev/... rw] | efi-cmdline [edit] | cmdline | exit (= continue boot)");
                elog_info("=========================================");
                {
                    static char *rescue_env[] = { (char *)"AETHER_INIT=rescue", 0 };
                    shell_main(1, (char *[]){ (char *)"aether", 0 }, rescue_env);
                }
                aether_exit(0);
            }
            if (pid > 0) {
                elog_info("rescue: rescue shell started on %s (pid=%d)", pl[i], pid);
                pids[nch++] = pid;
                opened++;
            } else {
                elog_err("rescue: fork rescue shell failed (%d), skip %s", AETHER_IS_ERR(pid) ? AETHER_ERR_CODE(pid) : 0, pl[i]);
            }
            sys_close(fd);
        }
    }

    /* 救援模式顺带后台启动 uevent 热插拔守护：设备即插即用（前提：root + /dev 可写） */
    {
        int hfd = (int)sys_fork();
        if (hfd == 0) {
            aether_uevent_monitor(); /* 失败仅打一行提示，不致命 */
            aether_exit(0);
        }
        if (hfd > 0) {
            elog_info("rescue: uevent hotplug daemon started (hotplug)");
            pids[nch++] = hfd;
        }
    }

    /* 后台自动配置网络：未配置则反复重试直到成功（cmdline 含 nonet 跳过） */
    {
        int nfd = (int)sys_fork();
        if (nfd == 0) {
            net_rescue_auto_retry(0);
            aether_exit(0);
        }
        if (nfd > 0) {
            elog_info("rescue: network auto-config started (background retry until success; 'nonet' to skip)");
            pids[nch++] = nfd;
        }
    }

    if (nch == 0) {
        elog_info("rescue: no usable tty/serial found; using current stdio");
        {
            static char *rescue_env[] = { (char *)"AETHER_INIT=rescue", 0 };
            shell_main(1, (char *[]){ (char *)"aether", 0 }, rescue_env);
        }
        elog_info("init: stdio rescue shell exited -- continuing boot");
        if (rescue_retry_boot() == 0) return; /* 不可达（已 exec） */
        elog_err("rescue: retry boot failed -- restarting rescue shell");
        continue;
    }
    /* PID 1 收到启动请求后停掉其余救援子进程，再切根。 */
    int retry = 0;
    long requester = 0;
    int live = nch;
    while (live > 0 && !retry) {
        int status = 0;
        long r = sys_wait4(-1, &status, 0, 0);
        if (AETHER_IS_ERR(r)) {
            if (AETHER_ERR_CODE(r) == 4) continue; /* EINTR */
            break; /* ECHILD：没有子进程了 */
        }
        live--;
        for (int i = 0; i < nch; i++) {
            if (pids[i] == r) pids[i] = 0;
        }
        if ((status & 0x7f) == 0) {
            int code = (status >> 8) & 0xff;
            if (code == AETHER_RESCUE_EXIT_BOOT || code == AETHER_RESCUE_EXIT_SWITCH) {
                retry = code;
                requester = r;
            }
        }
    }
    rescue_kill_all(pids, nch);
    if (retry) {
        if (retry == AETHER_RESCUE_EXIT_SWITCH) rescue_switch_root(requester);
        else rescue_retry_boot();
        elog_err("rescue: retry boot failed -- back to rescue shell");
    }
    /* 全部子进程退出（或 retry 失败）→ 重新进入救援（PID 1 常驻，绝不退出/panic） */
    }
}

/* =============== cmdline 解析 =============== */

static void cmdline_zero(aether_cmdline_t *o)
{
    for (size_t i = 0; i < sizeof(*o); i++) ((char *)o)[i] = 0;
    const char *def_init = "/sbin/init";
    size_t i = 0;
    while (def_init[i] && i + 1 < sizeof(o->init)) { o->init[i] = def_init[i]; i++; }
    o->init[i] = '\0';
}

static int has_prefix(const char *s, const char *pre)
{
    size_t i = 0;
    while (pre[i] != '\0') {
        if (s[i] != pre[i]) return 0;
        i++;
    }
    return 1;
}

static size_t copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (cap == 0) return 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static int cmdline_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int aether_parse_cmdline(const char *cmdline, aether_cmdline_t *out)
{
    cmdline_zero(out);
    const char *p = cmdline;
    while (*p) {
        while (cmdline_space(*p)) p++;
        if (*p == '\0') break;
        const char *tok = p;
        int quoted = 0;
        while (*p && (quoted || !cmdline_space(*p))) {
            if (*p == '"') quoted = !quoted;
            p++;
        }
        size_t tlen = (size_t)(p - tok);
        if (*tok == '"') {
            tok++;
            tlen--;
            if (tlen && tok[tlen - 1] == '"') tlen--;
        }

        const char *eq = 0;
        for (size_t i = 0; i < tlen; i++) {
            if (tok[i] == '=') { eq = tok + i; break; }
        }
        if (!eq) {
            /* 裸键：rootwait / ro / rw / noresume */
            if (tlen == 8 && aether_strncmp(tok, "rootwait", 8) == 0) out->rootwait = 1;
            else if (tlen == 2 && aether_strncmp(tok, "ro", 2) == 0) out->ro = 1;
            else if (tlen == 2 && aether_strncmp(tok, "rw", 2) == 0) out->ro = 0;
            else if (tlen == 8 && aether_strncmp(tok, "noresume", 8) == 0) out->noresume = 1;
            else if (tlen == 13 && aether_strncmp(tok, "aether_rescue", 13) == 0) out->aether_rescue = 1;
            continue;
        }
        size_t klen = (size_t)(eq - tok), vlen = tlen - klen - 1;
        const char *val = eq + 1;
        if (vlen && val[0] == '"') {
            val++;
            vlen--;
            if (vlen && val[vlen - 1] == '"') vlen--;
        }
        if (klen == 4 && aether_strncmp(tok, "root", 4) == 0) {
            size_t n = vlen < sizeof(out->root) - 1 ? vlen : sizeof(out->root) - 1;
            for (size_t i = 0; i < n; i++) out->root[i] = val[i];
            out->root[n] = '\0';
        } else if (klen == 10 && aether_strncmp(tok, "rootfstype", 10) == 0) {
            size_t n = vlen < sizeof(out->rootfstype) - 1 ? vlen : sizeof(out->rootfstype) - 1;
            for (size_t i = 0; i < n; i++) out->rootfstype[i] = val[i];
            out->rootfstype[n] = '\0';
        } else if (klen == 9 && aether_strncmp(tok, "rootflags", 9) == 0) {
            size_t n = vlen < sizeof(out->rootflags) - 1 ? vlen : sizeof(out->rootflags) - 1;
            for (size_t i = 0; i < n; i++) out->rootflags[i] = val[i];
            out->rootflags[n] = '\0';
            /* 含 ro 项 → 只读 */
            const char *r = out->rootflags;
            while (*r) {
                const char *s = r;
                while (*r && *r != ',') r++;
                if ((size_t)(r - s) == 2 && s[0] == 'r' && s[1] == 'o') out->ro = 1;
                if (*r == ',') r++;
            }
        } else if (klen == 4 && aether_strncmp(tok, "init", 4) == 0) {
            size_t n = vlen < sizeof(out->init) - 1 ? vlen : sizeof(out->init) - 1;
            for (size_t i = 0; i < n; i++) out->init[i] = val[i];
            out->init[n] = '\0';
        } else if (klen == 6 && aether_strncmp(tok, "resume", 6) == 0) {
            size_t n = vlen < sizeof(out->resume) - 1 ? vlen : sizeof(out->resume) - 1;
            for (size_t i = 0; i < n; i++) out->resume[i] = val[i];
            out->resume[n] = '\0';
        } else if (klen == 13 && aether_strncmp(tok, "aether_rescue", 13) == 0) {
            /* aether_rescue=1/y/yes/true/on → 1；其余（0/n/f/off/false）→ 0 */
            if (vlen == 1) {
                out->aether_rescue = (val[0] != '0' && val[0] != 'n' && val[0] != 'N'
                                   && val[0] != 'f' && val[0] != 'F'
                                   && val[0] != 'o' && val[0] != 'O') ? 1 : 0;
            } else if (vlen == 2 && (aether_strncmp(val, "no", 2) == 0 || aether_strncmp(val, "NO", 2) == 0)) {
                out->aether_rescue = 0;
            } else if (vlen == 3 && (aether_strncmp(val, "off", 3) == 0 || aether_strncmp(val, "OFF", 3) == 0)) {
                out->aether_rescue = 0;
            } else {
                out->aether_rescue = 1;
            }
        } else if (klen == 13 && aether_strncmp(tok, "resume_offset", 13) == 0) {
            unsigned long long v = 0;
            for (size_t i = 0; i < vlen; i++) {
                char ch = val[i];
                if (ch < '0' || ch > '9') { v = 0; break; }
                v = v * 10 + (unsigned)(ch - '0');
            }
            out->resume_offset = v;
        }
    }
    return 0;
}

int aether_cmdline_file(const char *path, aether_cmdline_t *out)
{
    char buf[4096];
    size_t len = 0;
    if (aether_read_file(path, buf, sizeof(buf) - 1, &len) != 0) return -1;
    buf[len] = '\0';
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    return aether_parse_cmdline(buf, out);
}

/* =============== root 解析 =============== */

static int uuid_parse(const char *s, unsigned char *out, size_t cap)
{
    int ni = 0;
    size_t oi = 0;
    int nib[2] = { -1, -1 };
    for (const char *q = s; *q; q++) {
        char c = *q;
        if (c == '-') continue;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        nib[ni++] = d;
        if (ni == 2) {
            if (oi >= cap) return -1;
            out[oi++] = (unsigned char)((nib[0] << 4) | nib[1]);
            ni = 0;
        }
    }
    return (oi == cap && ni == 0) ? 0 : -1;
}

static void devpath(char *buf, size_t cap, const char *name)
{
    const char *pre = "/dev/";
    size_t d = 0;
    for (size_t i = 0; pre[i] && d + 1 < cap; i++) buf[d++] = pre[i];
    for (size_t i = 0; name[i] && d + 1 < cap; i++) buf[d++] = name[i];
    buf[d] = '\0';
}

static const char *block_uevent_value(const char *dir, const char *key, char *buf, size_t cap)
{
    char path[96];
    if (aether_path_join(path, sizeof(path), dir, "uevent") != 0) return 0;
    size_t len;
    if (aether_read_file(path, buf, cap - 1, &len) != 0) return 0;
    buf[len] = '\0';
    size_t key_len = aether_strlen(key);
    for (char *line = buf; *line;) {
        char *end = line;
        while (*end && *end != '\n') end++;
        char *next = *end ? end + 1 : end;
        *end = '\0';
        if (has_prefix(line, key)) return line + key_len;
        line = next;
    }
    return 0;
}

static unsigned int disk_le32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int disk_read_at(int fd, long offset, unsigned char *buf, size_t size)
{
    return sys_lseek(fd, offset, SEEK_SET) == offset &&
           aether_read_all(fd, (char *)buf, size) == (long)size;
}

static int gpt_uuid_matches(int fd, unsigned int part, const char *want)
{
    unsigned char expected[16], header[92], id[16];
    unsigned int sector = 0;
    if (uuid_parse(want, expected, sizeof(expected)) != 0 ||
        sys_ioctl(fd, 0x1268 /* BLKSSZGET */, (unsigned long)&sector) != 0 ||
        sector < 512 || sector > 65536 ||
        !disk_read_at(fd, sector, header, sizeof(header)) ||
        aether_strncmp((const char *)header, "EFI PART", 8) != 0)
        return 0;

    unsigned long long table = disk_le32(header + 72) | ((unsigned long long)disk_le32(header + 76) << 32);
    unsigned int count = disk_le32(header + 80), size = disk_le32(header + 84);
    if (part > count || size < 128 || size > 4096 || (size & 127) || table < 2) return 0;
    unsigned long long entry = (unsigned long long)(part - 1) * size + 16; /* unique GUID */
    if (entry > LONG_MAX - sizeof(id) || table > (LONG_MAX - sizeof(id) - entry) / sector ||
        !disk_read_at(fd, (long)(table * sector + entry), id, sizeof(id)))
        return 0;

    /* GPT GUID 的前三个字段是小端整数，其余字段按字节存储。 */
    static const unsigned char order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    for (size_t i = 0; i < sizeof(id); i++) {
        if (expected[i] != id[order[i]]) return 0;
    }
    return 1;
}

static int disk_partuuid_matches(const char *partdir, const char *want)
{
    /* 旧 Linux 的分区 uevent 有 PARTN，但没有 PARTUUID。读取父盘分区表。 */
    char uevent[512], parent[80], disk[64];
    const char *value = block_uevent_value(partdir, "PARTN=", uevent, sizeof(uevent));
    long part = value ? aether_atol(value) : 0;
    if (part <= 0 || part > 255 ||
        aether_path_join(parent, sizeof(parent), partdir, "..") != 0)
        return 0;
    value = block_uevent_value(parent, "DEVNAME=", uevent, sizeof(uevent));
    if (!value || aether_path_join(disk, sizeof(disk), "/dev", value) != 0) return 0;

    long fd = sys_open(disk, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) return 0;
    unsigned char mbr[512], expected[5];
    int match = 0;
    if (disk_read_at((int)fd, 0, mbr, sizeof(mbr)) && mbr[510] == 0x55 && mbr[511] == 0xaa) {
        int protective = 0;
        for (int i = 0; i < 4; i++) {
            if (mbr[446 + 16 * i + 4] == 0xee) protective = 1;
        }
        if (protective) {
            match = gpt_uuid_matches((int)fd, (unsigned int)part, want);
        } else if (aether_strlen(want) == 11 && want[8] == '-' &&
                   uuid_parse(want, expected, sizeof(expected)) == 0 && expected[4] == part) {
            match = expected[0] == mbr[443] && expected[1] == mbr[442] &&
                    expected[2] == mbr[441] && expected[3] == mbr[440];
        }
    }
    sys_close((int)fd);
    return match;
}

static int part_matches(const char *partname, const char *want, int uuid)
{
    char dir[64], buf[512];
    if (aether_path_join(dir, sizeof(dir), "/sys/class/block", partname) != 0) return 0;
    const char *value = block_uevent_value(dir, uuid ? "PARTUUID=" : "PARTNAME=", buf, sizeof(buf));
    if (!value) return uuid && disk_partuuid_matches(dir, want);
    if (!uuid) return aether_streq(value, want);
    while (*value && *want) {
        char a = *value++, b = *want++;
        if (a >= 'A' && a <= 'F') a += 'a' - 'A';
        if (b >= 'A' && b <= 'F') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return *value == *want;
}

static int ext4_probe(const char *devpath, unsigned char uuid_out[16], char label_out[17])
{
    char buf[2048];
    long fd = sys_open(devpath, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) return -2; /* 打不开：权限不足 */
    long n = sys_read((int)fd, buf, sizeof(buf));
    sys_close((int)fd);
    if (n < 2048) return -1; /* 太短/读失败：当作不是 ext4 */
    const unsigned char *super = (const unsigned char *)buf + 1024;
    unsigned magic = super[0x38] | (super[0x39] << 8);
    if (magic != 0xEF53) return -1;
    for (int i = 0; i < 16; i++) uuid_out[i] = super[0x68 + i];
    for (int i = 0; i < 16; i++) {
        unsigned char c = super[0x78 + i];
        label_out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    label_out[16] = '\0';
    for (int i = 15; i >= 0 && label_out[i] == ' '; i--) label_out[i] = '\0';
    return 0;
}

static int vfat_probe(const char *devpath, unsigned char vol_id_out[4], char label_out[12])
{
    char buf[512];
    long fd = sys_open(devpath, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) return -2; /* 打不开：权限不足 */
    long n = sys_read((int)fd, buf, sizeof(buf));
    sys_close((int)fd);
    if (n < 512) return -1;
    if ((unsigned char)buf[510] != 0x55 || (unsigned char)buf[511] != 0xaa) return -1;
    int fat32 = buf[22] == 0 && buf[23] == 0;
    if (aether_strncmp(buf + (fat32 ? 0x52 : 0x36), "FAT", 3) != 0) return -1;
    int serial = fat32 ? 0x43 : 0x27;
    /* UUID 显示为大端十六进制，卷序列号在磁盘上为小端。 */
    for (int i = 0; i < 4; i++) vol_id_out[i] = (unsigned char)buf[serial + 3 - i];
    int lb = serial + 4;
    for (int i = 0; i < 11; i++) label_out[i] = (buf[lb+i] >= 0x20 && buf[lb+i] < 0x7F) ? buf[lb+i] : ' ';
    label_out[11] = '\0';
    for (int i = 10; i >= 0 && label_out[i] == ' '; i--) label_out[i] = '\0';
    return 0;
}

#define BTRFS_SUPER_OFF 65536 /* 主超级块 */

/* btrfs 主超级块：magic "_BHRfS_M" @+0x40，fsid(uuid) @+0x20 */
static int btrfs_probe(const char *devpath, unsigned char uuid_out[16])
{
    char buf[4096];
    long fd = sys_open(devpath, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) return -2; /* 打不开：权限不足 */
    long off = sys_lseek((int)fd, BTRFS_SUPER_OFF, SEEK_SET);
    if (AETHER_IS_ERR(off)) { sys_close((int)fd); return -1; }
    long n = sys_read((int)fd, buf, sizeof(buf));
    sys_close((int)fd);
    if (n < 4096) return -1;
    /* magic @0x40 */
    const unsigned char magic[8] = { '_', 'B', 'H', 'R', 'f', 'S', '_', 'M' };
    for (int i = 0; i < 8; i++)
        if (buf[0x40 + i] != magic[i]) return -1;
    for (int i = 0; i < 16; i++) uuid_out[i] = (unsigned char)buf[0x20 + i];
    return 0;
}

/* 命中回调：ctx 携带期望值，返回 1 = 找到 */
typedef struct {
    const char *want;           /* 期望字符串（PARTUUID/PARTLABEL/LABEL 直接比；UUID 用 bin） */
    unsigned char bin[16];      /* UUID 形式 */
    size_t uuid_len;
    int mode;                   /* 0=partuuid, 1=partlabel, 2=uuid, 3=label */
    char out[64];               /* 命中的 /dev/<name> */
    int found;
    int ioerr;                  /* 至少一个分区 open 失败（权限不足） */
} probe_ctx_t;

static int probe_cb(const char *devpath, const char *partname, void *vctx)
{
    probe_ctx_t *ctx = vctx;
    if (ctx->mode == 0 || ctx->mode == 1) {
        if (!part_matches(partname, ctx->want, ctx->mode == 0)) return 0;
        copy_str(ctx->out, sizeof(ctx->out), devpath);
        ctx->found = 1;
        return 1;
    }

    if (ctx->mode == 2) {
        unsigned char uuid[16];
        int rc;
        if (ctx->uuid_len == 4) {
            char label[12];
            rc = vfat_probe(devpath, uuid, label);
        } else {
            char label[17];
            rc = ext4_probe(devpath, uuid, label);
            if (rc == -1) rc = btrfs_probe(devpath, uuid);
        }
        if (rc == -2) ctx->ioerr = 1;
        if (rc != 0) return 0;
        for (size_t i = 0; i < ctx->uuid_len; i++) {
            if (uuid[i] != ctx->bin[i]) return 0;
        }
        copy_str(ctx->out, sizeof(ctx->out), devpath);
        ctx->found = 1;
        return 1;
    }

    if (ctx->mode == 3) {
        /* LABEL：ext4 volume name 或 vfat label */
        unsigned char uuid[16];
        char label[17];
        if (ext4_probe(devpath, uuid, label) == 0) {
            if (aether_streq(label, ctx->want)) { copy_str(ctx->out, sizeof(ctx->out), devpath); ctx->found = 1; return 1; }
        }
        unsigned char vol[4];
        char vlabel[12];
        if (vfat_probe(devpath, vol, vlabel) == 0) {
            if (aether_streq(vlabel, ctx->want)) { copy_str(ctx->out, sizeof(ctx->out), devpath); ctx->found = 1; return 1; }
        }
        return 0;
    }
    return 0;
}

static int foreach_partition(int (*cb)(const char *, const char *, void *), void *ctx)
{
    char buf[8192];
    size_t len = 0;
    if (aether_read_file("/proc/partitions", buf, sizeof(buf) - 1, &len) != 0) return -1;
    buf[len] = '\0';
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        size_t llen = (size_t)(p - ls);
        if (p < end) p++;
        size_t i = 0;
        while (i < llen && (ls[i] == ' ' || ls[i] == '\t')) i++;
        if (i >= llen || ls[i] < '0' || ls[i] > '9') continue;
        const char *tok[4] = {0};
        int nt = 0;
        size_t k = i;
        while (k < llen && nt < 4) {
            while (k < llen && (ls[k] == ' ' || ls[k] == '\t')) k++;
            if (k >= llen) break;
            tok[nt] = ls + k;
            while (k < llen && ls[k] != ' ' && ls[k] != '\t') k++;
            nt++;
        }
        if (nt < 4) continue;
        size_t nlen = llen - (size_t)(tok[3] - ls);
        char name[32];
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        for (size_t j = 0; j < nlen; j++) name[j] = tok[3][j];
        name[nlen] = '\0';
        char dp[64];
        devpath(dp, sizeof(dp), name);
        if (cb(dp, name, ctx)) return 1;
    }
    return 0;
}

/* swap_probe 定义在后面（resume 段），此处前置声明 */
static int swap_probe(const char *devpath, unsigned char uuid_out[16]);

/* =============== /dev/disk/by-uuid 映射 ===============
 * initramfs 里没有 udev：不建 by-uuid 链接的话，UUID= 找swap/root 只能靠 aether 自己扫签名；
 * 用户/脚本按常规路径（/dev/disk/by-uuid/<uuid>）引用设备的地方一样用不了。
 * 在 uevent 排空之后、解析 root 之前，扫描全部块设备签名并创建链接。
 * 需要 root（init 阶段天然 root）；失败仅警告。 */

static void uuid_bytes_to_str(const unsigned char b[16], char out[37])
{
    static const char hex[] = "0123456789abcdef";
    int n = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-';
        out[n++] = hex[(b[i] >> 4) & 0xf];
        out[n++] = hex[b[i] & 0xf];
    }
    out[n] = '\0';
}

static void vfat_uuid_to_str(const unsigned char v[4], char out[10])
{
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (i == 2) out[n++] = '-';
        out[n++] = hex[(v[i] >> 4) & 0xf];
        out[n++] = hex[v[i] & 0xf];
    }
    out[n] = '\0';
}

/* 创建 /dev/disk/by-uuid/<uuid> -> ../../<name> */
static int byuuid_link(const char *name, const char *uuidstr, int *created)
{
    char linkpath[128];
    size_t d = 0;
    const char *pre = "/dev/disk/by-uuid/";
    for (size_t i = 0; pre[i] && d + 1 < sizeof(linkpath); i++) linkpath[d++] = pre[i];
    for (size_t i = 0; uuidstr[i] && d + 1 < sizeof(linkpath); i++) linkpath[d++] = uuidstr[i];
    linkpath[d] = '\0';

    char target[64];
    size_t t = 0;
    const char *tp = "../../";
    for (size_t i = 0; tp[i] && t + 1 < sizeof(target); i++) target[t++] = tp[i];
    for (size_t i = 0; name[i] && t + 1 < sizeof(target); i++) target[t++] = name[i];
    target[t] = '\0';

    sys_unlinkat(AT_FDCWD, linkpath, 0); /* 已存在则覆盖 */
    long r = sys_symlink(target, linkpath);
    if (AETHER_IS_ERR(r)) return -1;
    (*created)++;
    return 0;
}

typedef struct { int created; } byuuid_ctx_t;

static int byuuid_ext4_cb(const char *devpath, const char *name, void *vctx)
{
    unsigned char uuid[16]; char label[17];
    if (ext4_probe(devpath, uuid, label) != 0) return 0;
    char s[37]; uuid_bytes_to_str(uuid, s);
    byuuid_link(name, s, &((byuuid_ctx_t *)vctx)->created);
    return 0;
}
static int byuuid_vfat_cb(const char *devpath, const char *name, void *vctx)
{
    unsigned char vol[4]; char vlabel[12];
    if (vfat_probe(devpath, vol, vlabel) != 0) return 0;
    char s[10]; vfat_uuid_to_str(vol, s);
    byuuid_link(name, s, &((byuuid_ctx_t *)vctx)->created);
    return 0;
}
static int byuuid_btrfs_cb(const char *devpath, const char *name, void *vctx)
{
    unsigned char uuid[16];
    if (btrfs_probe(devpath, uuid) != 0) return 0;
    char s[37]; uuid_bytes_to_str(uuid, s);
    byuuid_link(name, s, &((byuuid_ctx_t *)vctx)->created);
    return 0;
}
static int byuuid_swap_cb(const char *devpath, const char *name, void *vctx)
{
    unsigned char uuid[16];
    if (swap_probe(devpath, uuid) != 0) return 0;
    char s[37]; uuid_bytes_to_str(uuid, s);
    byuuid_link(name, s, &((byuuid_ctx_t *)vctx)->created);
    return 0;
}

static void aether_populate_by_uuid(void)
{
    /* 目录结构 /dev/disk/by-uuid */
    sys_mkdirat(AT_FDCWD, "/dev/disk", 0755);
    sys_mkdirat(AT_FDCWD, "/dev/disk/by-uuid", 0755);
    byuuid_ctx_t ctx = { 0 };
    foreach_partition(byuuid_ext4_cb, &ctx);
    foreach_partition(byuuid_vfat_cb, &ctx);
    foreach_partition(byuuid_btrfs_cb, &ctx);
    foreach_partition(byuuid_swap_cb, &ctx);
    elog_info("aether: /dev/disk/by-uuid populated (%d link%s)", ctx.created, ctx.created == 1 ? "" : "s");
}

static void prepare_rescue_boot(void)
{
    /* DELETE 可能发生在模块加载或建立 UUID 链接之前。 */
    if (!g_modules_loaded) {
        aether_load_modules();
        g_modules_loaded = 1;
    }
    aether_populate_by_uuid();
}

/* -2 表示设备暂未找到，rootwait 可以重试。 */
static int resolve_root(const char *spec, char *out, size_t cap, int quiet)
{
    if (!spec || !*spec) { if (!quiet) elog_err("root= is empty"); return -1; }

    if (has_prefix(spec, "PARTUUID=") || has_prefix(spec, "partuuid=")) {
        probe_ctx_t ctx; ctx.want = spec + 9; ctx.mode = 0; ctx.found = 0; ctx.ioerr = 0;
        if (foreach_partition(probe_cb, &ctx) > 0 && ctx.found) {
            copy_str(out, cap, ctx.out);
            return 0;
        }
        if (!quiet) elog_err("root=%s: no partition with that PARTUUID%s", spec, ctx.ioerr ? " (some partitions unreadable, need root)" : "");
        return -2;
    }
    if (has_prefix(spec, "PARTLABEL=") || has_prefix(spec, "partlabel=")) {
        probe_ctx_t ctx; ctx.want = spec + 10; ctx.mode = 1; ctx.found = 0; ctx.ioerr = 0;
        if (foreach_partition(probe_cb, &ctx) > 0 && ctx.found) {
            copy_str(out, cap, ctx.out);
            return 0;
        }
        if (!quiet) elog_err("root=%s: no partition with that PARTLABEL%s", spec, ctx.ioerr ? " (some partitions unreadable, need root)" : "");
        return -2;
    }
    if (has_prefix(spec, "UUID=") || has_prefix(spec, "uuid=")) {
        probe_ctx_t ctx; ctx.mode = 2; ctx.found = 0; ctx.ioerr = 0;
        const char *v = spec + 5;
        ctx.uuid_len = aether_strlen(v) == 9 && v[4] == '-' ? 4 : 16;
        if (uuid_parse(v, ctx.bin, ctx.uuid_len) != 0) {
            if (!quiet) elog_err("root=UUID=%s: bad format", v);
            return -1;
        }
        if (foreach_partition(probe_cb, &ctx) > 0 && ctx.found) {
            copy_str(out, cap, ctx.out);
            return 0;
        }
        if (!quiet) elog_err("root=%s: no partition with that UUID (ext4/vfat/btrfs only)%s", spec, ctx.ioerr ? " (some partitions unreadable, need root)" : "");
        return -2;
    }
    if (has_prefix(spec, "LABEL=") || has_prefix(spec, "label=")) {
        probe_ctx_t ctx; ctx.want = spec + 6; ctx.mode = 3; ctx.found = 0; ctx.ioerr = 0;
        if (foreach_partition(probe_cb, &ctx) > 0 && ctx.found) {
            copy_str(out, cap, ctx.out);
            return 0;
        }
        if (!quiet) elog_err("root=%s: no partition with that LABEL (ext4/vfat only)%s", spec, ctx.ioerr ? " (some partitions unreadable, need root)" : "");
        return -2;
    }
    if (spec[0] == '/') {
        long fd = sys_open(spec, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) {
            if (!quiet) elog_err("root=%s: does not exist or not accessible (%d)", spec, AETHER_ERR_CODE(fd));
            return fd == -2 || fd == -6 || fd == -19 ? -2 : -1; /* ENOENT / ENXIO / ENODEV */
        }
        sys_close((int)fd);
        copy_str(out, cap, spec);
        return 0;
    }
    if (!quiet) elog_err("root=%s: cannot parse (supported: /dev/*, PARTUUID=, PARTLABEL=, UUID=, LABEL=)", spec);
    return -1;
}

int aether_resolve_root(const char *spec, char *out, size_t cap)
{
    return resolve_root(spec, out, cap, 0);
}

/* =============== 休眠恢复（resume=） =============== */
/* 内核只认 /sys/power/resume 里的 major:minor（或 lateinit 时读过一次 cmdline）。
 * 本函数把用户给的 resume= spec 解析成 major:minor 并写回 sysfs——
 * 与 mkinitcpio 的 resume 钩子等价，但在 aether PID 1 内实现。 */

/* swap 魔数在页内 offset 4086："SWAPSPACE2"(v1/v5) 或 "SWAP-SPACE"(v0)；
 * swap UUID 在 1036 处（v1_2 头：1024 bootbits + 4 version + 4 last_page + 4 nr_badpages = 1036） */
static int swap_probe(const char *devpath, unsigned char uuid_out[16])
{
    long fd = sys_open(devpath, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) return -1;
    unsigned char buf[4096];
    long r = sys_read((int)fd, buf, sizeof(buf));
    sys_close((int)fd);
    if (AETHER_IS_ERR(r) || (size_t)r < sizeof(buf)) return -1;
    const char *magic = (const char *)buf + 4086;
    /* 识别三种签名：未休眠 swap（SWAPSPACE2 / SWAP-SPACE）以及休眠中 swap（S1SUSPEND——
     * 内核休眠时会覆盖掉 swap 签名写入休眠镜像头，见 kernel/power/swap.c）。
     * 休眠中的 swap 必须能被识别，否则 resume 目标设备定位失败、by-uuid 链接也建不出来。 */
    if (aether_strncmp(magic, "SWAPSPACE2", 10) != 0 &&
        aether_strncmp(magic, "SWAP-SPACE", 10) != 0 &&
        aether_strncmp(magic, "S1SUSPEND", 9) != 0)
        return -1;
    for (int i = 0; i < 16; i++) uuid_out[i] = buf[1036 + i];
    return 0;
}

typedef struct {
    const char *want;
    unsigned char want_uuid[16];
    char out[64];
    int found;
    int ioerr;
} resume_ctx_t;

static int resume_probe_cb(const char *devpath, const char *name, void *vctx)
{
    resume_ctx_t *ctx = (resume_ctx_t *)vctx;
    (void)name;
    unsigned char uuid[16];
    if (swap_probe(devpath, uuid) != 0) return 0;
    for (int i = 0; i < 16; i++) {
        unsigned char a = uuid[i], b = ctx->want_uuid[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    copy_str(ctx->out, sizeof(ctx->out), devpath);
    ctx->found = 1;
    return 1;
}

/* 从 /sys/class/block/<name>/dev 读 "major:minor"；成功返回 0 */
static int block_dev_majmin(const char *devpath, char *out, size_t cap)
{
    const char *name = devpath;
    for (const char *p = devpath; *p; p++) if (*p == '/') name = p + 1;
    if (*name == '\0') return -1;
    char path[96];
    size_t d = 0;
    const char *pre = "/sys/class/block/";
    for (size_t i = 0; pre[i] && d + 1 < sizeof(path); i++) path[d++] = pre[i];
    for (size_t i = 0; name[i] && d + 1 < sizeof(path); i++) path[d++] = name[i];
    const char *suf = "/dev";
    for (size_t i = 0; suf[i] && d + 1 < sizeof(path); i++) path[d++] = suf[i];
    path[d] = '\0';
    char buf[32];
    size_t len = 0;
    if (aether_read_file(path, buf, sizeof(buf) - 1, &len) != 0) {
        /* /sys/class/block/<name> 对分区/ dm-N 也直接存在（symlink） */
        return -1;
    }
    buf[len] = '\0';
    size_t o = 0;
    while (o < len && buf[o] != '\n' && buf[o] != '\r' && o + 1 < cap) { out[o] = buf[o]; o++; }
    out[o] = '\0';
    if (out[0] == '\0') return -1;
    return 0;
}

/* spec → major:minor 字符串。返回 0 成功；-1 失败（含日志）。 */
static int aether_resolve_resume(const char *spec, char *majmin, size_t majmin_cap,
                                 char *devpath_out, size_t devpath_cap)
{
    if (!spec || !*spec) return -1;
    if (devpath_out && devpath_cap) devpath_out[0] = '\0';

    /* 直接 major:minor（如 "259:0"）：原样透传 */
    {
        const char *p = spec;
        int saw_digit = 0;
        while (*p) {
            if (*p >= '0' && *p <= '9') { saw_digit = 1; p++; continue; }
            if (*p == ':') { p++; continue; }
            break;
        }
        if (saw_digit && *p == '\0') { copy_str(majmin, majmin_cap, spec); return 0; }
    }

    char dev[64];
    if (has_prefix(spec, "PARTUUID=") || has_prefix(spec, "partuuid=") ||
        has_prefix(spec, "PARTLABEL=") || has_prefix(spec, "partlabel=")) {
        /* 分区 UUID/Label：走 sysfs，不依赖文件系统魔数 */
        if (aether_resolve_root(spec, dev, sizeof(dev)) != 0) {
            elog_err("resume: %s: parse failed", spec);
            return -1;
        }
    } else if (has_prefix(spec, "UUID=") || has_prefix(spec, "uuid=")) {
        unsigned char want[16];
        if (uuid_parse(spec + 5, want, sizeof(want)) != 0) { elog_err("resume: invalid UUID '%s'", spec + 5); return -1; }
        /* 优先 /dev/disk/by-uuid/<uuid>(由 aether_populate_by_uuid 创建)——
         * 最可靠：不依赖本函数的签名探测（v1 swap 可识别；mkswap --swap-uuid 变体等边角情况也兜得住） */
        char linkpath[160];
        size_t d = 0;
        const char *pre = "/dev/disk/by-uuid/";
        const char *u = spec + 5;
        for (size_t i = 0; pre[i] && d + 1 < sizeof(linkpath); i++) linkpath[d++] = pre[i];
        for (size_t i = 0; u[i] && d + 1 < sizeof(linkpath); i++) linkpath[d++] = (u[i] >= 'A' && u[i] <= 'Z') ? (char)(u[i] + 32) : u[i];
        linkpath[d] = '\0';
        long lfd = sys_open(linkpath, O_RDONLY, 0);
        if (!AETHER_IS_ERR(lfd)) {
            sys_close((int)lfd);
            /* readlink 出目标（"../../nvme0n1p5"）→ /dev/<basename> */
            char target[64];
            long tl = sys_readlink(linkpath, target, sizeof(target) - 1);
            if (!AETHER_IS_ERR(tl) && tl > 0) {
                target[tl] = '\0';
                const char *name = target;
                for (const char *p = target; *p; p++) if (*p == '/') name = p + 1;
                char full[64];
                size_t fd_ = 0;
                const char *pre2 = "/dev/";
                for (size_t i = 0; pre2[i] && fd_ + 1 < sizeof(full); i++) full[fd_++] = pre2[i];
                for (size_t i = 0; name[i] && fd_ + 1 < sizeof(full); i++) full[fd_++] = name[i];
                full[fd_] = '\0';
                copy_str(dev, sizeof(dev), full);
            } else {
                copy_str(dev, sizeof(dev), linkpath); /* 走不通也递给 majmin 尝试 */
            }
            elog_info("resume: using %s -> %s", linkpath, dev);
        } else {
            resume_ctx_t ctx;
            ctx.want = spec + 5;
            for (int i = 0; i < 16; i++) ctx.want_uuid[i] = want[i];
            ctx.found = 0; ctx.ioerr = 0;
            if (foreach_partition(resume_probe_cb, &ctx) > 0 && ctx.found)
                copy_str(dev, sizeof(dev), ctx.out);
            else { elog_err("resume: no swap device with that UUID"); return -1; }
        }
    } else if (spec[0] == '/') {
        long fd = sys_open(spec, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) { elog_err("resume: device %s missing or not accessible (%d)", spec, AETHER_ERR_CODE(fd)); return -1; }
        sys_close((int)fd);
        copy_str(dev, sizeof(dev), spec);
    } else {
        elog_err("resume: unsupported spec '%s' (supported: /dev/..., PARTUUID=, PARTLABEL=, UUID=, major:minor)", spec);
        return -1;
    }

    if (block_dev_majmin(dev, majmin, majmin_cap) != 0) {
        elog_err("resume: cannot read major:minor of %s (/sys/class/block not ready?)", dev);
        return -1;
    }
    if (devpath_out) copy_str(devpath_out, devpath_cap, dev);
    return 0;
}

/* 写一行（带 \n）到 sysfs 文件 */
static int sysfs_write_line(const char *path, const char *data, size_t len)
{
    long fd = sys_open(path, O_WRONLY, 0);
    if (AETHER_IS_ERR(fd)) return -1;
    char buf[64];
    size_t d = 0;
    if (len >= sizeof(buf) - 2) len = sizeof(buf) - 2;
    for (size_t i = 0; i < len; i++) buf[d++] = data[i];
    buf[d++] = '\n';
    long r = sys_write((int)fd, buf, d);
    sys_close((int)fd);
    return AETHER_IS_ERR(r) ? -1 : 0;
}

/* 解析 /etc/aether/resume 文件内容（aether-resume hook 探测后写入镜像，格式：
 *   resume=UUID=xxx            → 仅 resume=
 *   resume=/dev/XXX resume_offset=N  → 两行拆成一个 spec（一行）
 * 失败/文件不存在 → 返回 -1（不修改 opts）
 * 不使用动态内存，POSIX-ish 行行解析 */
static int apply_resume_file(const char *path, aether_cmdline_t *opts)
{
    char buf[512];
    size_t len = 0;
    if (aether_read_file(path, buf, sizeof(buf) - 1, &len) != 0) return -1;
    buf[len] = '\0';
    /* 行内空白分隔 token 逐个处理：resume=XXX resume_offset=NNN */
    char *p = buf;
    int got = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        size_t tlen = (size_t)(p - tok);
        if (tlen >= 7 && aether_strncmp(tok, "resume=", 7) == 0) {
            size_t vlen = tlen - 7;
            size_t n = vlen < sizeof(opts->resume) - 1 ? vlen : sizeof(opts->resume) - 1;
            for (size_t i = 0; i < n; i++) opts->resume[i] = tok[7 + i];
            opts->resume[n] = '\0';
            got = 1;
        } else if (tlen >= 15 && aether_strncmp(tok, "resume_offset=", 15) == 0) {
            unsigned long long v = 0;
            for (size_t i = 15; i < tlen; i++) {
                char ch = tok[i];
                if (ch < '0' || ch > '9') { v = 0; break; }
                v = v * 10 + (unsigned)(ch - '0');
            }
            opts->resume_offset = v;
        }
    }
    return got ? 0 : -1;
}

/* 在 aether_boot_attempt 最前面调用：处理 resume= / resume_offset= / noresume。
 * 内核语义：resume= 表示"尝试从休眠镜像恢复"；没找到镜像就继续正常启动。
 * cmdline 含 resume= 时优先；否则 fallback 到 /etc/aether/resume（aether-resume hook 写入的探测值）。
 * 失败仅告警，不中断 boot。 */
static void try_hibernate_resume(const aether_cmdline_t *opts_orig)
{
    aether_cmdline_t opts_storage;
    const aether_cmdline_t *opts = opts_orig;
    if (opts_orig->noresume) {
        elog_info("resume: noresume in cmdline, skipping hibernation resume");
        return;
    }
    /* 优先 cmdline；其次 /etc/aether/resume（aether-resume hook 自动探测） */
    if (opts->resume[0] == '\0') {
        opts_storage = *opts_orig;
        if (apply_resume_file("/etc/aether/resume", &opts_storage) == 0 && opts_storage.resume[0]) {
            if (opts_storage.resume_offset)
                elog_info("resume: using /etc/aether/resume (auto-detected): %s resume_offset=%llu",
                          opts_storage.resume, opts_storage.resume_offset);
            else
                elog_info("resume: using /etc/aether/resume (auto-detected): %s", opts_storage.resume);
            opts = &opts_storage;
        }
    }
    if (opts->resume[0] == '\0') return;

    /* /sys/power/resume 存在？（需要 CONFIG_HIBERNATION；sysfs 已在 PID 1 step 1 挂载） */
    long fd = sys_open("/sys/power/resume", O_WRONLY, 0);
    if (AETHER_IS_ERR(fd)) {
        elog_info("resume: /sys/power/resume unavailable (hibernation disabled or /sys not mounted) -- skipping");
        return;
    }
    sys_close((int)fd);

    char majmin[32], devpath[64];
    if (aether_resolve_resume(opts->resume, majmin, sizeof(majmin), devpath, sizeof(devpath)) != 0) {
        elog_err("resume: could not parse '%s', hibernation resume skipped (boot continues)", opts->resume);
        return;
    }
    /* 写入 resume 会立即触发恢复，必须先设置 swapfile 的偏移。 */
    if (opts->resume_offset != 0) {
        char obuf[24];
        size_t n = aether_u64toa(obuf, sizeof(obuf), opts->resume_offset);
        if (sysfs_write_line("/sys/power/resume_offset", obuf, n) != 0) {
            elog_err("resume: write to /sys/power/resume_offset failed");
            return;
        }
        elog_info("resume: wrote /sys/power/resume_offset=%s (PAGE_SIZE units)", obuf);
    }
    if (sysfs_write_line("/sys/power/resume", majmin, aether_strlen(majmin)) != 0) {
        elog_err("resume: write to /sys/power/resume failed (permission? kernel disabled?)");
        return;
    }
    if (devpath[0])
        elog_info("resume: wrote /sys/power/resume=%s (device %s)", majmin, devpath);
    else
        elog_info("resume: wrote /sys/power/resume=%s", majmin);
}

/* =============== 挂载参数 =============== */

/* ro/rw 是 VFS 挂载选项，文件系统只接收其余 rootflags。 */
static void root_mount_data(const char *src, char *dst)
{
    char *out = dst;
    while (*src) {
        const char *item = src;
        while (*src && *src != ',') src++;
        size_t len = (size_t)(src - item);
        if (len && !(len == 2 && item[0] == 'r' && (item[1] == 'o' || item[1] == 'w'))) {
            if (out != dst) *out++ = ',';
            for (size_t i = 0; i < len; i++) *out++ = item[i];
        }
        if (*src) src++;
    }
    *out = '\0';
}

/* =============== 切根 + exec =============== */

typedef struct {
    int fd;
    unsigned long long mount_id;
    unsigned depth;
} initramfs_dir_t;

/* 清理被覆盖的旧 initramfs，跳过挂载点和符号链接目标。 */
static void remove_initramfs_entry(const char *name, unsigned char type, void *opaque)
{
    (void)type;
    initramfs_dir_t *parent = opaque;
    if (aether_streq(name, ".") || aether_streq(name, "..")) return;
    aether_statx_t st;
    if (aether_statx(parent->fd, name, AT_SYMLINK_NOFOLLOW, STATX_TYPE | STATX_MNT_ID, &st) != 0 ||
        !(st.stx_mask & STATX_MNT_ID) || st.stx_mnt_id != parent->mount_id)
        return;
    if ((st.stx_mode & 0170000) != 0040000) {
        sys_unlinkat(parent->fd, name, 0);
        return;
    }
    if (parent->depth == 64) return;
    long fd = sys_openat(parent->fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    if (AETHER_IS_ERR(fd)) return;
    if (aether_statx((int)fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &st) == 0 &&
        (st.stx_mask & STATX_MNT_ID) && st.stx_mnt_id == parent->mount_id) {
        initramfs_dir_t child = { (int)fd, parent->mount_id, parent->depth + 1 };
        aether_foreach_dirent_fd(child.fd, remove_initramfs_entry, &child);
        sys_unlinkat(parent->fd, name, AT_REMOVEDIR);
    }
    sys_close((int)fd);
}

static int move_initramfs_root(void)
{
    /* 只为内核启动的 PID 1 覆盖旧 rootfs。普通救援 shell 不使用此回退。 */
    if (sys_getpid() != 1 || (aether_is_mounted("/", "rootfs") != 1 &&
                             aether_is_mounted("/", "ramfs") != 1 &&
                             aether_is_mounted("/", "tmpfs") != 1))
        return -1;

    long oldfd = sys_open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (AETHER_IS_ERR(oldfd)) return -1;
    aether_statx_t oldst, newst;
    int reclaim = aether_statx((int)oldfd, "", AT_EMPTY_PATH, STATX_MNT_ID, &oldst) == 0 &&
                  (oldst.stx_mask & STATX_MNT_ID) &&
                  aether_statx(AT_FDCWD, ".", 0, STATX_TYPE, &newst) == 0 &&
                  (oldst.stx_dev_major != newst.stx_dev_major || oldst.stx_dev_minor != newst.stx_dev_minor);
    long r = sys_mount(".", "/", 0, MS_MOVE, 0);
    if (r == 0) r = sys_chroot(".");
    if (r == 0) r = sys_chdir("/");
    if (r == 0 && reclaim) {
        initramfs_dir_t oldroot = { (int)oldfd, oldst.stx_mnt_id, 0 };
        aether_foreach_dirent_fd(oldroot.fd, remove_initramfs_entry, &oldroot);
    } else if (r != 0) {
        elog_err("boot: moving new root failed (%d)", AETHER_ERR_CODE(r));
    }
    sys_close((int)oldfd);
    return r == 0 ? 0 : -1;
}

int aether_switch_root(const char *newroot)
{
    if (sys_chdir(newroot) != 0) {
        elog_err("switch_root: chdir %s failed", newroot);
        return -1;
    }
    /* 可用时把旧根叠在新根之上并卸载，无需在只读新根创建目录。 */
    long r = sys_pivot_root(".", ".");
    if (r == 0) {
        r = sys_umount2(".", MNT_DETACH);
        if (AETHER_IS_ERR(r)) elog_err("switch_root: detach old root failed (%d)", AETHER_ERR_CODE(r));
        return sys_chdir("/") == 0 ? 0 : -1;
    }
    /* 旧内核的初始 rootfs 无法 pivot_root。 */
    if (r == -22 /* EINVAL */ && move_initramfs_root() == 0) return 0;
    elog_err("switch_root: pivot_root failed (%d)", AETHER_ERR_CODE(r));
    sys_chdir("/");
    return -1;
}

static int do_final_switch(const char *newroot)
{
    /* 先恢复控制台，再确认监听结束前是否收到 DELETE。 */
    aether_del_watch_stop();
    if (aether_del_pressed()) return -1;

    if (sys_getpid() == 1 && sys_mount(0, "/", 0, MS_REC | MS_PRIVATE, 0) != 0) {
        elog_err("boot: cannot make initramfs mounts private");
        return -1;
    }

    /* 绑定已有伪文件系统，保留早期创建设备、UUID 链接和 efivarfs 等子挂载。 */
    static const char *const mounts[] = { "/proc", "/sys", "/dev" };
    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        char path[520];
        aether_path_join(path, sizeof(path), newroot, mounts[i] + 1);
        if (sys_mount(mounts[i], path, 0, MS_BIND | MS_REC, 0) != 0)
            elog_err("boot: warning: bind %s failed", mounts[i]);
    }
    return aether_switch_root(newroot);
}

static int switch_mounted_root(const char *newroot, const char *init)
{
    /* PID 1 可以切到普通目录上的 bind mount，但不允许把当前根再次切成根。 */
    aether_statx_t previous, next;
    if (aether_statx(AT_FDCWD, "/", 0, STATX_INO, &previous) != 0 ||
        aether_statx(AT_FDCWD, newroot, 0, STATX_INO, &next) != 0 ||
        (previous.stx_dev_major == next.stx_dev_major &&
         previous.stx_dev_minor == next.stx_dev_minor && previous.stx_ino == next.stx_ino)) {
        elog_err("switch_root: invalid new root %s", newroot);
        return -1;
    }
    if (!aether_init_is_executable(newroot, init)) {
        elog_err("switch_root: %s: init not executable in new root", init);
        return -1;
    }
    if (sys_mount(newroot, newroot, 0, MS_BIND | MS_REC, 0) != 0) {
        elog_err("switch_root: bind %s failed", newroot);
        return -1;
    }
    prepare_rescue_boot();
    if (do_final_switch(newroot) == 0) return exec_real_init(init);
    sys_umount2(newroot, MNT_DETACH);
    return -1;
}

static int exec_real_init(const char *init_path)
{
    /* exec init：/sbin/init 或 init= */
    char *child_env[] = {
        (char *)"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
        (char *)"HOME=/root",
        (char *)"TERM=linux",
        (char *)"LANG=C",
        0
    };
    char *a[] = { (char *)init_path, 0 };
    long er = sys_execve(init_path, a, child_env);
    elog_err("boot: exec %s failed (%d)", init_path, AETHER_IS_ERR(er) ? AETHER_ERR_CODE(er) : 0);

    /* 兜底：/bin/sh */
    char *a2[] = { (char *)"sh", 0 };
    er = sys_execve("/bin/sh", a2, child_env);
    elog_err("boot: exec /bin/sh also failed (%d) -- system cannot start", er);
    return -1;
}

int aether_init_is_executable(const char *root, const char *init)
{
    long pid = sys_fork();
    if (AETHER_IS_ERR(pid)) return 0;
    if (pid == 0) {
        if (sys_chroot(root) != 0 || sys_chdir("/") != 0) aether_exit(1);
        aether_statx_t st;
        if (aether_statx(AT_FDCWD, init, 0, STATX_TYPE, &st) != 0 ||
            (st.stx_mode & 0170000) != 0100000)
            aether_exit(1);
        aether_exit(sys_faccessat(AT_FDCWD, init, 1 /* X_OK */, 0) == 0 ? 0 : 1);
    }
    int status = 0;
    long r;
    do { r = sys_wait4((int)pid, &status, 0, 0); } while (r == -4 /* EINTR */);
    return r == pid && status == 0;
}

int aether_boot_attempt(const aether_cmdline_t *opts)
{
    char dev[64];
    /* 休眠恢复：必须在任何挂载动作之前触发（noresume 跳过；找不到镜像内核忽略） */
    try_hibernate_resume(opts);
    /* 设备路径和各类标识都要重新解析，驱动可能在开始解析后才注册设备。 */
    int resolved = resolve_root(opts->root, dev, sizeof(dev), opts->rootwait);
    if (opts->rootwait && resolved == -2) {
        elog_info("boot: waiting for root=%s", opts->root);
        for (int i = 0; i < 300 && resolved == -2; i++) {
            if (aether_del_pressed()) {
                elog_info("boot: DELETE pressed at boot -- aborting rootwait, entering rescue");
                return -1;
            }
            struct { long sec; long nsec; } ts = { 0, 100L * 1000L * 1000L };
            sys_nanosleep(&ts, 0);
            resolved = resolve_root(opts->root, dev, sizeof(dev), 1);
        }
        if (resolved == 0) aether_populate_by_uuid();
    }
    if (resolved != 0 && (!opts->rootwait || aether_resolve_root(opts->root, dev, sizeof(dev)) != 0))
        return -1;

    sys_mkdirat(AT_FDCWD, NEWROOT, 0755);
    unsigned long flags = opts->ro ? MS_RDONLY : 0;
    char mount_data[sizeof(opts->rootflags)];
    root_mount_data(opts->rootflags, mount_data);

    int mounted = 0;
    if (opts->rootfstype[0]) {
        if (sys_mount(dev, NEWROOT, opts->rootfstype, flags, mount_data) == 0) mounted = 1;
        else elog_err("boot: mount %s fstype=%s failed", dev, opts->rootfstype);
    } else {
        static const char *const fstypes[] = { "ext4", "ext2", "xfs", "btrfs", "f2fs", "reiserfs", "vfat", 0 };
        for (int i = 0; !mounted && fstypes[i]; i++) {
            if (sys_mount(dev, NEWROOT, fstypes[i], flags, mount_data) == 0) mounted = 1;
        }
        if (!mounted)
            elog_err("boot: cannot mount %s (tried ext4/ext2/xfs/btrfs/f2fs/reiserfs/vfat; specify rootfstype=)", dev);
    }
    if (!mounted) return -1;

    /* 提前确认 init 存在 */
    if (opts->init[0] != '/' || !aether_init_is_executable(NEWROOT, opts->init)) {
        elog_err("boot: init not executable in new root (%s)", opts->init);
    } else if (!aether_del_pressed() && do_final_switch(NEWROOT) == 0) {
        return exec_real_init(opts->init);
    }
    /* 切根前的失败可重试，卸载本次挂载的根和其子挂载。 */
    sys_umount2(NEWROOT, MNT_DETACH);
    return -1;
}

/* =============== 后台 DELETE 监听（PID 1 直到 exec 真正 init 前） =============
 * 设计（2026-11-15 重写）：不打扰 boot 的时间线——boot 期间并行 fork 一个
 * watcher 子进程持续监听，一旦看到 DELETE/Backspace 就通知 PID 1；PID 1 在
 * 下一个检查点（modload / uevent / cmdline / boot_attempt / exec 前）中止
 * boot 进入 rescue。启动零额外等待，boot 时间与监听完全并行。
 *
 * 为什么是 fork：aether 是单线程 nolibc 程序；fork 一个轻量 child 常驻监听
 * 最简单、与救援流程解耦（~2K 页 + 最多 32 个 evdev fd + 1 个控制台 fd）。
 *
 * 控制台 raw 的"泄漏"：raw 是 tty 级属性，按进程拿不到 per-fd raw。watcher
 * 只把**输入侧**切 raw（ICANON|ECHO|IEXTEN|ISIG 关闭），保留 OPOST——内核
 * printk 输出不会变成阶梯缩进。watcher 退出前负责把 termios 恢复成原样；
 * PID 1 的两条结束监听路径：
 *   - rescue_consoles() 入口：停 watcher（恢复控制台；进救援 shell）
 *   - do_final_switch() execve 之前：停 watcher（真正的 init 接管前恢复）
 * 停掉之后按 DEL 只是普通按键。
 *
 * 两个通道（与旧实现同构）：
 *   A  evdev（真键盘，PS/2/USB）：/dev/input/eventN，KEY_DELETE(111)/
 *      KEY_BACKSPACE(14) 的按下事件
 *   B  console raw（串口 ttyS0 / VT）：0x7f 与 ESC[3~
 */
static long g_del_watcher = -1;      /* >0 = watcher 子进程 pid */
static volatile int g_del_child_stop = 0;

static void del_rescue_handler(int sig) { (void)sig; g_del_rescue = 1; }
static void del_child_stop_handler(int sig) { (void)sig; g_del_child_stop = 1; }

/* evdev 常量：struct input_event = timeval + u16 type + u16 code + s32 value；
 * timeval 在 x86_64/aarch64/riscv64 = 16 字节 → sizeof(event)=24；i386 = 8 → 16 */
#define AETHER_EV_KEY        0x01
#define AETHER_KEY_BACKSPACE 14
#define AETHER_KEY_DELETE    111
#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv) || defined(__LP64__)
#define AETHER_INPUT_EVENT_SIZE 24u
#else
#define AETHER_INPUT_EVENT_SIZE 16u
#endif

/* 扫描并打开当前存在的 evdev 节点 /dev/input/eventN（N=0..31）。
 * 已打开的（opened_mask bit i）跳过。返回新打开的 evdev fd 数。 */
#define AETHER_EV_SCAN_MAX 32
static int aether_evscan(int evfds[], size_t capacity, int *nev, unsigned *opened_mask)
{
    static const char prefix[] = "/dev/input/event";
    int added = 0;
    for (int idx = 0; idx < AETHER_EV_SCAN_MAX && (size_t)*nev < capacity; idx++) {
        if (*opened_mask & (1u << idx)) continue;
        char path[32];
        int p = 0;
        while (prefix[p]) { path[p] = prefix[p]; p++; }
        unsigned v = (unsigned)idx;
        char num[3];
        int ni = 0;
        if (v == 0) num[ni++] = '0';
        while (v) { num[ni++] = (char)('0' + (v % 10)); v /= 10; }
        while (ni > 0) path[p++] = num[--ni];
        path[p] = '\0';
        long fd = sys_open(path, O_RDONLY | O_NONBLOCK, 0);
        if (AETHER_IS_ERR(fd)) continue;
        evfds[*nev] = (int)fd;
        (*nev)++;
        *opened_mask |= (1u << idx);
        added++;
    }
    return added;
}

static int aether_del_watcher_child(void)
{
    aether_signal(SIGTERM, del_child_stop_handler);
    aether_signal(SIGINT,  del_child_stop_handler);
    aether_signal(SIGUSR1, SIG_IGN);

    long ppid = sys_getppid();

    /* 通道 A：evdev（真键盘）。
     * 注意：USB HID 键盘可能比 PID 1 晚枚举出来——所以 evdev fd 不只在启动时打开一次，
     * 而是整个窗口内周期性重扫 event0..31，发现新节点就加入监听。 */
    int evfds[64];
    const size_t ev_capacity = sizeof(evfds) / sizeof(evfds[0]);
    int nev = 0;
    unsigned opened_mask = 0; /* bit i = /dev/input/eventN 已打开 */
    for (size_t idx = 0; idx < ev_capacity; idx++) evfds[idx] = -1;

    aether_evscan(evfds, ev_capacity, &nev, &opened_mask);

    /* 通道 B：console 输入侧 raw（OPOST 保留） */
    int fd = aether_tty_open_rw();
    int own_fd = (fd != 0 && fd != 1 && fd != 2) ? fd : 0;
    int have_tty = 0;
    aether_tty_t t;
    if (fd >= 0 && tty_save(fd, &t) == 0) {
        aether_termios_t tios = t.saved_tios;
        tios.c_lflag &= ~(unsigned)(TTY_ICANON | TTY_ECHO | TTY_IEXTEN | TTY_ISIG);
        tios.c_iflag &= ~(unsigned)(TTY_IXON);
        tios.c_cc[TTY_VMIN]  = 0;
        tios.c_cc[TTY_VTIME] = 0;
        have_tty = (sys_ioctl(fd, TTY_IOC_TCSETS, (unsigned long)&tios) == 0);
    }

    if (!have_tty && nev == 0) {
        elog_err("aether: DELETE watch unavailable (no console and no evdev device)");
        return 0;
    }

    int found = 0;
    unsigned char buf[64];
    int scan_ms = 0;              /* 距上次 evdev 重扫的毫秒数；满 100ms 再扫 */
    while (!found && !g_del_child_stop) {
        if (scan_ms >= 100) {
            aether_evscan(evfds, ev_capacity, &nev, &opened_mask);
            scan_ms = 0;
        }
        if (have_tty) {
            long n = sys_read(fd, buf, sizeof(buf));
            if (n > 0) {
                for (long k = 0; k < n; k++) {
                    if (buf[k] == 0x7f) { found = 1; break; }
                    if (k + 3 < n && buf[k] == 0x1b && buf[k+1] == '['
                        && buf[k+2] == '3' && buf[k+3] == '~') { found = 1; break; }
                }
            }
        }
        for (int j = 0; j < nev && !found; j++) {
            long n = sys_read(evfds[j], buf, sizeof(buf));
            if (n <= 0) continue; /* EAGAIN / EOF */
            for (long off = 0; off + AETHER_INPUT_EVENT_SIZE <= n; off += AETHER_INPUT_EVENT_SIZE) {
                const unsigned char *e = buf + off;
                unsigned short type, code;
                unsigned int value;
                if (AETHER_INPUT_EVENT_SIZE == 24u) {
                    type  = (unsigned short)(e[16] | (e[17] << 8));
                    code  = (unsigned short)(e[18] | (e[19] << 8));
                    value = (unsigned int)e[20] | ((unsigned int)e[21] << 8)
                          | ((unsigned int)e[22] << 16) | ((unsigned int)e[23] << 24);
                } else {
                    type  = (unsigned short)(e[8] | (e[9] << 8));
                    code  = (unsigned short)(e[10] | (e[11] << 8));
                    value = (unsigned int)e[12] | ((unsigned int)e[13] << 8)
                          | ((unsigned int)e[14] << 16) | ((unsigned int)e[15] << 24);
                }
                if (type == AETHER_EV_KEY
                    && (code == AETHER_KEY_DELETE || code == AETHER_KEY_BACKSPACE)
                    && value == 1)
                    found = 1;
            }
        }
        if (!found) {
            struct { long sec; long nsec; } ts = { 0, 5UL * 1000UL * 1000UL };
            sys_nanosleep(&ts, 0);
            scan_ms += 5;
        }
    }

    if (found && ppid > 0) sys_kill((int)ppid, SIGUSR1);

    /* 恢复控制台 → canonical（供后续 rescue 或真正的 init 使用） */
    if (have_tty) tty_restore(fd, &t);
    if (own_fd) sys_close(fd);
    for (int j = 0; j < nev; j++) sys_close(evfds[j]);
    return 0;
}

/* fork 并启动 watcher（非阻塞；失败仅告警，不影响 boot） */
static void aether_del_watch_start(void)
{
    if (g_del_watcher > 0) return;
    aether_signal(SIGUSR1, del_rescue_handler);
    long pid = sys_fork();
    if (AETHER_IS_ERR(pid)) {
        elog_err("aether: DELETE watch fork failed -- this boot does not support key interrupt (boot continues)");
        return;
    }
    if (pid == 0) {
        aether_del_watcher_child();
        aether_exit(0);
    }
    g_del_watcher = pid;
    elog_info("aether: DELETE background watch started (press during boot to enter rescue mode, adds no boot time)");
}

/* 停掉 watcher（SIGTERM → wait4 → 恢复控制台）。幂等。 */
static void aether_del_watch_stop(void)
{
    long pid = g_del_watcher;
    if (pid <= 0) return;
    g_del_watcher = -1;
    sys_kill((int)pid, SIGTERM);
    int st = 0;
    while (sys_wait4((int)pid, &st, 0, 0) == -4 /* EINTR */) {}
}

/* PID 1 检查点：DEL 已按下 → 停监听并进救援（rescue_consoles 内部会停掉
 * watcher 并恢复控制台） */
static int aether_del_pressed(void)
{
    return g_del_rescue ? 1 : 0;
}

static int aether_del_rescue_hit(void)
{
    if (!g_del_rescue) return 0;
    elog_info("");
    elog_info("=========================================");
    elog_info(" manual rescue mode (DELETE pressed during boot)");
    elog_info(" type rescue-boot / cmdline / efi-cmdline to retry booting");
    elog_info("=========================================");
    elog_info("");
    rescue_consoles();
    return 0;
}

int aether_init_main(char **envp)
{
    (void)envp;
    /* 1. 挂载 proc/sys/dev（尽快拿到 /dev/ttyS0 等控制台，以便随后探测按键） */
    sys_mkdirat(AT_FDCWD, "/proc", 0755);
    sys_mkdirat(AT_FDCWD, "/sys", 0755);
    sys_mkdirat(AT_FDCWD, "/dev", 0755);
    sys_mkdirat(AT_FDCWD, "/etc", 0755);
    sys_mount("proc", "/proc", "proc", 0, 0);
    sys_mount("sysfs", "/sys", "sysfs", 0, 0);
    sys_mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    /* efivars 尽力（只读） */
    {
        char dir[128];
        if (aether_efivars_dir(dir, sizeof(dir)) == 0)
            aether_ensure_efivarfs(0);
    }

    /* 1.6 DELETE 后台监听：fork watcher（阻塞时间≈0），boot 全程并行监听 */
    aether_del_watch_start();

    /* 1.7 DELETE 检查点 1：boot 前按键即进 rescue */
    if (aether_del_rescue_hit()) return 0; /* 不可达 */

    /* 1.5 按依赖加载镜像内模块（对应引用 mkinitcpio 的 udev 钩子） */
    aether_load_modules();
    g_modules_loaded = 1;

    /* 1.8 DELETE 检查点 2：模块加载期间按键即进 rescue */
    if (aether_del_rescue_hit()) return 0; /* 不可达 */

    /* 2. uevent 顶上 devtmpfs 缺口 */
    aether_uevent_pump(1500);

    /* 2.5 填充 /dev/disk/by-uuid（initramfs 无 udev；resume/root=UUID= 与用户脚本都靠它） */
    aether_populate_by_uuid();

    /* 1.9 DELETE 检查点 3：uevent 期间按键即进 rescue */
    if (aether_del_rescue_hit()) return 0; /* 不可达 */

    /* 3. 读 cmdline */
    aether_cmdline_t opts;
    if (aether_cmdline_file("/proc/cmdline", &opts) != 0) {
        elog_info("aether: cannot read /proc/cmdline");
        rescue_consoles();
        return 0; /* 不可达 */
    }

    /* 1.10 DELETE 检查点 4：boot 前最后一眼 */
    if (aether_del_rescue_hit()) return 0; /* 不可达 */

    /* 1.11 aether_rescue 标记：确定性进入救援（不 mount root、不 exec init，直接 rescue） */
    if (opts.aether_rescue) {
        elog_info("");
        elog_info("=========================================");
        elog_info(" FORCED RESCUE MODE (cmdline token: aether_rescue)");
        elog_info("  type rescue-boot / cmdline / efi-cmdline / exit (= continue boot)");
        elog_info("=========================================");
        elog_info("");
        rescue_consoles();
        return 0; /* 不可达 */
    }

    /* 4. boot */
    if (aether_boot_attempt(&opts) == 0)
        return 0; /* 不会到这里（已 exec init） */

    /* 5. 失败：回退到全部可用终端/串口 */
    elog_info("");
    elog_info("=========================================");
    elog_info(" BOOT FAILED -- entering rescue mode");
    elog_info(" possible causes: root= wrong / root device missing / mount failed");
    elog_info(" you can edit the EFI boot entry cmdline and retry:");
    elog_info("   rescue-boot                  # retry with current cmdline");
    elog_info("   rescue-boot root=/dev/... rw # retry with specified args");
    elog_info("   efi-cmdline                  # show EFI boot entry cmdline");
    elog_info("   efi-cmdline edit             # edit and write back EFI entry (needs root)");
    elog_info("   cmdline                      # show /proc/cmdline");
    elog_info("=========================================");
    elog_info("");
    rescue_consoles();
    return 0; /* 不可达 */
}

/* =============== 拯救命令：cmdline =============== */

int aether_cmd_cmdline(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;
    if (argc > 1) { elog_err("cmdline: usage: cmdline"); return 2; }
    char buf[4096];
    size_t len = 0;
    if (aether_read_file("/proc/cmdline", buf, sizeof(buf) - 1, &len) != 0) {
        elog_err("cmdline: cannot read /proc/cmdline");
        return 1;
    }
    buf[len] = '\0';
    sys_write(1, buf, len);
    return 0;
}

/* =============== rescue-boot =============== */

int aether_cmd_rescue_boot(int argc, char **argv, char **envp)
{
    (void)envp;
    aether_cmdline_t opts;
    static char cmdline_buf[2048];

    if (argc > 1) {
        /* 把 argv[1..] 用空格连成覆盖参数 */
        size_t d = 0;
        for (int i = 1; i < argc; i++) {
            size_t k = 0;
            if (i > 1 && d + 1 < sizeof(cmdline_buf)) cmdline_buf[d++] = ' ';
            while (argv[i][k] && d + 1 < sizeof(cmdline_buf)) cmdline_buf[d++] = argv[i][k++];
        }
        cmdline_buf[d] = '\0';
        if (aether_parse_cmdline(cmdline_buf, &opts) != 0) return 2;
        elog_info("rescue-boot: using override cmdline: %s", cmdline_buf);
    } else {
        /* 从默认 /proc/cmdline 构造 retry 用的 opts：剔除 aether_rescue（否则无法离开强制救援） */
        if (aether_cmdline_file("/proc/cmdline", &opts) != 0) {
            elog_err("rescue-boot: cannot read /proc/cmdline");
            return 1;
        }
        if (opts.aether_rescue) {
            opts.aether_rescue = 0; /* retry 强制进正常启动流程 */
            elog_info("rescue-boot: aether_rescue in cmdline ignored (retry normal boot)");
        }
        elog_info("rescue-boot: retrying boot (root=%s)...", opts.root[0] ? opts.root : "(not set)");
    }
    if (sys_getpid() == 1) prepare_rescue_boot();
    if (aether_boot_attempt(&opts) == 0) return 0; /* 不会到这里 */
    elog_err("rescue-boot: boot failed");
    return 1;
}

/* =============== efi-cmdline：查看/编辑 EFI efistub 启动项的 cmdline =============== */

typedef struct {
    unsigned int attrs;      /* efivarfs 文件级 attrs */
    unsigned int lo_attrs;   /* LoadOption attrs */
    unsigned short path_len;
    size_t dp_off;           /* DevicePath 在文件中的字节偏移 */
    size_t od_off;           /* OptionalData 偏移 */
    size_t od_len;
    char desc[128];
    char cmdline[512];
} efistub_t;

static int utf16_to_utf8(const unsigned char *in, size_t in_len, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < in_len && o + 1 < cap; i += 2) {
        unsigned cp = (unsigned)in[i] | ((unsigned)in[i + 1] << 8);
        if (cp == 0) break;
        out[o++] = (cp < 0x80) ? (char)cp : '?';
    }
    out[o] = '\0';
    return (int)o;
}

/* 解析 efivarfs 的 BootXXXX 文件内容 */
static int efistub_parse(const unsigned char *buf, size_t len, efistub_t *out)
{
    /* 文件布局: attrs(4) + loattrs(4) + path_len(2) + desc(UTF16 NUL) + DevicePath + OptionalData */
    if (len < 4 + 4 + 2 + 2) return -1;
    out->attrs = (unsigned)buf[0] | ((unsigned)buf[1] << 8) | ((unsigned)buf[2] << 16) | ((unsigned)buf[3] << 24);
    out->lo_attrs = (unsigned)buf[4] | ((unsigned)buf[5] << 8) | ((unsigned)buf[6] << 16) | ((unsigned)buf[7] << 24);
    out->path_len = (unsigned short)((unsigned)buf[8] | ((unsigned)buf[9] << 8));
    size_t dp = 10;
    /* Description：UTF-16LE，找 NUL */
    size_t d = dp;
    while (d + 1 < len) {
        if (buf[d] == 0 && buf[d + 1] == 0) { d += 2; break; }
        d += 2;
    }
    if (d > len) return -1;
    out->dp_off = d;
    if (out->dp_off + out->path_len > len) return -1;
    out->od_off = out->dp_off + out->path_len;
    out->od_len = len - out->od_off;

    /* desc */
    size_t di = 0;
    for (size_t i = dp; i + 1 < d && di + 1 < sizeof(out->desc); i += 2) {
        unsigned cp = (unsigned)buf[i] | ((unsigned)buf[i + 1] << 8);
        if (cp == 0) break;
        out->desc[di++] = (cp < 0x80) ? (char)cp : '?';
    }
    out->desc[di] = '\0';

    /* OptionalData → cmdline */
    utf16_to_utf8(buf + out->od_off, out->od_len, out->cmdline, sizeof(out->cmdline));
    return 0;
}

/* 读 BootCurrent；失败返回 -1 */
static int efivar_read_int(const char *name, unsigned short *out)
{
    char dir[128];
    if (aether_efivars_dir(dir, sizeof(dir)) != 0) return -1;
    char path[256];
    size_t d = 0;
    for (size_t i = 0; dir[i]; i++) path[d++] = dir[i];
    const char *mid = "/";
    for (size_t i = 0; mid[i]; i++) path[d++] = mid[i];
    for (size_t i = 0; name[i]; i++) path[d++] = name[i];
    const char *suf = "-" EFIVARS_GUID;
    for (size_t i = 0; suf[i]; i++) path[d++] = suf[i];
    path[d] = '\0';

    char buf[16];
    size_t len = 0;
    if (aether_read_file(path, buf, sizeof(buf), &len) != 0) return -1;
    if (len < 6) return -1; /* attrs(4)+u16 */
    *out = (unsigned short)((unsigned char)buf[4] | ((unsigned char)buf[5] << 8));
    return 0;
}

/* 读 BootXXXX 整块 */
static int efivar_read_boot(unsigned short bootnum, unsigned char *buf, size_t cap, size_t *len_out)
{
    char dir[128];
    if (aether_efivars_dir(dir, sizeof(dir)) != 0) return -1;
    const char *hex = "0123456789abcdef";
    char num[5] = { hex[(bootnum >> 12) & 0xf], hex[(bootnum >> 8) & 0xf],
                    hex[(bootnum >> 4) & 0xf], hex[bootnum & 0xf], '\0' };
    char path[256];
    size_t d = 0;
    for (size_t i = 0; dir[i]; i++) path[d++] = dir[i];
    path[d++] = '/';
    const char *pre = "Boot";
    for (size_t i = 0; pre[i]; i++) path[d++] = pre[i];
    for (size_t i = 0; num[i]; i++) path[d++] = num[i];
    const char *suf = "-" EFIVARS_GUID;
    for (size_t i = 0; suf[i]; i++) path[d++] = suf[i];
    path[d] = '\0';
    size_t len = 0;
    if (aether_read_file(path, buf, cap, &len) != 0) return -1;
    *len_out = len;
    return 0;
}

static int efivar_write_boot(unsigned short bootnum, const unsigned char *payload, size_t len)
{
    char dir[128];
    if (aether_efivars_dir(dir, sizeof(dir)) != 0) return -1;
    const char *hex = "0123456789abcdef";
    char num[5] = { hex[(bootnum >> 12) & 0xf], hex[(bootnum >> 8) & 0xf],
                    hex[(bootnum >> 4) & 0xf], hex[bootnum & 0xf], '\0' };
    char path[256];
    size_t d = 0;
    for (size_t i = 0; dir[i]; i++) path[d++] = dir[i];
    path[d++] = '/';
    const char *pre = "Boot";
    for (size_t i = 0; pre[i]; i++) path[d++] = pre[i];
    for (size_t i = 0; num[i]; i++) path[d++] = num[i];
    const char *suf = "-" EFIVARS_GUID;
    for (size_t i = 0; suf[i]; i++) path[d++] = suf[i];
    path[d] = '\0';
    long fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (AETHER_IS_ERR(fd)) return -1;
    long w = sys_write((int)fd, payload, len);
    sys_close((int)fd);
    return AETHER_IS_ERR(w) ? -1 : 0;
}

static int efi_show_current(void)
{
    unsigned short bootcur = 0;
    if (efivar_read_int("BootCurrent", &bootcur) != 0) {
        elog_err("efi-cmdline: cannot read BootCurrent (not an EFI boot?)");
        return 1;
    }
    elog_info("BootCurrent: %04x", bootcur);

    unsigned char buf[2048];
    size_t len = 0;
    if (efivar_read_boot(bootcur, buf, sizeof(buf), &len) != 0) {
        elog_err("efi-cmdline: cannot read Boot%04x", bootcur);
        return 1;
    }
    efistub_t e;
    if (efistub_parse(buf, len, &e) != 0) {
        elog_err("efi-cmdline: cannot parse EFI boot entry Boot%04x", bootcur);
        return 1;
    }
    elog_info("entry: %s", e.desc[0] ? e.desc : "(no description)");
    if (e.cmdline[0])
        elog_info("cmdline: %s", e.cmdline);
    else
        elog_info("cmdline: (empty, or not an efistub/grub direct-boot format)");
    return 0;
}

static int efi_edit_current(char **envp)
{
    (void)envp;
    if (sys_geteuid() != 0) { elog_err("efi-cmdline edit: needs root"); return 1; }

    unsigned short bootcur = 0;
    if (efivar_read_int("BootCurrent", &bootcur) != 0) {
        elog_err("efi-cmdline: cannot read BootCurrent");
        return 1;
    }
    unsigned char buf[2048];
    size_t len = 0;
    if (efivar_read_boot(bootcur, buf, sizeof(buf), &len) != 0) {
        elog_err("efi-cmdline: cannot read Boot%04x", bootcur);
        return 1;
    }
    efistub_t e;
    if (efistub_parse(buf, len, &e) != 0) {
        elog_err("efi-cmdline: cannot parse Boot%04x", bootcur);
        return 1;
    }

    elog_info("entry: %s", e.desc[0] ? e.desc : "(no description)");
    elog_info("current cmdline: %s", e.cmdline[0] ? e.cmdline : "(empty)");
    elog_info("enter new cmdline (empty = abort):");
    elog_write(2, "> ");
    char newcmd[1024];
    size_t n = 0;
    {
        char ch;
        long r;
        for (;;) {
            r = sys_read(0, &ch, 1);
            if (r <= 0) break;
            if (ch == '\n' || ch == '\r') break;
            if (n + 1 < sizeof(newcmd)) newcmd[n++] = ch;
        }
        newcmd[n] = '\0';
    }
    if (n == 0) { elog_err("efi-cmdline: aborted"); return 1; }

    /* 组装新内容：保留 attrs/lo_attrs/desc/DevicePath，仅替换 OptionalData */
    size_t header = e.od_off;
    size_t utf16_len = 0;
    size_t cap_u16 = (n + 1) * 2 + 2;
    unsigned char payload[2048];
    if (header + cap_u16 > sizeof(payload)) { elog_err("efi-cmdline: new cmdline too long"); return 1; }

    /* 拷贝头部 */
    for (size_t i = 0; i < header; i++) payload[i] = buf[i];
    /* OptionalData = UTF16LE(newcmd) + 尾 NUL */
    size_t d = header;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)newcmd[i];
        payload[d++] = c;
        payload[d++] = 0;
    }
    payload[d++] = 0; /* 尾部 NUL */
    payload[d++] = 0;
    (void)utf16_len; (void)cap_u16;

    elog_info("old: %s", e.cmdline);
    elog_info("new: %s", newcmd);
    if (!aether_ask("write back to EFI entry? [y/N] ", "y")) {
        elog_err("efi-cmdline: aborted");
        return 1;
    }
    if (efivar_write_boot(bootcur, payload, d) != 0) {
        elog_err("efi-cmdline: write failed (efivars must be rw, need root)");
        return 1;
    }
    elog_info("EFI entry updated");

    if (aether_ask("retry boot with new cmdline now? [y/N] ", "y")) {
        /* 直接 boot，不退出当前进程 */
        aether_cmdline_t opts;
        aether_parse_cmdline(newcmd, &opts);
        if (aether_boot_attempt(&opts) != 0) {
            elog_err("efi-cmdline: retry failed (EFI entry updated, will take effect on reboot)");
            return 1;
        }
        return 0; /* 不可达 */
    }
    elog_info("saved. reboot will use the new cmdline");
    return 0;
}

int aether_cmd_efi_cmdline(int argc, char **argv, char **envp)
{
    if (argc > 1 && aether_streq(argv[1], "edit"))
        return efi_edit_current(envp);
    if (argc > 1) { elog_err("efi-cmdline: usage: efi-cmdline [show|edit]"); return 2; }
    return efi_show_current();
}
