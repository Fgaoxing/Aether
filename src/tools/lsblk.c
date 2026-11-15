/*
 * lsblk.c — 块设备树形总览（§12.5，disks 合并版）
 * 输出：NAME MAJ:MIN RM SIZE RO TYPE MOUNTPOINT ESP；父盘/分区树状显示（├─ └─）。
 * ESP 列：探测 FAT 引导扇区签名（0x55 0xAA @510/511 且 0x20..0x3F 有 "FAT" 字样）→ 标记 "FAT(ESP?)"。
 * 结尾附 efivarfs 挂载状态与 BootOrder（救援高频信息，原 disks 功能）。
 * 数据源：/proc/partitions、/proc/mounts、sysfs（ro/removable）、efivarfs。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "syscall_shim.h"
#include "sysfsutil.h"
#include "util.h"

#define MAX_ENTRIES 64

typedef struct {
    char name[32];
    char parent[32];
    unsigned major, minor;
    unsigned long long kb;
    int is_part;
    int ro, rm;
    char mountpoint[128];  /* "" 未挂载 */
    int is_swap;
    int esp;               /* -1=探测失败/无权限, 0=否, 1=FAT 签名命中（分区） */
} lsblk_entry_t;

/* FAT 引导扇区探测：0x55 0xAA @510/511 且 "FAT" 出现在 offset 0x20..0x3F（救援 ESP 候选） */
static int probe_fat(const char *devpath)
{
    char buf[512];
    long fd = sys_open(devpath, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) return -1;
    long n = sys_read((int)fd, buf, sizeof(buf));
    sys_close((int)fd);
    if (n != 512) return -1;
    if ((unsigned char)buf[510] != 0x55 || (unsigned char)buf[511] != 0xaa)
        return 0;
    for (int i = 0x20; i < 0x40; i++) {
        if (buf[i] == 'F' && buf[i + 1] == 'A' && buf[i + 2] == 'T')
            return 1;
    }
    return 0;
}


static int read_one_byte(const char *path, int notfound)
{
    char buf[8];
    long fd = sys_open(path, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) return notfound;
    long n = sys_read((int)fd, buf, sizeof(buf) - 1);
    sys_close((int)fd);
    if (n <= 0) return notfound;
    return buf[0] == '1';
}

