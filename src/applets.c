/*
 * applets.c — applet 注册表（§2）
 */
#include "applets.h"
#include "elog.h"
#include "util.h"
#include "shell/shell_internal.h"

#ifndef AETHER_VERSION
#define AETHER_VERSION "0.2.0-m1"
#endif

static int cmd_help(int argc, char **argv, char **envp);

/* 未实现 applet 的公共兜底；argv[0] 即工具名 */
int tool_notimpl(int argc, char **argv)
{
    const char *name = argc > 0 && argv[0] ? aether_base_name(argv[0]) : "?";
    (void)argc;
    elog_err("aether: '%s' 尚未实现（§12 开发顺序中靠后）", name);
    return 127;
}

static const aether_applet_t applet_table[] = {
    { "help",       cmd_help,     "列出所有 applet 与状态" },
    { "less",       tool_less,    "mini 分页器" },
    { "editor",     tool_editor,  "nano 风格全屏编辑器（^S 保存，^X 退出）" },
    { "ps",         tool_ps,      "进程列表" },
    { "free",       tool_free,    "内存状态" },
    { "dmesg",      tool_dmesg,   "内核日志" },
    { "lsblk",      tool_lsblk,   "块设备树 + ESP 候选 + efivarfs/BootOrder（NAME MAJ:MIN RM SIZE RO TYPE ESP MOUNTPOINT）" },
    { "basename",   tool_basename, "取文件名" },
    { "dirname",    tool_dirname,  "取目录路径" },
    { "stat",       tool_stat,     "文件详细信息" },
    { "readlink",   tool_readlink, "查看软链接目标（-f 完整路径）" },
    { "realpath",   tool_realpath, "真实绝对路径" },
    { "chmod",      tool_chmod,    "改权限（八进制）" },
    { "chown",      tool_chown,    "改所有者（uid[:gid]）" },
    { "chgrp",      tool_chgrp,    "改所属组（gid）" },
    { "touch",      tool_touch,    "更新时间戳/新建空文件" },
    { "truncate",   tool_truncate, "截断/扩展文件大小" },
    { "mkdir",      tool_mkdir,    "新建目录（-p 递归创建）" },
    { "rmdir",      tool_rmdir,    "删除空目录" },
    { "rm",         tool_rm,       "删除文件（-r 递归，-f 强制）" },
    { "cp",         tool_cp,       "复制（-r 递归，-p 保留权限）" },
    { "mv",         tool_mv,       "移动/重命名" },
    { "ls",         tool_ls,       "列出目录（-l -a -h -R）" },
    { "dd",         tool_dd,       "块读写（dd if=x of=y bs=4k count=N）" },
    { "chattr",     tool_chattr,   "设置文件扩展属性（+i 锁文件等）" },
    { "lsattr",     tool_lsattr,   "查看文件扩展属性" },
    { "wc",         tool_wc,       "统计行数/单词/字节" },
    { "head",       tool_head,     "看文件开头（-n N）" },
    { "tail",       tool_tail,     "看文件末尾（-n N，-f 跟踪）" },
    { "strings",    tool_strings,  "从二进制提取可打印字符串" },
    { "sort",       tool_sort,     "排序" },
    { "uniq",       tool_uniq,     "去重（-c 计数）" },
    { "cut",        tool_cut,      "按列截取文本（-d -f / -c）" },
    { "paste",      tool_paste,    "按行拼接文件" },
    { "fold",       tool_fold,     "文本折行（-w 宽度）" },
    { "shuf",       tool_shuf,     "随机打乱行" },
    { "cmp",        tool_cmp,      "比较两个文件是否一样" },
    { "diff",       tool_diff,     "对比文本差异" },
    { "join",       tool_join,     "按字段合并文件" },
    { "split",      tool_split,    "分割文件（-l 行数）" },
    { "xxd",        tool_xxd,      "十六进制查看" },
    { "od",         tool_od,       "八进制/十六进制 dump" },
    { "zcat",       tool_zcat,     "解压 gzip/zstd 流到 stdout（自动 magic 识别）" },
    { "hexdump",    tool_hexdump,  "十六进制打印" },
    { "hexedit",    tool_hexedit,  "十六进制编辑器" },
    { "find",       tool_find,     "递归查找文件" },
    { "flock",      tool_flock,    "文件锁（-s/-x/-u/-n）" },
    { "fsync",      tool_fsync,    "强制刷缓存到磁盘" },
    { "sync",       tool_sync,     "同步缓存写入磁盘" },
    { "reboot",     tool_reboot,   "同步磁盘并重启系统（需要 root）" },
    { "halt",       tool_halt,     "同步磁盘并停机/断电（需要 root）" },
    { "grep",       tool_grep,     "扩展正则搜索（-i -v -n -c -l -q -e）" },
    { "lspci",      tool_lspci,    "PCI 设备列表" },
    { "lsusb",      tool_lsusb,    "USB 设备列表" },
    { "scsi",       tool_scsi,     "SCSI 设备列表" },
    { "serial",     tool_serial,   "串口设备（/proc/tty/driver/serial + sysfs）" },
    { "raidautorun", tool_raidautorun, "软 RAID 自动识别启动（mdadm 优先，ioctl 兜底）" },
    { "kbd_mode",   tool_kbd_mode, "键盘模式查看/切换（xlate/raw/unicode/off）" },
    { "hotplug",     tool_hotplug,  "udev/mdev 风格热插拔（常驻监听 uevent 建/删 /dev 节点）" },
    { "ifconfig",    tool_ifconfig, "网络接口查看/配置（[iface] [up|down|ip[/prefix] [netmask m]|dhcp]）" },
    { "udhcpc",      tool_udhcpc,   "DHCP 客户端（DORA，获得地址/掩码/网关/DNS）" },
    { "telnetd",     tool_telnetd,  "telnet 服务器（端口 23，可 -p 覆盖；无认证！）" },
    { "netup",       tool_netup,    "一键联网：检测网卡→DHCP→启动 telnetd" },
    { "efibootmgr", tool_efibootmgr, "EFI 启动项管理" },
};
#define N_APPLETS (sizeof(applet_table) / sizeof(applet_table[0]))

