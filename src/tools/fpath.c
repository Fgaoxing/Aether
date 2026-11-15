/*
 * fpath.c — 文件/路径辅助函数（cp/rm/realpath/ls 等工具共用）
 * 基于 statx/readlinkat/getdents64/unlinkat，不依赖 libc 的目录函数。
 */
#include <stddef.h>

#include "elog.h"
#include "statx.h"
#include "syscall_shim.h"
#include "sysfsutil.h"
#include "util.h"

#define MAX_DEPTH 32

/* 拼接 dir + "/" + name → dst */
int aether_path_join(char *dst, size_t cap, const char *dir, const char *name)
{
    size_t d = 0;
    if (cap < 2 || !dir || !name) return -1;
    while (dir[d] && d + 1 < cap) { dst[d] = dir[d]; d++; }
    dst[d] = '\0';
    while (d > 0 && dst[d - 1] == '/') dst[--d] = '\0';
    if (d + 2 > cap) return -1;
    dst[d++] = '/';
    for (size_t i = 0; name[i] && d + 1 < cap; i++) dst[d++] = name[i];
    dst[d] = '\0';
    return 0;
}

/* statx 简写：flag=AT_SYMLINK_NOFOLLOW 可用于 lstat */
int aether_stat(const char *path, aether_statx_t *st, int flags)
{
    long r = aether_statx(AT_FDCWD, path, flags, STATX_BASIC_STATS | STATX_NLINK, st);
    return AETHER_IS_ERR(r) ? -1 : 0;
}

/* 判断目录？（含符号链接指向目录按 lstat 语义：不解析符号链接） */
int aether_is_dir(const char *path)
{
    aether_statx_t st;
    if (aether_stat(path, &st, AT_SYMLINK_NOFOLLOW) != 0) return 0;
    return st.stx_mode >> 12 == 4; /* S_IFDIR=0040000 → mode>>12==4 */
}

/* 定时：文件 → YYYY-MM-DD HH:MM */
void aether_format_time(unsigned long long sec, char *out, size_t cap)
{
    aether_iso_time((long long)sec, out, cap);
}

/* 拷贝：src → dst（单文件）。保留 mode。成功 0，失败 -1 */
int aether_copy_file(const char *src, const char *dst, int preserve_mode)
{
    aether_statx_t st;
    if (aether_stat(src, &st, AT_SYMLINK_NOFOLLOW) != 0) return -1;

    int sfd = (int)sys_open(src, O_RDONLY, 0);
    if (AETHER_IS_ERR((long)sfd)) return -1;
    int dfd = (int)sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (AETHER_IS_ERR((long)dfd)) { sys_close(sfd); return -1; }

    char buf[32768];
    for (;;) {
        long n = sys_read(sfd, buf, sizeof(buf));
        if (n == 0) break;
        if (AETHER_IS_ERR(n)) { sys_close(sfd); sys_close(dfd); return -1; }
        size_t off = 0;
        while (off < (size_t)n) {
            long w = sys_write(dfd, buf + off, (size_t)n - off);
            if (AETHER_IS_ERR(w) || w == 0) { sys_close(sfd); sys_close(dfd); return -1; }
            off += (size_t)w;
        }
    }
    sys_close(sfd);
    sys_close(dfd);

    if (preserve_mode) {
        /* fchmodat 用 mode & 0777 */
        sys_chmod(dst, (unsigned int)(st.stx_mode & 07777));
    }
    return 0;
}

/* 递归删除（path 已由调用方 stat，无需再 stat） */
static int rm_recursive(const char *path, int depth);

typedef struct {
    const char *root;     /* 顶层路径（前缀，仅用于错误消息） */
    int errors;
    int dry;              /* 未使用，保留为 0 */
} rm_ctx_t;

typedef struct {
    const char *dir;      /* 当前正在遍历的目录路径 */
    rm_ctx_t *ctx;
    int depth;
} rm_walk_t;

static void rm_walk_cb(const char *name, unsigned char d_type, void *vctx)
{
    rm_walk_t *w = vctx;
    if (aether_streq(name, ".") || aether_streq(name, "..")) return;
    char child[512];
    if (aether_path_join(child, sizeof(child), w->dir, name) != 0) { w->ctx->errors++; return; }
    if (d_type == 4 /* DT_DIR */ || (d_type == 0 && aether_is_dir(child))) {
        rm_recursive(child, w->depth + 1);
    } else {
        if (sys_unlink(child) != 0) w->ctx->errors++;
    }
}

static int rm_recursive(const char *path, int depth)
{
    if (depth > MAX_DEPTH) return -1;
    rm_walk_t w;
    rm_ctx_t ctx;
    ctx.root = path;
    ctx.errors = 0;
    ctx.dry = 0;
    w.dir = path;
    w.ctx = &ctx;
    w.depth = depth;
    /* 先删子项，再删目录本身 */
    if (aether_foreach_dirent(path, rm_walk_cb, &w) != 0) ctx.errors++;
    if (sys_unlinkat(AT_FDCWD, path, AT_REMOVEDIR) != 0) ctx.errors++;
    return ctx.errors ? -1 : 0;
}

