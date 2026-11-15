/*
 * util.h — 共享字符串原语（§3：不依赖 libc 亦可工作；各分支共用）
 */
#ifndef AETHER_UTIL_H
#define AETHER_UTIL_H

#include <stddef.h>

int    aether_strcmp(const char *a, const char *b);
int    aether_streq(const char *a, const char *b);
int    aether_strncmp(const char *a, const char *b, size_t n);
size_t aether_strlen(const char *s);
char  *aether_strcpy(char *dst, const char *src, size_t cap);
size_t aether_u64toa(char *dst, size_t cap, unsigned long long v);
long   aether_atol(const char *s);
const  char *aether_base_name(const char *path);
/* 交互确认：打印 prompt，从 stdin 读一行，与 accept 完全一致才返回 1（如 "YES" 或 "y"） */
int    aether_ask(const char *prompt, const char *accept);
/* 分区分类：nvme/mmcblk/loop 及普通盘；parent 填父盘名（整盘为 ""）；*is_part=1 表示分区 */
void   aether_classify_part(const char *name, char *parent, size_t pcap, int *is_part);
/* KiB → 人类可读（如 1.3G；B/K/M/G/T 一位小数） */
void   aether_human_size(unsigned long long kb, char *out, size_t cap);
/* ISO 时间：epoch 秒 → "2026-11-14 04:33"（UTC，救援环境无时区） */
size_t aether_iso_time(long long sec, char *out, size_t cap);
/* 路径拼接：dir + "/" + name → dst（自动去重斜杠） */
int    aether_path_join(char *dst, size_t cap, const char *dir, const char *name);
/* mode 位 → "drwxr-xr-x"（10 字符，不含类型位则 [-]） */
void aether_mode_str(unsigned int mode, char out[11]);
/* 人类可读字节数（如 1.3G），已有 aether_human_size 吃 KiB；此函数吃字节 */
void aether_human_bytes(unsigned long long bytes, char *out, size_t cap);
/* 缓冲读取器：按字节/按行，自动缓冲（各文本工具共用） */
typedef struct {
    int fd;
    char buf[65536];
    size_t start, len;
} aether_reader_t;

void aether_reader_init(aether_reader_t *r, int fd);
/* 读一个字节；EOF 返回 -1 */
int aether_reader_getc(aether_reader_t *r);
/* 读一行到 out（含 '\n'？不含）；返回长度（0 = 空行，-1 = EOF） */
long aether_reader_line(aether_reader_t *r, char *out, size_t cap);

/* 快速读取全部 stdin/文件（用于需要内存化整个文件的工具，如 sort/shuf/wc） */
long aether_read_all(int fd, char *buf, size_t cap);

/* 人类：打印一行（带换行），不动 stdout 以外的 fd */
void aether_prints(const char *s);
/* 按显示宽度对齐填充（align: 0=左对齐 右补空格；1=右对齐 左补空格）。返回写入字节数 */
size_t aether_str_pad(char *dst, size_t cap, const char *src, size_t width, int align);
/* UTF-8 显示宽度（列数）：East Asian Wide/全角算 2 列，其余可打印字符 1 列，控制字符 0 */
size_t aether_utf8_cells(const char *s);

#endif /* AETHER_UTIL_H */
