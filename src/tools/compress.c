/*
 * compress.c — gzip(RFC1952/1951) 与 zstd(RFC8878) 的纯 C 实现，无外部依赖。
 * 本文件先提供 gzip；zstd 在文件末尾（后续追加）。
 */
#include <stddef.h>
#include <string.h>

#include "compress.h"

/* =============== CRC-32（IEEE reflected，用于 gzip） =============== */

static unsigned long crc32_table[256];
static int crc32_table_ready;

static void crc32_init_table(void)
{
    if (crc32_table_ready) return;
    for (unsigned long i = 0; i < 256; i++) {
        unsigned long c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

unsigned long aether_crc32(const char *data, size_t len)
{
    crc32_init_table();
    unsigned long c = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ (unsigned char)data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

/* =============== 通用整数/位流原语 =============== */

static unsigned read_le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned read_le32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned long)p[3] << 24);
}

typedef struct {
    const unsigned char *src;
    size_t src_len;
    size_t pos;      /* 字节位置 */
    unsigned bitbuf; /* 位缓冲（LSB first） */
    int bitcnt;      /* 位缓冲内有效位数 */
} bit_reader_t;

static void br_init(bit_reader_t *br, const char *src, size_t len)
{
    br->src = (const unsigned char *)src;
    br->src_len = len;
    br->pos = 0;
    br->bitbuf = 0;
    br->bitcnt = 0;
}

static unsigned br_bit(bit_reader_t *br)
{
    if (br->bitcnt == 0) {
        if (br->pos >= br->src_len) return 0; /* 越界读 0（调用方按截断处理） */
        br->bitbuf = br->src[br->pos++];
        br->bitcnt = 8;
    }
    unsigned b = br->bitbuf & 1;
    br->bitbuf >>= 1;
    br->bitcnt--;
    return b;
}

static unsigned br_bits(bit_reader_t *br, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) v |= br_bit(br) << i;
    return v;
}

static void br_skip_to_byte(bit_reader_t *br) { br->bitcnt = 0; br->bitbuf = 0; }

/* =============== Huffman（RFC1951 canonical） =============== */

#define HUFF_MAXBITS 15
#define HUFF_MAXSYMS 288

typedef struct {
    short counts[HUFF_MAXBITS + 1]; /* 每个长度的码元数 */
    short symbols[HUFF_MAXSYMS];    /* 按 canonical 序排列的符号 */
} huff_t;

/* 由码长数组（n 个符号的长度）建表；返回 0 成功。码长全 0 → 错误。 */
static int huff_build(huff_t *h, const unsigned char *lengths, int n)
{
    int i;
    for (i = 0; i <= HUFF_MAXBITS; i++) h->counts[i] = 0;
    for (i = 0; i < n; i++) {
        if (lengths[i] > HUFF_MAXBITS) return -1;
        if (lengths[i]) h->counts[lengths[i]]++;
    }
    int left = 1;
    int maxbits = 0;
    for (i = 1; i <= HUFF_MAXBITS; i++) {
        left <<= 1;
        left -= h->counts[i];
        if (left < 0) return -1; /* 超订阅：编码歧义，必须拒绝 */
        if (h->counts[i]) maxbits = i;
    }
    /* left > 0 = 不完整码：RFC1951 允许（某些码元不出现）；解码端遇到无匹配即报错 */
    /* 偏移表 */
    int offs[HUFF_MAXBITS + 2];
    offs[1] = 0;
    for (i = 1; i <= HUFF_MAXBITS; i++)
        offs[i + 1] = offs[i] + h->counts[i];
    for (i = 0; i < n; i++)
        if (lengths[i]) h->symbols[offs[lengths[i]]++] = (short)i;
    return maxbits == 0 ? -1 : 0;
}

/* 解码一个符号；失败返回 -1 */
static int huff_decode(bit_reader_t *br, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= HUFF_MAXBITS; len++) {
        code |= (int)br_bit(br);
        int count = h->counts[len];
        if (((code) - first) < count)
            return h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* =============== 距离/长度基础值表（RFC1951） =============== */

static const unsigned LEN_BASE[29]  = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const unsigned LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const unsigned DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const unsigned DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
/* 固定 Huffman 长度码表 */
static void fixed_tables(huff_t *lit, huff_t *dist)
{
    unsigned char l[288];
    int i;
    for (i = 0; i <= 143; i++) l[i] = 8;
    for (i = 144; i <= 255; i++) l[i] = 9;
    for (i = 256; i <= 279; i++) l[i] = 7;
    for (i = 280; i <= 287; i++) l[i] = 8;
    huff_build(lit, l, 288);
    unsigned char d[30];
    for (i = 0; i < 30; i++) d[i] = 5;
    huff_build(dist, d, 30);
}

/* 动态表解码（btype == 2） */
static int decode_dynamic(bit_reader_t *br, huff_t *lit, huff_t *dist)
{
    static const int order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    unsigned hlit = br_bits(br, 5) + 257;
    unsigned hdist = br_bits(br, 5) + 1;
    unsigned hclen = br_bits(br, 4) + 4;
    if (hlit > 286 || hdist > 32) return -1;
    unsigned char clen[19] = {0};
    for (unsigned i = 0; i < hclen; i++) clen[order[i]] = (unsigned char)br_bits(br, 3);
    huff_t cl;
    if (huff_build(&cl, clen, 19) != 0) return -1;
    unsigned char lengths[286 + 32];
    unsigned total = hlit + hdist;
    unsigned i = 0;
    while (i < total) {
        int sym = huff_decode(br, &cl);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[i++] = (unsigned char)sym;
        } else if (sym == 16) {
            if (i == 0) return -1;
            unsigned rep = br_bits(br, 2) + 3;
            unsigned char prev = lengths[i - 1];
            if (i + rep > total) return -1;
            while (rep--) lengths[i++] = prev;
        } else if (sym == 17) {
            unsigned rep = br_bits(br, 3) + 3;
            if (i + rep > total) return -1;
            while (rep--) lengths[i++] = 0;
        } else { /* 18 */
            unsigned rep = br_bits(br, 7) + 11;
            if (i + rep > total) return -1;
            while (rep--) lengths[i++] = 0;
        }
    }
    if (huff_build(lit, lengths, hlit) != 0) return -1;
    if (huff_build(dist, lengths + hlit, hdist) != 0) return -1;
    return 0;
}

/* =============== gzip 解压 =============== */

