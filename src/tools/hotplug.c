/*
 * hotplug.c — udev/mdev 风格的内核 uevent 热插拔守护
 *   hotplug          常驻监听：插入/拔出即建/删 /dev 节点（Ctrl-C 退出）
 *   hotplug --once   只处理积压事件后退出（等同 init 早期 pump）
 * 单用户 rescue 语义；节点 0660，root 所有。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "uevent.h"
#include "util.h"

int tool_hotplug(int argc, char **argv, char **envp)
{
    (void)envp;
    int once = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (aether_streq(a, "--once") || aether_streq(a, "-1")) once = 1;
        else if (aether_streq(a, "--help") || aether_streq(a, "-h")) {
            elog_info("usage: hotplug [--once]   常驻监听 uevent 并建/删 /dev 节点（--once 只处理积压）");
            return 0;
        } else {
            elog_err("hotplug: unknown option '%s'", a);
            return 2;
        }
    }
    if (once) return aether_uevent_pump(1500) == 0 ? 0 : 1;
    return aether_uevent_monitor();
}
