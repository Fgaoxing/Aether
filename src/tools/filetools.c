/*
 * filetools.c — 文件类 applet
 * ls stat readlink realpath basename dirname chmod chown chgrp touch truncate
 * mkdir rmdir rm cp mv dd
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "statx.h"
#include "syscall_shim.h"
#include "sysfsutil.h"
#include "util.h"

/* fpath.c 导出的辅助函数 */
int aether_path_join(char *dst, size_t cap, const char *dir, const char *name);
int aether_stat(const char *path, aether_statx_t *st, int flags);
int aether_is_dir(const char *path);
int aether_copy_file(const char *src, const char *dst, int preserve_mode);
int aether_remove_tree(const char *path);
int aether_copy_tree(const char *src, const char *dst, int preserve_mode);
int aether_realpath(const char *path, char *out, size_t cap);

/* ---------- basename / dirname ---------- */

int tool_basename(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("basename: usage: basename <path>"); return 2; }
    const char *path = argv[1];
    const char *base = aether_base_name(path);
    /* 去掉尾部斜杠后取名字 */
    size_t n = aether_strlen(base);
    while (n > 0 && base[n - 1] == '/') n--;
    if (n == 0) { elog_info("/"); return 0; }
    char buf[512];
    size_t d = 0;
    for (size_t i = 0; i < n && d + 1 < sizeof(buf); i++) buf[d++] = base[i];
    buf[d] = '\0';
    elog_info("%s", buf);
    return 0;
}

int tool_dirname(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("dirname: usage: dirname <path>"); return 2; }
    const char *path = argv[1];
    /* 去掉尾部斜杠 */
    size_t len = aether_strlen(path);
    if (len == 0) { elog_info("."); return 0; }
    size_t end = len;
    while (end > 1 && path[end - 1] == '/') end--;
    if (end == 1 && path[0] == '/') { elog_info("/"); return 0; }
    /* 找最后一个 '/' */
    size_t slash = end;
    while (slash > 0 && path[slash - 1] != '/') slash--;
    if (slash == 0) { elog_info("."); return 0; }
    while (slash > 1 && path[slash - 1] == '/') slash--;
    char buf[512];
    size_t d = 0;
    for (size_t i = 0; i < slash && d + 1 < sizeof(buf); i++) buf[d++] = path[i];
    buf[d] = '\0';
    elog_info("%s", buf);
    return 0;
}

/* ---------- stat ---------- */

static void print_statx_line(const char *path, const aether_statx_t *st)
{
    char mode[12], tbuf[48], oct[8];
    aether_mode_str((unsigned int)st->stx_mode, mode);
    aether_iso_time((long long)st->stx_mtime.tv_sec, tbuf, sizeof(tbuf));
    {
        unsigned int m = (unsigned int)st->stx_mode & 07777;
        oct[0] = '0'; oct[1] = '0' + ((m >> 6) & 7); oct[2] = '0' + ((m >> 3) & 7); oct[3] = '0' + (m & 7);
        oct[4] = '\0';
    }

    static const char *type_names[] = {
        [0] = "unknown", [1] = "fifo", [2] = "char", [4] = "dir", [6] = "blk",
        [8] = "file", [10] = "link", [12] = "socket", [14] = "unknown"
    };
    unsigned type = (unsigned)(st->stx_mode >> 12);
    const char *tname = (type < 16 && type_names[type]) ? type_names[type] : "unknown";

    elog_info("  File: %s", path);
    elog_info("  Size: %llu      Blocks: %llu      IO Block: %u   %s",
              st->stx_size, st->stx_blocks, st->stx_blksize, tname);
    elog_info("  Mode: (%s/%s)", oct, mode);
    elog_info("  Uid: (%u/%u)     Gid: (%u/%u)", st->stx_uid, st->stx_uid, st->stx_gid, st->stx_gid);
    elog_info("  Links: %u       Inode: %llu", st->stx_nlink, st->stx_ino);
    elog_info("  Modify: %s", tbuf);
}

