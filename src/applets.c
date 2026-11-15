/*
 * applets.c — applet 注册表（§2）
 */
#include "applets.h"
#include "elog.h"
#include "util.h"
#include "shell/shell_internal.h"
#include "syscall_shim.h"

#ifndef AETHER_VERSION
#define AETHER_VERSION "0.2.0-m1"
#endif

static int cmd_help(int argc, char **argv, char **envp);

/* 未实现 applet 的公共兜底；argv[0] 即工具名 */
int tool_notimpl(int argc, char **argv)
{
    const char *name = argc > 0 && argv[0] ? aether_base_name(argv[0]) : "?";
    (void)argc;
    elog_err("aether: '%s' not implemented yet (later in the development order)", name);
    return 127;
}

static int cmd_clear(int argc, char **argv, char **envp)
{
    static const char seq[] = "\x1b[2J\x1b[H";
    (void)argc; (void)argv; (void)envp;
    sys_write(1, seq, sizeof(seq) - 1);
    return 0;
}

static const aether_applet_t applet_table[] = {
    { "help",       cmd_help,     "list all applets and their status" },
    { "less",       tool_less,    "mini pager" },
    { "clear",      cmd_clear,    "clear screen" },
    { "editor",     tool_editor,  "nano-style full-screen editor (^S save, ^X quit)" },
    { "ps",         tool_ps,      "process list" },
    { "free",       tool_free,    "memory status" },
    { "dmesg",      tool_dmesg,   "kernel log" },
    { "lsblk",      tool_lsblk,   "block device tree + ESP candidates + efivarfs/BootOrder (NAME MAJ:MIN RM SIZE RO TYPE ESP MOUNTPOINT)" },
    { "basename",   tool_basename, "get file name" },
    { "dirname",    tool_dirname,  "get directory path" },
    { "stat",       tool_stat,     "file info" },
    { "readlink",   tool_readlink, "show symlink target (-f full path)" },
    { "realpath",   tool_realpath, "physical absolute path" },
    { "chmod",      tool_chmod,    "change mode (octal)" },
    { "chown",      tool_chown,    "change owner (uid[:gid])" },
    { "chgrp",      tool_chgrp,    "change group (gid)" },
    { "touch",      tool_touch,    "update timestamp / create empty file" },
    { "truncate",   tool_truncate, "truncate/extend file" },
    { "mkdir",      tool_mkdir,    "create directory (-p parents)" },
    { "rmdir",      tool_rmdir,    "remove empty directory" },
    { "rm",         tool_rm,       "delete file (-r recursive, -f force)" },
    { "cp",         tool_cp,       "copy (-r recursive, -p preserve mode)" },
    { "mv",         tool_mv,       "move/rename" },
    { "ls",         tool_ls,       "list directory (-l -a -h -R)" },
    { "dd",         tool_dd,       "block copy (dd if=x of=y bs=4k count=N)" },
    { "chattr",     tool_chattr,   "set file extended attribute (+i lock, etc.)" },
    { "lsattr",     tool_lsattr,   "show file extended attribute" },
    { "wc",         tool_wc,       "count lines/words/bytes" },
    { "head",       tool_head,     "show file head (-n N)" },
    { "tail",       tool_tail,     "show file tail (-n N, -f follow)" },
    { "strings",    tool_strings,  "extract printable strings from binary" },
    { "sort",       tool_sort,     "sort" },
    { "uniq",       tool_uniq,     "dedup (-c count)" },
    { "cut",        tool_cut,      "cut text by column (-d -f / -c)" },
    { "paste",      tool_paste,    "join files line by line" },
    { "fold",       tool_fold,     "fold text (-w width)" },
    { "shuf",       tool_shuf,     "shuffle lines" },
    { "cmp",        tool_cmp,      "compare files for equality" },
    { "diff",       tool_diff,     "show text diff" },
    { "join",       tool_join,     "join files by field" },
    { "split",      tool_split,    "split file (-l lines)" },
    { "xxd",        tool_xxd,      "hex dump viewer" },
    { "od",         tool_od,       "octal/hex dump" },
    { "zcat",       tool_zcat,     "decompress gzip/zstd stream to stdout (magic auto-detect)" },
    { "hexdump",    tool_hexdump,  "hex print" },
    { "hexedit",    tool_hexedit,  "hex editor" },
    { "find",       tool_find,     "find files recursively" },
    { "flock",      tool_flock,    "file lock (-s/-x/-u/-n)" },
    { "fsync",      tool_fsync,    "sync caches to disk" },
    { "sync",       tool_sync,     "sync caches to disk" },
    { "reboot",     tool_reboot,   "sync and reboot (needs root)" },
    { "halt",       tool_halt,     "sync and halt/poweroff (needs root)" },
    { "grep",       tool_grep,     "extended regex search (-i -v -n -c -l -q -e)" },
    { "lspci",      tool_lspci,    "PCI device list" },
    { "lsusb",      tool_lsusb,    "USB device list" },
    { "scsi",       tool_scsi,     "SCSI device list" },
    { "serial",     tool_serial,   "serial devices (/proc/tty/driver/serial + sysfs)" },
    { "raidautorun", tool_raidautorun, "soft RAID auto-run (mdadm preferred, ioctl fallback)" },
    { "kbd_mode",   tool_kbd_mode, "keyboard mode view/switch (xlate/raw/unicode/off)" },
    { "hotplug",     tool_hotplug,  "udev/mdev-style hotplug (daemon on uevent, create/delete /dev nodes)" },
    { "ifconfig",    tool_ifconfig, "network interface view/config ([iface] [up|down|ip[/prefix] [netmask m]|dhcp])" },
    { "udhcpc",      tool_udhcpc,   "DHCP client (DORA, get address/mask/gateway/DNS)" },
    { "telnetd",     tool_telnetd,  "telnet server (port 23, -p to override; no auth!)" },
    { "netup",       tool_netup,    "one-click network: detect NIC > DHCP > start telnetd" },
    { "efibootmgr", tool_efibootmgr, "EFI boot entry management" },
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
    elog_info("aether %s — initramfs rescue shell toolbox (x86_64 / aarch64 / i386 / riscv64)",
              AETHER_VERSION);
    elog_info("usage: aether <applet> [args...]; or hardlink this binary as <applet> and run it directly");
    elog_info("bare `aether` / `sh` enters the rescue shell");
    elog_info("applets:");
    const aether_applet_t *t = aether_applet_table(&n);
    for (i = 0; i < n; i++)
        elog_info("  %s\t%s", t[i].name, t[i].desc);
    elog_info("shell builtins:");
    for (i = 0; i < shell_builtin_count(); i++) {
        const shell_builtin_t *b = shell_builtin_at(i);
        if (b)
            elog_info("  %s\t%s", b->name, b->doc ? b->doc : "");
    }
    elog_info("bare `aether` / `sh` enters the rescue shell; use `help` anytime for this list");
    return 0;
}
