/*
 * uevent.c — 内核 KOBJECT_UEVENT 处理（init 补 devtmpfs 缺口 + hotplug 热插拔）
 * 与 boot.c 里原来的 inline 实现一致，并扩展为常驻监听模式：
 *   add/change → 按 MAJOR/MINOR/DEVTYPE mknod /dev/<DEVNAME>（已存在则跳过）
 *   remove     → unlink /dev/<DEVNAME>
 * 单用户 rescue 语义：节点 0660，root 所有。
 */
#include <stddef.h>

#include "uevent.h"
#include "elog.h"
#include "syscall_shim.h"
#include "sysfsutil.h"
#include "util.h"

#define AF_NETLINK 16
#define NETLINK_KOBJECT_UEVENT 15
#if !defined(S_IFBLK)
#define S_IFBLK 0060000
#endif
#if !defined(S_IFCHR)
#define S_IFCHR 0020000
#endif
#define SOCK_RAW_NB (3 | 0x800) /* SOCK_RAW | SOCK_NONBLOCK */

typedef struct { unsigned short family, pad; int pid; unsigned groups; } aether_nladdr_t;

static unsigned long long makedev64(unsigned maj, unsigned min)
{
    return ((unsigned long long)(min & 0xff))
         | (((unsigned long long)maj & 0xff) << 8)
         | (((unsigned long long)min & ~0xffULL) << 12)
         | (((unsigned long long)maj & ~0xffULL) << 32);
}

static int uevent_one(int fd)
{
    char buf[1024];
    long n = sys_recvfrom(fd, buf, sizeof(buf) - 1, 0, 0, 0);
    if (AETHER_IS_ERR(n) || n == 0)
        return (AETHER_IS_ERR(n) && AETHER_ERR_CODE(n) == 11 /*EAGAIN*/) ? 0 : -1;
    buf[n] = '\0';

    const char *p = buf;
    /* 头：<seq>@<path> 或 kernel@<path>，以第一个 NUL 结束 */
    const char *end = buf + n;
    const char *msg = p;
    while (msg < end && *msg) msg++;
    if (msg >= end) return 0;
    msg++; /* 跳过 NUL */

    const char *action = 0, *devname = 0, *devtype = 0;
    unsigned maj = 0, min = 0;
    while (msg < end && *msg) {
        const char *line = msg;
        while (msg < end && *msg) msg++;
        if (msg >= end) break;
        msg++; /* NUL */

        const char *eq = line;
        while (eq < msg - 1 && *eq != '=') eq++;
        if (*eq != '=') continue;
        size_t klen = (size_t)(eq - line);
        const char *val = eq + 1;
        size_t vlen = (size_t)((msg - 1) - val);

        if (klen == 6 && aether_strncmp(line, "ACTION", 6) == 0) {
            action = val;
        } else if (klen == 5 && aether_strncmp(line, "MAJOR", 5) == 0) {
            maj = 0; for (size_t i = 0; i < vlen; i++) { maj = maj * 10 + (unsigned)(val[i] - '0'); }
        } else if (klen == 5 && aether_strncmp(line, "MINOR", 5) == 0) {
            min = 0; for (size_t i = 0; i < vlen; i++) { min = min * 10 + (unsigned)(val[i] - '0'); }
        } else if (klen == 7 && aether_strncmp(line, "DEVNAME", 7) == 0) {
            devname = val;
        } else if (klen == 7 && aether_strncmp(line, "DEVTYPE", 7) == 0) {
            devtype = val;
        }
    }

    if (!devname || !devname[0]) return 0;
    /* 防路径穿越：DEVNAME 只允许 [A-Za-z0-9._-] 风格，不允许 '/' 与前导点 */
    for (const char *c = devname; *c; c++) if (*c == '/' || *c == '.') return 0;

    char path[96];
    size_t d = 0;
    const char *pre = "/dev/";
    for (size_t i = 0; pre[i]; i++) path[d++] = pre[i];
    for (size_t i = 0; devname[i]; i++) path[d++] = devname[i];
    path[d] = '\0';

    int is_add = action && (aether_strncmp(action, "add", 3) == 0 || aether_strncmp(action, "change", 6) == 0);
    int is_remove = action && aether_strncmp(action, "remove", 6) == 0;

    if (is_add) {
        unsigned type = S_IFCHR;
        if (devtype && (aether_strncmp(devtype, "disk", 4) == 0 || aether_strncmp(devtype, "partition", 9) == 0))
            type = S_IFBLK;
        /* 已存在则跳过 */
        long fd = sys_open(path, O_RDONLY | O_NONBLOCK, 0);
        if (!AETHER_IS_ERR(fd)) { sys_close((int)fd); return 0; }
        if (sys_mknodat(AT_FDCWD, path, type | 0660, makedev64(maj, min)) == 0)
            elog_info("uevent: + %s (%u:%u)", path, maj, min);
        else
            elog_err("uevent: mknod %s failed (%d)", path, AETHER_ERR_CODE(sys_mknodat(AT_FDCWD, path, type | 0660, makedev64(maj, min))));
        return 0;
    }
    if (is_remove) {
        long r = sys_unlink(path);
        if (!AETHER_IS_ERR(r))
            elog_info("uevent: - %s", path);
        /* 节点本就不存在则静默 */
        return 0;
    }
    return 0; /* bind/move/online/offline 等：忽略 */
}

static int uevent_bind(void)
{
    int fd = (int)sys_socket(AF_NETLINK, SOCK_RAW_NB, NETLINK_KOBJECT_UEVENT);
    if (AETHER_IS_ERR(fd)) return -1;
    aether_nladdr_t sa;
    sa.family = AF_NETLINK;
    sa.pad = 0;
    sa.pid = 0;
    sa.groups = 1; /* KOBJECT_UEVENT 组 */
    long r = sys_bind(fd, &sa, sizeof(sa));
    if (AETHER_IS_ERR(r)) { sys_close(fd); return -1; }
    return fd;
}

int aether_uevent_pump(int max_ms)
{
    int fd = uevent_bind();
    if (fd < 0) return -1;

    int elapsed = 0;
    int count = 0;
    while (elapsed < max_ms && count < 512) {
        int rc = uevent_one(fd);
        if (rc == 0) break;      /* 队列空（EAGAIN）或出错：结束 */
        count++;
    }
    sys_close(fd);
    return 0;
}

static volatile int uevent_monitor_stop;
static void uevent_monitor_handler(int sig) { (void)sig; uevent_monitor_stop = 1; }

int aether_uevent_monitor(void)
{
    int fd = uevent_bind();
    if (fd < 0) {
        elog_err("hotplug: cannot open netlink uevent socket (need root/CAP_SYS_ADMIN, or kernel uevent disabled)");
        return 1;
    }
    aether_signal(SIGINT, uevent_monitor_handler);
    aether_signal(SIGTERM, uevent_monitor_handler);
    elog_info("hotplug: listening for kernel uevents (Ctrl-C to exit)...");
    while (!uevent_monitor_stop) {
        /* SOCK_NONBLOCK：非零时间片内收完所有事件即再次轮询 */
        int rc = uevent_one(fd);
        if (rc == 0) {
            struct { long sec; long nsec; } ts = { 0, 100UL * 1000UL * 1000UL }; /* 100ms */
            sys_nanosleep(&ts, 0);
        }
    }
    sys_close(fd);
    elog_info("hotplug: stopped");
    return 0;
}
