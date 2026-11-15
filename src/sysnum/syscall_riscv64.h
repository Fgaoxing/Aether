/*
 * sysnum for riscv64 — 启动救援 Shell (aether)
 * 生成自 asm-generic/unistd.h (RV64, __BITS_PER_LONG==64)（本机 Linux UAPI 头文件，2026-11-14），不自创编号。
 * nolibc 分支专用：本地编译分支（USE_HOST_LIBC=ON）不使用本文件。
 * aarch64/riscv64 沿用 asm-generic 编号表，不存在 open/fork/vfork/pipe/dup2（见需求文档 5.1）
 * fork/vfork 用 clone(SIGCHLD, ...) 模拟；pipe 用 pipe2(fds, 0) 模拟；dup2 用 dup3(fd, fd2, 0) 模拟。
 */
#ifndef AETHER_SYSNUM_RISCV64_H
#define AETHER_SYSNUM_RISCV64_H

#define __NR_read            63
#define __NR_write           64
#define __NR_openat          56
#define __NR_close           57
#define __NR_lseek           62
#define __NR_mmap            222
#define __NR_mprotect        226
#define __NR_munmap          215
#define __NR_ioctl           29
#define __NR_fcntl           25
#define __NR_dup             23
#define __NR_dup3            24
#define __NR_pipe2           59
#define __NR_clone           220
#define __NR_clone3          435
#define __NR_execve          221
#define __NR_wait4           260
#define __NR_exit            93
#define __NR_exit_group      94
#define __NR_getpid          172
#define __NR_getppid         173
#define __NR_gettid          178
#define __NR_getuid          174
#define __NR_geteuid         175
#define __NR_getgid          176
#define __NR_getegid         177
#define __NR_chroot          51
#define __NR_chdir           49
#define __NR_getcwd          17
#define __NR_pivot_root      41
#define __NR_mount           40
#define __NR_umount2         39
#define __NR_faccessat       48
#define __NR_faccessat2      439
#define __NR_newfstatat      79
#define __NR_statx           291
#define __NR_unlinkat        35
#define __NR_syslog          116
#define __NR_rt_sigaction    134
#define __NR_rt_sigreturn    139
#define __NR_getdents64      61
#define __NR_mkdirat         34
#define __NR_nanosleep       101
#define __NR_socket          198
#define __NR_bind            200
#define __NR_recvfrom        207
#define __NR_mknodat         33
#define __NR_getpid          172
#define __NR_fchmodat        53
#define __NR_fchownat        54
#define __NR_readlinkat      78
#define __NR_utimensat       88
#define __NR_renameat        38
#define __NR_ftruncate       46
#define __NR_truncate        45
#define __NR_fsync           82
#define __NR_sync            81
#define __NR_flock           32
#define __NR_setpgid         154
#define __NR_getpgid         155
#define __NR_kill            129
#define __NR_listen          201
#define __NR_accept4         242
#define __NR_setsockopt      208
#define __NR_sendto          206
#define __NR_reboot          142
#define __NR_finit_module    273
#define __NR_init_module     105

#endif /* AETHER_SYSNUM_RISCV64_H */
