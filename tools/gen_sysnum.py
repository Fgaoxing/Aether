#!/usr/bin/env python3
"""
regenerate src/sysnum/syscall_<arch>.h from the local Linux UAPI headers.
用法：python3 tools/gen_sysnum.py   （需先有 asm/*.h、asm-generic/unistd.h）
原则：不自创编号；i386 的 mmap2/newfstatat 在表尾人工补充注释行（见 5.1 / unistd_32.h）。
"""
import re, sys, datetime

SYMBOLS = ["read","write","open","openat","close","lseek","mmap","mprotect","munmap",
           "ioctl","fcntl","dup","dup2","dup3","pipe","pipe2","fork","vfork","clone","clone3",
           "execve","wait4","exit","exit_group","getpid","getppid","gettid",
           "getuid","geteuid","getgid","getegid","chroot","chdir","getcwd","pivot_root",
           "mount","umount2","faccessat","faccessat2","newfstatat","statx",
           "unlinkat","syslog","rt_sigaction","rt_sigreturn","getdents64","mkdirat",
           "nanosleep","socket","bind","recvfrom","mknodat","getpid",
           "fchmodat","fchownat","readlinkat","symlinkat","setsid","swapon","swapoff","utimensat","renameat","renameat2",
           "ftruncate","truncate","fsync","sync","flock",
           "setpgid","getpgid","kill","listen","accept4","setsockopt","sendto","reboot","finit_module","init_module"]

def parse_x86(path):
    mp = {}
    for line in open(path):
        m = re.match(r'#define\s+__NR_([A-Za-z0-9_]+)\s+(\d+)\s*$', line)
        if m:
            mp[m.group(1)] = int(m.group(2))
    return mp

def parse_generic(path):
    raw = {}
    alias = {}
    for line in open(path):
        m = re.match(r'#define\s+__NR3264_([A-Za-z0-9_]+)\s+(\d+)\s*$', line)
        if m:
            raw['3264:' + m.group(1)] = int(m.group(2)); continue
        m = re.match(r'#define\s+__NR_([A-Za-z0-9_]+)\s+(\d+)\s*$', line)
        if m:
            raw[m.group(1)] = int(m.group(2)); continue
        m = re.match(r'#define\s+__NR_([A-Za-z0-9_]+)\s+__NR(3264)?_([A-Za-z0-9_]+)\s*$', line)
        if m:
            prefix = '3264:' if m.group(2) else ''
            alias[m.group(1)] = prefix + m.group(3)
    def resolve(name, depth=0):
        if depth > 5: return None
        if name in raw: return raw[name]
        if name in alias: return resolve(alias[name], depth + 1)
        return None
    return {s: resolve(s) for s in SYMBOLS if resolve(s) is not None}

SPECS = [
    ("x86_64",  "/usr/include/asm/unistd_64.h",       parse_x86,     "asm/unistd_64.h"),
    ("i386",    "/usr/include/asm/unistd_32.h",       parse_x86,     "asm/unistd_32.h"),
    ("aarch64", "/usr/include/asm-generic/unistd.h",  parse_generic, "asm-generic/unistd.h"),
    ("riscv64", "/usr/include/asm-generic/unistd.h",  parse_generic, "asm-generic/unistd.h (RV64, __BITS_PER_LONG==64)"),
]

for arch, path, fn, src_name in SPECS:
    table = fn(path)
    if arch == "riscv64":
        # asm-generic exposes renameat only under __ARCH_WANT_RENAMEAT.
        table.pop("renameat", None)
    if arch in ("x86_64", "i386"):
        table = {s: table[s] for s in SYMBOLS if s in table}
    out = []
    out.append("/*")
    out.append(f" * sysnum for {arch} — 启动救援 Shell (aether)")
    out.append(f" * 生成自 {src_name}（本机 Linux UAPI 头文件，{datetime.date.today().isoformat()}），不自创编号。")
    out.append(" * nolibc 分支专用：本地编译分支（USE_HOST_LIBC=ON）不使用本文件。")
    if arch in ("aarch64", "riscv64"):
        out.append(" * aarch64/riscv64 沿用 asm-generic 编号表，不存在 open/fork/vfork/pipe/dup2（见需求文档 5.1）")
        out.append(" * fork/vfork 用 clone(SIGCHLD, ...) 模拟；pipe 用 pipe2(fds, 0) 模拟；dup2 用 dup3(fd, fd2, 0) 模拟。")
    out.append(" */")
    out.append(f"#ifndef AETHER_SYSNUM_{arch.upper()}_H")
    out.append(f"#define AETHER_SYSNUM_{arch.upper()}_H")
    out.append("")
    for s in SYMBOLS:
        if s in table:
            out.append(f"#define __NR_{s:<15} {table[s]}")
    if arch == "i386":
        out.append("/* i386 int 0x80 下 __NR_mmap(90) 走内存传参，寄存器传参一律改用 __NR_mmap2(192)：offset 以 4K 页为单位 */")
        out.append("#define __NR_mmap2           192")
        out.append("/* i386 无 __NR_newfstatat：32 位内核表位 300 即 fstatat64（unistd_32.h 暴露为 __NR_fstatat64），语义等同 32 位 newfstatat */")
        out.append("#define __NR_newfstatat 300")
    out.append("")
    out.append(f"#endif /* AETHER_SYSNUM_{arch.upper()}_H */")
    out.append("")
    dst = f"src/sysnum/syscall_{arch}.h"
    open(dst, "w").write("\n".join(out))
    missing = [s for s in SYMBOLS if s not in table]
    print(f"{arch}: {len(table)} symbols, missing: {missing}")
