/*
 * boot.h — initramfs 救援 init（§1 启动链兜底）
 * aether 作为 PID 1 运行时：挂载伪文件系统 → 读 /proc/cmdline → 解析 root= → 挂载根 →
 * pivot_root → execve init；任一步失败即进入拯救模式（交互 shell + 明确提示）。
 * 也可在拯救 shell 中用内置命令手动重试：cmdline / rescue-boot / efi-cmdline。
 */
#ifndef AETHER_BOOT_H
#define AETHER_BOOT_H

#include <stddef.h>

typedef struct {
    char root[256];       /* root=...（完整 spec：/dev/x、PARTUUID=..、UUID=..、LABEL=..） */
    char rootfstype[32];  /* rootfstype= */
    char rootflags[64];   /* rootflags= 原始串 */
    char init[128];       /* init=（默认 /sbin/init） */
    char resume[256];     /* resume=...（休眠恢复设备：/dev/x、PARTUUID=..、UUID=..；noresume 可禁用） */
    unsigned long long resume_offset; /* resume_offset=（PAGE_SIZE 单位，仅 swapfile） */
    int  rootwait;        /* rootwait 标志 */
    int  ro;              /* 只读根（ro 标志或 rootflags 含 ro） */
    int  noresume;        /* noresume：禁止休眠恢复 */
    int  aether_rescue;   /* aether_rescue：确定性进入救援模式（不 boot，直接 rescue） */
} aether_cmdline_t;

/* 测试用：直接解析字符串 */
int aether_parse_cmdline(const char *cmdline, aether_cmdline_t *out);
/* 从文件读 cmdline（/proc/cmdline，自动去尾部换行） */
int aether_cmdline_file(const char *path, aether_cmdline_t *out);

/* spec → /dev/<name>。支持设备路径、PARTUUID、PARTLABEL、UUID（ext4/vfat 实测）、LABEL。
 * 返回 0 成功，非 0 失败（含提示信息打印到 stderr） */
int aether_resolve_root(const char *spec, char *out, size_t cap);

/* 完整启动流程；成功不返回（exec 了 init）；失败返回 -1（打印原因） */
int aether_boot_attempt(const aether_cmdline_t *opts);

/* uevent：监听 KOBJECT_UEVENT 最多 max_ms 毫秒，按 ACTION=add/remove 建/删 /dev 节点 */
int aether_uevent_pump(int max_ms);

/* PID 1 入口：挂载伪文件系统 → uevent 补齐 → cmdline → boot；失败进拯救 shell */
int aether_init_main(char **envp);

/* 拯救 shell builtins:cmdline / rescue-boot / efi-cmdline */
int aether_cmd_cmdline(int argc, char **argv, char **envp);
int aether_cmd_rescue_boot(int argc, char **argv, char **envp);
int aether_cmd_efi_cmdline(int argc, char **argv, char **envp);

#endif /* AETHER_BOOT_H */