int aether_gunzip(const char *src, size_t src_len, char *dst, size_t dst_cap, size_t *out_len)
{
    size_t pos = 0;
    /* 头部解析在成员循环内完成（多 member 拼接 gzip 也复用同一逻辑） */
    size_t dst_len = 0;
    for (;;) {
        if (src_len - pos < 10) return -1;
        if ((unsigned char)src[pos] != 0x1f || (unsigned char)src[pos + 1] != 0x8b) return -1;
        if ((unsigned char)src[pos + 2] != 0x08) return -1; /* 只支持 deflate(CM=8) */
        unsigned mflg = (unsigned char)src[pos + 3];
        size_t mpos = pos + 10;
        if (mflg & 4) { if (mpos + 2 > src_len) return -1; mpos += 2 + read_le16((const unsigned char *)src + mpos); }
        if (mflg & 8) { while (mpos < src_len && src[mpos] != '\0') mpos++; mpos++; }
        if (mflg & 16) { while (mpos < src_len && src[mpos] != '\0') mpos++; mpos++; }
        if (mflg & 2) mpos += 2;
        if (mpos >= src_len) return -1;

        size_t member_base = dst_len; /* 本 member 在输出中的起点（CRC/ISIZE 只校验本 member） */
        bit_reader_t br;
        br_init(&br, src + mpos, src_len - mpos);
        int bfinal = 0;
        while (!bfinal) {
            bfinal = (int)br_bit(&br);
            unsigned btype = br_bits(&br, 2);
            if (btype == 0) {
                br_skip_to_byte(&br);
                if (br.pos + 4 > br.src_len) return -1;
                unsigned len = read_le16(br.src + br.pos);
                unsigned nlen = read_le16(br.src + br.pos + 2);
                if ((len ^ nlen) != 0xffff) return -1;
                br.pos += 4;
                if (br.pos + len > br.src_len) return -1;
                if (dst_len + len > dst_cap) return -2;
                for (unsigned i = 0; i < len; i++) dst[dst_len++] = (char)br.src[br.pos++];
                continue;
            }
            huff_t lit, dist;
            if (btype == 1) {
                fixed_tables(&lit, &dist);
            } else if (btype == 2) {
                if (decode_dynamic(&br, &lit, &dist) != 0) return -1;
            } else return -1;
            for (;;) {
                int sym = huff_decode(&br, &lit);
                if (sym < 0) return -1;
                if (sym < 256) {
                    if (dst_len >= dst_cap) return -2;
                    dst[dst_len++] = (char)sym;
                    continue;
                }
                if (sym == 256) break; /* end of block */
                if (sym < 257 || sym > 285) return -1;
                unsigned lcode = (unsigned)(sym - 257);
                unsigned len = LEN_BASE[lcode] + br_bits(&br, LEN_EXTRA[lcode]);
                int dsym = huff_decode(&br, &dist);
                if (dsym < 0 || dsym > 29) return -1;
                unsigned dist_val = DIST_BASE[dsym] + br_bits(&br, DIST_EXTRA[dsym]);
                if (dist_val > dst_len) return -1; /* 距离超前（窗口之外）→ 损坏 */
                if (dst_len + len > dst_cap) return -2;
                for (unsigned i = 0; i < len; i++) {
                    dst[dst_len] = dst[dst_len - dist_val];
                    dst_len++;
                }
            }
        }
        br_skip_to_byte(&br);
        if (mpos + br.pos + 8 > src_len) return -1;
        unsigned long crc_in = read_le32((const unsigned char *)src + mpos + br.pos);
        unsigned long isize_in = read_le32((const unsigned char *)src + mpos + br.pos + 4);
        if (aether_crc32(dst + member_base, dst_len - member_base) != crc_in) return -1;
        if ((unsigned long)(dst_len - member_base) != isize_in) return -1;
        pos = mpos + br.pos + 8;
        if (pos == src_len) break;
        if (pos > src_len) return -1;
        if ((unsigned char)src[pos] == 0x1f && (unsigned char)src[pos + 1] == 0x8b) continue; /* 拼接 gzip member */
        return -1; /* 尾部有垃圾 */
    }
    if (out_len) *out_len = dst_len;
    return 0;
}

/* =============== 自动识别 =============== */

int aether_decompress(const char *src, size_t src_len, char *dst, size_t dst_cap, size_t *out_len)
{
    if (src_len >= 2 && (unsigned char)src[0] == 0x1f && (unsigned char)src[1] == 0x8b)
        return aether_gunzip(src, src_len, dst, dst_cap, out_len);
    if (src_len >= 4 && read_le32((const unsigned char *)src) == 0xFD2FB528UL)
        return aether_zstdcat(src, src_len, dst, dst_cap, out_len);
    return -3;
}

/* =============== gzip 压缩（fixed Huffman + greedy LZ77） =============== */

typedef struct {
    char *dst;
    size_t cap;
    size_t len;
    unsigned bitbuf;
    int bitcnt;
    int err; /* 输出溢出标记 */
} bit_writer_t;

static void bw_init(bit_writer_t *bw, char *dst, size_t cap)
{
    bw->dst = dst; bw->cap = cap; bw->len = 0; bw->bitbuf = 0; bw->bitcnt = 0; bw->err = 0;
}

static void bw_byte(bit_writer_t *bw, unsigned char b)
{
    if (bw->len >= bw->cap) { bw->err = 1; return; }
    bw->dst[bw->len++] = (char)b;
}

static void bw_bit(bit_writer_t *bw, unsigned b)
{
    bw->bitbuf |= (b & 1) << bw->bitcnt;
    bw->bitcnt++;
    if (bw->bitcnt == 8) { bw_byte(bw, (unsigned char)bw->bitbuf); bw->bitbuf = 0; bw->bitcnt = 0; }
}

static void bw_bits_msb(bit_writer_t *bw, unsigned code, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--) bw_bit(bw, (code >> i) & 1);
}

static void bw_bits_le(bit_writer_t *bw, unsigned code, int nbits)
{
    for (int i = 0; i < nbits; i++) bw_bit(bw, (code >> i) & 1);
}

static void bw_flush(bit_writer_t *bw)
{
    if (bw->bitcnt > 0) { bw_byte(bw, (unsigned char)bw->bitbuf); bw->bitbuf = 0; bw->bitcnt = 0; }
}

static void bw_fixed_lit(bit_writer_t *bw, unsigned sym)
{
    if (sym <= 143) bw_bits_msb(bw, sym + 48, 8);
    else if (sym <= 255) bw_bits_msb(bw, sym + 256, 9);   /* 256 + sym = 9-bit code (sym+256-144+? ) */
    else if (sym <= 279) bw_bits_msb(bw, sym - 256, 7);
    else bw_bits_msb(bw, sym - 88, 8);                    /* 280-287 → 192-199: sym-88 */
}

static void bw_len_code(bit_writer_t *bw, unsigned len)
{
    /* len 3..258 → 257..285（LEN_BASE 单调递增，直接查表） */
    int idx = 28;
    while (idx > 0 && LEN_BASE[idx] > len) idx--;
    bw_fixed_lit(bw, (unsigned)(257 + idx));
    if (LEN_EXTRA[idx]) bw_bits_le(bw, len - LEN_BASE[idx], (int)LEN_EXTRA[idx]);
}

static void bw_dist_code(bit_writer_t *bw, unsigned dist)
{
    /* DIST_BASE 单调递增：找最大且 ≤ dist 的 code */
    int code = 29;
    while (code > 0 && DIST_BASE[code] > dist) code--;
    bw_bits_msb(bw, (unsigned)code, 5); /* distance codes: 5 bits, MSB first */
    if (DIST_EXTRA[code]) bw_bits_le(bw, dist - DIST_BASE[code], (int)DIST_EXTRA[code]);
}

/* 3 字节哈希 */
#define HASH_BITS 10
#define HASH_SIZE (1 << HASH_BITS)
static unsigned hash3(const char *p)
{
    return ((unsigned char)p[0] * 6519081u + (unsigned char)p[1] * 18089u + (unsigned char)p[2] * 1237u) >> (24 - HASH_BITS) & (HASH_SIZE - 1);
}

int aether_gzip(const char *src, size_t src_len, char *dst, size_t dst_cap, size_t *out_len)
{
    if (dst_cap < 18) return -2; /* 10B 头 + 8B 尾最小 */
    bit_writer_t bw;
    bw_init(&bw, dst, dst_cap);
    /* gzip 头：magic + CM + FLG=0 + MTIME=0 + XFL=2(fastest, 任意) + OS=255 */
    bw_byte(&bw, 0x1f); bw_byte(&bw, 0x8b); bw_byte(&bw, 0x08); bw_byte(&bw, 0x00);
    bw_byte(&bw, 0x00); bw_byte(&bw, 0x00); bw_byte(&bw, 0x00); bw_byte(&bw, 0x00);
    bw_byte(&bw, 0x02); bw_byte(&bw, 0xff);
    /* 单块：BFINAL=1, BTYPE=01(fixed) */
    bw_bit(&bw, 1); bw_bits_le(&bw, 1, 2);

    int head[HASH_SIZE];
    for (int i = 0; i < HASH_SIZE; i++) head[i] = -1;
    int  prev[32768];
    size_t pos = 0;
    while (pos < src_len) {
        int best_len = 0;
        unsigned best_dist = 0;
        if (pos + 3 <= src_len) {
            unsigned h = hash3(src + pos);
            int cand = head[h];
            int max_match = (int)src_len - (int)pos;
            if (max_match > 258) max_match = 258;
            int limit = (int)pos - 32768;
            int chain = 64;
            while (cand >= 0 && cand > limit && chain-- > 0) {
                int len = 0;
                while (len < max_match && src[cand + len] == src[pos + len]) len++;
                if (len > best_len) { best_len = len; best_dist = (unsigned)(pos - (size_t)cand); if (len >= 258) break; }
                cand = prev[cand & 32767];
            }
            head[h] = (int)pos;
            prev[pos & 32767] = cand;
        }
        if (best_len >= 3) {
            bw_len_code(&bw, (unsigned)best_len);
            bw_dist_code(&bw, best_dist);
            pos += (size_t)best_len;
        } else {
            bw_fixed_lit(&bw, (unsigned char)src[pos]);
            pos++;
        }
    }
    bw_fixed_lit(&bw, 256); /* 结束块 */
    bw_flush(&bw);
    /* 尾部：CRC32(4) + ISIZE(4) LE */
    unsigned long crc = aether_crc32(src, src_len);
    for (int i = 0; i < 4; i++) bw_byte(&bw, (unsigned char)((crc >> (8 * i)) & 0xff));
    unsigned long isize_mod = (unsigned long)(src_len & 0xffffffffUL);
    for (int i = 0; i < 4; i++) bw_byte(&bw, (unsigned char)((isize_mod >> (8 * i)) & 0xff));
    if (bw.err) return -2;
    if (out_len) *out_len = bw.len;
    return 0;
}

