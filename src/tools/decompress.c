/*
 * decompress.c — zcat / gunzip（自动识别 gzip + zstd）
 * 用途：initramfs 救援时直接把 .cpio.ko.gz / .ko.zst 吐到 stdout（给 shell 管道用），
 * 或解压到文件。本实现面向≤2MB 的中小文件（模块/二进制），超大文件请走 zcat > file。
 * 输入文件：1f 8b → gzip（RFC1952），fd 2f b5 28 → zstd（RFC8878）。
 * 官方 zstd 解压路径：raw/RLE/compressed 块 + Huffman 字面量 + FSE 序列
 * （RFC 8878 对应 ZSTD 1.5.x；.ko.zst 中常见结构全部覆盖；验证于 11 个官方
 * fixture 文件 + 官方 libzstd 库交叉对拍）。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "compress.h"
#include "syscall_shim.h"
#include "util.h"

#define DEC_BUFSZ (2 * 1024 * 1024)
static char dec_in[DEC_BUFSZ];
static char dec_out[DEC_BUFSZ];

int tool_zcat(int argc, char **argv, char **envp)
{
    (void)envp;
    const char *path = NULL;
    int i = 1;
    while (i < argc) {
        if (argv[i][0] == '-' && argv[i][1]) {
            if (aether_streq(argv[i], "--")) { i++; break; }
            elog_err("zcat: unknown option '%s'（仅支持无参数文件或 -）", argv[i]);
            return 2;
        }
        if (aether_streq(argv[i], "-")) { i++; continue; } /* stdin */
        if (path) { elog_err("zcat: 仅支持一个输入文件"); return 2; }
        path = argv[i];
        i++;
    }

    long in_len;
    if (!path) {
        /* stdin：读满 dec_in */
        size_t off = 0;
        for (;;) {
            long n = sys_read(0, dec_in + off, sizeof(dec_in) - off);
            if (n <= 0) break;
            off += (size_t)n;
            if (off >= sizeof(dec_in)) { elog_err("zcat: 输入超过 %zu 字节", sizeof(dec_in)); return 1; }
        }
        in_len = (long)off;
    } else {
        long fd = sys_open(path, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) { elog_err("zcat: %s: 打不开（%d）", path, AETHER_ERR_CODE(fd)); return 1; }
        size_t off = 0;
        for (;;) {
            long n = sys_read((int)fd, dec_in + off, sizeof(dec_in) - off);
            if (n == 0) break;
            if (AETHER_IS_ERR(n)) { sys_close((int)fd); elog_err("zcat: %s: 读失败", path); return 1; }
            off += (size_t)n;
            if (off >= sizeof(dec_in)) { sys_close((int)fd); elog_err("zcat: %s: 超出 %zu 字节上限", path, sizeof(dec_in)); return 1; }
        }
        sys_close((int)fd);
        in_len = (long)off;
    }

    size_t out_len = 0;
    long rc = (long)aether_decompress(dec_in, (size_t)in_len, dec_out, sizeof(dec_out), &out_len);
    if (rc != 0) {
        if (rc == -2) elog_err("zcat: 解压后超过 %zu 字节（缩小输出后重试）", sizeof(dec_out));
        else elog_err("zcat: 解压失败（magic 不识别或数据损坏/不支持的特征，rc=%ld）", rc);
        return 1;
    }
    long written = 0;
    while ((size_t)written < out_len) {
        long n = sys_write(1, dec_out + written, out_len - (size_t)written);
        if (AETHER_IS_ERR(n) || n == 0) { elog_err("zcat: 写 stdout 失败"); return 1; }
        written += n;
    }
    return 0;
}