int tool_stat(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("stat: usage: stat <file>..."); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        aether_statx_t st;
        if (aether_stat(argv[i], &st, 0 /* follow links */) != 0) {
            elog_err("stat: cannot stat '%s'", argv[i]);
            rc = 1;
            continue;
        }
        print_statx_line(argv[i], &st);
    }
    return rc;
}

/* ---------- readlink / realpath ---------- */

int tool_readlink(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("readlink: usage: readlink <path>"); return 2; }
    int full = 0;
    int i = 1;
    if (aether_streq(argv[i], "-f")) { full = 1; i++; }
    if (i >= argc) { elog_err("readlink: missing operand"); return 2; }

    if (full) {
        char out[512];
        if (aether_realpath(argv[i], out, sizeof(out)) != 0) {
            elog_err("readlink: %s: no such file or not resolvable", argv[i]);
            return 1;
        }
        elog_info("%s", out);
        return 0;
    }
    char buf[512];
    long r = sys_readlink(argv[i], buf, sizeof(buf) - 1);
    if (AETHER_IS_ERR(r)) {
        elog_err("readlink: %s: %d", argv[i], AETHER_ERR_CODE(r));
        return 1;
    }
    buf[r] = '\0';
    elog_info("%s", buf);
    return 0;
}

int tool_realpath(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("realpath: usage: realpath <path>..."); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char out[512];
        if (aether_realpath(argv[i], out, sizeof(out)) != 0) {
            elog_err("realpath: %s: no such file or not resolvable", argv[i]);
            rc = 1;
            continue;
        }
        elog_info("%s", out);
    }
    return rc;
}

/* ---------- chmod / chown / chgrp ---------- */

static int parse_octal(const char *s, unsigned int *out)
{
    unsigned int v = 0;
    if (!s || !*s) return -1;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c < '0' || c > '7') return -1;
        v = v * 8 + (unsigned int)(c - '0');
    }
    *out = v;
    return 0;
}

static int parse_uidgid(const char *s, unsigned int *out)
{
    unsigned int v = 0;
    if (!s || !*s) return -1;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (unsigned int)(c - '0');
    }
    *out = v;
    return 0;
}

int tool_chmod(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 3) { elog_err("chmod: usage: chmod <octal-mode> <file>..."); return 2; }
    unsigned int mode;
    if (parse_octal(argv[1], &mode) != 0) { elog_err("chmod: invalid mode '%s'", argv[1]); return 2; }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        long r = sys_chmod(argv[i], mode);
        if (AETHER_IS_ERR(r)) { elog_err("chmod: '%s': %d", argv[i], AETHER_ERR_CODE(r)); rc = 1; }
    }
    return rc;
}

int tool_chown(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 3) { elog_err("chown: usage: chown <uid[:gid]> <file>..."); return 2; }
    const char *spec = argv[1];
    unsigned int uid, gid = 0xffffffff; /* -1 = 不改 gid */
    const char *colon = 0;
    for (const char *p = spec; *p; p++) if (*p == ':') colon = p;
    if (colon) {
        char u[16], g[16];
        size_t un = (size_t)(colon - spec);
        if (un >= sizeof(u)) { elog_err("chown: uid too long"); return 2; }
        for (size_t i = 0; i < un; i++) u[i] = spec[i];
        u[un] = '\0';
        size_t gn = aether_strlen(colon + 1);
        if (gn >= sizeof(g)) { elog_err("chown: gid too long"); return 2; }
        for (size_t i = 0; i < gn; i++) g[i] = colon[1 + i];
        g[gn] = '\0';
        if (parse_uidgid(u, &uid) != 0) { elog_err("chown: invalid uid '%s'", u); return 2; }
        if (parse_uidgid(g, &gid) != 0) { elog_err("chown: invalid gid '%s'", g); return 2; }
    } else {
        if (parse_uidgid(spec, &uid) != 0) { elog_err("chown: invalid uid '%s'", spec); return 2; }
    }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        unsigned int owner = uid, group = gid == 0xffffffff ? (unsigned int)-1 : gid;
        long r = sys_chown(argv[i], owner, group);
        if (AETHER_IS_ERR(r)) { elog_err("chown: '%s': %d", argv[i], AETHER_ERR_CODE(r)); rc = 1; }
    }
    return rc;
}