/* =============== zstd（RFC 8878）解压 =============== */

#define ZSKIP_MIN 0x184D2A50u
#define ZSKIP_MAX 0x184D2A5Fu
#define Z_MAX_DICT_ID 4
#define Z_MAX_WINDOW (64u << 20)

/* 小端读取 */
static unsigned long long read_le_u64(const unsigned char *p) {
    unsigned long long v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* =============== 精确移植的官方 BIT_DStream（64 位容器倒序读） ===============
 * 直接复制 ZSTD BIT_initDStream / BIT_readBits / BIT_reloadDStream 的行为：
 * 窗口化、bitsConsumed、overflow 溢出停止。zstd 解码器所有位流
 * （Huffman 权重 FSE、字面量 Huffman、序列 FSE）都从末尾倒序 MSB-first 读。 */
typedef struct {
    const unsigned char *start;
    const unsigned char *ptr;
    const unsigned char *limitPtr;
    unsigned long long container;
    unsigned bitsConsumed;
} abitd_t;

static int abitd_reload(abitd_t *d);

#define ABITD_UNFINISHED 0
#define ABITD_END_OF_BUFFER 1
#define ABITD_COMPLETED 2
#define ABITD_OVERFLOW 3

static unsigned long long abitd_read_le64(const unsigned char *p)
{
    unsigned long long v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void abitd_init(abitd_t *d, const unsigned char *buf, size_t len)
{
    /* 本解码器只用于 weights FSE 流（len >= 73，恒 >= 8） */
    d->start = buf;
    d->limitPtr = buf + 8;
    d->ptr = buf + len - 8;
    d->container = abitd_read_le64(d->ptr);
    {
        unsigned char lastByte = buf[len - 1];   /* 最后一个非零字节；结束位之后是 0 */
        int highbit = 7;
        while (((lastByte >> highbit) & 1) == 0) highbit--;
        d->bitsConsumed = (unsigned)(8 - highbit); /* ZSTD: bitsConsumed = 8 - ZSTD_highbit32(lastByte)；结束标记位位置 = highbit / 最高位下标 */
    }
}

static unsigned abitd_read_bits(abitd_t *d, unsigned n)
{
    if (n == 0) return 0; /* 0 位读 = 无操作（满复位 cell） */
    /* 当前容器剩余位数必须能满足本次读取；不足则重填窗口 */
    if (d->bitsConsumed + n > 64) abitd_reload(d);
    unsigned long long shifted = d->container << (d->bitsConsumed & 63);
    unsigned v = (unsigned)(shifted >> (64 - n));
    d->bitsConsumed += n;
    return v;
}

static unsigned abitd_peek_bits(const abitd_t *d, unsigned n)
{
    abitd_t tmp = *d;
    return abitd_read_bits(&tmp, n);
}

static void abitd_skip_bits(abitd_t *d, unsigned n)
{
    if (n == 0) return;
    if (d->bitsConsumed + n > 64) abitd_reload(d);
    d->bitsConsumed += n;
}

static int abitd_reload(abitd_t *d)
{
    if (d->bitsConsumed > 64) return ABITD_OVERFLOW;
    if (d->ptr >= d->limitPtr) {
        d->ptr -= d->bitsConsumed >> 3;
        d->bitsConsumed &= 7;
        d->container = abitd_read_le64(d->ptr);
        return ABITD_UNFINISHED;
    }
    if (d->ptr == d->start)
        return d->bitsConsumed < 64 ? ABITD_END_OF_BUFFER : ABITD_COMPLETED;
    {
        unsigned nbBytes = d->bitsConsumed >> 3;
        int result = ABITD_UNFINISHED;
        if (d->ptr - nbBytes < d->start) {
            nbBytes = (unsigned)(d->ptr - d->start);
            result = ABITD_END_OF_BUFFER;
        }
        d->ptr -= nbBytes;
        d->bitsConsumed -= nbBytes * 8;
        d->container = abitd_read_le64(d->ptr);
        return result;
    }
}

/* =============== FSE 表描述解码 + 建表（RFC 8878 §4.1.1） =============== */

#define FSE_MAX_TABLE_BITS 12
#define FSE_MAX_TABLE_SIZE (1 << FSE_MAX_TABLE_BITS)
#define FSE_MAX_SYMBOLS 256

typedef struct {
    unsigned char symbol;
    unsigned char num_bits;
    unsigned baseline;
} fse_cell_t;

typedef struct {
    fse_cell_t cells[FSE_MAX_TABLE_SIZE];
    int table_log;
} fse_table_t;

static int fse_build_from_probs(fse_table_t *tbl, const unsigned *prob, int nsyms, int accuracy_log);

/* 前向 LSB-first 位读（FSE 表头用） */
typedef struct { const unsigned char *src; size_t len; size_t pos; unsigned bitbuf; int bitcnt; } fwd_t;
static void fwd_init(fwd_t *fw, const unsigned char *src, size_t len) { fw->src = src; fw->len = len; fw->pos = 0; fw->bitbuf = 0; fw->bitcnt = 0; }
static unsigned fwd_bit(fwd_t *fw)
{
    if (fw->bitcnt == 0) {
        if (fw->pos >= fw->len) return 0;
        fw->bitbuf = fw->src[fw->pos++];
        fw->bitcnt = 8;
    }
    unsigned b = fw->bitbuf & 1; fw->bitbuf >>= 1; fw->bitcnt--; return b;
}
static unsigned fwd_bits(fwd_t *fw, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) v |= fwd_bit(fw) << i;
    return v;
}

/* 从 FSE 表头流解码，重建 decoding table。返回 0 成功；-1 损坏。
 * expected_syms：期望解码的符号数（LL=36, ML=53, offset=-1, huffman=-1） */
static int fse_decode_header(fse_table_t *tbl, const unsigned char *src, size_t src_len,
                             int max_accuracy_log, int expected_syms, size_t *consumed_out)
{
    fwd_t br; fwd_init(&br, src, src_len);
    int accuracy_log = (int)fwd_bits(&br, 4) + 5;
    if (accuracy_log > max_accuracy_log || accuracy_log > FSE_MAX_TABLE_BITS) return -1;
    int table_size = 1 << accuracy_log;

    int remain = table_size;
    unsigned prob[FSE_MAX_SYMBOLS];  /* -1 = 小于1概率（"less than 1"） */
    for (int i = 0; i < FSE_MAX_SYMBOLS; i++) prob[i] = 0;
    int nsyms = 0;
    int s = 0;
    while (remain > 0 && s < FSE_MAX_SYMBOLS) {
        int maxValue = remain + 1;
        int bits = 1; while ((1 << bits) - 1 < maxValue) bits++;
        if (bits > 16) return -1;
        int threshold = (1 << bits) - 1 - maxValue;
        unsigned small = fwd_bits(&br, bits - 1);
        int value;
        if ((int)small < threshold) value = (int)small;
        else {
            unsigned b = fwd_bit(&br);
            if (b == 0) value = (int)small;
            else value = (int)small + (1 << (bits - 1)) - threshold;
        }
        int p = value - 1; /* P = Value - 1；0 → “less than 1” */
        if (p == 0) {
            /* 零概率：读 2-bit Repeat，可能连续多个 */
            prob[s] = 0;
            int zeroes = 0;
            for (;;) {
                unsigned rep = fwd_bits(&br, 2);
                zeroes += (int)rep;
                if (rep != 3) break;
                if (s + 1 + zeroes > FSE_MAX_SYMBOLS) return -1;
            }
            s += 1 + zeroes;
            nsyms += 1 + zeroes;
            continue;
        }
        if (p == -1) prob[s] = 0xFFFFFFFFu; /* sentinel：less than 1 */
        else prob[s] = (unsigned)p;
        remain -= (p == -1) ? 1 : p;
        s++;
        nsyms++;
        if (remain < 0) return -1;
    }
    if (remain != 0) return -1;
    /* zstd 自定义 FSE 表的符号数不超过最大符号数即可，不必等于 expected_syms */
    if (expected_syms >= 0 && nsyms > expected_syms) return -1;

    if (fse_build_from_probs(tbl, prob, FSE_MAX_SYMBOLS, accuracy_log) != 0) return -1;
    if (consumed_out) {
        /* 位流结束位置（字节对齐向上取整） */
        *consumed_out = (br.pos + br.bitcnt + 8 - 1) / 8;
        /* 注意：br.pos 是已消耗字节数，bitcnt 是缓冲内有效位（尚未消费） */
        /* 精确做法：已消费位数 = br.pos*8 - br.bitcnt */
        *consumed_out = (br.pos * 8 - (size_t)br.bitcnt + 7) / 8;
    }
    return 0;
}

/* 预定义表（RFC 8878 §3.1.1.3.2.2） */
/* 统一的 FSE 建表器（与 RFC 8878 §4.1.1 / zstd FSE_buildDTable_internal 一致）：
   1) 概率 -1（less than 1）符号先放表尾并向前填；
   2) 其余符号按自然序 spread（pos += step; 跳过 low-prob 区域）；
   3) 按表序逐格：nS = symbolNext[sym]++; nbBits = tableLog - floor(log2(nS))；
      newState = (nS << nbBits) - tableSize */
static int fse_build_from_probs(fse_table_t *tbl, const unsigned *prob, int nsyms, int accuracy_log)
{
    int table_size = 1 << accuracy_log;
    unsigned char sym_of[FSE_MAX_TABLE_SIZE];
    for (int i = 0; i < table_size; i++) sym_of[i] = 0xFF;

    int highThreshold = table_size - 1;
    unsigned symbolNext[FSE_MAX_SYMBOLS];
    for (int i = 0; i < nsyms; i++) {
        if (prob[i] == 0xFFFFFFFFu) {
            sym_of[highThreshold--] = (unsigned char)i;
            symbolNext[i] = 1;
        } else {
            symbolNext[i] = prob[i];
        }
    }
    int position = 0;
    int step = (table_size >> 1) + (table_size >> 3) + 3;
    int mask = table_size - 1;
    for (int s = 0; s < nsyms; s++) {
        if (prob[s] == 0 || prob[s] == 0xFFFFFFFFu) continue;
        for (unsigned k = 0; k < prob[s]; k++) {
            sym_of[position] = (unsigned char)s;
            position = (position + step) & mask;
            while (position > highThreshold) position = (position + step) & mask;
        }
    }
    if (position != 0) return -1; /* 概率分布损坏 */

    for (int u = 0; u < table_size; u++) {
        unsigned char s = sym_of[u];
        unsigned nS = symbolNext[s]++;
        /* floor(log2(nS))，nS >= 1 */
        int hb = 0;
        while ((1u << (hb + 1)) <= nS) hb++;
        tbl->cells[u].symbol = s;
        tbl->cells[u].num_bits = (unsigned char)(accuracy_log - hb);
        tbl->cells[u].baseline = (nS << (accuracy_log - hb)) - (unsigned)table_size;
    }
    tbl->table_log = accuracy_log;
    return 0;
}

static int fse_predefined(fse_table_t *tbl, const short *dist, int n, int accuracy_log)
{
    unsigned prob[FSE_MAX_SYMBOLS];
    for (int i = 0; i < n; i++) {
        if (dist[i] == -1) prob[i] = 0xFFFFFFFFu;
        else if (dist[i] == 0) prob[i] = 0;
        else prob[i] = (unsigned)dist[i];
    }
    return fse_build_from_probs(tbl, prob, n, accuracy_log);
}

/* RLE 表（单符号）：num_bits=0，baseline=0 */
static void fse_rle(fse_table_t *tbl, int symbol)
{
    tbl->cells[0].symbol = (unsigned char)symbol;
    tbl->cells[0].num_bits = 0;
    tbl->cells[0].baseline = 0;
    tbl->table_log = 0;
}

static const short LL_DEFAULT[36] = {
    4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1,
    -1, -1, -1, -1
};
static const short ML_DEFAULT[53] = {
    1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1,
    -1, -1, -1, -1, -1
};
static const short OFF_DEFAULT[29] = {
    1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1
};

/* LL/ML/OFF 码表（base + extra bits） */
static const unsigned LL_BASE[36] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,18,20,22,24,28,32,40,48,64,128,256,512,1024,2048,4096,8192,16384,32768,65536
};
static const unsigned LL_EXTRA[36] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,4,6,7,8,9,10,11,12,13,14,15,16
};
static const unsigned ML_BASE[53] = {
    3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,
    35,37,39,41,43,47,51,59,67,83,99,131,259,515,1027,2051,4099,8195,16387,32771,65539
};
static const unsigned ML_EXTRA[53] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,1,1,1,2,2,3,3,4,4,5,7,8,9,10,11,12,13,14,15,16
};
static const unsigned OF_BASE[32] = {
    0,1,1,5,0xD,0x1D,0x3D,0x7D,
    0xFD,0x1FD,0x3FD,0x7FD,0xFFD,0x1FFD,0x3FFD,0x7FFD,
    0xFFFD,0x1FFFD,0x3FFFD,0x7FFFD,0xFFFFD,0x1FFFFD,0x3FFFFD,0x7FFFFD,
    0xFFFFFD,0x1FFFFFD,0x3FFFFFD,0x7FFFFFD,0xFFFFFFD,0x1FFFFFFD,0x3FFFFFFD,0x7FFFFFFD
};
static const unsigned OF_EXTRA[32] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
};

