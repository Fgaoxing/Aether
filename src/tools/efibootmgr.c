/*
 * efibootmgr.c — EFI 启动项管理（§10）
 * CLI 对齐原版 efibootmgr 的参数风格，只做救援场景刚需子集：
 *   efibootmgr                 列出所有启动项 + BootOrder
 *   efibootmgr -v              同上，附带设备路径解码（尽力展示）
 *   efibootmgr -b 0001 -B      删除 Boot0001（交互确认）
 *   efibootmgr -o 0002,0001,0000   设置 BootOrder（交互确认）
 * 其余参数报 unsupported option。删除/改序必须交互确认；一次命令只允许一个修改动作；
 * 未挂载的 efivars 会自动尝试挂载；写操作要求 euid==0；-B 必须搭配 -b；-o 与 -B 不允许共存。
 */
#include <stddef.h>

#include "applets.h"
#include "argparse.h"
#include "elog.h"
#include "mem.h"
#include "syscall_shim.h"
#include "sysfsutil.h"
#include "util.h"

#define BOOT_GUID_SUFFIX "-" EFIVARS_GUID

typedef struct {
    unsigned int  num;         /* 0x0001 */
    unsigned int  attrs;       /* LoadOption attrs（已剥掉 efivars 文件级前缀） */
    char          desc[64];    /* UTF-8 近似（ASCII 子集直拷，UTF-16 截断） */
    int           have_desc;
    char          path[512];   /* 原始设备路径字节（用于 -v 解码） */
    size_t        path_len;    /* FilePathListLength */
} boot_entry_t;

#define MAX_BOOT_ENTRIES 128

/* ---------- efivarfs 文件 → boot_entry_t ---------- */

static int le16(const char *p)
{
    return (unsigned char)p[0] | ((unsigned char)p[1] << 8);
}
static unsigned le32(const char *p)
{
    return (unsigned char)p[0] | ((unsigned char)p[1] << 8) | ((unsigned char)p[2] << 16) | ((unsigned char)p[3] << 24);
}

/* efivars 文件布局: u32 attrs + LoadOption{ attrs(4) + FilePathListLength(2) + Description(UTF16,NUL) + FilePathList + OptionalData } */
static int parse_boot_var(const char *path, boot_entry_t *e)
{
    char buf[4096];
    size_t len = 0;
    if (aether_read_file(path, buf, sizeof(buf), &len) != 0) return -1;
    if (len < EFIVARS_ATTRS_LEN + 6) return -1;

    const char *opt = buf + EFIVARS_ATTRS_LEN;   /* LoadOption 起点 */
    size_t opt_len = len - EFIVARS_ATTRS_LEN;
    const char *dend = buf + len;                /* 整个 LoadOption 的终点 */
    if (opt_len < 6) return -1;

    e->attrs = le32(opt);
    size_t fpl = (size_t)le16(opt + 4);          /* FilePathListLength */

    /* Description：UTF-16LE，NUL 终止；仅抄 ASCII 子集 */
    const char *d = opt + 6;
    size_t di = 0;
    e->have_desc = 0;
    while (d + 1 < dend && di + 1 < sizeof(e->desc)) {
        unsigned ch = (unsigned char)d[0] | ((unsigned char)d[1] << 8);
        if (ch == 0) { d += 2; break; }
        if (ch < 0x80) { e->desc[di++] = (char)ch; e->have_desc = 1; }
        else           e->desc[di++] = '?';        /* 非 ASCII：占位 */
        d += 2;
    }
    e->desc[di] = '\0';

    /* FilePathList：紧接 Description 之后 */
    if (d <= dend) {
        size_t remaining = (size_t)(dend - d);
        e->path_len = fpl < remaining ? fpl : remaining;
        for (size_t i = 0; i < e->path_len; i++) e->path[i] = d[i];
    } else {
        e->path_len = 0;
    }
    return 0;
}

/* ---------- 解码设备路径 ---------- */