int aether_remove_tree(const char *path)
{
    return rm_recursive(path, 0);
}

/* 递归拷贝 src → dst（已假定 src 是目录） */
static int cp_recursive(const char *src, const char *dst, int preserve_mode, int depth);

typedef struct {
    const char *src_root;
    const char *dst_root;
    int preserve_mode;
    int errors;
} cp_ctx_t;

typedef struct {
    const char *dst;
    cp_ctx_t *ctx;
    int depth;
} cp_walk_t;

static void cp_walk_cb(const char *name, unsigned char d_type, void *vctx)
{
    cp_walk_t *w = vctx;
    if (aether_streq(name, ".") || aether_streq(name, "..")) return;
    char src_path[512], dst_path[512];
    /* src_path 需要拼接 —— 但 walk 回调不知道 src 根，所以 src 由外层拼接 */
    /* 这里 src_root 是父目录，拼接 */
    if (aether_path_join(src_path, sizeof(src_path), (const char *)w->ctx->src_root, name) != 0) { w->ctx->errors++; return; }
    if (aether_path_join(dst_path, sizeof(dst_path), w->dst, name) != 0) { w->ctx->errors++; return; }

    if (d_type == 4 || (d_type == 0 && aether_is_dir(src_path))) {
        cp_recursive(src_path, dst_path, w->ctx->preserve_mode, w->depth + 1);
    } else {
        if (aether_copy_file(src_path, dst_path, w->ctx->preserve_mode) != 0) w->ctx->errors++;
    }
}

static int cp_recursive(const char *src, const char *dst, int preserve_mode, int depth)
{
    if (depth > MAX_DEPTH) return -1;

    aether_statx_t st;
    if (aether_stat(src, &st, AT_SYMLINK_NOFOLLOW) != 0) return -1;

    /* 先创建目标目录（若已存在，复用） */
    if (sys_mkdirat(AT_FDCWD, dst, 0755) != 0) {
        if (!aether_is_dir(dst)) return -1;
    }

    cp_walk_t w;
    cp_ctx_t ctx;
    ctx.src_root = src;
    ctx.dst_root = dst;
    ctx.preserve_mode = preserve_mode;
    ctx.errors = 0;
    w.dst = dst;
    w.ctx = &ctx;
    w.depth = depth;
    if (aether_foreach_dirent(src, cp_walk_cb, &w) != 0) ctx.errors++;
    return ctx.errors ? -1 : 0;
}

int aether_copy_tree(const char *src, const char *dst, int preserve_mode)
{
    return cp_recursive(src, dst, preserve_mode, 0);
}

/* ---------- realpath 规范化（含符号链接解析） ---------- */

/* 组件缓存：路径拆成组件后，realpath 反复替换符号链接组件 */
typedef struct {
    char comp[192][256];
    int ncomp;
    int depth;         /* 符号链接展开深度，防死循环 */
    int errors;
    int expanded;      /* 最近一次 expand 是否发生了组件替换 */
} path_parts_t;

static int path_split(path_parts_t *pp, const char *path)
{
    pp->ncomp = 0;
    pp->depth = 0;
    pp->errors = 0;
    pp->expanded = 0;
    const char *p = path;
    if (*p == '/') p++;
    while (*p) {
        if (*p == '/') { p++; continue; }
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - s);
        if (len >= sizeof(pp->comp[0])) { pp->errors++; p = pp->comp[0]; len = sizeof(pp->comp[0]) - 1; }
        for (size_t i = 0; i < len; i++) pp->comp[pp->ncomp][i] = s[i];
        pp->comp[pp->ncomp][len] = '\0';
        pp->ncomp++;
        if (pp->ncomp >= 191) { pp->errors++; return -1; }
    }
    return 0;
}