int tool_chgrp(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 3) { elog_err("chgrp: usage: chgrp <gid> <file>..."); return 2; }
    unsigned int gid;
    if (parse_uidgid(argv[1], &gid) != 0) { elog_err("chgrp: invalid gid '%s'", argv[1]); return 2; }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        /* owner=-1 表示不改 owner */
        long r = sys_chown(argv[i], (unsigned int)-1, gid);
        if (AETHER_IS_ERR(r)) { elog_err("chgrp: '%s': %d", argv[i], AETHER_ERR_CODE(r)); rc = 1; }
    }
    return rc;
}

/* ---------- touch / truncate ---------- */

int tool_touch(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("touch: usage: touch <file>..."); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        long fd = sys_open(argv[i], O_WRONLY | O_CREAT, 0666);
        if (AETHER_IS_ERR(fd)) { elog_err("touch: cannot create '%s'", argv[i]); rc = 1; continue; }
        sys_close((int)fd);
        long r = sys_utimensat(AT_FDCWD, argv[i], 0, 0);
        if (AETHER_IS_ERR(r)) { elog_err("touch: cannot set times on '%s'", argv[i]); rc = 1; }
    }
    return rc;
}

static int parse_size(const char *s, unsigned long long *out)
{
    unsigned long long v = 0;
    unsigned long long mult;
    int i = 0;
    if (!s || !*s) return -1;
    while (s[i] && s[i] != 'K' && s[i] != 'M' && s[i] != 'G' && s[i] != 'T') {
        char c = s[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (unsigned int)(c - '0');
        i++;
    }
    switch (s[i]) {
    case 'K': mult = 1024ULL; break;
    case 'M': mult = 1024ULL * 1024; break;
    case 'G': mult = 1024ULL * 1024 * 1024; break;
    case 'T': mult = 1024ULL * 1024 * 1024 * 1024; break;
    case '\0': mult = 1; break;
    default: return -1;
    }
    *out = v * mult;
    return 0;
}

int tool_truncate(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 4) { elog_err("truncate: usage: truncate -s <size> <file>..."); return 2; }
    if (!aether_streq(argv[1], "-s")) { elog_err("truncate: -s required"); return 2; }
    unsigned long long size;
    if (parse_size(argv[2], &size) != 0) { elog_err("truncate: invalid size '%s'", argv[2]); return 2; }
    int rc = 0;
    for (int i = 3; i < argc; i++) {
        long fd = sys_open(argv[i], O_RDWR | O_CREAT, 0666);
        if (AETHER_IS_ERR(fd)) { elog_err("truncate: cannot open '%s'", argv[i]); rc = 1; continue; }
        long r = sys_ftruncate((int)fd, (long)size);
        sys_close((int)fd);
        if (AETHER_IS_ERR(r)) { elog_err("truncate: '%s': %d", argv[i], AETHER_ERR_CODE(r)); rc = 1; }
    }
    return rc;
}

/* ---------- mkdir / rmdir ---------- */

static int mkdir_p(const char *path, unsigned int mode)
{
    char buf[512];
    size_t d = 0;
    if (!path[0]) return -1;
    const char *p = path;
    if (*p == '/') { buf[d++] = '/'; p++; }
    while (*p) {
        while (*p == '/') p++;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - s);
        if (len == 0) break;
        if (d + len + 1 >= sizeof(buf)) return -1;
        for (size_t i = 0; i < len; i++) buf[d++] = s[i];
        buf[d] = '\0';
        long r = sys_mkdirat(AT_FDCWD, buf, mode);
        if (AETHER_IS_ERR(r) && AETHER_ERR_CODE(r) != 17 /*EEXIST*/) return -1;
        if (*p) buf[d++] = '/';
    }
    return 0;
}

