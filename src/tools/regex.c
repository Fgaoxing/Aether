/*
 * regex.c — 精简扩展正则引擎（供 grep 用）
 * 支持：字面量、'.'、'*'、'+'、'?'、'{n}' '{n,}' '{n,m}'、
 *       '^' '$'、'[...]' '[^...]'（含 a-z 范围、\d \s \w）、
 *       '(...)' 分组、'|' 交替、'\' 转义（\n \t \r 及特殊字符自身）。
 * 不支持：捕获/反向引用、环视、[:alpha:]。
 * 实现：编译为节点树，回溯枚举匹配终点（对 rescue 场景的行足够）。
 * 无动态内存：静态节点池，每次编译重置。
 */
#include <stddef.h>

#include "util.h"

#define RE_MAX_NODES 512
#define RE_MAX_ENDS  64

typedef enum {
    N_LIT = 1,   /* c：单字符（re_ci 时已归一化小写） */
    N_DOT,       /* . ：除 \n 外任意字符 */
    N_CLASS,     /* cls[]：字符位图，反向类已取反 */
    N_BOL,       /* ^ ：行首 */
    N_EOL,       /* $ ：行尾 */
    N_SEQ,       /* a 然后 b */
    N_ALT,       /* a | b */
    N_STAR,      /* a* */
    N_QUEST,     /* a? */
    N_EMPTY,     /* 空序列（如 "a|" 的空分支）：恒匹配零宽 */
} re_kind;

typedef struct {
    unsigned char kind;
    unsigned char c;
    unsigned long long cls[4];  /* 256 位 */
    int a, b;                   /* 子节点索引，-1 = 无 */
} re_node_t;

static re_node_t re_nodes[RE_MAX_NODES];
static int re_nnodes;
static const char *re_pat;
static int re_len;
static int re_pos;
static int re_ci;
static int re_root;

static int re_peek(void) { return re_pos < re_len ? (unsigned char)re_pat[re_pos] : -1; }
static int re_getc(void) { return re_pos < re_len ? (unsigned char)re_pat[re_pos++] : -1; }

static int re_new(int kind)
{
    if (re_nnodes >= RE_MAX_NODES) return -1;
    int id = re_nnodes++;
    re_node_t *n = &re_nodes[id];
    n->kind = (unsigned char)kind;
    n->c = 0;
    n->cls[0] = n->cls[1] = n->cls[2] = n->cls[3] = 0;
    n->a = n->b = -1;
    return id;
}

static void re_class_add(re_node_t *n, unsigned char ch)
{
    int idx = (int)(ch >> 6);
    int bit = (int)(ch & 63);
    n->cls[idx] |= (1ULL << bit);
}

static void re_class_add_ci(re_node_t *n, unsigned char ch)
{
    re_class_add(n, ch);
    if (re_ci) {
        if (ch >= 'a' && ch <= 'z') re_class_add(n, (unsigned char)(ch - 32));
        else if (ch >= 'A' && ch <= 'Z') re_class_add(n, (unsigned char)(ch + 32));
    }
}

static int re_parse_alt(void);
static int re_parse_seq(void);
static int re_parse_atom(void);
static int re_parse_class(void);
static void re_reset_state(const char *pattern, int ci);
static int re_parse_class_escape_d(void);

static int re_new_unary(int kind, int child)
{
    int id = re_new(kind);
    if (id < 0 || child < 0) return -1;
    re_nodes[id].a = child;
    return id;
}

static int re_new_plus(int child)
{
    int star = re_new_unary(N_STAR, child);
    if (star < 0) return -1;
    int id = re_new(N_SEQ);
    if (id < 0) return -1;
    re_nodes[id].a = child;
    re_nodes[id].b = star;
    return id;
}

/* 区间 a-b（含端点），并按 re_ci 补齐大小写 */
static void re_class_add_range(re_node_t *n, int lo, int hi)
{
    for (int k = lo; k <= hi; k++) {
        re_class_add(n, (unsigned char)k);
        if (re_ci) { /* 大小写不敏感时补齐字母对 */
            if (k >= 'a' && k <= 'z') re_class_add(n, (unsigned char)(k - 32));
            else if (k >= 'A' && k <= 'Z') re_class_add(n, (unsigned char)(k + 32));
        }
    }
}



