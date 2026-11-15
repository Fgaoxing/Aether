/*
 * elog.h — 统一错误/日志输出（§9）
 * 只经 sys_write 直接写 fd，可用于 libc / nolibc 两个分支；不经过 stdio。
 * 格式符：%d %i %u %x %c %s，长度前缀 l / ll / z（%lu、%lld、%zu ...），%% 输出 %。
 * 宽度（v1 相对原设计的小扩展，用于表格对齐）：%10s %-10s %10lld %-8u ... 不足补空格；
 * 不支持精度/星号宽度/多字节宽度（按字节计）。
 */
#ifndef AETHER_ELOG_H
#define AETHER_ELOG_H

void elog_write(int fd, const char *fmt, ...);
void elog_info(const char *fmt, ...); /* stdout，自动补 '\n' */
void elog_err(const char *fmt, ...);  /* stderr，自动补 '\n' */
void elog_die(int code, const char *fmt, ...); /* stderr + '\n' + aether_exit(code) */

#endif /* AETHER_ELOG_H */
