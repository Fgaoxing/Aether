/*
 * uevent.h — 内核 KOBJECT_UEVENT 处理（§1 devtmpfs 补齐 / §12 热插拔）
 * uevent_pump：启动早期一次性补 devtmpfs 缺口（处理积压事件）；
 * uevent_monitor：常驻监听，动态增删 /dev 设备节点（udev/mdev 风格热插拔）。
 */
#ifndef AETHER_UEVENT_H
#define AETHER_UEVENT_H

/* 一次性处理积压 uevent（最多 max_ms 毫秒或队列空）。0 成功，-1 bind 失败 */
int aether_uevent_pump(int max_ms);
/* 常驻监听 uevent；收到 SIGINT/SIGTERM 时退出。返回 0 */
int aether_uevent_monitor(void);

#endif /* AETHER_UEVENT_H */