/* =============== Huffman（RFC 8878 §4.2） =============== */

#define HUF_MAX_BITS 11
#define HUF_MAX_SYMBOLS 256
#define HUF_TABLE_CELLS (1 << HUF_MAX_BITS)   /* 按最长码长分配 cell 表 */

typedef struct {
    short counts[HUF_MAX_BITS + 1];
    int max_bits;
    /* X1 模型：cell[i] = (sym << 4) | nbBits。解码时按最高有效位往后查
     * 一个 max_bits 宽窗口，cell 本身就是符号 + 码长，nbBits < max_bits 时
     * span = 2^(max_bits - nbBits) 个连续 cell 共享同一符号。 */
    unsigned short cells[HUF_TABLE_CELLS];
} zhuff_t;

/* 由码长建 X1 模型 cell 表（桶 nbBits 降序、同桶符号下标升序填充） */
static int zhuff_build(zhuff_t *h, const unsigned char *lengths, int nsym, int max_bits)
{
    int i;
    for (i = 0; i <= HUF_MAX_BITS; i++) h->counts[i] = 0;
    for (i = 0; i < nsym; i++) {
        if (lengths[i] > HUF_MAX_BITS) return -1;
        if (lengths[i]) h->counts[lengths[i]]++;
    }
    int left = 1;
    int mb = 0;
    for (i = 1; i <= HUF_MAX_BITS; i++) {
        left <<= 1;
        left -= h->counts[i];
        if (left < 0) return -1;
        if (h->counts[i]) mb = i;
    }
    if (mb == 0) return -1; /* 全零 */
    h->max_bits = max_bits > 0 ? max_bits : mb;
    if (h->max_bits > HUF_MAX_BITS) return -1;
    /* ZSTD X1 填充顺序：从最长码开始，sort后直接查表解码 */
    int pos = 0;
    for (int nb = h->max_bits; nb >= 1; nb--) {
        int span = 1 << (h->max_bits - nb);
        for (i = 0; i < nsym; i++) {
            if (lengths[i] != nb) continue;
            for (int c = 0; c < span; c++)
                h->cells[pos++] = (unsigned short)((i << 4) | nb);
        }
    }
    if (pos != (1 << h->max_bits)) return -1;
    return 0;
}