int tool_lsblk(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;
    if (argc > 1) { elog_err("lsblk: usage: lsblk"); return 2; }

    /* 读 /proc/partitions */
    char buf[8192];
    size_t len = 0;
    if (aether_read_file("/proc/partitions", buf, sizeof(buf) - 1, &len) != 0) {
        elog_err("lsblk: cannot read /proc/partitions (/proc not mounted? try mount -t proc proc /proc)");
        return 1;
    }
    buf[len] = '\0';

    lsblk_entry_t entries[MAX_ENTRIES];
    int ne = 0;
    const char *p = buf, *end = buf + len;
    while (p < end && ne < MAX_ENTRIES) {
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

        unsigned major = 0, minor = 0;
        for (const char *q = tok[0]; q < ls + llen && *q >= '0' && *q <= '9'; q++) major = major * 10 + (unsigned)(*q - '0');
        for (const char *q = tok[1]; q < ls + llen && *q >= '0' && *q <= '9'; q++) minor = minor * 10 + (unsigned)(*q - '0');
        unsigned long long blocks = 0;
        for (const char *q = tok[2]; q < ls + llen && *q >= '0' && *q <= '9'; q++) blocks = blocks * 10 + (unsigned)(*q - '0');

        size_t nlen = llen - (size_t)(tok[3] - ls);
        if (nlen >= sizeof(entries[0].name)) nlen = sizeof(entries[0].name) - 1;
        for (size_t k = 0; k < nlen; k++) entries[ne].name[k] = tok[3][k];
        entries[ne].name[nlen] = '\0';

        aether_classify_part(entries[ne].name, entries[ne].parent, sizeof(entries[ne].parent), &entries[ne].is_part);
        entries[ne].major = major;
        entries[ne].minor = minor;
        entries[ne].kb = blocks;
        entries[ne].mountpoint[0] = '\0';
        entries[ne].is_swap = 0;
        entries[ne].esp = -1;

        /* sysfs ro / removable（分区从父盘继承） */
        char ropath[80];
        const char *pre = "/sys/block/";
        size_t d = 0;
        for (size_t k = 0; pre[k]; k++) ropath[d++] = pre[k];
        for (size_t k = 0; entries[ne].name[k]; k++) { ropath[d] = entries[ne].name[k]; d++; }
        const char *suf = "/ro";
        for (size_t k = 0; suf[k]; k++) ropath[d++] = suf[k];
        ropath[d] = '\0';
        entries[ne].ro = read_one_byte(ropath, 0);

        char rmpath[80];
        d = 0;
        for (size_t k = 0; pre[k]; k++) rmpath[d++] = pre[k];
        for (size_t k = 0; entries[ne].name[k]; k++) rmpath[d++] = entries[ne].name[k];
        const char *suf2 = "/removable";
        for (size_t k = 0; suf2[k]; k++) rmpath[d++] = suf2[k];
        rmpath[d] = '\0';
        int rm = read_one_byte(rmpath, -1);
        entries[ne].rm = rm; /* -1 = 无此文件（分区），打印时从父盘取 */
        ne++;
    }

    /* 挂载点：/proc/mounts 中 /dev/<name> 精确匹配 */
    {
        char mbuf[16384];
        size_t mlen = 0;
        if (aether_read_file("/proc/mounts", mbuf, sizeof(mbuf) - 1, &mlen) == 0) {
            mbuf[mlen] = '\0';
            const char *mp = mbuf, *mend = mbuf + mlen;
            while (mp < mend) {
                const char *ls = mp;
                while (mp < mend && *mp != '\n') mp++;
                size_t llen = (size_t)(mp - ls);
                if (mp < mend) mp++;
                size_t i = 0;
                while (i < llen && (ls[i] == ' ' || ls[i] == '\t')) i++;
                const char *dev = ls + i;
                while (i < llen && ls[i] != ' ' && ls[i] != '\t') i++;
                size_t devlen = (size_t)(ls + i - dev);
                while (i < llen && (ls[i] == ' ' || ls[i] == '\t')) i++;
                const char *tgt = ls + i;
                while (i < llen && ls[i] != ' ' && ls[i] != '\t') i++;
                size_t tgt_len = (size_t)(ls + i - tgt);
                while (i < llen && (ls[i] == ' ' || ls[i] == '\t')) i++;
                const char *fs = ls + i;
                while (i < llen && ls[i] != ' ' && ls[i] != '\t') i++;
                size_t fs_len = (size_t)(ls + i - fs);

                const char *pre = "/dev/";
                size_t plen = 5;
                if (devlen > plen && aether_strncmp(dev, pre, plen) == 0) {
                    const char *dname = dev + plen;
                    size_t dnlen = devlen - plen;
                    for (int j = 0; j < ne; j++) {
                        if (aether_strlen(entries[j].name) == dnlen &&
                            aether_strncmp(entries[j].name, dname, dnlen) == 0) {
                            if (tgt_len >= sizeof(entries[j].mountpoint))
                                tgt_len = sizeof(entries[j].mountpoint) - 1;
                            for (size_t k = 0; k < tgt_len; k++) entries[j].mountpoint[k] = tgt[k];
                            entries[j].mountpoint[tgt_len] = '\0';
                            entries[j].is_swap = (fs_len == 4 && aether_strncmp(fs, "swap", 4) == 0);
                            break;
                        }
                    }
                }
            }
        }
    }

    /* ESP 探测（需要读块设备权限；整盘跳过，只测分区；失败不算错） */
    for (int i = 0; i < ne; i++) {
        if (!entries[i].is_part) { entries[i].esp = 0; continue; }
        char dev[64];
        size_t d = 0;
        const char *pre = "/dev/";
        for (size_t k = 0; pre[k]; k++) dev[d++] = pre[k];
        for (size_t k = 0; entries[i].name[k]; k++) dev[d++] = entries[i].name[k];
        dev[d] = '\0';
        entries[i].esp = probe_fat(dev);
    }

    /* 整盘可移动性向下传递（分区无 removable 文件） */
    for (int i = 1; i < ne; i++) {
        if (entries[i].rm == -1 && entries[i].parent[0]) {
            for (int j = 0; j < i; j++) {
                if (aether_streq(entries[j].name, entries[i].parent)) {
                    entries[i].rm = entries[j].rm;
                    break;
                }
            }
            if (entries[i].rm == -1) entries[i].rm = 0;
        }
    }

    /* 输出树 */
    {
        char h1[28], h2[10], h3[4], h4[9], h5[4], h6[8], h7[12];
        aether_str_pad(h1, sizeof(h1), "NAME", 26, 0);
        aether_str_pad(h2, sizeof(h2), "MAJ:MIN", 9, 0);
        aether_str_pad(h3, sizeof(h3), "RM", 3, 1);
        aether_str_pad(h4, sizeof(h4), "SIZE", 8, 1);
        aether_str_pad(h5, sizeof(h5), "RO", 3, 1);
        aether_str_pad(h6, sizeof(h6), "TYPE", 7, 0);
        aether_str_pad(h7, sizeof(h7), "ESP", 10, 0);
        elog_info("%s %s %s %s %s %s %s MOUNTPOINT", h1, h2, h3, h4, h5, h6, h7);
    }

    for (int i = 0; i < ne; i++) {
        lsblk_entry_t *e = &entries[i];
        char sz[32];
        aether_human_size(e->kb, sz, sizeof(sz));

        /* 树前缀：分区用 ├─ / └─（该盘最后一个分区用 └─） */
        char namecell[64];
        if (e->is_part) {
            int last_of_disk = 1;
            for (int j = i + 1; j < ne; j++) {
                if (aether_streq(entries[j].parent, e->parent) && entries[j].is_part) { last_of_disk = 0; break; }
            }
            size_t d = 0;
            const char *art = last_of_disk ? "└─" : "├─";
            for (size_t k = 0; art[k]; k++) namecell[d++] = art[k];
            for (size_t k = 0; e->name[k]; k++) namecell[d++] = e->name[k];
            namecell[d] = '\0';
        } else {
            const char *src = e->name;
            size_t k = 0;
            for (; src[k]; k++) namecell[k] = src[k];
            namecell[k] = '\0';
        }

        char mm[16];
        {
            char num1[8], num2[8];
            unsigned v;
            size_t m = 0;
            v = e->major;
            if (v == 0) num1[m++] = '0';
            while (v) { num1[m++] = (char)('0' + v % 10); v /= 10; }
            for (size_t i = 0, j = m - 1; i < j; i++, j--) { char t = num1[i]; num1[i] = num1[j]; num1[j] = t; }
            num1[m] = '\0';
            m = 0;
            v = e->minor;
            if (v == 0) num2[m++] = '0';
            while (v) { num2[m++] = (char)('0' + v % 10); v /= 10; }
            for (size_t i = 0, j = m - 1; i < j; i++, j--) { char t = num2[i]; num2[i] = num2[j]; num2[j] = t; }
            num2[m] = '\0';
            size_t d = 0;
            for (size_t k = 0; num1[k]; k++) mm[d++] = num1[k];
            mm[d++] = ':';
            for (size_t k = 0; num2[k]; k++) mm[d++] = num2[k];
            mm[d] = '\0';
        }

        char c1[28], c2[10], c3[4], c4[9], c5[4], c6[8], c7[12];
        aether_str_pad(c1, sizeof(c1), namecell, 26, 0);
        aether_str_pad(c2, sizeof(c2), mm, 9, 0);
        char rmbuf[4] = { (char)('0' + (e->rm > 0 ? 1 : 0)), '\0' };
        aether_str_pad(c3, sizeof(c3), rmbuf, 3, 1);
        aether_str_pad(c4, sizeof(c4), sz, 8, 1);
        char robuf[4] = { (char)('0' + (e->ro ? 1 : 0)), '\0' };
        aether_str_pad(c5, sizeof(c5), robuf, 3, 1);
        aether_str_pad(c6, sizeof(c6), e->is_part ? "part" : "disk", 7, 0);
        aether_str_pad(c7, sizeof(c7), e->esp == 1 ? "FAT(ESP?)" : (e->esp == 0 ? "-" : "?"), 10, 0);
        const char *mp = e->mountpoint[0] ? e->mountpoint : (e->is_swap ? "[SWAP]" : "");
        elog_info("%s %s %s %s %s %s %s %s", c1, c2, c3, c4, c5, c6, c7, mp);
    }
    /* efivarfs 状态（disks 原功能） */
    char dir[128];
    if (aether_efivars_dir(dir, sizeof(dir)) != 0) {
        elog_info("efivars: no UEFI variable filesystem found (not UEFI or /sys/firmware/efi missing)");
        return 0;
    }
    int mounted = aether_is_mounted(dir, "efivarfs");
    int mnt_rw = 0;
    if (mounted == 1) aether_mount_info(dir, "efivarfs", &mnt_rw);
    if (mounted != 1) {
        elog_info("efivars: found %s but not mounted (mount: mount -t efivarfs efivarfs %s)", dir, dir);
    } else if (aether_efivars_writable()) {
        elog_info("efivars: %s mounted (rw, writable)", dir);
    } else if (mnt_rw) {
        elog_info("efivars: %s mounted (rw, but no write permission for current user; need root)", dir);
    } else {
        elog_info("efivars: %s mounted (ro); to write: mount -o remount,rw efivarfs", dir);
    }
    unsigned short bo[64];
    int nb = 0;
    if (aether_efivars_bootorder(bo, 64, &nb) == 0 && nb > 0) {
        char order[256];
        size_t d = 0;
        order[0] = '\0';
        for (int i = 0; i < nb && d + 7 < sizeof(order); i++) {
            char hex[6];
            const char *hx = "0123456789abcdef";
            hex[0] = hx[(bo[i] >> 12) & 0xf];
            hex[1] = hx[(bo[i] >> 8) & 0xf];
            hex[2] = hx[(bo[i] >> 4) & 0xf];
            hex[3] = hx[bo[i] & 0xf];
            hex[4] = '\0';
            if (i) { order[d++] = ','; }
            for (size_t k = 0; hex[k]; k++) order[d++] = hex[k];
            order[d] = '\0';
        }
        elog_info("BootOrder: %s", order);
    } else {
        elog_info("BootOrder: not set or unreadable (not an UEFI boot entry?)");
    }
    return 0;
}