int tool_mkdir(int argc, char **argv, char **envp)
{
    (void)envp;
    int p = 0, i = 1;
    if (argc > 1 && aether_streq(argv[1], "-p")) { p = 1; i = 2; }
    if (i >= argc) { elog_err("mkdir: usage: mkdir [-p] <dir>..."); return 2; }
    int rc = 0;
    for (int k = i; k < argc; k++) {
        if (p) { if (mkdir_p(argv[k], 0755) != 0) { elog_err("mkdir: '%s' failed", argv[k]); rc = 1; } }
        else {
            long r = sys_mkdirat(AT_FDCWD, argv[k], 0755);
            if (AETHER_IS_ERR(r)) { elog_err("mkdir: '%s': %d", argv[k], AETHER_ERR_CODE(r)); rc = 1; }
        }
    }
    return rc;
}

int tool_rmdir(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("rmdir: usage: rmdir <dir>..."); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        long r = sys_unlinkat(AT_FDCWD, argv[i], AT_REMOVEDIR);
        if (AETHER_IS_ERR(r)) { elog_err("rmdir: '%s': %d", argv[i], AETHER_ERR_CODE(r)); rc = 1; }
    }
    return rc;
}

/* ---------- rm ---------- */

int tool_rm(int argc, char **argv, char **envp)
{
    (void)envp;
    int recursive = 0, force = 0, i = 1;
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-r") || aether_streq(argv[i], "-R")) recursive = 1;
        else if (aether_streq(argv[i], "-f")) force = 1;
        else if (argv[i][0] == '-' && argv[i][1]) { elog_err("rm: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    if (i >= argc) { elog_err("rm: missing operand"); return 2; }
    int rc = 0;
    for (int k = i; k < argc; k++) {
        aether_statx_t st;
        if (aether_stat(argv[k], &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (!force) { elog_err("rm: '%s': No such file or directory", argv[k]); rc = 1; }
            continue;
        }
        unsigned type = (unsigned)(st.stx_mode >> 12);
        if (type == 4 && !recursive) { elog_err("rm: '%s' is a directory (use -r)", argv[k]); rc = 1; continue; }
        if (type == 4) { if (aether_remove_tree(argv[k]) != 0) rc = 1; }
        else if (sys_unlink(argv[k]) != 0) { if (!force) { elog_err("rm: '%s': %d", argv[k], AETHER_ERR_CODE(sys_unlink(argv[k]))); rc = 1; } }
    }
    return rc;
}

/* ---------- cp / mv ---------- */

int tool_cp(int argc, char **argv, char **envp)
{
    (void)envp;
    int recursive = 0, preserve_mode = 0, i = 1;
    for (; i < argc; i++) {
        if (aether_streq(argv[i], "-r") || aether_streq(argv[i], "-R")) recursive = 1;
        else if (aether_streq(argv[i], "-p")) preserve_mode = 1;
        else if (argv[i][0] == '-' && argv[i][1]) { elog_err("cp: unknown option '%s'", argv[i]); return 2; }
        else break;
    }
    int nsrc = argc - i - 1;
    if (nsrc < 1) { elog_err("cp: missing destination file operand"); return 2; }
    const char *dst = argv[argc - 1];
    int multi = nsrc > 1;

    /* 目标为现有目录 → 复制到目录下 */
    int dst_is_dir = aether_is_dir(dst);

    int rc = 0;
    for (int k = i; k < argc - 1; k++) {
        const char *src = argv[k];
        char dstpath[512];
        const char *final = dst;
        if (dst_is_dir) {
            const char *base = aether_base_name(src);
            if (aether_path_join(dstpath, sizeof(dstpath), dst, base) != 0) { elog_err("cp: path too long"); rc = 1; continue; }
            final = dstpath;
        } else if (multi) {
            elog_err("cp: target '%s' is not a directory", dst);
            return 2;
        }

        aether_statx_t st;
        if (aether_stat(src, &st, AT_SYMLINK_NOFOLLOW) != 0) { elog_err("cp: '%s': No such file or directory", src); rc = 1; continue; }
        unsigned type = (unsigned)(st.stx_mode >> 12);
        if (type == 4) {
            if (!recursive) { elog_err("cp: '%s' is a directory (use -r)", src); rc = 1; continue; }
            if (aether_copy_tree(src, final, preserve_mode) != 0) rc = 1;
        } else {
            if (aether_copy_file(src, final, preserve_mode) != 0) { elog_err("cp: '%s' -> '%s' failed", src, final); rc = 1; }
        }
    }
    return rc;
}

int tool_mv(int argc, char **argv, char **envp)
{
    (void)envp;
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        const char *opt = argv[i];
        if (aether_streq(opt, "-f") || aether_streq(opt, "-v")) i++;
        else { elog_err("mv: unknown option '%s'", opt); return 2; }
    }
    int nsrc = argc - i - 1;
    if (nsrc < 1) { elog_err("mv: missing destination operand"); return 2; }
    const char *dst = argv[argc - 1];
    int multi = nsrc > 1;
    int dst_is_dir = aether_is_dir(dst);

    int rc = 0;
    for (int k = i; k < argc - 1; k++) {
        const char *src = argv[k];
        char dstpath[512];
        const char *final = dst;
        if (dst_is_dir) {
            const char *base = aether_base_name(src);
            if (aether_path_join(dstpath, sizeof(dstpath), dst, base) != 0) { elog_err("mv: path too long"); rc = 1; continue; }
            final = dstpath;
        } else if (multi) {
            elog_err("mv: target '%s' is not a directory", dst);
            return 2;
        }
        long r = sys_renameat(AT_FDCWD, src, AT_FDCWD, final);
        if (AETHER_IS_ERR(r)) {
            int e = AETHER_ERR_CODE(r);
            if (e == 18 /*EXDEV*/) {
                /* 跨设备：cp + rm */
                aether_statx_t st;
                if (aether_stat(src, &st, AT_SYMLINK_NOFOLLOW) == 0 && (unsigned)(st.stx_mode >> 12) == 4) {
                    if (aether_copy_tree(src, final, 1) != 0 || aether_remove_tree(src) != 0) { rc = 1; elog_err("mv: cross-device move of '%s' failed", src); continue; }
                } else if (aether_copy_file(src, final, 1) != 0 || sys_unlink(src) != 0) { rc = 1; elog_err("mv: cross-device move of '%s' failed", src); continue; }
            } else {
                elog_err("mv: '%s' -> '%s': %d", src, final, e);
                rc = 1;
            }
        }
    }
    return rc;
}

/* ---------- ls ---------- */

typedef struct {
    int show_all, long_fmt, human, recursive;
    const char *prefix;   /* 递归时的前缀 */
    int errors, n;
} ls_ctx_t;

static void print_one(const char *path, const char *name, int d_type, ls_ctx_t *ctx)
{
    (void)d_type;
    if (!ctx->show_all && name[0] == '.') return;
    ctx->n++;

    if (!ctx->long_fmt) {
        elog_info("%s", name);
        return;
    }
    aether_statx_t st;
    if (aether_stat(path, &st, AT_SYMLINK_NOFOLLOW) != 0) { elog_info("? %s", name); return; }
    char mode[12], tbuf[32];
    aether_mode_str((unsigned int)st.stx_mode, mode);
    aether_iso_time((long long)st.stx_mtime.tv_sec, tbuf, sizeof(tbuf));
    if (ctx->human) {
        char sz[24];
        aether_human_bytes(st.stx_size, sz, sizeof(sz));
        elog_info("%s %2u %u %u %8s %s %s", mode, st.stx_nlink, st.stx_uid, st.stx_gid, sz, tbuf, name);
    } else {
        elog_info("%s %2u %u %u %8llu %s %s", mode, st.stx_nlink, st.stx_uid, st.stx_gid, st.stx_size, tbuf, name);
    }
}

typedef struct {
    const char *prefix;
    ls_ctx_t *ctx;
    int depth;
} ls_walk_t;

#define LS_MAX_DEPTH 20

static void ls_walk_cb(const char *name, unsigned char d_type, void *vctx)
{
    ls_walk_t *w = vctx;
    char path[512];
    if (aether_path_join(path, sizeof(path), w->prefix, name) != 0) { w->ctx->errors++; return; }
    int is_dir = (d_type == 4) || (d_type == 0 && aether_is_dir(path));
    print_one(path, name, d_type, w->ctx);
    /* -R：递归子目录（. .. 不展开） */
    if (w->ctx->recursive && is_dir && w->depth < LS_MAX_DEPTH
        && !aether_streq(name, ".") && !aether_streq(name, "..")) {
        elog_info("%s:", path);
        ls_walk_t child;
        child.prefix = path;
        child.ctx = w->ctx;
        child.depth = w->depth + 1;
        aether_foreach_dirent(path, ls_walk_cb, &child);
    }
}

static void ls_one(const char *target, ls_ctx_t *ctx)
{
    aether_statx_t st;
    if (aether_stat(target, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        elog_err("ls: cannot access '%s'", target);
        ctx->errors++;
        return;
    }
    unsigned type = (unsigned)(st.stx_mode >> 12);
    if (type != 4) {
        const char *base = aether_base_name(target);
        print_one(target, base, 0, ctx);
        return;
    }
    if (ctx->recursive) elog_info("%s:", target);
    ls_walk_t w;
    w.prefix = target;
    w.ctx = ctx;
    w.depth = 0;
    aether_foreach_dirent(target, ls_walk_cb, &w);
}

int tool_ls(int argc, char **argv, char **envp)
{
    (void)envp;
    ls_ctx_t ctx;
    ctx.show_all = 0; ctx.long_fmt = 0; ctx.human = 0; ctx.recursive = 0;
    ctx.prefix = ""; ctx.errors = 0; ctx.n = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] && !aether_streq(a, "--")) {
            /* 短选项组合：-lh / -la / -lR 等（BusyBox 风格） */
            if (a[1] == '-') break;
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'l': ctx.long_fmt = 1; break;
                case 'a': case 'A': ctx.show_all = 1; break;
                case 'h': ctx.human = 1; break;
                case 'R': ctx.recursive = 1; break;
                default: elog_err("ls: unknown option '-%c'", *p); return 2;
                }
            }
            continue;
        }
        if (aether_streq(a, "--")) { i++; break; }
        break;
    }
    if (i >= argc) { ls_one(".", &ctx); return ctx.errors ? 1 : 0; }
    if (argc - i > 1 || argv[i][0] != '-') { }
    for (int k = i; k < argc; k++) ls_one(argv[k], &ctx);
    return ctx.errors ? 1 : 0;
}