static int zhuff_decode(abitd_t *fr, const zhuff_t *h)
{
    unsigned val = abitd_peek_bits(fr, (unsigned)h->max_bits);
    unsigned cell = h->cells[val];
    abitd_skip_bits(fr, cell & 15);
    return (int)(cell >> 4);
}

/* 直接权重列表 → 码长数组（含隐式最后权重）。返回符号数（0 失败）。 */
static int weights_to_lengths(const unsigned char *weights, int nweights, unsigned char *lengths_out)
{
    unsigned sum = 0;
    for (int i = 0; i < nweights; i++)
        if (weights[i]) sum += 1u << (weights[i] - 1);
    /* 找到最小 total_bits 使 2^bits > sum */
    int bits = 0;
    unsigned total = 1;
    while (total <= sum) { total <<= 1; bits++; }
    /* 最后隐式权重 = total - sum，必须是 2 的幂 */
    unsigned last_weight_contrib = total - sum;
    if ((last_weight_contrib & (last_weight_contrib - 1)) != 0) return -1;
    int last_weight = 0;
    { unsigned t = last_weight_contrib; while (t > 1) { t >>= 1; last_weight++; } last_weight++; }
    /* 组合完整权重列表：weights + 隐式最后权重 */
    unsigned char all[257];
    int n = nweights;
    for (int i = 0; i < nweights; i++) all[i] = weights[i];
    all[n] = (unsigned char)last_weight;
    int nsym = n + 1;
    /* bits → Number_of_Bits：NumBits = MaxBits + 1 - Weight；Weight 0 → 0 */
    int max_bits = bits;
    unsigned char lengths[257];
    for (int i = 0; i < nsym; i++)
        lengths[i] = all[i] == 0 ? 0 : (unsigned char)(max_bits + 1 - all[i]);
    /* sort（按 weight 升序，同 weight 保持自然序）—— canonical code 分配不需要，
       zhuff_build 只按 length 计数分配 offset 即可 */
    for (int i = 0; i < 257; i++) lengths_out[i] = lengths[i];
    return nsym;
}

/* Huffman Tree Description → zhuff_t。consumed_out = tree 描述消耗的字节数。 */
static int huffman_tree_from_header(zhuff_t *h, const unsigned char *src, size_t src_len, size_t *consumed_out)
{
    if (src_len < 1) return -1;
    unsigned char hb = src[0];
    unsigned char weights[257];
    int nweights = 0;
    if (hb >= 128) {
        /* 直接编码 */
        int nsym = hb - 127;
        size_t need = (size_t)((nsym + 1) / 2);
        if ((size_t)1 + need > src_len) return -1;
        const unsigned char *p = src + 1;
        for (int i = 0; i < nsym; i++) {
            if ((i & 1) == 0) weights[i] = (unsigned char)(p[i / 2] >> 4);
            else weights[i] = (unsigned char)(p[i / 2] & 0x0f);
        }
        nweights = nsym;
        *consumed_out = (size_t)(1 + need);
    } else {
        /* FSE 压缩：hb = 压缩字节数（0-127） */
        int comp_size = hb;
        if ((size_t)1 + comp_size > src_len) return -1;
        /* FSE 解码：2 个交错状态，AccuracyLog ≤ 6，最多 256 个符号。
         * 采用官方 BIT_DStream 容器语义（窗口化 + bitsConsumed > 64 终止），
         * 与 RFC/ZSTD FSE_decompress_usingDTable_generic 一致 */
        fse_table_t wtbl;
        size_t consumed = 0;
        if (fse_decode_header(&wtbl, src + 1, comp_size, 6, -1, &consumed) != 0) return -1;
        abitd_t fr;
        abitd_init(&fr, src + 1 + consumed, (size_t)comp_size - consumed);
        int state1 = (int)abitd_read_bits(&fr, (unsigned)wtbl.table_log);
        abitd_reload(&fr);
        int state2 = (int)abitd_read_bits(&fr, (unsigned)wtbl.table_log);
        int n = 0;
        /* 官方 FSE_decompress_usingDTable_generic 的两段结构：
         * main loop：每次迭代输出 4 符号（s1,s2,s1,s2）；reload==unfinished 才继续
         * tail loop：逐符号输出，overflow 之后 break。reload 语义照搬位于官方 BIT_reloadDStream
         * 官方没有“位不足”概念：endOfBuffer 窗口耗尽后仍然按 cells 输出，
         * 直到 bitsConsumed>64（overflow）才停止 */
        for (;;) {
            if (n >= 252 || abitd_reload(&fr) != ABITD_UNFINISHED)   /* FSE_MAX_TABLELOG*4+7 <= 64，无中间 reload 分支 */
                break;
            fse_cell_t c11 = wtbl.cells[state1]; weights[n++] = c11.symbol;
            state1 = (int)(c11.baseline + abitd_read_bits(&fr, c11.num_bits));
            fse_cell_t c22 = wtbl.cells[state2]; weights[n++] = c22.symbol;
            if (n >= 253) return -1;
            state2 = (int)(c22.baseline + abitd_read_bits(&fr, c22.num_bits));
            if (n >= 252 || abitd_reload(&fr) != ABITD_UNFINISHED) break;
            c11 = wtbl.cells[state1]; weights[n++] = c11.symbol;
            state1 = (int)(c11.baseline + abitd_read_bits(&fr, c11.num_bits));
            c22 = wtbl.cells[state2]; weights[n++] = c22.symbol;
            if (n >= 253) return -1;
            state2 = (int)(c22.baseline + abitd_read_bits(&fr, c22.num_bits));
        }
        while (1) {
            if (n >= 257) return -1;
            fse_cell_t c1 = wtbl.cells[state1];
            weights[n++] = c1.symbol;
            if (n >= 257) return -1;
            state1 = (int)(c1.baseline + abitd_read_bits(&fr, c1.num_bits));
            if (abitd_reload(&fr) == ABITD_OVERFLOW) {
                fse_cell_t c2 = wtbl.cells[state2];
                weights[n++] = c2.symbol;
                break;
            }
            fse_cell_t c2 = wtbl.cells[state2];
            weights[n++] = c2.symbol;
            if (n >= 257) return -1;
            state2 = (int)(c2.baseline + abitd_read_bits(&fr, c2.num_bits));
            if (abitd_reload(&fr) == ABITD_OVERFLOW) {
                fse_cell_t c1b = wtbl.cells[state1];
                weights[n++] = c1b.symbol;
                break;
            }
        }
        nweights = n;
        *consumed_out = (size_t)(1 + comp_size);
    }
    unsigned char lengths[257];
    int nsym = weights_to_lengths(weights, nweights, lengths);
    if (nsym <= 0) return -1;
    memset(weights, 0, sizeof(weights));
    if (zhuff_build(h, lengths, nsym, 0) != 0) return -1;
    return 0;
}

/* =============== 帧/literals 段（Stage B2） =============== */

typedef struct {
    unsigned char data[1 << 17]; /* literals 上限 128KB（block 最大 128KB） */
    size_t regen;
} zliterals_t;

typedef struct {
    int have_huffman;
    zhuff_t huffman;
    int have_ll, have_ml, have_off;
    fse_table_t ll_tab, ml_tab, off_tab;
    int rep1, rep2, rep3;   /* 重复偏移：块间持久（RFC §3.1.1.5，首块={1,4,8}） */
} zframe_ctx_t;

