/* test_argparse.c — aether_parse_args / 查询接口的单元测试 */
#include <stdio.h>
#include <string.h>

#include "argparse.h"
#include "mem.h"

static int failures, checks;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %d: %s\n", __LINE__, #cond); } \
} while (0)

static void mk_argv(char **argv, const char **src, int n)
{
    for (int i = 0; i < n; i++) argv[i] = (char *)src[i];
}

int main(void)
{
    aether_option_t opts[2] = {
        { 'v', 0, 0, 0, "verbose" },
        { 'b', 1, 0, 0, "boot entry id" },
    };
    const size_t nopts = sizeof(opts) / sizeof(opts[0]);

    mem_arena_t *arena = mem_arena_create();
    CHECK(arena != 0);

    /* 用例 1：空参数 */
    {
        for (size_t i = 0; i < nopts; i++) { opts[i].value = 0; opts[i].count = 0; }
        char *argv[] = { "prog" };
        aether_args_t out;
        CHECK(aether_parse_args("prog", opts, nopts, 1, argv, arena, &out) == 0);
        CHECK(out.positional_count == 0);
        CHECK(aether_opt_count(opts, nopts, 'v') == 0);
    }
    mem_arena_reset(arena);

    /* 用例 2：-v 开关 */
    {
        for (size_t i = 0; i < nopts; i++) { opts[i].value = 0; opts[i].count = 0; }
        const char *src[] = { "prog", "-v" };
        char *argv[2]; mk_argv(argv, src, 2);
        aether_args_t out;
        CHECK(aether_parse_args("prog", opts, nopts, 2, argv, arena, &out) == 0);
        CHECK(aether_opt_count(opts, nopts, 'v') == 1);
        CHECK(aether_opt_count(opts, nopts, 'b') == 0);
    }
    mem_arena_reset(arena);

    /* 用例 3：-b 0001（分离参数） */
    {
        for (size_t i = 0; i < nopts; i++) { opts[i].value = 0; opts[i].count = 0; }
        const char *src[] = { "prog", "-b", "0001" };
        char *argv[3]; mk_argv(argv, src, 3);
        aether_args_t out;
        CHECK(aether_parse_args("prog", opts, nopts, 3, argv, arena, &out) == 0);
        CHECK(aether_opt_count(opts, nopts, 'b') == 1);
        CHECK(aether_opt_value(opts, nopts, 'b') && strcmp(aether_opt_value(opts, nopts, 'b'), "0001") == 0);
    }
    mem_arena_reset(arena);

    /* 用例 4：-b0001（连写参数） */
    {
        for (size_t i = 0; i < nopts; i++) { opts[i].value = 0; opts[i].count = 0; }
        const char *src[] = { "prog", "-b0001" };
        char *argv[2]; mk_argv(argv, src, 2);
        aether_args_t out;
        CHECK(aether_parse_args("prog", opts, nopts, 2, argv, arena, &out) == 0);
        CHECK(aether_opt_value(opts, nopts, 'b') && strcmp(aether_opt_value(opts, nopts, 'b'), "0001") == 0);
    }
    mem_arena_reset(arena);

    /* 用例 5：-- 终止 */
    {
        for (size_t i = 0; i < nopts; i++) { opts[i].value = 0; opts[i].count = 0; }
        const char *src[] = { "prog", "--", "-v", "file" };
        char *argv[4]; mk_argv(argv, src, 4);
        aether_args_t out;
        CHECK(aether_parse_args("prog", opts, nopts, 4, argv, arena, &out) == 0);
        CHECK(aether_opt_count(opts, nopts, 'v') == 0);
        CHECK(out.positional_count == 2);
        CHECK(strcmp(out.positionals[0], "-v") == 0 && strcmp(out.positionals[1], "file") == 0);
    }
    mem_arena_reset(arena);

    /* 用例 6：未知选项 → -1 */
    {
        for (size_t i = 0; i < nopts; i++) { opts[i].value = 0; opts[i].count = 0; }
        const char *src[] = { "prog", "-x" };
        char *argv[2]; mk_argv(argv, src, 2);
        aether_args_t out;
        CHECK(aether_parse_args("prog", opts, nopts, 2, argv, arena, &out) == -1);
    }
    mem_arena_reset(arena);

    /* 用例 7：-b 缺参数 → -1 */
    {
        for (size_t i = 0; i < nopts; i++) { opts[i].value = 0; opts[i].count = 0; }
        const char *src[] = { "prog", "-b" };
        char *argv[2]; mk_argv(argv, src, 2);
        aether_args_t out;
        CHECK(aether_parse_args("prog", opts, nopts, 2, argv, arena, &out) == -1);
    }
    mem_arena_reset(arena);

    /* 用例 8：短选项聚合 -vv */
    {
        for (size_t i = 0; i < nopts; i++) { opts[i].value = 0; opts[i].count = 0; }
        const char *src[] = { "prog", "-vv" };
        char *argv[2]; mk_argv(argv, src, 2);
        aether_args_t out;
        CHECK(aether_parse_args("prog", opts, nopts, 2, argv, arena, &out) == 0);
        CHECK(aether_opt_count(opts, nopts, 'v') == 2);
    }
    mem_arena_reset(arena);

    /* 用例 9：聚合里带参选项吃掉剩余 -vb0005 */
    {
        for (size_t i = 0; i < nopts; i++) { opts[i].value = 0; opts[i].count = 0; }
        const char *src[] = { "prog", "-vb0005" };
        char *argv[2]; mk_argv(argv, src, 2);
        aether_args_t out;
        CHECK(aether_parse_args("prog", opts, nopts, 2, argv, arena, &out) == 0);
        CHECK(aether_opt_count(opts, nopts, 'v') == 1);
        CHECK(aether_opt_value(opts, nopts, 'b') && strcmp(aether_opt_value(opts, nopts, 'b'), "0005") == 0);
    }
    mem_arena_reset(arena);

    /* 用例 10：位置参数与选项交错 */
    {
        for (size_t i = 0; i < nopts; i++) { opts[i].value = 0; opts[i].count = 0; }
        const char *src[] = { "prog", "file1", "-v", "file2" };
        char *argv[4]; mk_argv(argv, src, 4);
        aether_args_t out;
        CHECK(aether_parse_args("prog", opts, nopts, 4, argv, arena, &out) == 0);
        CHECK(aether_opt_count(opts, nopts, 'v') == 1);
        CHECK(out.positional_count == 2);
        CHECK(strcmp(out.positionals[0], "file1") == 0);
        CHECK(strcmp(out.positionals[1], "file2") == 0);
    }
    mem_arena_reset(arena);

    /* 用例 11：单独的 "-" 是位置参数（stdin 约定） */
    {
        for (size_t i = 0; i < nopts; i++) { opts[i].value = 0; opts[i].count = 0; }
        const char *src[] = { "prog", "-" };
        char *argv[2]; mk_argv(argv, src, 2);
        aether_args_t out;
        CHECK(aether_parse_args("prog", opts, nopts, 2, argv, arena, &out) == 0);
        CHECK(out.positional_count == 1 && strcmp(out.positionals[0], "-") == 0);
    }
    mem_arena_reset(arena);

    /* 用例 12：未注册的查询返回 -1 / 0 */
    CHECK(aether_opt_count(opts, nopts, 'q') == -1);
    CHECK(aether_opt_value(opts, nopts, 'q') == 0);

    mem_arena_destroy(arena);

    printf("test_argparse: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
