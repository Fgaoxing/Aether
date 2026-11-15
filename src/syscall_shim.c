/*
 * syscall_shim.c — 双分支实现（§4.3 / §5）
 * nolibc asm 模板按需求文档 4.1/5.1 的 ABI：
 *   x86_64 : syscall    rax=号 rdi/rsi/rdx/r10/r8/r9=参
 *   aarch64: svc #0     x8=号  x0~x5=参
 *   i386   : int 0x80   eax=号 ebx/ecx/edx/esi/edi/ebp=参（mmap 走 mmap2，offset 单位为 4K 页）
 *   riscv64: ecall      a7=号  a0~a5=参
 */
#include "syscall_shim.h"

#include <errno.h>

#if defined(USE_LIBC_SYSCALL)
#include <string.h> /* memset */
#include <signal.h> /* sigaction */
#endif

/* libc 分支：把 glibc syscall(3) 的 -1/errno 翻译回内核约定（负 -errno），
 * 使两分支所有调用方的判负逻辑一致 */
#if defined(USE_LIBC_SYSCALL)
long aether_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
    errno = 0;
    long r = syscall(nr, a0, a1, a2, a3, a4, a5);
    if (r == -1)
        return -(long)errno;
    return r;
}
#endif

#if !defined(USE_LIBC_SYSCALL)

long aether_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
#if defined(__x86_64__)
    long ret;
    register long r10 __asm__("r10") = a3;
    register long r8  __asm__("r8")  = a4;
    register long r9  __asm__("r9")  = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
#elif defined(__aarch64__)
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory");
    return x0;
#elif defined(__i386__)
    long ret;
    register long ebx __asm__("ebx") = a0;
    register long ecx __asm__("ecx") = a1;
    register long edx __asm__("edx") = a2;
    register long esi __asm__("esi") = a3;
    register long edi __asm__("edi") = a4;
    register long ebp __asm__("ebp") = a5;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "r"(ebx), "r"(ecx), "r"(edx), "r"(esi), "r"(edi), "r"(ebp)
                     : "memory");
    return ret;
#elif defined(__riscv) && (__riscv_xlen == 64)
    register long ra7 __asm__("a7") = nr;
    register long ra0 __asm__("a0") = a0;
    register long ra1 __asm__("a1") = a1;
    register long ra2 __asm__("a2") = a2;
    register long ra3 __asm__("a3") = a3;
    register long ra4 __asm__("a4") = a4;
    register long ra5 __asm__("a5") = a5;
    __asm__ volatile("ecall"
                     : "+r"(ra0)
                     : "r"(ra1), "r"(ra2), "r"(ra3), "r"(ra4), "r"(ra5), "r"(ra7)
                     : "memory");
    return ra0;
#else
    /* syscall_shim.h 已 #error，此分支不可达 */
    (void)nr; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -38; /* -ENOSYS */
#endif
}

#endif /* !USE_LIBC_SYSCALL */

long sys_read(int fd, void *buf, size_t count)
{
    return syscall_nr(AETHER_NR(read), (long)fd, (long)buf, (long)count, 0, 0, 0);
}

long sys_write(int fd, const void *buf, size_t count)
{
    return syscall_nr(AETHER_NR(write), (long)fd, (long)buf, (long)count, 0, 0, 0);
}

long sys_open(const char *path, int flags, int mode)
{
#if AETHER_OPEN_IS_DIRECT
    return syscall_nr(AETHER_NR(open), (long)path, (long)flags, (long)mode, 0, 0, 0);
#else
    /* aarch64 / riscv64 没有 open(2)（asm-generic），统一走 openat(AT_FDCWD) */
    return syscall_nr(AETHER_NR(openat), AT_FDCWD, (long)path, (long)flags, (long)mode, 0, 0);
#endif
}

long sys_close(int fd)
{
    return syscall_nr(AETHER_NR(close), (long)fd, 0, 0, 0, 0, 0);
}

long sys_lseek(int fd, long offset, int whence)
{
    return syscall_nr(AETHER_NR(lseek), (long)fd, offset, (long)whence, 0, 0, 0);
}