/* ---------- dd ---------- */

static int parse_dd_size(const char *s, unsigned long long *out)
{
    unsigned long long v = 0;
    int i = 0;
    if (!s || !*s) return -1;
    while (s[i]) {
        char c = s[i];
        if (c >= '0' && c <= '9') { v = v * 10 + (unsigned int)(c - '0'); i++; }
        else if (c == 'K' || c == 'k') { v *= 1024; i++; }
        else if (c == 'M') { v *= 1024ULL * 1024; i++; }
        else if (c == 'G') { v *= 1024ULL * 1024 * 1024; i++; }
        else if (c == 'T') { v *= 1024ULL * 1024 * 1024 * 1024; i++; }
        else if (c == 'c') { i++; } /* 1 byte */
        else return -1;
    }
    *out = v;
    return 0;
}

/* ---------- chattr / lsattr（FS_IOC_GETFLAGS/SETFLAGS；单用户 rescue 语义） ---------- */

/* _IOC 编码（内核通用布局，按 sizeof(long) 编译期计算，双分支一致） */
#define AE_IOC_NRSHIFT   0
#define AE_IOC_TYPESHIFT 8
#define AE_IOC_SIZESHIFT 16
#define AE_IOC_DIRSHIFT  30
#define AE_IOC_READ      2UL
#define AE_IOC_WRITE     1UL
#define AE_IOC(dir,type,nr,size) \
    ((((dir) & 3UL) << AE_IOC_DIRSHIFT) | (((unsigned long)(type) & 0xffUL) << AE_IOC_TYPESHIFT) | \
     (((nr) & 0xffUL) << AE_IOC_NRSHIFT) | (((unsigned long)(size) & 0x3fffUL) << AE_IOC_SIZESHIFT))
