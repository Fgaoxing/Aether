/*
 * modload.c — PID 1 早期按序 finit_module 加载镜像内模块
 * 对应 mkinitcpio 的 udev 钩子（自动按依赖加载 MODULES）。
 * 模块列表：/etc/aether/modules.order（每行一个模块路径，可带 # 注释）
 * 模块文件名保持内核包里的原样（.ko.zst 由 CONFIG_MODULE_DECOMPRESS=y 解压）。
 * 任何一步失败只报错不致命（rescue 环境可能没有对应硬件）。
 */
#include <stddef.h>

#include "modload.h"
#include "elog.h"
#include "syscall_shim.h"
#include "sysfsutil.h"
#include "util.h"
#include "uevent.h"

/* 按文件魔数判断是否需要 MODULE_INIT_COMPRESSED_FILE（内核侧解压）。
 * 0x7f 'E' 'L' 'F' → 裸 .ko（flags=0）；否则按压缩格式尝试 flag=4。 */
#define MODULE_INIT_COMPRESSED_FILE 4
static int module_flags_of(long fd)
{
    unsigned char magic[4] = {0, 0, 0, 0};
    long r = sys_lseek((int)fd, 0, 0 /* SEEK_SET */);
    if (AETHER_IS_ERR(r)) return 0;
    r = sys_read((int)fd, magic, sizeof(magic));
    sys_lseek((int)fd, 0, 0); /* 倒回，finit_module/init_module 需要从头读 */
    if (r != 4) return 0;
    if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') return 0;
    return MODULE_INIT_COMPRESSED_FILE;
}

/* 无 /etc/aether/modules.order 时的兜底：
 * 多遍扫描 /lib/modules 下所有 *.ko / *.ko.zst，每遍尝试全部未加载模块，
 * 直到某一遍零成功。依赖顺序由多遍自然满足（依赖先于其使用者成功）。 */
#define MODULE_MAX 256
static char g_module_paths[MODULE_MAX][256];
static int g_module_n;

typedef struct { char prefix[192]; int depth; } collect_ctx_t;

static void collect_cb(const char *name, unsigned char d_type, void *vctx)
{
    collect_ctx_t *c = (collect_ctx_t *)vctx;
    if (name[0] == '.') return;
    char full[256];
    size_t d = 0;
    while (c->prefix[d] && d + 1 < sizeof(full)) { full[d] = c->prefix[d]; d++; }
    if (d + 1 + aether_strlen(name) + 1 > sizeof(full)) return;
    if (d > 0 && full[d - 1] != '/') full[d++] = '/';
    size_t i = 0;
    while (name[i] && d + 1 < sizeof(full)) full[d++] = name[i++];
    full[d] = '\0';

    if (d_type == 4 /* DT_DIR */) {
        collect_ctx_t sub;
        size_t k = 0;
        for (k = 0; full[k] && k + 1 < sizeof(sub.prefix); k++) sub.prefix[k] = full[k];
        sub.prefix[k] = '\0';
        sub.depth = c->depth + 1;
        if (sub.depth <= 6 && aether_foreach_dirent(sub.prefix, collect_cb, &sub) == 0)
            return;
        return;
    }
    /* 普通文件：*.ko 或 *.ko.zst（.ko.zst 魔数判定在 finit 前由 module_flags_of 完成） */
    size_t len = aether_strlen(full);
    int is_ko = (len >= 3 && aether_strncmp(full + len - 3, ".ko", 3) == 0)
             || (len >= 7 && aether_strncmp(full + len - 7, ".ko.zst", 7) == 0);
    if (is_ko && g_module_n < MODULE_MAX) {
        size_t j = 0;
        while (full[j] && j + 1 < sizeof(g_module_paths[g_module_n])) { g_module_paths[g_module_n][j] = full[j]; j++; }
        g_module_paths[g_module_n][j] = '\0';
        g_module_n++;
    }
}

static void collect_modules(const char *dir)
{
    collect_ctx_t ctx;
    size_t k = 0;
    while (dir[k] && k + 1 < sizeof(ctx.prefix)) { ctx.prefix[k] = dir[k]; k++; }
    ctx.prefix[k] = '\0';
    ctx.depth = 0;
    aether_foreach_dirent(dir, collect_cb, &ctx);
}