long sys_mmap(void *addr, size_t length, int prot, int flags, int fd, long offset)
{
#if defined(__i386__)
    /* int 0x80 下 __NR_mmap(90) 走内存传参，无论 libc 还是 nolibc 分支一律改用 mmap2(192)：
     * offset 以 4K 页为单位 */
    return syscall_nr(AETHER_NR(mmap2), (long)addr, (long)length, (long)prot,
                      (long)flags, (long)fd, offset >> 12);
#else
    return syscall_nr(AETHER_NR(mmap), (long)addr, (long)length, (long)prot,
                      (long)flags, (long)fd, offset);
#endif
}

long sys_munmap(void *addr, size_t length)
{
    return syscall_nr(AETHER_NR(munmap), (long)addr, (long)length, 0, 0, 0, 0);
}

long sys_ioctl(int fd, unsigned long request, unsigned long arg)
{
    return syscall_nr(AETHER_NR(ioctl), (long)fd, (long)request, (long)arg, 0, 0, 0);
}

long sys_dup(int oldfd)
{
    return syscall_nr(AETHER_NR(dup), (long)oldfd, 0, 0, 0, 0, 0);
}

long sys_dup2(int oldfd, int newfd)
{
#if defined(USE_LIBC_SYSCALL) && !defined(SYS_dup2)
    return syscall_nr(SYS_dup3, (long)oldfd, (long)newfd, 0, 0, 0, 0);
#elif !defined(USE_LIBC_SYSCALL) && !defined(__NR_dup2)
    return syscall_nr(__NR_dup3, (long)oldfd, (long)newfd, 0, 0, 0, 0);
#else
    return syscall_nr(AETHER_NR(dup2), (long)oldfd, (long)newfd, 0, 0, 0, 0);
#endif
}

long sys_pipe(int fds[2])
{
#if defined(USE_LIBC_SYSCALL) && !defined(SYS_pipe)
    return syscall_nr(SYS_pipe2, (long)fds, 0, 0, 0, 0, 0);
#elif !defined(USE_LIBC_SYSCALL) && !defined(__NR_pipe)
    return syscall_nr(__NR_pipe2, (long)fds, 0, 0, 0, 0, 0);
#else
    return syscall_nr(AETHER_NR(pipe), (long)fds, 0, 0, 0, 0, 0);
#endif
}

long sys_fork(void)
{
#if defined(USE_LIBC_SYSCALL) && defined(SYS_fork)
    return syscall_nr(SYS_fork, 0, 0, 0, 0, 0, 0);
#elif !defined(USE_LIBC_SYSCALL) && defined(__NR_fork)
    return syscall_nr(__NR_fork, 0, 0, 0, 0, 0, 0);
#else
    /* aarch64 / riscv64 没有 fork(2)：clone(SIGCHLD) 语义等同 fork */
    return syscall_nr(AETHER_NR(clone), SIGCHLD, 0, 0, 0, 0, 0);
#endif
}

long sys_execve(const char *path, char *const argv[], char *const envp[])
{
    return syscall_nr(AETHER_NR(execve), (long)path, (long)argv, (long)envp, 0, 0, 0);
}

long sys_wait4(int pid, int *status, int options, void *rusage)
{
    return syscall_nr(AETHER_NR(wait4), (long)pid, (long)status, (long)options,
                      (long)rusage, 0, 0);
}

long sys_unlinkat(int dirfd, const char *path, int flags)
{
    return syscall_nr(AETHER_NR(unlinkat), (long)dirfd, (long)path, (long)flags, 0, 0, 0);
}

long sys_unlink(const char *path)
{
    return sys_unlinkat(AT_FDCWD, path, 0);
}

long sys_syslog(int type, char *buf, int len)
{
    return syscall_nr(AETHER_NR(syslog), (long)type, (long)buf, (long)len, 0, 0, 0);
}

long sys_getdents64(int fd, void *buf, size_t count)
{
    return syscall_nr(AETHER_NR(getdents64), (long)fd, (long)buf, (long)count, 0, 0, 0);
}

long sys_mkdirat(int dirfd, const char *path, int mode)
{
    return syscall_nr(AETHER_NR(mkdirat), (long)dirfd, (long)path, (long)mode, 0, 0, 0);
}

