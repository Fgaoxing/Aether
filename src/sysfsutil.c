/*
 * sysfsutil.c — /sys /proc 读写（§9）
 */
#include "sysfsutil.h"
#include "syscall_shim.h"
#include "util.h"
#include "elog.h"

#define DT_DIR 4
#define DT_REG 8

/* linux_dirent64（UAPI，四架构一致） */
typedef struct {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
} aether_dirent_t;

int aether_read_file(const char *path, void *buf, size_t cap, size_t *len_out)
{
    long fd = sys_open(path, O_RDONLY, 0);
    size_t off = 0;
    if (AETHER_IS_ERR(fd)) return -1;
    for (;;) {
        long n = sys_read((int)fd, (char *)buf + off, cap - off);
        if (n == 0) break;
        if (AETHER_IS_ERR(n)) { sys_close((int)fd); return -1; }
        off += (size_t)n;
        if (off >= cap) break; /* 截断 */
    }
    sys_close((int)fd);
    if (len_out) *len_out = off;
    return 0;
}

int aether_foreach_dirent(const char *dirpath, aether_dirent_cb cb, void *ctx)
{
    long fd = sys_open(dirpath, O_RDONLY | O_DIRECTORY, 0);
    if (AETHER_IS_ERR(fd)) return -1;
    int r = aether_foreach_dirent_fd((int)fd, cb, ctx);
    sys_close((int)fd);
    return r;
}

int aether_foreach_dirent_fd(int fd, aether_dirent_cb cb, void *ctx)
{
    char buf[4096];
    long n;
    while ((n = sys_getdents64(fd, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            aether_dirent_t *d = (aether_dirent_t *)(buf + off);
            if (d->d_reclen == 0) return -1; /* 防御损坏数据 */
            cb(d->d_name, d->d_type, ctx);
            off += d->d_reclen;
        }
    }
    return n == 0 ? 0 : -1;
}

/* /proc/mounts 逐行解析：device target fstype opts ... → fields[1]==target && (fstype==0 || fields[2]==fstype) */
static int mounts_scan(const char *target, const char *fstype, int *rw_out)
{
    char buf[16384];
    size_t len = 0;
    if (aether_read_file("/proc/mounts", buf, sizeof(buf) - 1, &len) != 0) return -1;
    buf[len] = '\0';
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        size_t llen = (size_t)(p - ls);
        if (p < end) p++;

        /* 字段切分（最多 6 段，记录起始与长度） */
        const char *fstart[6] = {0};
        size_t flen[6] = {0};
        int nf = 0;
        const char *cur = 0;
        for (size_t i = 0; i <= llen && nf < 6; i++) {
            char c = (i < llen) ? ls[i] : '\0'; /* 行尾哨兵 */
            if (c == ' ' || c == '\t' || c == '\0') {
                if (cur != 0) {
                    fstart[nf] = cur;
                    flen[nf] = (ls + i) - cur;
                    nf++;
                    cur = 0;
                }
            } else if (cur == 0) {
                cur = ls + i;
            }
        }
        if (nf < 4) continue;

        const char *t = fstart[1];
        size_t tlen = flen[1];
        const char *fs = fstart[2];
        size_t fslen = flen[2];
        if (tlen == aether_strlen(target) && aether_strncmp(t, target, tlen) == 0 &&
            (!fstype || (fslen == aether_strlen(fstype) && aether_strncmp(fs, fstype, fslen) == 0))) {
            if (rw_out) {
                const char *opts = fstart[3];
                size_t olen = flen[3];
                int rw = 0;
                for (size_t k = 0; k + 2 <= olen; k++) {
                    if (opts[k] == 'r' && opts[k + 1] == 'w' &&
                        (k == 0 || opts[k - 1] == ',') &&
                        (k + 2 == olen || opts[k + 2] == ',')) { rw = 1; break; }
                }
                *rw_out = rw;
            }
            return 1;
        }
    }
    return 0;
}

int aether_is_mounted(const char *target, const char *fstype)
{
    return mounts_scan(target, fstype, 0);
}

int aether_mount_info(const char *target, const char *fstype, int *is_rw)
{
    int rw = 0;
    int r = mounts_scan(target, fstype, &rw);
    if (r != 1) return r; /* 0 未挂载，-1 读失败 */
    if (is_rw) *is_rw = rw;
    return 1;
}

int aether_efivars_dir(char *out, size_t cap)
{
    static const char *const candidates[] = { "/sys/firmware/efi/efivars", "/sys/firmware/efi/vars", 0 };
    for (int i = 0; candidates[i]; i++) {
        long fd = sys_open(candidates[i], O_RDONLY | O_DIRECTORY, 0);
        if (!AETHER_IS_ERR(fd)) {
            sys_close((int)fd);
            size_t n = aether_strlen(candidates[i]);
            if (n + 1 > cap) return -1;
            for (size_t k = 0; k <= n; k++) out[k] = candidates[i][k];
            return 0;
        }
    }
    return -1;
}

int aether_ensure_efivarfs(int remount_rw)
{
    char dir[128];
    if (aether_efivars_dir(dir, sizeof(dir)) == 0) {
        int mounted = aether_is_mounted(dir, "efivarfs");
        if (mounted == 1) {
            if (remount_rw) {
                int rw = 0;
                mounts_scan(dir, "efivarfs", &rw);
                if (!rw)
                    return sys_mount("efivarfs", dir, "efivarfs", MS_REMOUNT, 0) == 0 ? 0 : -1;
            }
            return 0;
        }
        return sys_mount("efivarfs", dir, "efivarfs", 0, 0) == 0 ? 0 : -1;
    }
    /* 路径不存在：EFI 变量文件系统可能完全缺失（非 EFI 机器） */
    return -1;
}