static int fallback_load(void)
{
    collect_modules("/lib/modules");
    if (g_module_n == 0) { elog_info("modload: no module files found under /lib/modules"); return 0; }

    int done[MODULE_MAX];
    for (int i = 0; i < MODULE_MAX; i++) done[i] = 0;
    int loaded_total = 0;

    /* 一个 pass：全部未加载模块并行 fork 子进程 finit_module（.ko.zst 由内核在
     * 调用进程上下文解压，天然 SMP 并行）；wait4 收集结果。失败的模块留给
     * 下一 pass 重试。依赖顺序由多遍自然保证（依赖先载入成功、使用者下一遍再试）。
     * fork 失败时该模块退回串行加载，不影响其余。 */
    static long pass_pids[MODULE_MAX];
    static int  pass_idx[MODULE_MAX];

    for (int pass = 0; pass < g_module_n; pass++) {
        int nproc = 0, loaded_pass = 0;
        for (int i = 0; i < g_module_n; i++) {
            if (done[i]) continue;
            long pid = sys_fork();
            if (AETHER_IS_ERR(pid)) {
                /* fork 失败：串行兜底 */
                long fd = sys_open(g_module_paths[i], O_RDONLY, 0);
                if (!AETHER_IS_ERR(fd)) {
                    long r = sys_finit_module((int)fd, "", module_flags_of(fd));
                    sys_close((int)fd);
                    if (!AETHER_IS_ERR(r) || AETHER_ERR_CODE(r) == 17) { done[i] = 1; loaded_pass++; }
                }
                continue;
            }
            if (pid == 0) {
                long fd = sys_open(g_module_paths[i], O_RDONLY, 0);
                if (AETHER_IS_ERR(fd)) aether_exit(43);
                long r = sys_finit_module((int)fd, "", module_flags_of(fd));
                sys_close((int)fd);
                if (!AETHER_IS_ERR(r)) aether_exit(0);
                if (AETHER_ERR_CODE(r) == 17) aether_exit(42); /* EEXIST 也算成功 */
                aether_exit(44);
            }
            pass_pids[nproc] = pid;
            pass_idx[nproc] = i;
            nproc++;
        }
        for (int j = 0; j < nproc; j++) {
            int st = 0;
            long r = sys_wait4((int)pass_pids[j], &st, 0, 0);
            int code = 44;
            if (!AETHER_IS_ERR(r)) code = (st >> 8) & 0xff;
            if (code == 0 || code == 42) { done[pass_idx[j]] = 1; loaded_pass++; }
        }
        loaded_total += loaded_pass;
        if (loaded_pass == 0) break;
    }

    int failed = 0;
    for (int i = 0; i < g_module_n; i++) if (done[i] == 0) failed++;
    elog_info("modload: fallback scan of %d modules -> %d loaded, %d failed (multiple passes)", g_module_n, loaded_total, failed);
    if (loaded_total) aether_uevent_pump(1500);
    return loaded_total;
}

int aether_load_modules(void)
{
    char buf[8192];
    size_t len = 0;
    if (aether_read_file("/etc/aether/modules.order", buf, sizeof(buf) - 1, &len) != 0)
        return fallback_load(); /* 无模块列表（如 mkinitcpio 生成的镜像）：多遍兜底扫描 */
    buf[len] = '\0';

    int loaded = 0, failed = 0;
    const char *p = buf;
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        size_t llen = (size_t)(p - line);
        if (*p == '\n') p++;
        /* 去首尾空白 */
        while (llen > 0 && (line[0] == ' ' || line[0] == '\t')) { line++; llen--; }
        while (llen > 0 && (line[llen - 1] == ' ' || line[llen - 1] == '\t' || line[llen - 1] == '\r')) llen--;
        if (llen == 0 || line[0] == '#') continue;
        char path[256];
        size_t copy = llen < sizeof(path) - 1 ? llen : sizeof(path) - 1;
        for (size_t i = 0; i < copy; i++) path[i] = line[i];
        path[copy] = '\0';

        long fd = sys_open(path, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) {
            elog_err("modload: open %s failed (%d)", path, AETHER_ERR_CODE(fd));
            failed++;
            continue;
        }
        long r = sys_finit_module((int)fd, "", module_flags_of(fd));
        sys_close((int)fd);
        if (AETHER_IS_ERR(r)) {
            int e = AETHER_ERR_CODE(r);
            /* EEXIST（17）= 模块已内建/已加载：不是错 */
            if (e == 17) elog_info("modload: %s already loaded (skip)", path);
            else { elog_err("modload: finit_module %s failed (%d)", path, e); failed++; }
        } else {
            elog_info("modload: + %s", path);
            loaded++;
        }
    }
    if (loaded || failed)
        elog_info("modload: %d modules loaded, %d failed", loaded, failed);
    if (loaded)
        aether_uevent_pump(1500); /* 新设备节点落地 */
    return loaded;
}