long sys_faccessat(int dirfd, const char *path, int mode, int flags)
{
    return syscall_nr(AETHER_NR(faccessat), (long)dirfd, (long)path, (long)mode, (long)flags, 0, 0);
}

long sys_geteuid(void)
{
    return syscall_nr(AETHER_NR(geteuid), 0, 0, 0, 0, 0, 0);
}

long sys_getpid(void)
{
    return syscall_nr(AETHER_NR(getpid), 0, 0, 0, 0, 0, 0);
}

long sys_getppid(void)
{
    return syscall_nr(AETHER_NR(getppid), 0, 0, 0, 0, 0, 0);
}

long sys_nanosleep(const void *req, void *rem)
{
    return syscall_nr(AETHER_NR(nanosleep), (long)req, (long)rem, 0, 0, 0, 0);
}

long sys_socket(int domain, int type, int protocol)
{
    return syscall_nr(AETHER_NR(socket), (long)domain, (long)type, (long)protocol, 0, 0, 0);
}

long sys_bind(int fd, const void *addr, unsigned int addrlen)
{
    return syscall_nr(AETHER_NR(bind), (long)fd, (long)addr, (long)addrlen, 0, 0, 0);
}

long sys_recvfrom(int fd, void *buf, size_t len, int flags, void *src, void *srclen)
{
    return syscall_nr(AETHER_NR(recvfrom), (long)fd, (long)buf, (long)len, (long)flags,
                      (long)src, (long)srclen);
}

long sys_mknodat(int dirfd, const char *path, unsigned int mode, unsigned long long dev)
{
    return syscall_nr(AETHER_NR(mknodat), (long)dirfd, (long)path, (long)mode, (long)dev, 0, 0);
}

long sys_chmod(const char *path, unsigned int mode)
{
    /* fchmodat(AT_FDCWD, path, mode, 0) — 四架构统一 */
    return syscall_nr(AETHER_NR(fchmodat), AT_FDCWD, (long)path, (long)mode, 0, 0, 0);
}

long sys_chown(const char *path, unsigned int owner, unsigned int group)
{
    /* fchownat(AT_FDCWD, path, owner, group, 0) */
    return syscall_nr(AETHER_NR(fchownat), AT_FDCWD, (long)path, (long)owner, (long)group, 0, 0);
}

long sys_readlink(const char *path, char *buf, size_t bufsiz)
{
    /* readlinkat(AT_FDCWD, path, buf, bufsiz) */
    return syscall_nr(AETHER_NR(readlinkat), AT_FDCWD, (long)path, (long)buf, (long)bufsiz, 0, 0);
}

long sys_utimensat(int dirfd, const char *path, const void *times, int flags)
{
    /* times == NULL → 设为当前时间 */
    return syscall_nr(AETHER_NR(utimensat), (long)dirfd, (long)path, (long)times, (long)flags, 0, 0);
}

long sys_renameat(int olddirfd, const char *old, int newdirfd, const char *new)
{
    return syscall_nr(AETHER_NR(renameat), (long)olddirfd, (long)old, (long)newdirfd,
                      (long)new, 0, 0);
}

long sys_ftruncate(int fd, long length)
{
    return syscall_nr(AETHER_NR(ftruncate), (long)fd, length, 0, 0, 0, 0);
}

long sys_sync(void)
{
    return syscall_nr(AETHER_NR(sync), 0, 0, 0, 0, 0, 0);
}

long sys_reboot(unsigned int magic1, unsigned int magic2, unsigned int cmd, void *arg)
{
    return syscall_nr(AETHER_NR(reboot), (long)magic1, (long)magic2, (long)cmd, (long)arg, 0, 0);
}

long sys_finit_module(int fd, const char *param_values, int flags)
{
    return syscall_nr(AETHER_NR(finit_module), (long)fd, (long)param_values, (long)flags, 0, 0, 0);
}

long sys_fsync(int fd)
{
    return syscall_nr(AETHER_NR(fsync), (long)fd, 0, 0, 0, 0, 0);
}

long sys_flock(int fd, int operation)
{
    return syscall_nr(AETHER_NR(flock), (long)fd, (long)operation, 0, 0, 0, 0);
}

