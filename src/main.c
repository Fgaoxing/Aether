/*
 * main.c — aether 入口分发（§2 / §9）
 * BusyBox 式：一个静态 ELF，多个入口，靠 argv[0] 的 basename 分发到 applet（见 applets.c）。
 * 主入口（aether / sh / ash）裸进入救援 shell；`aether <applet> ...` 执行 applet。
 *
 * 例外说明：顶层 --help / --version 直接特判，不经 shell/argparse。
 * envp 说明：两个分支一致——libc 由运行时提供，nolibc 由 _start 摆到 rdi/rsi/rdx。
 */
#include <stddef.h>

#include "applets.h"
#include "boot.h"
#include "elog.h"
#include "syscall_shim.h"
#include "util.h"

#ifndef AETHER_VERSION
#define AETHER_VERSION "0.2.0-m1"
#endif

#define EXIT_NOTIMPL 127

static int run_help(char **envp)
{
    char *a[] = { (char *)"help", 0 };
    return aether_applet_run("help", 1, a, envp);
}

int main(int argc, char **argv, char **envp)
{
    const char *name;

    /* PID 1（initramfs 启动入口）：走初始化流程，失败进拯救模式 */
    if (sys_getpid() == 1)
        return aether_init_main(envp);

    if (argc < 1 || argv[0] == 0) { /* 异常现场，按 init 惯例跑 help */
        return run_help(envp);
    }

    name = aether_base_name(argv[0]);

    /* 顶层特判（不经 shell/argparse） */
    if (argc >= 2) {
        if (aether_streq(argv[1], "--help") || aether_streq(argv[1], "-h"))
            return run_help(envp);
        if (aether_streq(argv[1], "--version") || aether_streq(argv[1], "-V")) {
            elog_info("aether %s", AETHER_VERSION);
            return 0;
        }
    }

    /* 主入口（aether / sh / ash）：`aether <applet> ...` 分发 applet；裸 `aether` 落 shell */
    if (aether_streq(name, "aether") || aether_streq(name, "sh") || aether_streq(name, "ash")) {
        if (argc >= 2) {
            const char *sub = argv[1];
            int rc = aether_applet_run(sub, argc - 1, argv + 1, envp);
            if (rc >= 0) return rc;
            elog_err("aether: unknown applet '%s'（`aether help` 查看列表）", sub);
            return EXIT_NOTIMPL;
        }
        return shell_main(argc, argv, envp);
    }

    /* 硬链接调用：argv[0] 本身就是 applet 名 */
    {
        int r = aether_applet_run(name, argc, argv, envp);
        if (r >= 0) return r;
    }

    elog_err("aether: unknown applet '%s'（`aether help` 查看列表）", name);
    return EXIT_NOTIMPL;
}