/* 从 pp 的 comp[i] 起展开：如果 comp[i] 是符号链接，则用读到的目标替换该组件 */
static int expand_symlinks(path_parts_t *pp, int i)
{
    if (pp->depth > 40) { pp->errors++; return -1; }
    if (pp->ncomp == 0 || i >= pp->ncomp) return 0;

    /* 拼出到 comp[i] 的路径前缀 */
    char prefix[512];
    size_t d = 0;
    prefix[d++] = '/';
    for (int k = 0; k < i; k++) {
        const char *c = pp->comp[k];
        if (d + 1 >= sizeof(prefix)) return -1;
        for (size_t j = 0; c[j] && d + 1 < sizeof(prefix); j++) prefix[d++] = c[j];
        prefix[d++] = '/';
    }
    /* 去掉最后的 '/' */
    if (d > 1 && prefix[d - 1] == '/') prefix[d - 1] = '\0';
    else prefix[d] = '\0';

    char full[512];
    if (aether_path_join(full, sizeof(full), prefix, pp->comp[i]) != 0) return -1;

    /* 读链接 */
    aether_statx_t st;
    if (aether_stat(full, &st, AT_SYMLINK_NOFOLLOW) != 0) return 0; /* 不存在就不展开 */
    unsigned type = (unsigned)(st.stx_mode >> 12);
    if (type != 10) return 0; /* 非符号链接 */

    char target[512];
    long r = sys_readlink(full, target, sizeof(target) - 1);
    if (AETHER_IS_ERR(r)) return -1;
    target[r] = '\0';

    /* 组件替换：如果目标是绝对路径，清空之前的组件 */
    if (target[0] == '/') {
        path_parts_t sub;
        if (path_split(&sub, target) != 0) return -1;
        /* 把 target 各组件插入到位置 i，替换原组件 */
        int remain = pp->ncomp - i - 1;
        /* 把 remain 个组件搬到 i + sub.ncomp */
        if (pp->ncomp - i - 1 + sub.ncomp > 191) { pp->errors++; return -1; }
        for (int k = remain - 1; k >= 0; k--) {
            size_t n = aether_strlen(pp->comp[i + 1 + k]);
            size_t t = n;
            for (size_t j = 0; j <= t; j++) pp->comp[i + sub.ncomp + k][j] = pp->comp[i + 1 + k][j];
        }
        for (int k = 0; k < sub.ncomp; k++) {
            size_t n = aether_strlen(sub.comp[k]);
            size_t t = n;
            for (size_t j = 0; j <= t; j++) pp->comp[i + k][j] = sub.comp[k][j];
        }
        pp->ncomp = i + sub.ncomp + remain;
        pp->expanded = 1;
    } else {
        /* 相对路径：把 target 组件插入到 i，原 i 被替换为 target 各组件 */
        path_parts_t sub;
        if (path_split(&sub, target) != 0) return -1;
        int remain = pp->ncomp - i - 1;
        for (int k = remain - 1; k >= 0; k--) {
            size_t n = aether_strlen(pp->comp[i + 1 + k]);
            size_t t = n;
            for (size_t j = 0; j <= t; j++) pp->comp[i + sub.ncomp + k][j] = pp->comp[i + 1 + k][j];
        }
        for (int k = 0; k < sub.ncomp; k++) {
            size_t n = aether_strlen(sub.comp[k]);
            size_t t = n;
            for (size_t j = 0; j <= t; j++) pp->comp[i + k][j] = sub.comp[k][j];
        }
        pp->ncomp = i + sub.ncomp + remain;
        pp->expanded = 1;
    }
    pp->depth++;
    return 0;
}

/* 解析为绝对路径并规范化。返回 0 成功 */
int aether_realpath(const char *path, char *out, size_t cap)
{
    if (!path || !path[0] || cap < 2) return -1;
    char abs[512];
    size_t d = 0;
    if (path[0] == '/') {
        for (size_t i = 0; path[i] && d + 1 < sizeof(abs); i++) abs[d++] = path[i];
        abs[d] = '\0';
    } else {
        char cwd[256];
        long n = sys_getcwd(cwd, sizeof(cwd));
        if (AETHER_IS_ERR(n) || n == 0) return -1;
        cwd[n] = '\0'; /* 原始返回含尾 NUL？内核 getcwd 返回长度含尾 NUL */
        /* 处理含尾 NUL：n 可能是 strlen+1 */
        size_t clen = aether_strlen(cwd);
        for (size_t i = 0; i < clen && d + 1 < sizeof(abs); i++) abs[d++] = cwd[i];
        if (clen > 0 && abs[clen - 1] != '/' && d + 1 < sizeof(abs)) abs[d++] = '/';
        for (size_t i = 0; path[i] && d + 1 < sizeof(abs); i++) abs[d++] = path[i];
        abs[d] = '\0';
    }

    path_parts_t pp;
    if (path_split(&pp, abs) != 0) return -1;

    /* 展开符号链接：发生替换就重扫（保守但正确） */
    for (int i = 0; i < pp.ncomp; ) {
        pp.expanded = 0;
        if (expand_symlinks(&pp, i) < 0) return -1;
        if (pp.expanded) i = 0; else i++;
    }

    /* 处理 "." 和 ".."：重走一遍 */
    path_parts_t out_parts;
    out_parts.ncomp = 0;
    out_parts.depth = 0;
    out_parts.errors = 0;
    for (int i = 0; i < pp.ncomp; i++) {
        const char *c = pp.comp[i];
        if (aether_streq(c, ".")) continue;
        if (aether_streq(c, "..")) { if (out_parts.ncomp > 0) out_parts.ncomp--; continue; }
        if (out_parts.ncomp >= 191) { out_parts.errors++; break; }
        size_t n = aether_strlen(c);
        for (size_t j = 0; j <= n; j++) out_parts.comp[out_parts.ncomp][j] = c[j];
        out_parts.ncomp++;
    }

    /* 输出 */
    d = 0;
    if (cap < 2) return -1;
    out[d++] = '/';
    for (int i = 0; i < out_parts.ncomp && d + 1 < cap; i++) {
        const char *c = out_parts.comp[i];
        size_t n = aether_strlen(c);
        for (size_t j = 0; j < n && d + 1 < cap; j++) out[d++] = c[j];
        if (i + 1 < out_parts.ncomp && d + 1 < cap) out[d++] = '/';
    }
    out[d] = '\0';
    return out_parts.errors ? -1 : 0;
}
