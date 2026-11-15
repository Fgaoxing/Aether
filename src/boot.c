/*
 * boot.c — aether 的 PID 1 / 救援 init 实现（§1 启动链兜底，§5.3 switch_root 语义）
 *
 * PID 1 流程：挂载伪文件系统 → uevent 补齐 /dev → 读 /proc/cmdline → 解析 root= →
 * resolve 根设备 → 挂载 /newroot → 绑定 proc/sys/dev → pivot_root → execve(init)。
 * 任一步失败：打印明确的 "rescue mode" 信息并进入交互拯救 shell。
 *
 * 拯救 shell 内置命令：cmdline / rescue-boot [override] / efi-cmdline。
 */
#include <stddef.h>

#include "boot.h"
#include "applets.h"
#include "elog.h"
#include "mem.h"
#include "shell/shell_internal.h"
#include "syscall_shim.h"
#include "sysfsutil.h"
#include "uevent.h"
#include "modload.h"
#include "ttyutil.h"
#include "util.h"

#define NEWROOT "/newroot"
#define STASH   ".aether_stash"

/* 前向声明：DELETE 后台监听的收尾函数（定义在本文件下方） */
static void aether_del_watch_stop(void);
static int  aether_del_pressed(void);

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
    if (sys_ioctl((int)fd, TTY_IOC_TCGETS, (unsigned long)&t) != 0) {
        sys_close((int)fd);
        return -1;
    }
    return (int)fd;
}

/* init 失败回退：在全部可用终端/串口上各起一个救援 shell（PID 1 等所有子进程） */
static void rescue_consoles(void)
{
    /* 先停掉后台 DELETE 监听并恢复控制台（rescue shell 自带 cbreak/restore） */
    aether_del_watch_stop();
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
                elog_info(" 可能原因：root= 参数错误 / 根设备未出现 / 挂载失败");
                elog_info(" 命令：rescue-boot [root=/dev/... rw] | efi-cmdline [edit] | cmdline");
                elog_info("=========================================");
                shell_main(1, (char *[]){ (char *)"aether", 0 }, 0);
                aether_exit(0);
            }
            if (pid > 0) {
                elog_info("rescue: 已在 %s 启动救援 shell (pid=%d)", pl[i], pid);
                nch++;
                opened++;
            } else {
                elog_err("rescue: fork 救援 shell 失败（%d），跳过 %s", AETHER_IS_ERR(pid) ? AETHER_ERR_CODE(pid) : 0, pl[i]);
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
            elog_info("rescue: uevent 热插拔守护已启动（hotplug）");
            nch++; /* 计入 PID 1 等待集合 */
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
            elog_info("rescue: 网络自动配置已启动（后台重试直到成功；含 nonet 可跳过）");
            nch++; /* 计入 PID 1 等待集合 */
        }
    }

    if (nch == 0) {
        elog_info("rescue: 未找到可用终端/串口；使用当前标准输入/输出");
        shell_main(1, (char *[]){ (char *)"aether", 0 }, 0);
        aether_exit(0);
    }
    /* PID 1：等所有 rescue shell 结束；全部结束也绝不退出（否则 panic） */
    for (;;) {
        int status = 0;
        long r = sys_wait4(-1, &status, 0, 0);
        if (AETHER_IS_ERR(r)) {
            if (AETHER_ERR_CODE(r) == 4) continue; /* EINTR */
            break; /* ECHILD：没有子进程了 */
        }
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

int aether_parse_cmdline(const char *cmdline, aether_cmdline_t *out)
{
    cmdline_zero(out);
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        size_t tlen = (size_t)(p - tok);

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
            continue;
        }
        size_t klen = (size_t)(eq - tok), vlen = tlen - klen - 1;
        const char *val = eq + 1;
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

static int uuid_parse(const char *s, unsigned char out[16])
{
    int ni = 0, oi = 0;
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
            if (oi >= 16) return -1;
            out[oi++] = (unsigned char)((nib[0] << 4) | nib[1]);
            ni = 0;
        }
    }
    return (oi == 16 && ni == 0) ? 0 : -1;
}

