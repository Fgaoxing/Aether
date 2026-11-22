/*
 * sysnum for ARM 32-bit EABI — 启动救援 Shell (aether)
 * 基于 Linux v6.12 arch/arm/tools/syscall.tbl（EABI/common），不自创编号。
 * nolibc 分支专用：本地编译分支（USE_HOST_LIBC=ON）不使用本文件。
 * arm32 使用自身 syscall 号表（非 asm-generic）；部分调用语义与 i386 相近（如 mmap2）。
 */
#ifndef AETHER_SYSNUM_ARM_H
#define AETHER_SYSNUM_ARM_H

#define __NR_read            3
#define __NR_write           4
#define __NR_open            5
#define __NR_openat          322
#define __NR_close           6
#define __NR_lseek           19
#define __NR_mmap2           192
#define __NR_mprotect        125
#define __NR_munmap          91
#define __NR_ioctl           54
#define __NR_fcntl           55
#define __NR_dup             41
#define __NR_dup2            63
#define __NR_dup3            358
#define __NR_pipe            42
#define __NR_pipe2           359
#define __NR_clone           120
#define __NR_clone3          435
#define __NR_execve          11
#define __NR_wait4           114
#define __NR_exit            1
#define __NR_exit_group      248
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
#define __NR_pivot_root      218
#define __NR_mount           21
#define __NR_umount2         52
#define __NR_faccessat       334
#define __NR_faccessat2      439
#define __NR_newfstatat      327 /* arm: fstatat64 */
#define __NR_statx           397
#define __NR_unlinkat        328
#define __NR_syslog          103
#define __NR_rt_sigaction    174
#define __NR_rt_sigreturn    173
#define __NR_getdents64      217
#define __NR_mkdirat         323
#define __NR_nanosleep       162
#define __NR_socket          281
#define __NR_bind            282
#define __NR_recvfrom        292
#define __NR_mknodat         324
#define __NR_fchmodat        333
#define __NR_fchownat        325
#define __NR_readlinkat      332
#define __NR_symlinkat       331
#define __NR_setsid          66
#define __NR_swapon          87
#define __NR_swapoff         115
#define __NR_utimensat       348
#define __NR_renameat        329
#define __NR_ftruncate       93
#define __NR_truncate        92
#define __NR_fsync           118
#define __NR_sync            36
#define __NR_flock           143
#define __NR_setpgid         57
#define __NR_getpgid         132
#define __NR_kill            37
#define __NR_listen          284
#define __NR_accept4         366
#define __NR_setsockopt      294
#define __NR_sendto          290
#define __NR_reboot          88
#define __NR_finit_module    379
#define __NR_init_module     128

#endif /* AETHER_SYSNUM_ARM_H */