int aether_efivars_writable(void)
{
    char dir[128];
    if (aether_efivars_dir(dir, sizeof(dir)) != 0) return 0;
    if (aether_is_mounted(dir, "efivarfs") != 1) return 0;
    int rw = 0;
    mounts_scan(dir, "efivarfs", &rw);
    long euid = sys_geteuid();
    return (rw && euid == 0) ? 1 : 0;
}

/* 读 BootOrder：efivarfs 文件 = u32 attrs + u16[n] */
int aether_efivars_bootorder(unsigned short *out, int max, int *count)
{
    char dir[128];
    if (aether_efivars_dir(dir, sizeof(dir)) != 0) return -1;
    char path[256];
    size_t d = 0;
    const char *pre = dir;
    for (size_t i = 0; pre[i]; i++) path[d++] = pre[i];
    const char *mid = "/BootOrder-";
    for (size_t i = 0; mid[i]; i++) path[d++] = mid[i];
    const char *guid = EFIVARS_GUID;
    for (size_t i = 0; guid[i]; i++) path[d++] = guid[i];
    path[d] = '\0';

    char buf[1024];
    size_t len = 0;
    if (aether_read_file(path, buf, sizeof(buf), &len) != 0) return -1;
    size_t data = len >= EFIVARS_ATTRS_LEN ? len - EFIVARS_ATTRS_LEN : 0;
    int n = (int)(data / 2);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        size_t off = EFIVARS_ATTRS_LEN + (size_t)i * 2;
        out[i] = (unsigned short)((unsigned char)buf[off] | ((unsigned char)buf[off + 1] << 8));
    }
    *count = n;
    return 0;
}

/* 写 BootOrder：保留原文件 attrs（否则默认 7 = NVRAM|BOOTSERVICE_ACCESS） */
int aether_efivars_set_bootorder(const unsigned short *list, int n)
{
    char dir[128];
    if (aether_efivars_dir(dir, sizeof(dir)) != 0) return -1;
    char path[256];
    size_t d = 0;
    const char *pre = dir;
    for (size_t i = 0; pre[i]; i++) path[d++] = pre[i];
    const char *mid = "/BootOrder-";
    for (size_t i = 0; mid[i]; i++) path[d++] = mid[i];
    const char *guid = EFIVARS_GUID;
    for (size_t i = 0; guid[i]; i++) path[d++] = guid[i];
    path[d] = '\0';

    unsigned int attrs = 7;
    {
        char buf[16];
        size_t len = 0;
        if (aether_read_file(path, buf, sizeof(buf), &len) == 0 && len >= 4)
            attrs = (unsigned int)((unsigned char)buf[0] | ((unsigned char)buf[1] << 8) |
                                   ((unsigned char)buf[2] << 16) | ((unsigned char)buf[3] << 24));
    }

    char data[4 + 512];
    data[0] = (char)(attrs & 0xff);
    data[1] = (char)((attrs >> 8) & 0xff);
    data[2] = (char)((attrs >> 16) & 0xff);
    data[3] = (char)((attrs >> 24) & 0xff);
    int k = 4;
    for (int i = 0; i < n; i++) {
        data[k++] = (char)(list[i] & 0xff);
        data[k++] = (char)((list[i] >> 8) & 0xff);
    }
    long fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (AETHER_IS_ERR(fd)) return -1;
    long w = sys_write((int)fd, data, (size_t)k);
    sys_close((int)fd);
    return AETHER_IS_ERR(w) ? -1 : 0;
}

static int bootnum_path(char *out, size_t cap, unsigned int bootnum)
{
    char dir[128];
    if (aether_efivars_dir(dir, sizeof(dir)) != 0) return -1;
    /* Boot%04x-GUID */
    const char *hex = "0123456789abcdef";
    char num[5] = { hex[(bootnum >> 12) & 0xf], hex[(bootnum >> 8) & 0xf],
                    hex[(bootnum >> 4) & 0xf], hex[bootnum & 0xf], '\0' };
    size_t d = 0;
    for (size_t i = 0; dir[i]; i++) {
        if (d + 1 >= cap) return -1;
        out[d++] = dir[i];
    }
    const char *mid = "/Boot";
    for (size_t i = 0; mid[i]; i++) { if (d + 1 >= cap) return -1; out[d++] = mid[i]; }
    for (size_t i = 0; num[i]; i++) { if (d + 1 >= cap) return -1; out[d++] = num[i]; }
    const char *suf = "-" EFIVARS_GUID;
    for (size_t i = 0; suf[i]; i++) { if (d + 1 >= cap) return -1; out[d++] = suf[i]; }
    out[d] = '\0';
    return 0;
}

int aether_efivars_boot_exists(unsigned int bootnum)
{
    char path[256];
    if (bootnum_path(path, sizeof(path), bootnum) != 0) return 0;
    long fd = sys_open(path, O_RDONLY, 0);
    if (AETHER_IS_ERR(fd)) return 0;
    sys_close((int)fd);
    return 1;
}

int aether_efivars_delete_boot(unsigned int bootnum)
{
    char path[256];
    if (bootnum_path(path, sizeof(path), bootnum) != 0) return -1;
    return sys_unlink(path) == 0 ? 0 : -1;
}
