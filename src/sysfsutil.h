/*
 * sysfsutil.h — /sys /proc 读写工具函数（§9 sysfsutil）
 * 目录枚举走 getdents64；文件读走整文件一次读入；不依赖 libc。
 */
#ifndef AETHER_SYSFUTIL_H
#define AETHER_SYSFUTIL_H

#include <stddef.h>

/* 整文件读（小文件；大文件截断到 cap）。0 成功，-1 失败 */
int aether_read_file(const char *path, void *buf, size_t cap, size_t *len_out);

/* 目录项枚举；name/type 通过 cb 回调；0 成功，-1 失败（无法打开目录） */
typedef void (*aether_dirent_cb)(const char *name, unsigned char d_type, void *ctx);
int aether_foreach_dirent(const char *dirpath, aether_dirent_cb cb, void *ctx);

/* /proc/mounts 中找某挂载点（fstype 可为 NULL=任意）：1=已挂载，0=未挂载，-1=读失败 */
int aether_is_mounted(const char *target, const char *fstype);
/* 同上，但同时返回该挂载是否 rw：1=rw，0=ro；失败 -1 */
int aether_mount_info(const char *target, const char *fstype, int *is_rw);

/* /proc/mounts 中找 efivarfs 挂载点：0 成功（写入 out），-1 未找到/非 UEFI */
int aether_efivars_dir(char *out, size_t cap);

/* 挂载 efivarfs：remount_rw=1 时若已挂 ro 则 remount,rw。0 成功，-1 失败（含非 UEFI） */
int aether_ensure_efivarfs(int remount_rw);

/* 当前进程是否能写 efivars（root 且挂载 rw）：0=不能，1=能（含未挂却能挂的场景由调用方处理） */
int aether_efivars_writable(void);

/* 读 BootOrder（u16 数组；为空返回 count=0）：0 成功，-1 变量不存在/读失败 */
int aether_efivars_bootorder(unsigned short *out, int max, int *count);

/* 写 BootOrder：list=u16 数组 n 个。0 成功，-1 失败（含 euid!=0） */
int aether_efivars_set_bootorder(const unsigned short *list, int n);

/* 删除 BootXXXX：unlink。0 成功 */
int aether_efivars_delete_boot(unsigned int bootnum);

/* 指定 BootXXXX 是否存在 */
int aether_efivars_boot_exists(unsigned int bootnum);

/* efivars 文件级 attribute 前缀长度（efivarfs 格式：u32 attrs + data） */
#define EFIVARS_ATTRS_LEN 4

#define EFIVARS_GUID "8be4df61-93ca-11d2-aa0d-00e098032b8c"

#endif /* AETHER_SYSFUTIL_H */