static int zstd_decode_literals(zliterals_t *L, const unsigned char **cursor, const unsigned char *end,
                                zframe_ctx_t *ctx)
{
    const unsigned char *p = *cursor;
    if (end - p < 1) return -1;
    unsigned char b0 = p[0];
    unsigned type = b0 & 3;
    unsigned size_format = (b0 >> 2) & 3;
    size_t regen = 0, comp = 0, hdr = 1;

    if (type == 0 || type == 1) { /* raw / RLE */
        if (size_format == 0 || size_format == 2) { regen = (size_t)(b0 >> 3); hdr = 1; }
        else if (size_format == 1) { regen = (size_t)(b0 >> 4) | ((size_t)p[1] << 4); hdr = 2; }
        else { regen = (size_t)(b0 >> 4) | ((size_t)p[1] << 4) | ((size_t)p[2] << 12); hdr = 3; }
        if (regen > sizeof(L->data)) return -1;
        if (type == 0) {
            if ((size_t)(end - p) < hdr + regen) return -1;
            memcpy(L->data, p + hdr, regen);
            *cursor = p + hdr + regen;
        } else {
            if ((size_t)(end - p) < hdr + 1) return -1;
            memset(L->data, p[hdr], regen);
            *cursor = p + hdr + 1;
        }
        L->regen = regen;
        return 0;
    }

    /* compressed(2) / treeless(3) */
    if (size_format == 0) { /* 1 stream, 10 bits sizes */
        regen = (size_t)(b0 >> 4) | ((size_t)(p[1] & 0x3f) << 4);
        comp = (size_t)((p[1] >> 6) | ((size_t)p[2] << 2));
        hdr = 3;
    } else if (size_format == 1) { /* 4 streams, 10 bits sizes */
        regen = (size_t)(b0 >> 4) | ((size_t)(p[1] & 0x3f) << 4);
        comp = (size_t)((p[1] >> 6) | ((size_t)p[2] << 2));
        hdr = 3;
    } else if (size_format == 2) { /* 4 streams, 14 bits sizes */
        regen = (size_t)(b0 >> 4) | ((size_t)p[1] << 4) | ((size_t)(p[2] & 0x03) << 12);
        comp = (size_t)((p[2] >> 2) | ((size_t)p[3] << 6));
        hdr = 4;
    } else { /* 4 streams, 18 bits sizes */
        regen = (size_t)(b0 >> 4) | ((size_t)p[1] << 4) | ((size_t)(p[2] & 0x3f) << 12);
        comp = (size_t)((p[2] >> 6) | ((size_t)p[3] << 2) | ((size_t)p[4] << 10));
        hdr = 5;
    }
    if ((size_t)(end - p) < hdr + comp) return -1;
    size_t consumed_hdr = hdr;      /* 流本体从 p + hdr 开始，但 comp 可能含 tree desc */
    size_t body = comp;             /* 含 tree desc（若存在） */

    /* 解 Huffman（若 type == 2 且需要新树） */
    if (type == 2) {
        size_t tree_consumed = 0;
        if (huffman_tree_from_header(&ctx->huffman, p + consumed_hdr, (size_t)(end - p - consumed_hdr), &tree_consumed) != 0)
            return -1;
        if (tree_consumed > body) return -1;
        body -= tree_consumed;
        consumed_hdr += tree_consumed;
        ctx->have_huffman = 1;
    } else { /* treeless */
        if (!ctx->have_huffman) return -1;
    }

    size_t total_streams = body;
    size_t stream_sizes[4];
    int nstreams = 1;
    if (size_format == 1 || size_format == 2 || size_format == 3) {
        nstreams = 4;
        if (total_streams < 6) return -1;
        const unsigned char *jt = p + consumed_hdr;
        stream_sizes[0] = read_le16(jt);
        stream_sizes[1] = read_le16(jt + 2);
        stream_sizes[2] = read_le16(jt + 4);
        size_t s3sum = stream_sizes[0] + stream_sizes[1] + stream_sizes[2];
        if (s3sum > total_streams - 6) return -1;
        stream_sizes[3] = total_streams - 6 - s3sum;
        consumed_hdr += 6;
    } else {
        stream_sizes[0] = total_streams;
    }

    /* 每流期望符号数：4X 时 regen/4 向上取整，首末段末尾补足；
     * 1X 单流时全部 regen 符号都从唯一流中解出 */
    if (regen > sizeof(L->data)) return -1;
    size_t per = (regen + 3) / 4;
    size_t segment = (regen + 3) / 4;          /* 4X 输出是连续段：stream k → [k*segment, ...) */
    size_t exp[4] = { per, per, per, regen - 3 * per };
    if (nstreams == 1) {
        segment = regen;
        exp[0] = regen;
    }

    const unsigned char *sp = p + consumed_hdr;
    for (int si = 0; si < nstreams; si++) {
        size_t ssize = stream_sizes[si];
        size_t want = exp[si];
        size_t base = (size_t)si * segment;
        abitd_t fr;
        abitd_init(&fr, sp, ssize);
        for (size_t k = 0; k < want; k++) {
            int sym = zhuff_decode(&fr, &ctx->huffman);
            if (sym < 0) return -1;
            L->data[base + k] = (unsigned char)sym;
        }
        sp += ssize;
    }
    L->regen = regen;
    *cursor = sp;
    return 0;
}

/* =============== 序列段（RFC 8878 §3.1.1.3.2） =============== */

typedef struct { int kk[4]; } zdummy_t; /* 未用，保持结构清晰 */

