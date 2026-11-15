/*
 * sysnum for x86_64 — 启动救援 Shell (aether)
 * 生成自 asm/unistd_64.h（本机 Linux UAPI 头文件，2026-11-14），不自创编号。
 * nolibc 分支专用：本地编译分支（USE_HOST_LIBC=ON）不使用本文件。
 */
#ifndef AETHER_SYSNUM_X86_64_H
#define AETHER_SYSNUM_X86_64_H

#define __NR_read            0
#define __NR_write           1
#define __NR_open            2
#define __NR_openat          257
#define __NR_close           3
#define __NR_lseek           8
#define __NR_mmap            9
#define __NR_mprotect        10
#define __NR_munmap          11
#define __NR_ioctl           16
#define __NR_fcntl           72
#define __NR_dup             32
#define __NR_dup2            33
#define __NR_dup3            292
#define __NR_pipe            22
#define __NR_pipe2           293
#define __NR_fork            57
#define __NR_vfork           58
#define __NR_clone           56
#define __NR_clone3          435
#define __NR_execve          59
#define __NR_wait4           61
#define __NR_exit            60
#define __NR_exit_group      231
#define __NR_getpid          39
#define __NR_getppid         110
#define __NR_gettid          186
#define __NR_getuid          102
#define __NR_geteuid         107
#define __NR_getgid          104
#define __NR_getegid         108
#define __NR_chroot          161
#define __NR_chdir           80
#define __NR_getcwd          79
#define __NR_pivot_root      155
#define __NR_mount           165
#define __NR_umount2         166
#define __NR_faccessat       269
#define __NR_faccessat2      439
#define __NR_newfstatat      262
#define __NR_statx           332
#define __NR_unlinkat        263
#define __NR_syslog          103
#define __NR_rt_sigaction    13
#define __NR_rt_sigreturn    15
#define __NR_getdents64      217
#define __NR_mkdirat         258
#define __NR_nanosleep       35
#define __NR_socket          41
#define __NR_bind            49
#define __NR_recvfrom        45
#define __NR_mknodat         259
#define __NR_getpid          39
#define __NR_fchmodat        268
#define __NR_fchownat        260
#define __NR_readlinkat      267
#define __NR_utimensat       280
#define __NR_renameat        264
#define __NR_ftruncate       77
#define __NR_truncate        76
#define __NR_fsync           74
#define __NR_sync            162
#define __NR_flock           73
#define __NR_setpgid         109
#define __NR_getpgid         121
#define __NR_kill            62
#define __NR_listen          50
#define __NR_accept4         288
#define __NR_setsockopt      54
#define __NR_sendto          44
#define __NR_reboot          169
#define __NR_finit_module    313
#define __NR_setsid         112
#define __NR_init_module     175

#endif /* AETHER_SYSNUM_X86_64_H */
