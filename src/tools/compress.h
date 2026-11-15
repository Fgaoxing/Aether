/*
 * compress.h — zstd / gzip 的纯 C 实现（无外部依赖，nolibc 可用）
 * 覆盖：aether 内置解压（.ko.zst/.ko.gz rescue 场景 + zcat applet）
 *       以及压缩端（gzip fixed-Huffman+LZ77 greedy 压缩；zstd raw/RLE 块压缩）。
 * 返回码：0 成功；-1 数据损坏；-2 输出缓冲太小；-3 不支持的特征。
 * 注意：zstd 压缩端只输出 raw/RLE 块（合法 .zst，压缩率≈1）；
 *       zstd 解压端是完整实现（raw/RLE/compressed 块、Huffman literals、FSE 序列）。
 */
#ifndef AETHER_COMPRESS_H
#define AETHER_COMPRESS_H

#include <stddef.h>

/* 自动识别 magic（1f 8b → gzip；fd 2f b5 28 → zstd）并解压 */
int aether_decompress(const char *src, size_t src_len, char *dst, size_t dst_cap, size_t *out_len);

/* gzip / zstd 分别处理 */
int aether_gunzip(const char *src, size_t src_len, char *dst, size_t dst_cap, size_t *out_len);
int aether_zstdcat(const char *src, size_t src_len, char *dst, size_t dst_cap, size_t *out_len);
int aether_gzip(const char *src, size_t src_len, char *dst, size_t dst_cap, size_t *out_len);
int aether_zstd(const char *src, size_t src_len, char *dst, size_t dst_cap, size_t *out_len);

unsigned long aether_crc32(const char *data, size_t len); /* IEEE reflected（与 gzip 一致） */

#endif /* AETHER_COMPRESS_H */