#define AE_IOR(type,nr,size) AE_IOC(AE_IOC_READ, (type), (nr), (size))
#define AE_IOW(type,nr,size) AE_IOC(AE_IOC_WRITE, (type), (nr), (size))
#define AE_FS_IOC_GETFLAGS AE_IOR('f', 1, sizeof(long))
#define AE_FS_IOC_SETFLAGS AE_IOW('f', 2, sizeof(long))

#define AE_FS_SECRM_FL       0x00000001UL
#define AE_FS_UNRM_FL        0x00000002UL
#define AE_FS_COMPR_FL       0x00000004UL
#define AE_FS_SYNC_FL        0x00000008UL
#define AE_FS_IMMUTABLE_FL   0x00000010UL
#define AE_FS_APPEND_FL      0x00000020UL
#define AE_FS_NODUMP_FL      0x00000040UL
#define AE_FS_NOATIME_FL     0x00000080UL
#define AE_FS_INDEX_FL       0x00001000UL
#define AE_FS_JOURNAL_DATA_FL 0x00004000UL
#define AE_FS_NOTAIL_FL      0x00008000UL
#define AE_FS_DIRSYNC_FL     0x00010000UL
#define AE_FS_EXTENT_FL      0x00080000UL
#define AE_FS_NOCOW_FL       0x00800000UL

