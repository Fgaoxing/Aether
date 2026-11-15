/* modload.h — PID 1 早期模块加载（对应 mkinitcpio 的 udev 钩子） */
#ifndef AETHER_MODLOAD_H
#define AETHER_MODLOAD_H

/* 读取 /etc/aether/modules.order 并按序 finit_module；返回成功加载数。
 * 无列表/无硬件：静默返回 0。失败只记录，不中断启动。 */
int aether_load_modules(void);

#endif /* AETHER_MODLOAD_H */