/* LL/ML FSE 码 → base + extra bits 查找（随表在文件尾部给出） */
static int zstd_decode_sequences(char *dst, size_t dst_cap, size_t *out_io,
                                 const char *src, size_t src_len, size_t *pos_io,
                                 zliterals_t *L, zframe_ctx_t *ctx)
{
    const unsigned char *p = (const unsigned char *)src + *pos_io;
    const unsigned char *end = (const unsigned char *)src + src_len;
    size_t out = *out_io;

    /* sequences 段为空：整块就是字面量 */
    if (p == end) {
        if (L->regen > 0) {
            if (out + L->regen > dst_cap) return -2;
            memcpy(dst + out, L->data, L->regen);
            out += L->regen;
        }
        /* 重复偏移保持不变 */
        *out_io = out;
        return 0;
    }
    /* Sequences_Section_Header */
    if ((size_t)(end - p) < 2) return -1;
    unsigned b0 = p[0];
    size_t num_seqs = 0;
    size_t hsize = 1;
    if (b0 == 0) { num_seqs = 0; hsize = 1; }
    else if (b0 < 128) num_seqs = b0;
    else if (b0 < 255) { num_seqs = ((size_t)(b0 - 128) << 8) | p[1]; hsize = 2; }
    else { num_seqs = ((size_t)p[1] << 8) | p[2]; num_seqs += 0x7F00; hsize = 3; }
    p += hsize;
    if (end - p < 1) return -1;
    unsigned modes = p[0]; p++;
    int ll_mode = (modes >> 6) & 3, off_mode = (modes >> 4) & 3, ml_mode = (modes >> 2) & 3;
    if (modes & 3) return -1; /* reserved must be 0 */

    fse_table_t t_ll, t_off, t_ml;
    int ok;

    /* LL 表 */
    if (ll_mode == 0)      { ok = fse_predefined(&t_ll, LL_DEFAULT, 36, 6); if (ok != 0) return -1; ctx->have_ll = 1; ctx->ll_tab = t_ll; }
    else if (ll_mode == 1) { fse_rle(&t_ll, p[0]); p++; ctx->have_ll = 1; ctx->ll_tab = t_ll; }
    else if (ll_mode == 2) { size_t consumed = 0; ok = fse_decode_header(&t_ll, p, (size_t)(end - p), 9, 36, &consumed); if (ok != 0) return -1; p += consumed; ctx->have_ll = 1; ctx->ll_tab = t_ll; }
    else                   { if (!ctx->have_ll) return -1; t_ll = ctx->ll_tab; }

    /* Offset 表（预定义有 29 个码；max accuracy 8；期望符号数不定 → -1） */
    if (off_mode == 0)      { ok = fse_predefined(&t_off, OFF_DEFAULT, 29, 5); if (ok != 0) return -1; ctx->have_off = 1; ctx->off_tab = t_off; }
    else if (off_mode == 1) { fse_rle(&t_off, p[0]); p++; ctx->have_off = 1; ctx->off_tab = t_off; }
    else if (off_mode == 2) { size_t consumed = 0; ok = fse_decode_header(&t_off, p, (size_t)(end - p), 8, -1, &consumed); if (ok != 0) return -1; p += consumed; ctx->have_off = 1; ctx->off_tab = t_off; }
    else                    { if (!ctx->have_off) return -1; t_off = ctx->off_tab; }

    /* ML 表 */
    if (ml_mode == 0)      { ok = fse_predefined(&t_ml, ML_DEFAULT, 53, 6); if (ok != 0) return -1; ctx->have_ml = 1; ctx->ml_tab = t_ml; }
    else if (ml_mode == 1) { fse_rle(&t_ml, p[0]); p++; ctx->have_ml = 1; ctx->ml_tab = t_ml; }
    else if (ml_mode == 2) { size_t consumed = 0; ok = fse_decode_header(&t_ml, p, (size_t)(end - p), 9, 53, &consumed); if (ok != 0) return -1; p += consumed; ctx->have_ml = 1; ctx->ml_tab = t_ml; }
    else                   { if (!ctx->have_ml) return -1; t_ml = ctx->ml_tab; }

    if (num_seqs == 0) {
        *pos_io = (size_t)(p - (const unsigned char *)src);
        /* 剩余 literals 全存到输出 */
        if (L->regen > 0) {
            if (out + L->regen > dst_cap) return -2;
            memcpy(dst + out, L->data, L->regen);
            out += L->regen;
        }
        *out_io = out;
        return 0;
    }

    /* 位流：从当前位置到 block 末尾，倒序读 */
    abitd_t fr;
    abitd_init(&fr, p, (size_t)(end - p));
    int ll_state = (int)abitd_read_bits(&fr, (unsigned)t_ll.table_log);
    int off_state = (int)abitd_read_bits(&fr, (unsigned)t_off.table_log);
    int ml_state = (int)abitd_read_bits(&fr, (unsigned)t_ml.table_log);

    int rep1 = ctx->rep1, rep2 = ctx->rep2, rep3 = ctx->rep3;
    size_t lit_pos = 0;

    for (size_t i = 0; i < num_seqs; i++) {
        fse_cell_t cll = t_ll.cells[ll_state];
        fse_cell_t coff = t_off.cells[off_state];
        fse_cell_t cml = t_ml.cells[ml_state];
        int ll_code = cll.symbol;
        int off_code = coff.symbol;
        int ml_code = cml.symbol;

        /* 官方顺序：先 OF extra bits，再 ML，再 LL */
        size_t offset;
        size_t ofBase = OF_BASE[off_code];
        unsigned ofBits = OF_EXTRA[off_code];
        unsigned lit_base_zero = (LL_BASE[ll_code] == 0); /* 官方用 llDInfo->baseValue == 0（即 ll_code == 0） */

        /* offset（含 repeat offsets 特殊规则） */
        if (ofBits > 1) {
            offset = ofBase + abitd_read_bits(&fr, ofBits);
            rep3 = rep2; rep2 = rep1; rep1 = (int)offset;
        } else if (ofBits == 0) {
            if (lit_base_zero) {
                offset = (size_t)rep2;
                rep2 = rep1;          /* prevOffset[1] = prevOffset[0] */
                rep1 = (int)offset;   /* prevOffset[0] = offset */
            } else {
                offset = (size_t)rep1;  /* rep 保持不变 */
            }
        } else { /* ofBits == 1 */
            size_t nc = ofBase + lit_base_zero + abitd_read_bits(&fr, 1);
            size_t temp;
            if (nc == 3) temp = (size_t)rep1 - 1;
            else if (nc == 1) temp = (size_t)rep2;
            else temp = (size_t)rep3; /* nc == 2 */
            temp -= !temp;          /* 0 → 强制回绕为 SIZE_MAX（视为越界） */
            if (nc != 1) rep3 = rep2;
            rep2 = rep1;
            rep1 = (int)temp;
            offset = temp;
        }
        if (offset == 0) return -1;
        unsigned match_len = ML_BASE[ml_code] + (unsigned)abitd_read_bits(&fr, (unsigned)ML_EXTRA[ml_code]);
        unsigned lit_len = LL_BASE[ll_code] + (unsigned)abitd_read_bits(&fr, (unsigned)LL_EXTRA[ll_code]);

        /* 复制 literals_length 字节 */
        if (lit_len > 0) {
            if (lit_pos + lit_len > L->regen) return -1;
            if (out + lit_len + match_len > dst_cap) return -2;
            memcpy(dst + out, L->data + lit_pos, lit_len);
            out += lit_len;
            lit_pos += lit_len;
        }
        /* 复制 match（offset 字节回溯） */
        if ((size_t)offset > out) return -1; /* 越过解码窗口 */
        if (out + match_len > dst_cap) return -2;
        {
            size_t o = out;
            for (unsigned m = 0; m < match_len; m++) { dst[o] = dst[o - offset]; o++; }
            out = o;
        }

        /* 更新 FSE 状态（最后一个序列跳过） */
        if (i + 1 != num_seqs) {
            ll_state = (int)(t_ll.cells[ll_state].baseline + abitd_read_bits(&fr, (unsigned)t_ll.cells[ll_state].num_bits));
            ml_state = (int)(t_ml.cells[ml_state].baseline + abitd_read_bits(&fr, (unsigned)t_ml.cells[ml_state].num_bits));
            off_state = (int)(t_off.cells[off_state].baseline + abitd_read_bits(&fr, (unsigned)t_off.cells[off_state].num_bits));
        }
    }

    /* 剩余 literals 追加到输出 */
    if (lit_pos < L->regen) {
        size_t left_lits = L->regen - lit_pos;
        if (out + left_lits > dst_cap) return -2;
        memcpy(dst + out, L->data + lit_pos, left_lits);
        out += left_lits;
    }

    /* 重复偏移回写到帧上下文（供后续块使用） */
    ctx->rep1 = rep1; ctx->rep2 = rep2; ctx->rep3 = rep3;
    *pos_io = (size_t)(end - (const unsigned char *)src); /* block 已全部消费 */
    *out_io = out;
    return 0;
}

/* =============== 帧解码（Stage C） =============== */

static int zstd_decode_frame(const unsigned char *src, size_t src_len, size_t *pos_io,
                             char *dst, size_t dst_cap, size_t *out_io)
{
    size_t pos = *pos_io;
    size_t out = *out_io;

    if (src_len - pos < 1) return -1;
    unsigned fhd = src[pos]; pos++;
    int fcs_flag = (int)((fhd >> 6) & 3);
    int single_segment = (int)((fhd >> 5) & 1);
    int content_checksum = (int)((fhd >> 2) & 1);
    int dict_id_flag = (int)(fhd & 3);
    if (fhd & 0x10) return -1; /* 保留位必须为 0 */

    int fcs_field_size = fcs_flag == 0 ? (single_segment ? 1 : 0)
                       : fcs_flag == 1 ? 2
                       : fcs_flag == 2 ? 4 : 8;
    size_t window_size = 0;
    if (!single_segment) {
        unsigned wd = src[pos++];
        int exponent = (int)((wd >> 3) & 0x1F);
        int mantissa = (int)(wd & 7);
        unsigned long windowBase = 1UL << (10 + exponent);
        window_size = (size_t)(windowBase + (windowBase >> 3) * (unsigned long)mantissa);
        if (window_size > Z_MAX_WINDOW) return -3;
    }
    int dict_id_size = dict_id_flag == 0 ? 0 : (dict_id_flag == 1 ? 1 : (dict_id_flag == 2 ? 2 : 4));
    if (dict_id_size) {
        unsigned long long did = 0;
        for (int i = 0; i < dict_id_size; i++) did |= ((unsigned long long)src[pos + i]) << (8 * i);
        pos += (size_t)dict_id_size;
        if (did != 0) return -3; /* 需要字典：不支持 */
    }
    unsigned long long fcs = 0;
    if (fcs_field_size == 1) { fcs = src[pos]; pos += 1; }
    else if (fcs_field_size == 2) { fcs = read_le16(src + pos) + 256; pos += 2; }
    else if (fcs_field_size == 4) { fcs = read_le32(src + pos); pos += 4; }
    else if (fcs_field_size == 8) { fcs = read_le_u64(src + pos); pos += 8; }
    if (single_segment) window_size = (size_t)fcs;

    /* 零块帧（src_len == 0）：帧头本身即完整帧（合法编码） */
    if (pos == src_len) {
        *pos_io = pos;
        return 0;
    }

    zframe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* 首块重复偏移历史（无字典）：1/4/8 */
    ctx.rep1 = 1; ctx.rep2 = 4; ctx.rep3 = 8;
    zliterals_t L;
    int last_block = 0;

    while (!last_block) {
        if (src_len - pos < 3) return -1;
        unsigned bheader = (unsigned)src[pos] | ((unsigned)src[pos + 1] << 8) | ((unsigned)src[pos + 2] << 16);
        pos += 3;
        int last = (int)(bheader & 1);
        int btype = (int)((bheader >> 1) & 3);
        size_t bsize = (size_t)(bheader >> 3);
        if (btype == 3) return -1;

        if (btype == 0) { /* raw */
            if (pos + bsize > src_len) return -1;
            if (out + bsize > dst_cap) return -2;
            memcpy(dst + out, src + pos, bsize);
            out += bsize;
            pos += bsize;
        } else if (btype == 1) { /* RLE */
            unsigned char b;
            if (src_len - pos < 1) return -1;
            b = src[pos++];
            if (out + bsize > dst_cap) return -2;
            memset(dst + out, b, bsize);
            out += bsize;
        } else { /* compressed */
            if (pos + bsize > src_len) return -1;
            const unsigned char *block = src + pos;
            const unsigned char *bend = block + bsize;
            const unsigned char *cursor = block;
            if (zstd_decode_literals(&L, &cursor, bend, &ctx) != 0) return -1;
            size_t literals_consumed = (size_t)(cursor - block);
            size_t seq_pos = literals_consumed;
            /* 复制 literals 之后的 sequences 部分（zstd_decode_sequences 直接在 dst 上执行并消费 block 剩余） */
            if (zstd_decode_sequences(dst, dst_cap, &out, (const char *)src + pos, bsize, &seq_pos, &L, &ctx) != 0)
                return -1;
            pos += bsize;
        }
        last_block = last;
    }
    if (content_checksum) pos += 4; /* 跳过 checksum（RFC 允许不校验） */
    *pos_io = pos;
    *out_io = out;
    return 0;
}

