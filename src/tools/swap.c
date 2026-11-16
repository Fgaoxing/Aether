/*
 * swap.c — swapon / swapoff（启用/停用 swap；休眠前必须启用）
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "shell/shell_internal.h"
#include "syscall_shim.h"
#include "util.h"

int tool_swapon(int argc, char **argv, char **envp)
{
    (void)envp;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (aether_streq(a, "-h") || aether_streq(a, "--help")) {
            elog_info("usage: swapon <device|file>   enable swap (needs root)");
            return 0;
        }
        long r = sys_swapon(a, 0);
        if (AETHER_IS_ERR(r)) {
            elog_err("swapon: %s: cannot enable (error %d)", a, AETHER_ERR_CODE(r));
            return 1;
        }
        elog_info("swapon: %s enabled", a);
    }
    if (argc < 2) { elog_info("usage: swapon <device|file>   enable swap (needs root)"); return 2; }
    return 0;
}

int tool_swapoff(int argc, char **argv, char **envp)
{
    (void)envp;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (aether_streq(a, "-h") || aether_streq(a, "--help")) {
            elog_info("usage: swapoff <device|file>   disable swap (needs root)");
            return 0;
        }
        long r = sys_swapoff(a);
        if (AETHER_IS_ERR(r)) {
            elog_err("swapoff: %s: cannot disable (error %d)", a, AETHER_ERR_CODE(r));
            return 1;
        }
        elog_info("swapoff: %s disabled", a);
    }
    if (argc < 2) { elog_info("usage: swapoff <device|file>   disable swap (needs root)"); return 2; }
    return 0;
}
