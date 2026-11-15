/*
 * syscall_shim.h — 双分支 syscall 封装（§4.3 / §5）
 *
 * 上层业务代码只用这里的 sys_* 与 aether_syscall6()，不关心：
 *   - 底层是 host libc 的 syscall(3)（USE_LIBC_SYSCALL=ON，本地编译分支）
 *   - 还是自带 inline asm（nolibc 官方包分支，-nostdlib）
 * 返回值与 raw syscall 一致：错误为 -errno（内核负值），调用方判负，不经过 libc errno。
 */
#ifndef AETHER_SYSCALL_SHIM_H
#define AETHER_SYSCALL_SHIM_H

#include <stddef.h> /* size_t：两个分支都可用（编译器自带） */
#include <stdarg.h>

#if defined(USE_LIBC_SYSCALL)
  /* ---- 本地编译分支：架构/编号差异全部由宿主 libc 承担（§4.2） ----
   * 注意：glibc syscall(3) 把内核 -errno 翻译成 -1 + errno；
   * 统一转回内核约定（返回负 -errno），两分支调用方语义一致。 */
  #include <unistd.h>
  #include <sys/syscall.h>
  #include <sys/mman.h>
  #include <fcntl.h>
  #include <sys/wait.h>
  #include <errno.h>
  long aether_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
  #define syscall_nr(n, a0, a1, a2, a3, a4, a5) \
      aether_syscall6((long)(n), (long)(a0), (long)(a1), (long)(a2), (long)(a3), (long)(a4), (long)(a5))
  #define AETHER_NR(name) SYS_##name
  #if defined(SYS_open)
  #define AETHER_OPEN_IS_DIRECT 1
  #else
  #define AETHER_OPEN_IS_DIRECT 0
  #endif
#else
  /* ---- 预编译 nolibc 分支：按架构宏四选一，其他架构直接 #error（§4.1 / §4.3） ---- */
  #if defined(__x86_64__)
    #include "sysnum/syscall_x86_64.h"
  #elif defined(__aarch64__)
    #include "sysnum/syscall_aarch64.h"
  #elif defined(__i386__)
    #include "sysnum/syscall_i386.h"
  #elif defined(__riscv) && (__riscv_xlen == 64)
    #include "sysnum/syscall_riscv64.h"
  #else
    #error "nolibc branch supports only x86_64 / aarch64 / i386 / riscv64; other arches require USE_HOST_LIBC=ON"
  #endif
  #define AETHER_NR(name) __NR_##name
  #if defined(__NR_open)
  #define AETHER_OPEN_IS_DIRECT 1
  #else
  #define AETHER_OPEN_IS_DIRECT 0
  #endif
  long aether_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
  #define syscall_nr(n, a0, a1, a2, a3, a4, a5) \
      aether_syscall6((long)(n), (long)(a0), (long)(a1), (long)(a2), (long)(a3), (long)(a4), (long)(a5))


  /* nolibc 无 libc 头文件，这里补齐用到的最小 UAPI 常数（编号与 glibc/内核一致） */
  #ifndef AT_FDCWD
  #define AT_FDCWD (-100)
  #endif
  #ifndef SEEK_SET
  #define SEEK_SET 0
  #define SEEK_CUR 1
  #define SEEK_END 2
  #endif
  #define O_RDONLY   00
  #define O_WRONLY   01
  #define O_RDWR     02
  #define O_CREAT    0100
  #define O_TRUNC    01000
  #define O_APPEND   02000
  #define O_NONBLOCK 04000
  #define O_DIRECTORY 0200000
  #define O_CLOEXEC  02000000
  #define O_NOCTTY   0400
  #define PROT_READ  1
  #define PROT_WRITE 2
  #define PROT_EXEC  4
  #define MAP_SHARED    0x01
  #define MAP_PRIVATE   0x02
  #define MAP_ANONYMOUS 0x20
  #define MAP_FAILED    ((void *)-1)
  #define SIGCHLD    17
  #define WNOHANG     1
  #define WUNTRACED   2
#endif

#ifndef AT_FDCWD
#define AT_FDCWD (-100) /* libc 分支也统一补一个，封装层直用 */
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef WNOHANG
#define WNOHANG 1
#endif
#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGPIPE 13
#define SIGTERM 15
#define SIGWINCH 28
#define SIGTSTP  20
/* SIGUSR1：x86_64/i386=10，aarch64/riscv64（asm-generic）=16 */
#ifndef SIGUSR1
#if defined(__x86_64__) || defined(__i386__)
#define SIGUSR1 10
#else
#define SIGUSR1 16
#endif
#endif
#ifndef SIGTTIN
#define SIGTTIN 21
#define SIGTTOU 22
#endif
#ifndef SIG_DFL
#define SIG_DFL ((void (*)(int))0)
#endif
#ifndef SIG_IGN
#define SIG_IGN ((void (*)(int))1)
#endif
#define MS_RDONLY   0x00001
#define MS_NOSUID   0x00008
#define MS_NODEV    0x00004
#define MS_NOEXEC   0x00002
#define MS_NOATIME  0x00400
#define MS_REMOUNT  0x00020
#define MS_BIND     0x01000
#define MS_REC      0x04000
#define MS_I_SYNCHRONOUS 0x00010 /* MS_SYNCHRONOUS，未用，保留给将来 */

/* raw syscall 返回值判负：内核只把 -4095..-1 当作 errno */
#define AETHER_IS_ERR(ret)   ((ret) < 0 && (ret) >= -4095L)
#define AETHER_ERR_CODE(ret) ((int)(-(ret)))