static void guid_str(const char *b, char *out, size_t cap)
{
    /* EFI GUID：前三段按 LE 解释，展示成标准 xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    if (cap < 37) { if (cap) out[0] = '\0'; return; }
    const char *hx = "0123456789abcdef";
    char *p = out;
    unsigned a = ((unsigned)(unsigned char)b[3] << 24) | ((unsigned)(unsigned char)b[2] << 16) |
                 ((unsigned)(unsigned char)b[1] << 8)  | (unsigned char)b[0];
    unsigned c = ((unsigned)(unsigned char)b[5] << 8) | (unsigned char)b[4];
    unsigned e = ((unsigned)(unsigned char)b[7] << 8) | (unsigned char)b[6];
    for (int i = 28; i >= 0; i -= 4) *p++ = hx[(a >> i) & 0xf];
    *p++ = '-';
    for (int i = 12; i >= 0; i -= 4) *p++ = hx[(c >> i) & 0xf];
    *p++ = '-';
    for (int i = 12; i >= 0; i -= 4) *p++ = hx[(e >> i) & 0xf];
    *p++ = '-';
    *p++ = hx[((unsigned char)b[8] >> 4) & 0xf]; *p++ = hx[(unsigned char)b[8] & 0xf];
    *p++ = hx[((unsigned char)b[9] >> 4) & 0xf]; *p++ = hx[(unsigned char)b[9] & 0xf];
    *p++ = '-';
    for (int i = 10; i < 16; i++) { *p++ = hx[((unsigned char)b[i] >> 4) & 0xf]; *p++ = hx[(unsigned char)b[i] & 0xf]; }
    *p = '\0';
}

static int hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 1–4 位十六进制 Boot 编号 */
static int parse_bootnum(const char *s, unsigned long long *out)
{
    unsigned long long v = 0;
    int n = 0;
    if (!s || !*s) return -1;
    while (*s) {
        int d = hex_nib(*s);
        if (d < 0 || n >= 4) return -1;
        v = (v << 4) | (unsigned long long)d;
        s++; n++;
    }
    *out = v;
    return 0;
}

static void decode_device_path(const char *path, size_t len)
{
    size_t i = 0;
    while (i + 4 <= len) {
        unsigned type = (unsigned char)path[i];
        unsigned subtype = (unsigned char)path[i + 1];
        unsigned nlen = (unsigned)le16(path + i + 2);
        if (nlen < 4 || i + nlen > len) {
            elog_info("  [unknown device path node: bad length]");
            return;
        }
        const char *node = path + i;
        const char *data = node + 4;
        size_t dlen = nlen - 4;

        if (type == 0x04 && subtype == 0x01 && dlen >= 37) {
            /* Hard Drive: partNumber(4) partitionStart(8) partitionSize(8) signature(16 GUID) sigType(1) */
            char guid[40];
            unsigned pnum = le32(data);
            unsigned long long startl = le32(data + 4), starth = le32(data + 8);
            unsigned long long sizel = le32(data + 12), sizeh = le32(data + 16);
            unsigned long long start = startl | (starth << 32);
            unsigned long long size  = sizel | (sizeh << 32);
            unsigned sigtype = (unsigned char)data[36];
            const char *stype = sigtype == 2 ? "GPT" : sigtype == 1 ? "MBR" : "?";
            guid_str(data + 20, guid, sizeof(guid));
            elog_info("  HD(part=%d, %s, start=0x%llx, size=0x%llx, sig=%s)",
                      pnum, stype, start, size, guid);
        } else if (type == 0x04 && subtype == 0x04) {
            /* File: UTF-16LE 路径 */
            char s[256];
            size_t k = 0, j = 0;
            while (j + 1 < dlen && k + 1 < sizeof(s)) {
                unsigned ch = (unsigned char)data[j] | ((unsigned char)data[j + 1] << 8);
                if (ch == 0) break;
                s[k++] = (ch < 0x80) ? (char)ch : '?';
                j += 2;
            }
            s[k] = '\0';
            elog_info("  %s", s);
        } else if (type == 0x02) {
            elog_info("  [ACPI/PciRoot path: type=0x%02x subtype=0x%02x]", type, subtype);
        } else if (type == 0x7f) {
            return; /* End of Device Path */
        } else {
            elog_info("  [unknown device path node: type=0x%02x subtype=0x%02x]", type, subtype);
        }
        i += nlen;
    }
}

/* ---------- 主命令 ---------- */

typedef struct {
    boot_entry_t entries[MAX_BOOT_ENTRIES];
    int          n;
} boot_list_t;

static void list_boot_cb(const char *name, unsigned char d_type, void *ctx)
{
    (void)d_type;
    boot_list_t *list = ctx;
    /* BootXXXX-<guid>；efivarfs 的 GUID 一律小写 */
    if (name[0] != 'B' || name[1] != 'o' || name[2] != 'o' || name[3] != 't') return;
    const char *nhex = name + 4;
    unsigned num = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_nib(nhex[i]);
        if (d < 0) return;
        num = (num << 4) | (unsigned)d;
    }
    if (nhex[4] != '-') return;
    /* 整体长度 = 8 + strlen("-guid") */
    size_t suffix_len = aether_strlen(BOOT_GUID_SUFFIX);
    if (aether_strlen(name) != 8 + suffix_len) return;
    if (aether_strncmp(name + 8, BOOT_GUID_SUFFIX, suffix_len) != 0) return;

    /* 拼完整路径 */
    char dir[128];
    if (aether_efivars_dir(dir, sizeof(dir)) != 0) return;
    char path[512];
    size_t d = 0;
    for (size_t i = 0; dir[i]; i++) path[d++] = dir[i];
    path[d++] = '/';
    for (size_t i = 0; name[i]; i++) path[d++] = name[i];
    path[d] = '\0';

    if (list->n >= MAX_BOOT_ENTRIES) return;
    boot_entry_t *e = &list->entries[list->n];
    e->num = num;
    if (parse_boot_var(path, e) == 0)
        list->n++;
}

