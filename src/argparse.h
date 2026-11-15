/*
 * argparse.h — GNU 风格子集参数解析状态机（§6）
 *
 * 支持：无参短选项(-v)、带参短选项(-b 0001 / -b0001)、短选项聚合(-vv)、
 *       位置参数、`--` 终止标记。
 * 不支持（v1）：长选项、可选参数、GNU 风格超长错误文案。
 *
 * 组合合法性（如 -B 必须搭配 -b、-o 与 -B 不允许同时出现）由调用方在解析后做，
 * 用 aether_opt_count() 查询；见 efibootmgr 实现（§10）。
 * positionals 数组从传入的 arena 分配（§8.2 A）。
 */
#ifndef AETHER_ARGPARSE_H
#define AETHER_ARGPARSE_H

#include <stddef.h>

typedef struct mem_arena mem_arena_t; /* §8：positionals 从 arena 分配 */

typedef struct {
    char        short_name; /* 短选项字符；0 表示本项未启用 */
    int         takes_arg;  /* 1 = 带参数 */
    const char *value;      /* 解析后：参数值（takes_arg 项） */
    int         count;      /* 出现次数 */
    const char *help;       /* usage 输出用 */
} aether_option_t;

typedef struct {
    const char **positionals;     /* arena 分配，无需手动释放 */
    size_t       positional_count;
} aether_args_t;

/* 成功返回 0；失败打印错误信息到 stderr 并返回 -1 */
int aether_parse_args(const char *prog, aether_option_t *opts, size_t nopts,
                      int argc, char **argv, mem_arena_t *arena, aether_args_t *out);

/* 查询：返回出现次数；字符未注册为选项时返回 -1 */
int aether_opt_count(const aether_option_t *opts, size_t nopts, char short_name);

/* 查询：返回参数值；未设置/非带参选项/未注册时返回 0 */
const char *aether_opt_value(const aether_option_t *opts, size_t nopts, char short_name);

void aether_usage(const char *prog, const char *synopsis,
                  const aether_option_t *opts, size_t nopts);

#endif /* AETHER_ARGPARSE_H */