static int re_parse_class(void)
{
    int id = re_new(N_CLASS);
    if (id < 0) return -1;
    re_node_t *n = &re_nodes[id];
    int negate = 0;
    if (re_peek() == '^') { negate = 1; re_getc(); }
    int first = 1;
    int closed = 0;
    while (re_pos < re_len) {
        int c = re_getc();
        if (c == ']' && !first) { closed = 1; break; }
        first = 0;

        /* 预定义类 \d \s \w */
        if (c == '\\') {
            int e = re_getc();
            if (e < 0) return -1;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 'r') c = '\r';
            else if (e == 'd') { re_class_add_range(n, '0', '9'); continue; }
            else if (e == 's') { re_class_add(n, ' '); re_class_add(n, '\t'); re_class_add(n, '\n'); re_class_add(n, '\r'); continue; }
            else if (e == 'w') { re_class_add_range(n, 'a', 'z'); re_class_add_range(n, 'A', 'Z'); re_class_add_range(n, '0', '9'); re_class_add(n, '_'); continue; }
            else c = e; /* 其他转义：字面字符 */
        }

        /* 范围 lo-hi */
        if (re_peek() == '-') {
            re_getc(); /* '-' */
            int hi = re_getc();
            if (hi == ']') { re_class_add(n, '-'); re_class_add_ci(n, (unsigned char)c); continue; }
            if (hi == '\\') {
                int e = re_getc();
                if (e < 0) return -1;
                if (e == 'n') hi = '\n';
                else if (e == 't') hi = '\t';
                else if (e == 'r') hi = '\r';
                else hi = e;
            }
            if (hi < c) { int t = c; c = hi; hi = t; }
            re_class_add_range(n, c, hi);
            continue;
        }
        re_class_add_ci(n, (unsigned char)c);
    }
    if (!closed) return -1; /* 未闭合的 '[' → 语法错误 */
    if (negate) for (int i = 0; i < 4; i++) n->cls[i] = ~n->cls[i];
    return id;
}

