/*
 * applets.h — BusyBox 式 applet 注册表（§2 / §9）
 * 主入口（aether/sh/ash）与硬链接调用共用这张表；shell 也用它在 pipeline 中分发同二进制 applet。
 * applet 收到的 argv[0] 为自己的名字（§2）。envp 由 main.c 透传。
 */
#ifndef AETHER_APPLETS_H
#define AETHER_APPLETS_H

#include <stddef.h>

typedef int (*aether_applet_fn)(int argc, char **argv, char **envp);

typedef struct {
    const char     *name;  /* argv[0] 的 basename */
    aether_applet_fn run;
    const char     *desc;
} aether_applet_t;

const aether_applet_t *aether_applet_table(size_t *count);

/* 找到并执行；未找到返回 -1（调用方自行决定报错码） */
int aether_applet_run(const char *name, int argc, char **argv, char **envp);

/* 未实现 applet 的公共兜底（argv[0] 即工具名） */
int tool_notimpl(int argc, char **argv);

/* 各工具入口（tools 目录下的源文件）；未实现的在表内仍指向 tool_notimpl */
int shell_main(int argc, char **argv, char **envp);
int tool_less(int argc, char **argv, char **envp);
int tool_editor(int argc, char **argv, char **envp);
int tool_ps(int argc, char **argv, char **envp);
int tool_free(int argc, char **argv, char **envp);
int tool_dmesg(int argc, char **argv, char **envp);
int tool_lsblk(int argc, char **argv, char **envp);
int tool_basename(int argc, char **argv, char **envp);
int tool_dirname(int argc, char **argv, char **envp);
int tool_stat(int argc, char **argv, char **envp);
int tool_readlink(int argc, char **argv, char **envp);
int tool_realpath(int argc, char **argv, char **envp);
int tool_chmod(int argc, char **argv, char **envp);
int tool_chown(int argc, char **argv, char **envp);
int tool_chgrp(int argc, char **argv, char **envp);
int tool_touch(int argc, char **argv, char **envp);
int tool_truncate(int argc, char **argv, char **envp);
int tool_mkdir(int argc, char **argv, char **envp);
int tool_rmdir(int argc, char **argv, char **envp);
int tool_rm(int argc, char **argv, char **envp);
int tool_cp(int argc, char **argv, char **envp);
int tool_mv(int argc, char **argv, char **envp);
int tool_ls(int argc, char **argv, char **envp);
int tool_dd(int argc, char **argv, char **envp);
int tool_chattr(int argc, char **argv, char **envp);
int tool_lsattr(int argc, char **argv, char **envp);
int tool_wc(int argc, char **argv, char **envp);
int tool_head(int argc, char **argv, char **envp);
int tool_tail(int argc, char **argv, char **envp);
int tool_strings(int argc, char **argv, char **envp);
int tool_sort(int argc, char **argv, char **envp);
int tool_uniq(int argc, char **argv, char **envp);
int tool_cut(int argc, char **argv, char **envp);
int tool_paste(int argc, char **argv, char **envp);
int tool_fold(int argc, char **argv, char **envp);
int tool_shuf(int argc, char **argv, char **envp);
int tool_cmp(int argc, char **argv, char **envp);
int tool_diff(int argc, char **argv, char **envp);
int tool_join(int argc, char **argv, char **envp);
int tool_split(int argc, char **argv, char **envp);
int tool_xxd(int argc, char **argv, char **envp);
int tool_od(int argc, char **argv, char **envp);
int tool_zcat(int argc, char **argv, char **envp);
int tool_hexdump(int argc, char **argv, char **envp);
int tool_hexedit(int argc, char **argv, char **envp);
int tool_find(int argc, char **argv, char **envp);
int tool_flock(int argc, char **argv, char **envp);
int tool_fsync(int argc, char **argv, char **envp);
int tool_sync(int argc, char **argv, char **envp);
int tool_reboot(int argc, char **argv, char **envp);
int tool_swapon(int argc, char **argv, char **envp);
int tool_swapoff(int argc, char **argv, char **envp);
int tool_halt(int argc, char **argv, char **envp);
int tool_grep(int argc, char **argv, char **envp);
int tool_lspci(int argc, char **argv, char **envp);
int tool_lsusb(int argc, char **argv, char **envp);
int tool_scsi(int argc, char **argv, char **envp);
int tool_serial(int argc, char **argv, char **envp);
int tool_raidautorun(int argc, char **argv, char **envp);
int tool_kbd_mode(int argc, char **argv, char **envp);
int tool_hotplug(int argc, char **argv, char **envp);
int tool_ifconfig(int argc, char **argv, char **envp);
int tool_udhcpc(int argc, char **argv, char **envp);
int tool_telnetd(int argc, char **argv, char **envp);
int tool_netup(int argc, char **argv, char **envp);
/* rescue 后台自动联网（boot.c 用）：未配置网络则反复重试直到成功，无需用户干预 */
int net_rescue_auto_retry(char **envp);
int tool_efibootmgr(int argc, char **argv, char **envp);

#endif /* AETHER_APPLETS_H */