long sys_setpgid(int pid, int pgid)
{
    return syscall_nr(AETHER_NR(setpgid), (long)pid, (long)pgid, 0, 0, 0, 0);
}

long sys_getpgid(int pid)
{
    return syscall_nr(AETHER_NR(getpgid), (long)pid, 0, 0, 0, 0, 0);
}

long sys_kill(int pid, int sig)
{
    return syscall_nr(AETHER_NR(kill), (long)pid, (long)sig, 0, 0, 0, 0);
}

long sys_listen(int fd, int backlog)
{
    return syscall_nr(AETHER_NR(listen), (long)fd, (long)backlog, 0, 0, 0, 0);
}

long sys_accept4(int fd, void *addr, void *addrlen, int flags)
{
    return syscall_nr(AETHER_NR(accept4), (long)fd, (long)addr, (long)addrlen, (long)flags, 0, 0);
}

long sys_setsockopt(int fd, int level, int optname, const void *optval, unsigned int optlen)
{
    return syscall_nr(AETHER_NR(setsockopt), (long)fd, (long)level, (long)optname,
                      (long)optval, (long)optlen, 0);
}

long sys_sendto(int fd, const void *buf, size_t len, int flags, const void *dest, unsigned int destlen)
{
    return syscall_nr(AETHER_NR(sendto), (long)fd, (long)buf, (long)len, (long)flags,
                      (long)dest, (long)destlen);
}

long aether_statx(int dirfd, const char *path, int flags, unsigned int mask, void *out)
{
    return syscall_nr(AETHER_NR(statx), (long)dirfd, (long)path, (long)flags,
                      (long)mask, (long)out, 0);
}

long aether_signal(int signum, void (*handler)(int))
{
#if defined(USE_LIBC_SYSCALL)
    /* libc 分支：用 libc sigaction（sigset_t 布局与内核不同，不能混用） */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    return sigaction(signum, &sa, 0) == 0 ? 0 : -1;
#else
    /* nolibc 分支：内核 sigaction 布局 + rt_sigaction */
    struct aether_sigaction sa;
    sa.sa_handler = (void *)handler;
#if defined(__x86_64__) || defined(__i386__)
    sa.sa_restorer = (void *)aether_sig_restorer; /* 见 nolibc/sigreturn_*.S */
    sa.sa_flags = AETHER_SA_RESTORER;
#else
    sa.sa_flags = 0;
#endif
#if defined(__i386__)
    sa.sa_mask[0] = 0; sa.sa_mask[1] = 0;
#else
    sa.sa_mask = 0;
#endif
    long r = syscall_nr(AETHER_NR(rt_sigaction), (long)signum, (long)&sa, 0, AETHER_SIGSETSIZE, 0, 0);
    return AETHER_IS_ERR(r) ? -1 : 0;
#endif
}

long sys_chroot(const char *path)
{
    return syscall_nr(AETHER_NR(chroot), (long)path, 0, 0, 0, 0, 0);
}

long sys_chdir(const char *path)
{
    return syscall_nr(AETHER_NR(chdir), (long)path, 0, 0, 0, 0, 0);
}

long sys_getcwd(char *buf, size_t size)
{
    return syscall_nr(AETHER_NR(getcwd), (long)buf, (long)size, 0, 0, 0, 0);
}

long sys_pivot_root(const char *new_root, const char *put_old)
{
    return syscall_nr(AETHER_NR(pivot_root), (long)new_root, (long)put_old, 0, 0, 0, 0);
}

long sys_mount(const char *source, const char *target, const char *fstype,
               unsigned long flags, const void *data)
{
    return syscall_nr(AETHER_NR(mount), (long)source, (long)target, (long)fstype,
                      (long)flags, (long)data, 0);
}

long sys_umount2(const char *target, int flags)
{
    return syscall_nr(AETHER_NR(umount2), (long)target, (long)flags, 0, 0, 0, 0);
}

long sys_exit_group(int code)
{
    aether_exit(code);
}

void aether_exit(long code)
{
    syscall_nr(AETHER_NR(exit_group), code, 0, 0, 0, 0, 0);
    for (;;) { /* exit_group 必不返回，防御性死循环 */ }
}
