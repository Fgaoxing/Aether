/*
 * find.c — find（递归查找文件）
 * find [path...] [-name glob] [-type f|d|l] [-maxdepth N]
 * 输出：每个匹配的路径一行（救援够用，不必与 GNU find 输出一致）
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "statx.h"
#include "syscall_shim.h"
#include "sysfsutil.h"
#include "util.h"

/* fpath.c 提供的 stat 包装（非静态） */
int aether_stat(const char *path, aether_statx_t *st, int flags);

/* glob 匹配（* ? []） */
static int glob_match(const char *pattern, const char *str)
{
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*str) {
                if (glob_match(pattern, str)) return 1;
                str++;
            }
            return 0;
        }
        if (*pattern == '?') {
            if (!*str) return 0;
            pattern++;
            str++;
            continue;
        }
        if (*pattern == '[') {
            pattern++;
            int negate = 0;
            if (*pattern == '!' || *pattern == '^') { negate = 1; pattern++; }
            if (!*str) return 0;
            int matched = 0;
            while (*pattern && *pattern != ']') {
                if (*pattern == *str) matched = 1;
                pattern++;
            }
            if (*pattern == ']') pattern++;
            if (matched == negate) return 0;
            str++;
            continue;
        }
        if (*pattern != *str) return 0;
        pattern++;
        str++;
    }
    return *str == '\0';
}

typedef struct {
    const char *name_glob;
    char type_char;
    int maxdepth;
    int matched;
} find_ctx_t;

static void find_walk(const char *path, int depth, find_ctx_t *ctx);

static void find_cb(const char *name, unsigned char d_type, void *vctx)
{
    typedef struct { const char *dirpath; int depth; find_ctx_t *ctx; } w_t;
    w_t *w = vctx;
    find_ctx_t *ctx = w->ctx;
    (void)d_type;
    if (aether_streq(name, ".") || aether_streq(name, "..")) return;

    char path[1024];
    if (aether_path_join(path, sizeof(path), w->dirpath, name) != 0) return;

    aether_statx_t st;
    if (aether_stat(path, &st, AT_SYMLINK_NOFOLLOW) != 0) return;
    unsigned type = (unsigned)(st.stx_mode >> 12);

    int matches = 1;
    if (ctx->type_char == 'f') matches = (type == 8);
    else if (ctx->type_char == 'd') matches = (type == 4);
    else if (ctx->type_char == 'l') matches = (type == 10);
    if (matches && ctx->name_glob) matches = glob_match(ctx->name_glob, name);

    if (matches) {
        elog_info("%s", path);
        ctx->matched++;
    }
    if (type == 4 && (ctx->maxdepth < 0 || w->depth + 1 < ctx->maxdepth)) {
        find_walk(path, w->depth + 1, ctx);
    }
}

static void find_walk(const char *path, int depth, find_ctx_t *ctx)
{
    typedef struct { const char *dirpath; int depth; find_ctx_t *ctx; } w_t;
    w_t w;
    w.dirpath = path;
    w.depth = depth;
    w.ctx = ctx;
    aether_foreach_dirent(path, find_cb, &w);
}

int tool_find(int argc, char **argv, char **envp)
{
    (void)envp;
    find_ctx_t ctx;
    ctx.name_glob = 0;
    ctx.type_char = 0;
    ctx.maxdepth = -1;
    ctx.matched = 0;

    int i = 1;
    const char *paths[16];
    int npaths = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (aether_streq(a, "-name") && i + 1 < argc) ctx.name_glob = argv[++i];
        else if (aether_streq(a, "-type") && i + 1 < argc) {
            const char *t = argv[++i];
            if (aether_streq(t, "f")) ctx.type_char = 'f';
            else if (aether_streq(t, "d")) ctx.type_char = 'd';
            else if (aether_streq(t, "l")) ctx.type_char = 'l';
            else { elog_err("find: unknown type '%s'", t); return 2; }
        } else if (aether_streq(a, "-maxdepth") && i + 1 < argc) ctx.maxdepth = (int)aether_atol(argv[++i]);
        else if (a[0] == '-' && a[1]) { elog_err("find: unknown option '%s'", a); return 2; }
        else { paths[npaths++] = a; if (npaths >= 16) break; }
    }
    if (npaths == 0) paths[npaths++] = ".";

    for (int p = 0; p < npaths; p++) {
        const char *path = paths[p];
        aether_statx_t st;
        if (aether_stat(path, &st, AT_SYMLINK_NOFOLLOW) != 0) { elog_err("find: '%s': no such file", path); continue; }
        unsigned type = (unsigned)(st.stx_mode >> 12);

        int matches = 1;
        if (ctx.type_char == 'f') matches = (type == 8);
        else if (ctx.type_char == 'd') matches = (type == 4);
        else if (ctx.type_char == 'l') matches = (type == 10);
        if (matches && ctx.name_glob) matches = glob_match(ctx.name_glob, aether_base_name(path));
        if (matches) { elog_info("%s", path); ctx.matched++; }

        if (type == 4) find_walk(path, 0, &ctx);
    }
    return 0;
}
