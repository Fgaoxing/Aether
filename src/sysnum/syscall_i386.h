/*
 * sysnum for i386 — 启动救援 Shell (aether)
 * 生成自 asm/unistd_32.h（本机 Linux UAPI 头文件，2026-11-14），不自创编号。
 * nolibc 分支专用：本地编译分支（USE_HOST_LIBC=ON）不使用本文件。
 */
#ifndef AETHER_SYSNUM_I386_H
#define AETHER_SYSNUM_I386_H

#define __NR_read            3
#define __NR_write           4
#define __NR_open            5
#define __NR_openat          295
#define __NR_close           6
#define __NR_lseek           19
#define __NR_mmap            90
#define __NR_mprotect        125
#define __NR_munmap          91
#define __NR_ioctl           54
#define __NR_fcntl           55
#define __NR_dup             41
#define __NR_dup2            63
#define __NR_dup3            330
#define __NR_pipe            42
#define __NR_pipe2           331
#define __NR_fork            2
#define __NR_vfork           190
#define __NR_clone           120
#define __NR_clone3          435
#define __NR_execve          11
#define __NR_wait4           114
#define __NR_exit            1
#define __NR_exit_group      252
#define __NR_getpid          20
#define __NR_getppid         64
#define __NR_gettid          224
#define __NR_getuid          24
#define __NR_geteuid         49
#define __NR_getgid          47
#define __NR_getegid         50
#define __NR_chroot          61
#define __NR_chdir           12
#define __NR_getcwd          183
#define __NR_pivot_root      217
#define __NR_mount           21
#define __NR_umount2         52
#define __NR_faccessat       307
#define __NR_faccessat2      439
#define __NR_statx           383
#define __NR_unlinkat        301
#define __NR_syslog          103
#define __NR_rt_sigaction    174
#define __NR_rt_sigreturn    173
#define __NR_getdents64      220
#define __NR_mkdirat         296
#define __NR_nanosleep       162
#define __NR_socket          359
#define __NR_bind            361
#define __NR_recvfrom        371
#define __NR_mknodat         297
#define __NR_getpid          20
#define __NR_fchmodat        306
#define __NR_fchownat        298
#define __NR_readlinkat      305
#define __NR_utimensat       320
#define __NR_renameat        302
#define __NR_ftruncate       93
#define __NR_truncate        92
#define __NR_fsync           118
#define __NR_sync            36
#define __NR_flock           143
#define __NR_setpgid         57
#define __NR_getpgid         132
#define __NR_kill            37
#define __NR_listen          363
#define __NR_accept4         364
#define __NR_setsockopt      366
#define __NR_sendto          369
#define __NR_reboot          88
#define __NR_finit_module    350
#define __NR_init_module     128
/* i386 int 0x80 下 __NR_mmap(90) 走内存传参，寄存器传参一律改用 __NR_mmap2(192)：offset 以 4K 页为单位 */
#define __NR_mmap2           192
/* i386 无 __NR_newfstatat：32 位内核表位 300 即 fstatat64（unistd_32.h 暴露为 __NR_fstatat64），语义等同 32 位 newfstatat */
#define __NR_setsid         66
#define __NR_newfstatat 300

#endif /* AETHER_SYSNUM_I386_H */