const aether_applet_t *aether_applet_table(size_t *count)
{
    if (count) *count = N_APPLETS;
    return applet_table;
}

int aether_applet_run(const char *name, int argc, char **argv, char **envp)
{
    size_t i;
    if (!name) return -1;
    for (i = 0; i < N_APPLETS; i++) {
        if (aether_streq(name, applet_table[i].name))
            return applet_table[i].run(argc, argv, envp);
    }
    return -1;
}

static int cmd_help(int argc, char **argv, char **envp)
{
    size_t i, n;
    (void)argc; (void)argv; (void)envp;
    elog_info("aether %s — initramfs 启动救援 Shell 工具箱（x86_64 / aarch64 / i386 / riscv64）",
              AETHER_VERSION);
    elog_info("文档：启动救援Shell-需求文档.md（§12 开发顺序）");
    elog_info("用法：aether <applet> [参数...]；或把本二进制硬链接为 <applet> 后直接执行");
    elog_info("裸 `aether` / `sh` 进入救援 shell");
    elog_info("applets：");
    const aether_applet_t *t = aether_applet_table(&n);
    for (i = 0; i < n; i++)
        elog_info("  %s\t%s", t[i].name, t[i].desc);
    elog_info("shell 内置命令：");
    for (i = 0; i < shell_builtin_count(); i++) {
        const shell_builtin_t *b = shell_builtin_at(i);
        if (b)
            elog_info("  %s\t%s", b->name, b->doc ? b->doc : "");
    }
    elog_info("裸 `aether` / `sh` 进入救援 shell；用 `help` 随时查看本列表");
    return 0;
}