typedef struct { unsigned long mask; char ch; } ae_flag_t;
static const ae_flag_t ae_flags[] = {
    { AE_FS_SECRM_FL, 's' }, { AE_FS_UNRM_FL, 'u' }, { AE_FS_COMPR_FL, 'c' }, { AE_FS_SYNC_FL, 'S' },
    { AE_FS_IMMUTABLE_FL, 'i' }, { AE_FS_APPEND_FL, 'a' }, { AE_FS_NODUMP_FL, 'd' }, { AE_FS_NOATIME_FL, 'A' },
    { AE_FS_INDEX_FL, 'I' }, { AE_FS_JOURNAL_DATA_FL, 'j' }, { AE_FS_NOTAIL_FL, 't' }, { AE_FS_DIRSYNC_FL, 'D' },
    { AE_FS_EXTENT_FL, 'e' }, { AE_FS_NOCOW_FL, 'C' },
};
#define AE_NFLAGS (sizeof(ae_flags) / sizeof(ae_flags[0]))

int tool_lsattr(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 2) { elog_err("lsattr: usage: lsattr <file>..."); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        long fd = sys_open(argv[i], O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) { elog_err("lsattr: cannot open '%s'", argv[i]); rc = 1; continue; }
        unsigned long flags = 0;
        long r = sys_ioctl((int)fd, AE_FS_IOC_GETFLAGS, (unsigned long)&flags);
        sys_close((int)fd);
        if (AETHER_IS_ERR(r)) { elog_err("lsattr: '%s': %d", argv[i], AETHER_ERR_CODE(r)); rc = 1; continue; }
        char str[16];
        for (size_t k = 0; k < sizeof(str) - 1; k++) str[k] = '-';
        str[sizeof(str) - 1] = '\0';
        for (size_t k = 0; k < AE_NFLAGS; k++) {
            if (flags & ae_flags[k].mask) str[k] = ae_flags[k].ch;
        }
        elog_info("%s %s", str, argv[i]);
    }
    return rc;
}