static int cmd_list(int verbose)
{
    unsigned short bo[64];
    int nb = 0, have_bo = 0;
    if (aether_efivars_bootorder(bo, 64, &nb) == 0) have_bo = 1;

    if (have_bo) {
        char order[512];
        size_t d = 0;
        order[0] = '\0';
        for (int i = 0; i < nb && d + 6 < sizeof(order); i++) {
            const char *hx = "0123456789abcdef";
            char hex[5];
            hex[0] = hx[(bo[i] >> 12) & 0xf]; hex[1] = hx[(bo[i] >> 8) & 0xf];
            hex[2] = hx[(bo[i] >> 4) & 0xf];  hex[3] = hx[bo[i] & 0xf];
            hex[4] = '\0';
            if (i) { order[d++] = ','; order[d] = '\0'; }
            for (size_t k = 0; hex[k]; k++) order[d++] = hex[k];
            order[d] = '\0';
        }
        elog_info("BootOrder: %s", order);
    } else {
        elog_info("BootOrder: 未设置");
    }

    boot_list_t list;
    list.n = 0;
    char dir[128];
    if (aether_efivars_dir(dir, sizeof(dir)) != 0) {
        elog_err("efivars: 未发现 UEFI 变量文件系统（非 UEFI 或已禁用）");
        return 1;
    }
    if (aether_foreach_dirent(dir, list_boot_cb, &list) != 0)
        elog_err("efivars: 读取 %s 失败", dir);

    /* 数字序输出 */
    for (int i = 0; i < list.n; i++) {
        /* 找最小 num（简单选择排序） */
        int min_idx = i;
        for (int j = i + 1; j < list.n; j++)
            if (list.entries[j].num < list.entries[min_idx].num) min_idx = j;
        if (min_idx != i) {
            boot_entry_t tmp = list.entries[i];
            list.entries[i] = list.entries[min_idx];
            list.entries[min_idx] = tmp;
        }
        boot_entry_t *e = &list.entries[i];
        const char *active = (e->attrs & 1) ? "*" : " ";
        elog_info("Boot%04x%s %s", e->num, active, e->have_desc ? e->desc : "(no description)");
        if (verbose) decode_device_path(e->path, e->path_len);
    }
    return 0;
}

static int cmd_delete(unsigned int bootnum)
{
    if (sys_geteuid() != 0) { elog_err("efibootmgr: 删除需要 root 权限"); return 1; }
    if (!aether_efivars_boot_exists(bootnum)) {
        elog_err("efibootmgr: Boot%04x 不存在", bootnum);
        return 1;
    }
    char prompt[64];
    char *p = prompt;
    const char *pre = "You are about to delete Boot";
    while (*pre) *p++ = *pre++;
    const char *hx = "0123456789abcdef";
    *p++ = hx[(bootnum >> 12) & 0xf]; *p++ = hx[(bootnum >> 8) & 0xf];
    *p++ = hx[(bootnum >> 4) & 0xf];  *p++ = hx[bootnum & 0xf];
    const char *suf = ". Proceed? [y/N] ";
    while (*suf) *p++ = *suf++;
    *p = '\0';

    if (!aether_ask(prompt, "y")) { elog_err("efibootmgr: aborted"); return 1; }
    if (aether_efivars_delete_boot(bootnum) != 0) {
        elog_err("efibootmgr: 删除 Boot%04x 失败（efivars 未挂载为 rw？）", bootnum);
        return 1;
    }
    elog_info("Boot%04x 已删除", bootnum);
    return 0;
}

static int parse_bootorder_list(const char *s, unsigned short *out, int max, int *n_out)
{
    int n = 0;
    const char *p = s;
    for (;;) {
        unsigned v = 0, nd = 0;
        while (*p && *p != ',') {
            int d = hex_nib(*p);
            if (d < 0 || nd >= 4) return -1;
            v = (v << 4) | (unsigned)d;
            p++; nd++;
        }
        if (nd == 0) return -1;
        if (n >= max) return -1;
        out[n++] = (unsigned short)v;
        if (*p == '\0') break;
        p++;
    }
    *n_out = n;
    return 0;
}