/* 信号：libc 分支直接调 libc sigaction；nolibc 分支用内核 sigaction 布局 + rt_sigaction（§9） */
#if defined(USE_LIBC_SYSCALL)
#include <signal.h>
#else
#define AETHER_SA_RESTORER 0x04000000 /* SA_RESTORER（x86/i386 必需；ag 架构无此宏但 flag 无害） */
#if defined(__x86_64__)
/* x86_64: handler(8) flags(8) restorer(8) mask(8) */
struct aether_sigaction { void *sa_handler; unsigned long sa_flags; void *sa_restorer; unsigned long sa_mask; };
#define AETHER_SIGSETSIZE 8
#elif defined(__i386__)
/* i386: handler(4) mask(8, _NSIG_WORDS=2×4B) flags(4) restorer(4)；sigsetsize 必须为 8（内核
 * 对 _NSIG/8 校验，4 会 EINVAL）。UAPI asm/signal.h 里的 4 字节 mask 已过时，勿信。 */
struct aether_sigaction { void *sa_handler; unsigned long sa_mask[2]; unsigned int sa_flags; void *sa_restorer; };
#define AETHER_SIGSETSIZE 8
#else
/* aarch64 / riscv64 (asm-generic): handler(8) flags(8) mask(8，紧邻，无 restorer 字段) */
struct aether_sigaction { void *sa_handler; unsigned long sa_flags; unsigned long sa_mask; };
#define AETHER_SIGSETSIZE 8
#endif
extern void aether_sig_restorer(void); /* 见 nolibc/sigreturn_*.S（仅 x86_64/i386） */
#endif

/* 注册信号 handler；返回 0 成功，-1 失败。nolibc 分支 SA_RESTORER 由 arch 决定 */
long aether_signal(int signum, void (*handler)(int));

long sys_getppid(void);

long sys_read(int fd, void *buf, size_t count);
long sys_write(int fd, const void *buf, size_t count);
long sys_open(const char *path, int flags, int mode); /* 无 open 的架构归一为 openat(AT_FDCWD,...)（§5.1） */
long sys_close(int fd);
long sys_lseek(int fd, long offset, int whence);      /* i386 为 32 位偏移，>2GB 文件不支持 */
long sys_unlinkat(int dirfd, const char *path, int flags);
long sys_unlink(const char *path);                    /* unlinkat(AT_FDCWD, path, 0) 归一 */
long sys_syslog(int type, char *buf, int len);        /* dmesg 用；type=3 READ_ALL */
long sys_getdents64(int fd, void *buf, size_t count); /* 目录枚举（efivarfs、/proc/[pid]） */
long sys_mkdirat(int dirfd, const char *path, int mode);
long sys_faccessat(int dirfd, const char *path, int mode, int flags);
long sys_geteuid(void);
long sys_getpid(void);
long sys_nanosleep(const void *req, void *rem);
long sys_socket(int domain, int type, int protocol);
long sys_bind(int fd, const void *addr, unsigned int addrlen);
long sys_recvfrom(int fd, void *buf, size_t len, int flags, void *src, void *srclen);
long sys_mknodat(int dirfd, const char *path, unsigned int mode, unsigned long long dev);
long sys_chmod(const char *path, unsigned int mode);
long sys_chown(const char *path, unsigned int owner, unsigned int group);
long sys_readlink(const char *path, char *buf, size_t bufsiz);
long sys_symlinkat(const char *target, int newdirfd, const char *linkpath);
long sys_symlink(const char *target, const char *linkpath);
long sys_utimensat(int dirfd, const char *path, const void *times, int flags);
long sys_renameat(int olddirfd, const char *old, int newdirfd, const char *new);
long sys_ftruncate(int fd, long length);
long sys_sync(void);
long sys_reboot(unsigned int magic1, unsigned int magic2, unsigned int cmd, void *arg);
long sys_finit_module(int fd, const char *param_values, int flags);
long sys_fsync(int fd);
long sys_flock(int fd, int operation);
long sys_setpgid(int pid, int pgid);
long sys_getpgid(int pid);
long sys_setsid(void);
long sys_kill(int pid, int sig);
long sys_listen(int fd, int backlog);
long sys_accept4(int fd, void *addr, void *addrlen, int flags);
long sys_setsockopt(int fd, int level, int optname, const void *optval, unsigned int optlen);
long sys_sendto(int fd, const void *buf, size_t len, int flags, const void *dest, unsigned int destlen);
long aether_statx(int dirfd, const char *path, int flags, unsigned int mask, void *out);
long sys_mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
long sys_munmap(void *addr, size_t length);
long sys_ioctl(int fd, unsigned long request, unsigned long arg);
long sys_dup(int oldfd);
long sys_dup2(int oldfd, int newfd);                  /* 无 dup2 的架构归一为 dup3(fd, fd2, 0) */
long sys_pipe(int fds[2]);                            /* 无 pipe 的架构归一为 pipe2(fds, 0) */
long sys_fork(void);                                  /* 无 fork 的架构归一为 clone(SIGCHLD, 0, ...) */
long sys_execve(const char *path, char *const argv[], char *const envp[]);
long sys_wait4(int pid, int *status, int options, void *rusage);
long sys_chroot(const char *path);
long sys_chdir(const char *path);
long sys_getcwd(char *buf, size_t size);
long sys_pivot_root(const char *new_root, const char *put_old);
long sys_mount(const char *source, const char *target, const char *fstype,
               unsigned long flags, const void *data);
long sys_umount2(const char *target, int flags);
long sys_exit_group(int code) __attribute__((noreturn));

/* _start(nolibc) 与本地编译分支共用。返回不了，永不返回。 */
void aether_exit(long code) __attribute__((noreturn));

#endif /* AETHER_SYSCALL_SHIM_H */