static void devpath(char *buf, size_t cap, const char *name)
{
    const char *pre = "/dev/";
    size_t d = 0;
    for (size_t i = 0; pre[i] && d + 1 < cap; i++) buf[d++] = pre[i];
    for (size_t i = 0; name[i] && d + 1 < cap; i++) buf[d++] = name[i];
    buf[d] = '\0';
}

static int part_sysfs(char *buf, size_t cap, const char *partname, const char *attr)
{
    char parent[32];
    int is_part = 0;
    aether_classify_part(partname, parent, sizeof(parent), &is_part);
    const char *pre = "/sys/block/";
    size_t d = 0;
    for (size_t i = 0; pre[i] && d + 1 < cap; i++) buf[d++] = pre[i];
    for (size_t i = 0; parent[i] && d + 1 < cap; i++) buf[d++] = parent[i];
    if (d + 1 >= cap) return -1;
    buf[d++] = '/';
    for (size_t i = 0; partname[i] && d + 1 < cap; i++) buf[d++] = partname[i];
    if (d + 1 >= cap) return -1;
    buf[d++] = '/';
    for (size_t i = 0; attr[i] && d + 1 < cap; i++) buf[d++] = attr[i];
    buf[d] = '\0';
    return 0;
}

static int ext4_probe(const char *devpath, unsigned char uuid_out[16], char label_out[17])
{    char buf[2048];
    long fd = sys_open(devpath, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) return -2; /* 打不开：权限不足 */
    long n = sys_read((int)fd, buf, sizeof(buf));
    sys_close((int)fd);
    if (n < 2048) return -1; /* 太短/读失败：当作不是 ext4 */
    unsigned magic = (unsigned char)buf[1080] | ((unsigned char)buf[1081] << 8);
    if (magic != 0xEF53) return -1;
    for (int i = 0; i < 16; i++) uuid_out[i] = (unsigned char)buf[1184 + i];
    for (int i = 0; i < 16; i++) {
        unsigned char c = (unsigned char)buf[1200 + i];
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
    int fat32 = -1;
    for (int i = 0x36; i < 0x44 && i + 5 < 512; i++)
        if (buf[i] == 'F' && buf[i+1] == 'A' && buf[i+2] == 'T') { fat32 = 0; goto found; }
    for (int i = 0x52; i < 0x5A && i + 5 < 512; i++)
        if (buf[i] == 'F' && buf[i+1] == 'A' && buf[i+2] == 'T') { fat32 = 1; goto found; }
    return -1;
found:
    vol_id_out[0] = (unsigned char)buf[0x43];
    vol_id_out[1] = (unsigned char)buf[0x44];
    vol_id_out[2] = (unsigned char)buf[0x45];
    vol_id_out[3] = (unsigned char)buf[0x46];
    {
        int lb = fat32 ? 0x47 : 0x2B;
        for (int i = 0; i < 11; i++) label_out[i] = (buf[lb+i] >= 0x20 && buf[lb+i] < 0x7F) ? buf[lb+i] : ' ';
        label_out[11] = '\0';
        for (int i = 10; i >= 0 && label_out[i] == ' '; i--) label_out[i] = '\0';
    }
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
    int mode;                   /* 0=partuuid, 1=partlabel, 2=uuid, 3=label */
    char out[64];               /* 命中的 /dev/<name> */
    int found;
    int ioerr;                  /* 至少一个分区 open 失败（权限不足） */
} probe_ctx_t;

static int probe_cb(const char *devpath, const char *partname, void *vctx)
{
    probe_ctx_t *ctx = vctx;
    char path[96];

    if (ctx->mode == 0 || ctx->mode == 1) {
        /* partuuid / partlabel 在 sysfs */
        const char *attr = ctx->mode == 0 ? "partuuid" : "partlabel";
        if (part_sysfs(path, sizeof(path), partname, attr) != 0) return 0;
        char buf[96];
        size_t len = 0;
        if (aether_read_file(path, buf, sizeof(buf) - 1, &len) != 0) return 0;
        buf[len] = '\0';
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
        /* 大小写不敏感比较（uuid） */
        const char *a = buf, *b = ctx->want;
        size_t la = aether_strlen(a), lb = aether_strlen(b);
        if (la != lb) return 0;
        for (size_t i = 0; i < la; i++) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return 0;
        }
        copy_str(ctx->out, sizeof(ctx->out), devpath);
        ctx->found = 1;
        return 1;
    }

    if (ctx->mode == 2) {
        unsigned char uuid[16];
        char label[17];
        int rc = ext4_probe(devpath, uuid, label);
        if (rc == 0) {
            for (int i = 0; i < 16; i++) if (uuid[i] != ctx->bin[i]) goto try_vfat;
            copy_str(ctx->out, sizeof(ctx->out), devpath);
            ctx->found = 1;
            return 1;
        }
        if (rc == -2) ctx->ioerr = 1;
try_vfat:
        {
            unsigned char vol[4];
            char vlabel[12];
            rc = vfat_probe(devpath, vol, vlabel);
            if (rc == 0) {
                /* vfat UUID 只有 4 字节；ctx->bin 前 4 字节存 vol_id 的期望值 */
                if (ctx->bin[0] == vol[0] && ctx->bin[1] == vol[1] && ctx->bin[2] == vol[2] && ctx->bin[3] == vol[3]) {
                    copy_str(ctx->out, sizeof(ctx->out), devpath);
                    ctx->found = 1;
                    return 1;
                }
            } else if (rc == -2) ctx->ioerr = 1;
        }
        /* btrfs：fsid 与 ext4 形式相同（16 字节） */
        {
            unsigned char buuid[16];
            rc = btrfs_probe(devpath, buuid);
            if (rc == 0) {
                for (int i = 0; i < 16; i++) if (buuid[i] != ctx->bin[i]) return 0;
                copy_str(ctx->out, sizeof(ctx->out), devpath);
                ctx->found = 1;
                return 1;
            }
            if (rc == -2) ctx->ioerr = 1;
        }
        return 0;
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

int aether_resolve_root(const char *spec, char *out, size_t cap)
{
    if (!spec || !*spec) { elog_err("root= 为空"); return -1; }

    if (has_prefix(spec, "PARTUUID=") || has_prefix(spec, "partuuid=")) {
        probe_ctx_t ctx; ctx.want = spec + 9; ctx.mode = 0; ctx.found = 0; ctx.ioerr = 0;
        if (foreach_partition(probe_cb, &ctx) > 0 && ctx.found) {
            copy_str(out, cap, ctx.out);
            return 0;
        }
        elog_err("root=%s: 未找到该 PARTUUID 的分区%s", spec, ctx.ioerr ? "（部分分区无读权限，需要 root）" : "");
        return -1;
    }
    if (has_prefix(spec, "PARTLABEL=") || has_prefix(spec, "partlabel=")) {
        probe_ctx_t ctx; ctx.want = spec + 10; ctx.mode = 1; ctx.found = 0; ctx.ioerr = 0;
        if (foreach_partition(probe_cb, &ctx) > 0 && ctx.found) {
            copy_str(out, cap, ctx.out);
            return 0;
        }
        elog_err("root=%s: 未找到该 PARTLABEL 的分区%s", spec, ctx.ioerr ? "（部分分区无读权限，需要 root）" : "");
        return -1;
    }
    if (has_prefix(spec, "UUID=") || has_prefix(spec, "uuid=")) {
        probe_ctx_t ctx; ctx.mode = 2; ctx.found = 0; ctx.ioerr = 0;
        const char *v = spec + 5;
        if (has_prefix(v, "8be4") || 0) { }
        /* vfat 短式 UUID（XXXX-XXXX）：前 4 字节存 vol_id；否则按 16 字节 UUID */
        size_t vlen = aether_strlen(v);
        int dashes = 0;
        for (size_t i = 0; v[i]; i++) if (v[i] == '-') dashes++;
        if (dashes == 1 && vlen == 9) {
            /* 解析 XXXX-XXXX */
            unsigned x0 = 0, x1 = 0;
            char c;
            int ok = 1;
            for (int i = 0; i < 4 && ok; i++) {
                c = v[i];
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else { ok = 0; break; }
                x0 = (x0 << 4) | (unsigned)d;
            }
            for (int i = 5; i < 9 && ok; i++) {
                c = v[i];
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else { ok = 0; break; }
                x1 = (x1 << 4) | (unsigned)d;
            }
            if (!ok) { elog_err("root=UUID=%s: 格式错误", v); return -1; }
            ctx.bin[0] = (unsigned char)(x0 & 0xff);
            ctx.bin[1] = (unsigned char)((x0 >> 8) & 0xff);
            ctx.bin[2] = (unsigned char)(x1 & 0xff);
            ctx.bin[3] = (unsigned char)((x1 >> 8) & 0xff);
        } else {
            if (uuid_parse(v, ctx.bin) != 0) { elog_err("root=UUID=%s: 格式错误", v); return -1; }
        }
        if (foreach_partition(probe_cb, &ctx) > 0 && ctx.found) {
            copy_str(out, cap, ctx.out);
            return 0;
        }
        elog_err("root=%s: 未找到该 UUID 的分区（仅支持 ext4/vfat/btrfs）%s", spec, ctx.ioerr ? "（部分分区无读权限，需要 root）" : "");
        return -1;
    }
    if (has_prefix(spec, "LABEL=") || has_prefix(spec, "label=")) {
        probe_ctx_t ctx; ctx.want = spec + 6; ctx.mode = 3; ctx.found = 0; ctx.ioerr = 0;
        if (foreach_partition(probe_cb, &ctx) > 0 && ctx.found) {
            copy_str(out, cap, ctx.out);
            return 0;
        }
        elog_err("root=%s: 未找到该 LABEL 的分区（仅支持 ext4/vfat）%s", spec, ctx.ioerr ? "（部分分区无读权限，需要 root）" : "");
        return -1;
    }
    if (spec[0] == '/') {
        long fd = sys_open(spec, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) { elog_err("root=%s: 不存在或不可访问（%d）", spec, AETHER_ERR_CODE(fd)); return -1; }
        sys_close((int)fd);
        copy_str(out, cap, spec);
        return 0;
    }
    elog_err("root=%s: 无法解析（支持 /dev/*、PARTUUID=、PARTLABEL=、UUID=、LABEL=）", spec);
    return -1;
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
    unsigned char buf[4104];
    long r = sys_read((int)fd, buf, sizeof(buf));
    sys_close((int)fd);
    if (AETHER_IS_ERR(r) || (size_t)r < sizeof(buf)) return -1;
    const char *magic = (const char *)buf + 4086;
    if (aether_strncmp(magic, "SWAPSPACE2", 10) != 0 &&
        aether_strncmp(magic, "SWAP-SPACE", 10) != 0)
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
            elog_err("resume: %s 解析失败", spec);
            return -1;
        }
    } else if (has_prefix(spec, "UUID=") || has_prefix(spec, "uuid=")) {
        unsigned char want[16];
        if (uuid_parse(spec + 5, want) != 0) { elog_err("resume: 无效的 UUID '%s'", spec + 5); return -1; }
        resume_ctx_t ctx;
        ctx.want = spec + 5;
        for (int i = 0; i < 16; i++) ctx.want_uuid[i] = want[i];
        ctx.found = 0; ctx.ioerr = 0;
        if (foreach_partition(resume_probe_cb, &ctx) > 0 && ctx.found)
            copy_str(dev, sizeof(dev), ctx.out);
        else { elog_err("resume: 未找到含该 UUID 的 swap 设备"); return -1; }
    } else if (spec[0] == '/') {
        long fd = sys_open(spec, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) { elog_err("resume: 设备 %s 不存在或不可访问（%d）", spec, AETHER_ERR_CODE(fd)); return -1; }
        sys_close((int)fd);
        copy_str(dev, sizeof(dev), spec);
    } else {
        elog_err("resume: 不支持的 spec '%s'（支持 /dev/...、PARTUUID=、PARTLABEL=、UUID=、major:minor）", spec);
        return -1;
    }

    if (block_dev_majmin(dev, majmin, majmin_cap) != 0) {
        elog_err("resume: 无法读 %s 的 major:minor（/sys/class/block 未就绪？）", dev);
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

/* 在 aether_boot_attempt 最前面调用：处理 resume= / resume_offset= / noresume。
 * 内核语义：resume= 表示"尝试从休眠镜像恢复"；没找到镜像就继续正常启动。
 * 失败仅告警，不中断 boot。 */
static void try_hibernate_resume(const aether_cmdline_t *opts)
{
    if (opts->noresume) {
        elog_info("resume: cmdline 含 noresume，跳过休眠恢复");
        return;
    }
    if (opts->resume[0] == '\0') return;

    /* /sys/power/resume 存在？（需要 CONFIG_HIBERNATION；sysfs 已在 PID 1 step 1 挂载） */
    long fd = sys_open("/sys/power/resume", O_WRONLY, 0);
    if (AETHER_IS_ERR(fd)) {
        elog_info("resume: /sys/power/resume 不可用（内核未启用休眠或 /sys 未挂载）——跳过");
        return;
    }
    sys_close((int)fd);

    char majmin[32], devpath[64];
    if (aether_resolve_resume(opts->resume, majmin, sizeof(majmin), devpath, sizeof(devpath)) != 0) {
        elog_err("resume: 无法解析 '%s'，休眠恢复被跳过（boot 继续）", opts->resume);
        return;
    }
    if (sysfs_write_line("/sys/power/resume", majmin, aether_strlen(majmin)) != 0) {
        elog_err("resume: 写 /sys/power/resume 失败（权限？内核禁用？）");
        return;
    }
    elog_info("resume: 已写入 /sys/power/resume=%s%s%s%s", majmin,
              devpath[0] ? "（设备 " : "", devpath[0] ? devpath : "", devpath[0] ? "）" : "");
    if (opts->resume_offset != 0) {
        char obuf[24];
        size_t n = aether_u64toa(obuf, sizeof(obuf), opts->resume_offset);
        if (sysfs_write_line("/sys/power/resume_offset", obuf, n) != 0)
            elog_err("resume: 写 /sys/power/resume_offset 失败");
        else
            elog_info("resume: 已写入 /sys/power/resume_offset=%s（PAGE_SIZE 单位）", obuf);
    }
}

/* =============== 挂载参数 =============== */

/* 只解析 ro 为挂载标志；其余 rootflags（如 subvol=@）原样透传给内核 */
static int ro_flag_of(const char *s)
{
    const char *p = s;
    while (*p) {
        const char *item = p;
        while (*p && *p != ',') p++;
        size_t ilen = (size_t)(p - item);
        if (ilen == 2 && aether_strncmp(item, "ro", 2) == 0) return MS_RDONLY;
        if (*p == ',') p++;
    }
    return 0;
}

/* =============== pivot_root + exec =============== */

static int do_final_switch(const char *init_path)
{
    /* 绑定 /proc /sys /dev 进新根并 pivot */
    if (sys_mount("proc", NEWROOT "/proc", "proc", 0, 0) != 0)
        elog_err("boot: warning: mount proc 失败");
    if (sys_mount("sysfs", NEWROOT "/sys", "sysfs", 0, 0) != 0)
        elog_err("boot: warning: mount sys 失败");
    if (sys_mount("devtmpfs", NEWROOT "/dev", "devtmpfs", 0, 0) != 0)
        elog_err("boot: warning: mount dev 失败");

    sys_mkdirat(AT_FDCWD, NEWROOT "/" STASH, 0700);

    if (sys_chdir(NEWROOT) != 0) { elog_err("boot: chdir %s 失败", NEWROOT); return -1; }
    long r = sys_pivot_root(".", STASH);
    if (AETHER_IS_ERR(r)) {
        elog_err("boot: pivot_root 失败（%d）——initramfs 需要内核支持 pivot_root", AETHER_ERR_CODE(r));
        sys_chdir("/");
        return -1;
    }
    /* 停掉后台 DELETE 监听：真正的 init 接管前恢复控制台（幂等） */
    aether_del_watch_stop();
    if (aether_del_pressed()) {
        elog_info("boot: 启动期按下 DELETE——不 exec init，进入救援");
        return -1;
    }
    sys_chdir("/");
    sys_umount2("/" STASH, 2 /*MNT_DETACH*/);
    sys_unlinkat(AT_FDCWD, STASH, 0x200 /*AT_REMOVEDIR*/);

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
    elog_err("boot: exec %s 失败（%d）", init_path, AETHER_IS_ERR(er) ? AETHER_ERR_CODE(er) : 0);

    /* 兜底：/bin/sh */
    char *a2[] = { (char *)"sh", 0 };
    er = sys_execve("/bin/sh", a2, child_env);
    elog_err("boot: exec /bin/sh 也失败（%d）——系统无法启动", er);
    return -1;
}

static int check_init_exists(const char *init)
{
    /* init 路径必须在新根中存在（否则先失败，不 pivot） */
    if (init[0] == '/') {
        char p[256];
        size_t d = 0;
        const char *pre = NEWROOT;
        for (size_t i = 0; pre[i] && d + 1 < sizeof(p); i++) p[d++] = pre[i];
        for (size_t i = 0; init[i] && d + 1 < sizeof(p); i++) p[d++] = init[i];
        p[d] = '\0';
        long fd = sys_open(p, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) return -1;
        sys_close((int)fd);
        return 0;
    }
    return -1;
}

int aether_boot_attempt(const aether_cmdline_t *opts)
{
    char dev[64];
    /* 休眠恢复：必须在任何挂载动作之前触发（noresume 跳过；找不到镜像内核忽略） */
    try_hibernate_resume(opts);
    if (aether_resolve_root(opts->root, dev, sizeof(dev)) != 0)
        return -1;

    /* 等待设备出现（rootwait） */
    int tries = opts->rootwait ? 300 : 20;
    for (int i = 0;; i++) {
        long fd = sys_open(dev, O_RDONLY, 0);
        if (!AETHER_IS_ERR(fd)) { sys_close((int)fd); break; }
        if (aether_del_pressed()) {
            elog_info("boot: 启动期按下 DELETE——中止 rootwait 等待，进入救援");
            return -1;
        }
        if (i >= tries) {
            elog_err("boot: 根设备 %s 未出现（rootwait 超时）", dev);
            return -1;
        }
        struct { long sec; long nsec; } ts = { 0, 100UL * 1000UL * 1000UL }; /* 100ms */
        sys_nanosleep(&ts, 0);
    }

    /* 挂载 /newroot：ro 作为挂载标志，其余 rootflags 原样透传（btrfs subvol=@ 等） */
    sys_mkdirat(AT_FDCWD, NEWROOT, 0755);
    unsigned long flags = opts->ro ? MS_RDONLY : 0;
    if (opts->rootflags[0]) flags |= (unsigned long)ro_flag_of(opts->rootflags);
    const char *mount_data = opts->rootflags[0] ? opts->rootflags : "";

    int mounted = 0;
    if (opts->rootfstype[0]) {
        if (sys_mount(dev, NEWROOT, opts->rootfstype, flags, mount_data) == 0) mounted = 1;
        else elog_err("boot: 挂载 %s fstype=%s 失败", dev, opts->rootfstype);
    } else {
        static const char *const fstypes[] = { "ext4", "ext2", "xfs", "btrfs", "f2fs", "reiserfs", "vfat", 0 };
        for (int i = 0; !mounted && fstypes[i]; i++) {
            if (sys_mount(dev, NEWROOT, fstypes[i], flags, mount_data) == 0) mounted = 1;
        }
        if (!mounted)
            elog_err("boot: 无法挂载 %s（尝试了 ext4/ext2/xfs/btrfs/f2fs/reiserfs/vfat；请指定 rootfstype=）", dev);
    }
    if (!mounted) return -1;

    /* 提前确认 init 存在 */
    if (check_init_exists(opts->init) != 0) {
        elog_err("boot: 新根中找不到 init（%s）", opts->init);
        return -1;
    }

    /* boot_attempt 收尾检查：DEL 后不 exec（监听仍在，但下一处理会更精确） */
    if (aether_del_pressed()) return -1;

    if (do_final_switch(opts->init) != 0) return -1;
    return 0; /* 不可达（exec 成功不返回） */
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
static volatile int g_del_rescue = 0;
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

static int aether_del_watcher_child(void)
{
    aether_signal(SIGTERM, del_child_stop_handler);
    aether_signal(SIGINT,  del_child_stop_handler);
    aether_signal(SIGUSR1, SIG_IGN);

    long ppid = sys_getppid();

    /* 通道 A：evdev（真键盘） */
    int evfds[32];
    int nev = 0;
    {
        static const char prefix[] = "/dev/input/event";
        for (int idx = 0; idx < 32; idx++) {
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
            if (nev < (int)(sizeof(evfds) / sizeof(evfds[0]))) evfds[nev++] = (int)fd;
            else sys_close((int)fd);
        }
    }

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
        elog_err("aether: DELETE 监听不可用（无控制台也无 evdev 设备）");
        return 0;
    }

    int found = 0;
    unsigned char buf[64];
    while (!found && !g_del_child_stop) {
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
        elog_err("aether: DELETE 监听 fork 失败——本次启动不支持按键中止（boot 继续）");
        return;
    }
    if (pid == 0) {
        aether_del_watcher_child();
        aether_exit(0);
    }
    g_del_watcher = pid;
    elog_info("aether: DELETE 后台监听已启动（启动期按下即进入救援模式，不增加启动时间）");
}

/* 停掉 watcher（SIGTERM → wait4 → 恢复控制台）。幂等。 */
static void aether_del_watch_stop(void)
{
    long pid = g_del_watcher;
    if (pid <= 0) return;
    g_del_watcher = -1;
    sys_kill((int)pid, SIGTERM);
    int st = 0;
    sys_wait4((int)pid, &st, 0, 0);
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
    elog_info(" 手动 rescue 模式（启动期按下 DELETE）");
    elog_info(" 输入 rescue-boot / cmdline / efi-cmdline 重试启动");
    elog_info("=========================================");
    elog_info("");
    rescue_consoles();
    return 0;
}

int aether_init_main(char **envp)
{
    (void)envp;
    /* 1. 挂载 proc/sys/dev（尽快拿到 /dev/ttyS0 等控制台，以便随后探测按键） */
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

    /* 1.8 DELETE 检查点 2：模块加载期间按键即进 rescue */
    if (aether_del_rescue_hit()) return 0; /* 不可达 */

    /* 2. uevent 顶上 devtmpfs 缺口 */
    aether_uevent_pump(1500);

    /* 1.9 DELETE 检查点 3：uevent 期间按键即进 rescue */
    if (aether_del_rescue_hit()) return 0; /* 不可达 */

    /* 3. 读 cmdline */
    aether_cmdline_t opts;
    if (aether_cmdline_file("/proc/cmdline", &opts) != 0) {
        elog_info("aether: 无法读取 /proc/cmdline");
        rescue_consoles();
        return 0; /* 不可达 */
    }

    /* 1.10 DELETE 检查点 4：boot 前最后一眼 */
    if (aether_del_rescue_hit()) return 0; /* 不可达 */

    /* 4. boot */
    if (aether_boot_attempt(&opts) == 0)
        return 0; /* 不会到这里（已 exec init） */

    /* 5. 失败：回退到全部可用终端/串口 */
    elog_info("");
    elog_info("=========================================");
    elog_info(" BOOT FAILED — 进入拯救模式 (rescue mode)");
    elog_info(" 可能原因：root= 参数错误 / 根设备未出现 / 挂载失败");
    elog_info(" 你可以编辑 EFI 启动项的 cmdline 后重试：");
    elog_info("   rescue-boot                  # 用当前 cmdline 重试");
    elog_info("   rescue-boot root=/dev/... rw # 用指定参数重试");
    elog_info("   efi-cmdline                  # 查看 EFI 启动项的 cmdline");
    elog_info("   efi-cmdline edit             # 编辑并回写 EFI 启动项（需 root）");
    elog_info("   cmdline                      # 查看 /proc/cmdline");
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
        elog_err("cmdline: 无法读 /proc/cmdline");
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
        elog_info("rescue-boot: 使用覆盖 cmdline: %s", cmdline_buf);
    } else {
        if (aether_cmdline_file("/proc/cmdline", &opts) != 0) {
            elog_err("rescue-boot: 无法读 /proc/cmdline");
            return 1;
        }
        elog_info("rescue-boot: 重试启动（root=%s）...", opts.root[0] ? opts.root : "(未设置)");
    }
    if (aether_boot_attempt(&opts) == 0) return 0; /* 不会到这里 */
    elog_err("rescue-boot: 启动失败");
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
        elog_err("efi-cmdline: 无法读取 BootCurrent（可能不是从 EFI 启动的）");
        return 1;
    }
    elog_info("BootCurrent: %04x", bootcur);

    unsigned char buf[2048];
    size_t len = 0;
    if (efivar_read_boot(bootcur, buf, sizeof(buf), &len) != 0) {
        elog_err("efi-cmdline: 无法读取 Boot%04x", bootcur);
        return 1;
    }
    efistub_t e;
    if (efistub_parse(buf, len, &e) != 0) {
        elog_err("efi-cmdline: 无法解析 Boot%04x 的 EFI 启动项", bootcur);
        return 1;
    }
    elog_info("启动项: %s", e.desc[0] ? e.desc : "(无描述)");
    if (e.cmdline[0])
        elog_info("cmdline: %s", e.cmdline);
    else
        elog_info("cmdline: (空，或非 efistub/grub 直启格式)");
    return 0;
}