static int re_parse_atom(void)
{
    int c = re_getc();
    if (c < 0) return -1;
    int atom = -1;

    if (c == '(') {
        atom = re_parse_alt();
        if (re_getc() != ')') return -1;
    } else if (c == '[') {
        atom = re_parse_class();
    } else if (c == '.') {
        atom = re_new(N_DOT);
    } else if (c == '^') {
        atom = re_new(N_BOL);
    } else if (c == '$') {
        atom = re_new(N_EOL);
    } else if (c == '\\') {
        int e = re_getc();
        if (e < 0) return -1;
        if (e == 'n') e = '\n';
        else if (e == 't') e = '\t';
        else if (e == 'r') e = '\r';
        else if (e == 'd') {
            int cls = re_parse_class_escape_d();
            if (cls < 0) return -1;
            atom = cls;
            goto quantifier;
        } else if (e == 's') {
            atom = re_new(N_CLASS);
            if (atom < 0) return -1;
            re_class_add(&re_nodes[atom], ' ');
            re_class_add(&re_nodes[atom], '\t');
            re_class_add(&re_nodes[atom], '\n');
            re_class_add(&re_nodes[atom], '\r');
            goto quantifier;
        } else if (e == 'w') {
            atom = re_new(N_CLASS);
            if (atom < 0) return -1;
            re_class_add_range(&re_nodes[atom], 'a', 'z');
            re_class_add_range(&re_nodes[atom], 'A', 'Z');
            re_class_add_range(&re_nodes[atom], '0', '9');
            re_class_add(&re_nodes[atom], '_');
            goto quantifier;
        }
        atom = re_new(N_LIT);
        if (atom < 0) return -1;
        if (re_ci && e >= 'A' && e <= 'Z') e += 32;
        re_nodes[atom].c = (unsigned char)e;
    } else if (c == '*' || c == '+' || c == '?') {
        return -1; /* 量词不能出现在原子位置 */
    } else {
        atom = re_new(N_LIT);
        if (atom < 0) return -1;
        if (re_ci && c >= 'A' && c <= 'Z') c += 32;
        re_nodes[atom].c = (unsigned char)c;
    }
    if (atom < 0) return -1;

quantifier:
    for (;;) {
        int q = re_peek();
        if (q == '*') { re_getc(); atom = re_new_unary(N_STAR, atom); }
        else if (q == '+') { re_getc(); atom = re_new_plus(atom); }
        else if (q == '?') { re_getc(); atom = re_new_unary(N_QUEST, atom); }
        else if (q == '{') {
            /* {n} / {n,} / {n,m}；异常则当字面量 '{' */
            int save = re_pos;
            re_getc(); /* '{' */
            int n1 = 0, nd = 0;
            while (re_peek() >= '0' && re_peek() <= '9') { n1 = n1 * 10 + (re_getc() - '0'); nd++; }
            if (nd == 0) { re_pos = save; break; }
            int n2 = -1;
            if (re_peek() == ',') {
                re_getc();
                int m = 0, md = 0;
                while (re_peek() >= '0' && re_peek() <= '9') { m = m * 10 + (re_getc() - '0'); md++; }
                if (md) n2 = m;
            } else n2 = n1;
            if (re_getc() != '}') { re_pos = save; break; }
            /* 展开：n1 个必选拷贝 +（n2==-1 ? STAR(orig) : (n2-n1) 个可选拷贝） */
            int orig = atom;
            int result = -1;
            for (int k = 0; k < n1; k++) {
                int cp = re_new(re_nodes[orig].kind);
                if (cp < 0) return -1;
                re_nodes[cp] = re_nodes[orig];
                if (result >= 0) {
                    int id = re_new(N_SEQ);
                    if (id < 0) return -1;
                    re_nodes[id].a = result;
                    re_nodes[id].b = cp;
                    result = id;
                } else result = cp;
            }
            if (n2 == -1) {
                int star = re_new(N_STAR);
                if (star < 0) return -1;
                re_nodes[star].a = orig;
                if (result >= 0) {
                    int id = re_new(N_SEQ);
                    if (id < 0) return -1;
                    re_nodes[id].a = result;
                    re_nodes[id].b = star;
                    result = id;
                } else result = star;
            } else {
                for (int k = n1; k < n2; k++) {
                    int cp = re_new(re_nodes[orig].kind);
                    if (cp < 0) return -1;
                    re_nodes[cp] = re_nodes[orig];
                    int q = re_new_unary(N_QUEST, cp);
                    if (q < 0) return -1;
                    if (result >= 0) {
                        int id = re_new(N_SEQ);
                        if (id < 0) return -1;
                        re_nodes[id].a = result;
                        re_nodes[id].b = q;
                        result = id;
                    } else result = q;
                }
            }
            atom = result;
        }
        else break;
        if (atom < 0) return -1;
    }
    return atom;
}

static int re_parse_class_escape_d(void)
{
    int cls = re_new(N_CLASS);
    if (cls < 0) return -1;
    re_class_add_range(&re_nodes[cls], '0', '9');
    return cls;
}

static int re_parse_seq(void)
{
    int head = -1;
    while (re_pos < re_len) {
        int p = re_peek();
        if (p == '|' || p == ')') break;
        int atom = re_parse_atom();
        if (atom < 0) return -1;
        if (head < 0) head = atom;
        else {
            int id = re_new(N_SEQ);
            if (id < 0) return -1;
            re_nodes[id].a = head;
            re_nodes[id].b = atom;
            head = id;
        }
    }
    if (head < 0) head = re_new(N_EMPTY); /* 空序列（如 "a|" 的空分支）：恒零宽匹配 */
    return head;
}

static int re_parse_alt(void)
{
    int left = re_parse_seq();
    if (left < 0) return -1;
    while (re_peek() == '|') {
        re_getc();
        int right = re_parse_seq();
        if (right < 0) return -1;
        int id = re_new(N_ALT);
        if (id < 0) return -1;
        re_nodes[id].a = left;
        re_nodes[id].b = right;
        left = id;
    }
    return left;
}