int tool_chattr(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc < 3) { elog_err("chattr: usage: chattr [+-]<flags> <file>..."); return 2; }
    const char *spec = argv[1];
    int op = 0; /* +1 = set, -1 = clear */
    if (spec[0] == '+') op = 1;
    else if (spec[0] == '-') op = -1;
    else { elog_err("chattr: flags must start with + or -"); return 2; }
    unsigned long want = 0;
    for (size_t i = 1; spec[i]; i++) {
        char c = spec[i];
        int found = 0;
        for (size_t k = 0; k < AE_NFLAGS; k++) {
            if (ae_flags[k].ch == c) { want |= ae_flags[k].mask; found = 1; break; }
        }
        if (!found) { elog_err("chattr: unknown flag '%c'", c); return 2; }
    }
    /* 只读位（extent 标记由文件系统自动设置）set 时过滤，免得 EINVAL 困惑用户 */
    if (op == 1) want &= ~AE_FS_EXTENT_FL;

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        long fd = sys_open(argv[i], O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) { elog_err("chattr: cannot open '%s'", argv[i]); rc = 1; continue; }
        unsigned long flags = 0;
        long r = sys_ioctl((int)fd, AE_FS_IOC_GETFLAGS, (unsigned long)&flags);
        if (AETHER_IS_ERR(r)) {
            elog_err("chattr: '%s': cannot get flags (%d)", argv[i], AETHER_ERR_CODE(r));
            sys_close((int)fd);
            rc = 1;
            continue;
        }
        if (op == 1) flags |= want; else flags &= ~want;
        r = sys_ioctl((int)fd, AE_FS_IOC_SETFLAGS, (unsigned long)&flags);
        sys_close((int)fd);
        if (AETHER_IS_ERR(r)) {
            elog_err("chattr: '%s': cannot set flags (%d)", argv[i], AETHER_ERR_CODE(r));
            rc = 1;
        }
    }
    return rc;
}

int tool_dd(int argc, char **argv, char **envp)
{
    (void)envp;
    const char *in_path = 0, *out_path = 0;
    unsigned long long bs = 512, count = ~0ULL, skip = 0, seek = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *eq = 0;
        for (const char *p = a; *p; p++) if (*p == '=') { eq = p; break; }
        if (!eq) { elog_err("dd: unknown operand '%s'", a); return 2; }
        size_t klen = (size_t)(eq - a);
        const char *val = eq + 1;
        if (aether_strncmp(a, "if", klen) == 0) in_path = val;
        else if (aether_strncmp(a, "of", klen) == 0) out_path = val;
        else if (aether_strncmp(a, "bs", klen) == 0) { if (parse_dd_size(val, &bs) != 0) { elog_err("dd: invalid bs '%s'", val); return 2; } }
        else if (aether_strncmp(a, "count", klen) == 0) { if (parse_dd_size(val, &count) != 0) { elog_err("dd: invalid count '%s'", val); return 2; } }
        else if (aether_strncmp(a, "skip", klen) == 0) { if (parse_dd_size(val, &skip) != 0) { elog_err("dd: invalid skip '%s'", val); return 2; } }
        else if (aether_strncmp(a, "seek", klen) == 0) { if (parse_dd_size(val, &seek) != 0) { elog_err("dd: invalid seek '%s'", val); return 2; } }
        else { elog_err("dd: unknown operand '%s'", a); return 2; }
    }

    int infd = in_path ? (int)sys_open(in_path, O_RDONLY, 0) : 0;
    if (AETHER_IS_ERR((long)infd)) { elog_err("dd: cannot open '%s'", in_path); return 1; }
    int outfd = out_path ? (int)sys_open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666) : 1;
    if (AETHER_IS_ERR((long)outfd)) { elog_err("dd: cannot open '%s'", out_path); sys_close(infd); return 1; }

    /* skip/seek */
    if (skip) sys_lseek(infd, (long)(skip * bs), SEEK_SET);
    if (seek) sys_lseek(outfd, (long)(seek * bs), SEEK_SET);

    static char buf[128 * 1024];
    unsigned long long total = 0;
    unsigned long long blocks = 0;
    for (;;) {
        if (blocks >= count) break;
        long n = sys_read(infd, buf, (size_t)(bs < sizeof(buf) ? bs : sizeof(buf)));
        if (n == 0) break;
        if (AETHER_IS_ERR(n)) { elog_err("dd: read error"); sys_close(infd); sys_close(outfd); return 1; }
        size_t off = 0;
        while (off < (size_t)n) {
            long w = sys_write(outfd, buf + off, (size_t)n - off);
            if (AETHER_IS_ERR(w) || w == 0) { elog_err("dd: write error"); sys_close(infd); sys_close(outfd); return 1; }
            off += (size_t)w;
        }
        total += (unsigned long long)n;
        blocks++;
    }
    if (in_path) sys_close(infd);
    if (out_path) sys_close(outfd);
    elog_info("%llu bytes copied", total);
    return 0;
}