static int efi_edit_current(char **envp)
{
    (void)envp;
    if (sys_geteuid() != 0) { elog_err("efi-cmdline edit: 需要 root 权限"); return 1; }

    unsigned short bootcur = 0;
    if (efivar_read_int("BootCurrent", &bootcur) != 0) {
        elog_err("efi-cmdline: 无法读取 BootCurrent");
        return 1;
    }
    unsigned char buf[2048];
    size_t len = 0;
    if (efivar_read_boot(bootcur, buf, sizeof(buf), &len) != 0) {
        elog_err("efi-cmdline: 无法读取 Boot%04x", bootcur);
        return 1;
    }
    efistub_t e;
    if (efistub_parse(buf, len, &e) != 0) {
        elog_err("efi-cmdline: 无法解析 Boot%04x", bootcur);
        return 1;
    }

    elog_info("启动项: %s", e.desc[0] ? e.desc : "(无描述)");
    elog_info("当前 cmdline: %s", e.cmdline[0] ? e.cmdline : "(空)");
    elog_info("输入新 cmdline（原样回车 = 放弃）:");
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
    if (n == 0) { elog_err("efi-cmdline: 已放弃"); return 1; }

    /* 组装新内容：保留 attrs/lo_attrs/desc/DevicePath，仅替换 OptionalData */
    size_t header = e.od_off;
    size_t utf16_len = 0;
    size_t cap_u16 = (n + 1) * 2 + 2;
    unsigned char payload[2048];
    if (header + cap_u16 > sizeof(payload)) { elog_err("efi-cmdline: 新 cmdline 过长"); return 1; }

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

    elog_info("旧: %s", e.cmdline);
    elog_info("新: %s", newcmd);
    if (!aether_ask("写回 EFI 启动项? [y/N] ", "y")) {
        elog_err("efi-cmdline: 已放弃");
        return 1;
    }
    if (efivar_write_boot(bootcur, payload, d) != 0) {
        elog_err("efi-cmdline: 写回失败（efivars 需 rw 且 root）");
        return 1;
    }
    elog_info("EFI 启动项已更新");

    if (aether_ask("用新 cmdline 立即重试启动? [y/N] ", "y")) {
        /* 直接 boot，不退出当前进程 */
        aether_cmdline_t opts;
        aether_parse_cmdline(newcmd, &opts);
        if (aether_boot_attempt(&opts) != 0) {
            elog_err("efi-cmdline: 重试启动失败（原 EFI 项已更新，重启后即生效）");
            return 1;
        }
        return 0; /* 不可达 */
    }
    elog_info("已保存。重启即按新 cmdline 启动");
    return 0;
}

int aether_cmd_efi_cmdline(int argc, char **argv, char **envp)
{
    if (argc > 1 && aether_streq(argv[1], "edit"))
        return efi_edit_current(envp);
    if (argc > 1) { elog_err("efi-cmdline: usage: efi-cmdline [show|edit]"); return 2; }
    return efi_show_current();
}