/* ---------- 编译 / 匹配 ---------- */

int re_compile(const char *pattern, int ci)
{
    re_reset_state(pattern, ci);
    int root = re_parse_alt();
    if (root < 0) return -1;
    if (re_pos != re_len) return -1; /* 有未消费的输入 */
    re_root = root;
    return 0;
}

static void re_reset_state(const char *pattern, int ci)
{
    re_pat = pattern;
    re_len = 0;
    while (pattern[re_len]) re_len++;
    re_pos = 0;
    re_nnodes = 0;
    re_root = -1;
    re_ci = ci;
}

typedef struct { int pos[RE_MAX_ENDS]; int n; } re_ends_t;

static void re_ends_add(re_ends_t *l, int p)
{
    if (l->n >= RE_MAX_ENDS) return;
    for (int i = 0; i < l->n; i++) if (l->pos[i] == p) return;
    l->pos[l->n++] = p;
}

static int re_class_test(const re_node_t *n, unsigned char ch)
{
    int idx = (int)(ch >> 6);
    int bit = (int)(ch & 63);
    if (re_ci) {
        /* 大小写都试：先按原字符试，不行再按互转试 */
        if (n->cls[idx] & (1ULL << bit)) return 1;
        if (ch >= 'A' && ch <= 'Z') { ch = (unsigned char)(ch + 32); }
        else if (ch >= 'a' && ch <= 'z') { ch = (unsigned char)(ch - 32); }
        idx = (int)(ch >> 6);
        bit = (int)(ch & 63);
    }
    return (int)((n->cls[idx] >> bit) & 1ULL);
}

static int re_norm_ci(int c)
{
    if (re_ci && c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static void re_mre(int id, const char *s, int slen, int i, re_ends_t *out)
{
    if (id < 0 || out->n >= RE_MAX_ENDS) return;
    const re_node_t *n = &re_nodes[id];
    switch (n->kind) {
    case N_LIT:
        if (i < slen) {
            int sc = re_norm_ci((unsigned char)s[i]);
            if (sc == n->c) re_ends_add(out, i + 1);
        }
        return;
    case N_DOT:
        if (i < slen && s[i] != '\n') re_ends_add(out, i + 1);
        return;
    case N_CLASS:
        if (i < slen && re_class_test(n, (unsigned char)s[i])) re_ends_add(out, i + 1);
        return;
    case N_BOL:
        if (i == 0) re_ends_add(out, i);
        return;
    case N_EOL:
        if (i == slen) re_ends_add(out, i);
        return;
    case N_EMPTY:
        re_ends_add(out, i);
        return;
    case N_SEQ: {
        re_ends_t mid;
        mid.n = 0;
        re_mre(n->a, s, slen, i, &mid);
        for (int k = 0; k < mid.n && out->n < RE_MAX_ENDS; k++)
            re_mre(n->b, s, slen, mid.pos[k], out);
        return;
    }
    case N_ALT:
        re_mre(n->a, s, slen, i, out);
        re_mre(n->b, s, slen, i, out);
        return;
    case N_STAR: {
        re_ends_t acc;
        acc.n = 0;
        re_ends_add(&acc, i);
        int fi = 0;
        while (fi < acc.n && acc.n < RE_MAX_ENDS) {
            int cur = acc.pos[fi++];
            re_ends_t step;
            step.n = 0;
            re_mre(n->a, s, slen, cur, &step);
            for (int k = 0; k < step.n; k++) {
                if (step.pos[k] == cur) continue; /* 空匹配防自旋 */
                re_ends_add(&acc, step.pos[k]);
            }
        }
        for (int k = 0; k < acc.n; k++) re_ends_add(out, acc.pos[k]);
        return;
    }
    case N_QUEST:
        re_ends_add(out, i);
        re_mre(n->a, s, slen, i, out);
        return;
    default:
        return;
    }
}

int re_search(const char *s, int slen)
{
    if (re_root < 0) return 0;
    for (int i = 0; i <= slen; i++) {
        re_ends_t out;
        out.n = 0;
        re_mre(re_root, s, slen, i, &out);
        if (out.n > 0) return 1;
    }
    return 0;
}