static int cmd_set_order(const char *listarg)
{
    if (sys_geteuid() != 0) { elog_err("efibootmgr: 写 NVRAM 需要 root 权限"); return 1; }

    unsigned short newlist[64];
    int n = 0;
    if (parse_bootorder_list(listarg, newlist, 64, &n) != 0) {
        elog_err("efibootmgr: -o 格式应为 '0002,0001,0000'");
        return 2;
    }
    /* 校验存在性 */
    for (int i = 0; i < n; i++) {
        if (!aether_efivars_boot_exists(newlist[i])) {
            elog_err("efibootmgr: Boot%04x 不存在，拒绝写入 BootOrder", newlist[i]);
            return 1;
        }
    }
    /* 显示对比并确认 */
    unsigned short old[64];
    int nb = 0, have_old = 0;
    if (aether_efivars_bootorder(old, 64, &nb) == 0) have_old = 1;

    elog_info("Current BootOrder: %s", have_old ? "" : "(unset)");
    if (have_old) {
        for (int i = 0; i < nb; i++) elog_info("  Boot%04x", old[i]);
    }
    elog_info("New BootOrder:");
    for (int i = 0; i < n; i++) elog_info("  Boot%04x", newlist[i]);

    if (!aether_ask("Proceed? [y/N] ", "y")) { elog_err("efibootmgr: aborted"); return 1; }

    if (aether_efivars_set_bootorder(newlist, n) != 0) {
        elog_err("efibootmgr: 写 BootOrder 失败（efivars 未挂载？试：mount -t efivarfs efivarfs %s 或 -o remount,rw）",
                 "/sys/firmware/efi/efivars");
        return 1;
    }
    elog_info("BootOrder 已更新");
    return 0;
}

int tool_efibootmgr(int argc, char **argv, char **envp)
{
    (void)envp;
    mem_arena_t *arena = mem_arena_create();
    aether_option_t opts[] = {
        { 'v', 0, 0, 0, "显示设备路径" },
        { 'b', 1, 0, 0, "Boot 编号（如 0001）" },
        { 'B', 0, 0, 0, "删除 -b 指定的启动项" },
        { 'o', 1, 0, 0, "设置 BootOrder（如 0002,0001）" },
    };
    const size_t nopts = sizeof(opts) / sizeof(opts[0]);
    aether_args_t out;

    if (aether_parse_args("efibootmgr", opts, nopts, argc, argv, arena, &out) != 0)
        return 2;
    if (out.positional_count > 0) {
        elog_err("efibootmgr: unsupported option -- '%s'", out.positionals[0]);
        mem_arena_destroy(arena);
        return 2;
    }

    int have_v  = aether_opt_count(opts, nopts, 'v') > 0;
    int have_b  = aether_opt_count(opts, nopts, 'b') > 0;
    int have_B  = aether_opt_count(opts, nopts, 'B') > 0;
    int have_o  = aether_opt_count(opts, nopts, 'o') > 0;
    const char *bval = aether_opt_value(opts, nopts, 'b');
    const char *oval = aether_opt_value(opts, nopts, 'o');

    /* 组合合法性（§6） */
    if (have_B && !have_b) { elog_err("efibootmgr: -b 必须搭配 -B"); mem_arena_destroy(arena); return 2; }
    if (have_b && !have_B) { elog_err("efibootmgr: -b 必须搭配 -B（v1 只支持删除语义）"); mem_arena_destroy(arena); return 2; }
    if (have_o && have_B)  { elog_err("efibootmgr: -o 与 -B 不能同时出现"); mem_arena_destroy(arena); return 2; }
    if (have_v && (have_B || have_o)) { elog_err("efibootmgr: -v 仅用于列表"); mem_arena_destroy(arena); return 2; }

    /* 若 NVRAM 写需要，先确保 efivars 可写（只读时尽量 remount） */
    if (have_B || have_o) {
        if (aether_ensure_efivarfs(1) != 0) {
            elog_err("efibootmgr: 无法挂载/重挂载 efivarfs（需要 root；非 UEFI 系统？）");
            mem_arena_destroy(arena);
            return 1;
        }
    }

    int rc;
    if (have_B) {
        unsigned long long bootnum = 0;
        if (!bval || parse_bootnum(bval, &bootnum) != 0) {
            elog_err("efibootmgr: -b 应为 4 位十六进制，如 0001");
            mem_arena_destroy(arena);
            return 2;
        }
        rc = cmd_delete((unsigned int)bootnum);
    } else if (have_o) {
        rc = cmd_set_order(oval);
    } else {
        rc = cmd_list(have_v);
    }
    mem_arena_destroy(arena);
    return rc;
}