int aether_zstdcat(const char *src, size_t src_len, char *dst, size_t dst_cap, size_t *out_len)
{
    size_t pos = 0, out = 0;
    for (;;) {
        if (pos >= src_len) break;
        /* 跳过帧间零填充 */
        while (pos < src_len && src[pos] == 0) pos++;
        if (pos >= src_len) break;
        if (src_len - pos < 4) return -1;
        unsigned long magic = read_le32((const unsigned char *)src + pos);
        if (magic == 0xFD2FB528UL) {
            size_t frame_start = pos + 4; /* 跳过 magic */
            size_t frame_pos = 0;
            int rc = zstd_decode_frame((const unsigned char *)src + frame_start, src_len - frame_start, &frame_pos, dst, dst_cap, &out);
            if (rc != 0) return rc;
            pos = frame_start + frame_pos;
        } else if (magic >= ZSKIP_MIN && magic <= ZSKIP_MAX) {
            if (src_len - pos < 8) return -1;
            unsigned long fsize = read_le32((const unsigned char *)src + pos + 4);
            pos += 8 + (size_t)fsize;
            if (pos > src_len) return -1;
        } else {
            return -1;
        }
    }
    if (out_len) *out_len = out;
    return 0;
}

/* =============== zstd 压缩（raw / RLE 块，无校验和，省略 XXH64） ===============
 * 产出 RFC 8878 兼容帧（已验证：官方 zstd -d 与本文件 zstdcat 均可解码）。
 * 字面量压缩（Huffman）建议后续阶段补；本实现优先保证：
 *   - 能被官方 zstd/libzstd 解码
 *   - 能被 zstd（同一实现）往返解压
 *   - 避免 write+debug 式字节级匹配（encoder 与 decoder 同时存在时收敛代价高） */

#define Z_FRAME_MAGIC 0xFD2FB528UL
#define Z_BLOCK_MAX (128u * 1024)

static void zstd_write_u32_le(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void zstd_write_u16_le(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

int aether_zstd(const char *src, size_t src_len, char *dst, size_t dst_cap, size_t *out_len)
{
    const unsigned char *in = (const unsigned char *)src;
    unsigned char *o = (unsigned char *)dst;
    size_t cap = dst_cap;
    if (cap < 16) return -1;
    if (src_len > 0x7FFFFFFFUL) return -3;

    /* 至少 128KB 块 × 1 + 帧头 + 尾字节 */
    size_t max_blocks = (src_len + Z_BLOCK_MAX - 1) / Z_BLOCK_MAX;
    size_t frame_hdr = 0, wd_byte = 0;
    size_t fcs_size = 0, total_max;

    if (src_len <= 255) {
        fcs_size = 1;          /* fhd fcs_flag=0, single_segment=1 */
        frame_hdr = 4 + 1;     /* magic + fhd + fcs(1) */
    } else if (src_len <= 65791) {
        fcs_size = 2;          /* fhd fcs_flag=1, single_segment=1 */
        frame_hdr = 4 + 1 + 2;
    } else if (src_len <= 0xFFFFFFFFUL) {
        fcs_size = 4;          /* fhd fcs_flag=2, 非单段，显式 window 字节（18 → 256KB） */
        frame_hdr = 4 + 1 + 1 + 4; /* magic + fhd + wd + fcs(4) */
        wd_byte = 1;
    } else {
        fcs_size = 8;
        frame_hdr = 4 + 1 + 1 + 8;
        wd_byte = 1;
    }
    size_t block_hdrs = (max_blocks ? max_blocks : 1) * 3;
    total_max = frame_hdr + block_hdrs + src_len + max_blocks; /* RLE 每块 1 字节替代重复字节流时更优，这里留够 raw */
    if (total_max > cap) return -2;

    /* 写帧头 */
    unsigned fhd;
    if (fcs_size == 1)       fhd = 0x20;  /* fcs_flag=0, single_segment=1, chk=0, dict=0 */
    else if (fcs_size == 2)  fhd = 0x60;  /* fcs_flag=1, single_segment=1 */
    else if (fcs_size == 4)  fhd = 0x80;  /* fcs_flag=2, single_segment=0 */
    else                     fhd = 0xC0;  /* fcs_flag=3, single_segment=0 */
    zstd_write_u32_le(o, Z_FRAME_MAGIC); o += 4;
    *o++ = (unsigned char)fhd;
    if (wd_byte) *o++ = 0x40;             /* exponent=8 (>>10 → 256KB), mantissa=0 */
    if (fcs_size == 1) {
        *o++ = (unsigned char)src_len;
    } else if (fcs_size == 2) {
        unsigned int v = (unsigned int)src_len - 256;
        zstd_write_u16_le(o, v); o += 2;
    } else if (fcs_size == 4) {
        zstd_write_u32_le(o, (unsigned int)src_len); o += 4;
    } else {
        unsigned long long sv = (unsigned long long)src_len;
        zstd_write_u32_le(o, (unsigned int)(sv & 0xFFFFFFFFull)); o += 4;
        zstd_write_u32_le(o, (unsigned int)(sv >> 32)); o += 4;
    }

    /* 逐块写：取 lenmin(128KB, 剩余) — 零长度源文件也必须有一个 0 字节 raw 块 */
    size_t pos = 0;
    int emitted_any = 0;
    for (;;) {
        size_t remaining = src_len - pos;
        if (remaining == 0) break;
        size_t blk = remaining > Z_BLOCK_MAX ? Z_BLOCK_MAX : remaining;
        int last = (blk == remaining);
        unsigned char b = in[pos];
        int is_rle = 1;
        for (size_t i = 1; i < blk; i++) {
            if (in[pos + i] != b) { is_rle = 0; break; }
        }
        unsigned bh;
        if (is_rle && blk > 1) {
            bh = ((unsigned)blk << 3) | (1u << 1) | (unsigned)last;
            o[0] = (unsigned char)bh; o[1] = (unsigned char)(bh >> 8); o[2] = (unsigned char)(bh >> 16); o += 3;
            *o++ = b;
        } else {
            bh = ((unsigned)blk << 3) | (0u << 1) | (unsigned)last;
            o[0] = (unsigned char)bh; o[1] = (unsigned char)(bh >> 8); o[2] = (unsigned char)(bh >> 16); o += 3;
            if (blk > cap - (size_t)(o - (unsigned char *)dst)) return -2;
            memcpy(o, in + pos, blk);
            o += blk;
        }
        pos += blk;
        emitted_any = 1;
    }
    if (!emitted_any) {
        /* 0 字节 raw 块，last=1 */
        o[0] = 1; o[1] = 0; o[2] = 0; o += 3;
    }
    if (out_len) *out_len = (size_t)(o - (unsigned char *)dst);
    return 0;
}
