/*
 * pda.c — 单下推自动机：一遍扫描一行，边分词边判语法（§7）
 * v1 语法子集（§7.2）：简单命令 + 参数、|、;、&、< > >> 2> 2>&1、引号、#注释、反斜杠转义。
 * 明确不做：&& ||、流程控制、函数、glob、命令替换、变量展开（见 §7.3）。
 *
 * 错误处理：失败返回 -1 并打印原因（行内位置不进 v1，§7.1 已接受报错粗定位）。
 */
#include "shell_internal.h"
#include "elog.h"
#include "mem.h"
#include "util.h"

#include <stddef.h>

typedef struct {
    const char *line;
    size_t      i;      /* 当前位置 */
    mem_arena_t *arena;
} cursor_t;

static char peek(cursor_t *c) { return c->line[c->i]; }
static char get(cursor_t *c)  { return c->line[c->i] ? c->line[c->i++] : '\0'; }

static void skip_ws_and_comments(cursor_t *c)
{
    for (;;) {
        char ch = peek(c);
        if (ch == ' ' || ch == '\t') { c->i++; continue; }
        if (ch == '#') { /* 注释到行尾 */
            while (peek(c) && peek(c) != '\n') c->i++;
            return;
        }
        return;
    }
}

static int is_opchar(char ch)
{
    return ch == '|' || ch == ';' || ch == '&' || ch == '<' || ch == '>';
}

/* 解析一个 token：arena 分配的字符串。失败（未闭合引号/悬空反斜杠/裸操作符）返回 0。
 * 空串 token 仅在引号显式给出时合法（如 ''）；裸遇操作符即返回 0 报语法错误。 */
static char *parse_token(cursor_t *c)
{
    size_t cap = 32, n = 0;
    int has_content = 0;
    char *buf = mem_arena_alloc(c->arena, cap);
    if (!buf) return 0;

    for (;;) {
        char ch = peek(c);
        if (ch == '\0' || ch == '\n' || ch == ' ' || ch == '\t' || is_opchar(ch))
            break;
        if (ch == '\'' || ch == '"') {
            char quote = ch;
            has_content = 1; /* 显式引号：空串亦为合法 token（如 echo ''） */
            get(c);
            for (;;) {
                ch = get(c);
                if (ch == '\0') { /* 引号未闭合 */
                    return 0;
                }
                if (ch == quote) break;
                if (quote == '"' && ch == '\\') {
                    char esc = get(c);
                    if (esc == '\0') return 0;
                    /* 双引号内仅少量转义：\\ \" \$ \n \t */
                    switch (esc) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    default:  ch = esc;  break;
                    }
                }
                if (n + 2 >= cap) {
                    cap *= 2;
                    char *nb = mem_arena_alloc(c->arena, cap);
                    if (!nb) return 0;
                    for (size_t k = 0; k < n; k++) nb[k] = buf[k];
                    buf = nb; /* 旧 buf 留给 arena，无需释放 */
                }
                buf[n++] = ch;
                has_content = 1;
            }
            continue;
        }
        if (ch == '\\') {
            get(c);
            char esc = get(c);
            if (esc == '\0') return 0; /* 行尾悬空反斜杠 */
            buf[n++] = esc;
            has_content = 1;
            continue;
        }
        get(c);
        if (n + 2 >= cap) {
            cap *= 2;
            char *nb = mem_arena_alloc(c->arena, cap);
            if (!nb) return 0;
            for (size_t k = 0; k < n; k++) nb[k] = buf[k];
            buf = nb;
        }
        buf[n++] = ch;
        has_content = 1;
    }
    buf[n] = '\0';
    if (!has_content) return 0; /* 裸遇操作符/空白起始：语法错误 */
    return buf;
}

static shell_cmd_t *cur_cmd(shell_script_t *s)
{
    shell_pipeline_t *p = &s->units[s->n_units - 1].p;
    return &p->cmds[p->n_cmds - 1];
}

static int cmd_is_empty(const shell_cmd_t *c)
{
    return c->argc == 0 && !c->redir_in && !c->redir_out && !c->redir_append &&
           !c->redir_err && !c->redir_err_append && !c->err_to_out;
}

static void new_cmd(shell_script_t *s)
{
    shell_pipeline_t *p = &s->units[s->n_units - 1].p;
    if (p->n_cmds >= SHELL_MAX_CMDS) return;
    p->cmds[p->n_cmds].argc = 0;
    p->cmds[p->n_cmds].redir_in = p->cmds[p->n_cmds].redir_out = p->cmds[p->n_cmds].redir_append = 0;
    p->cmds[p->n_cmds].redir_err = p->cmds[p->n_cmds].redir_err_append = 0;
    p->cmds[p->n_cmds].err_to_out = 0;
    p->n_cmds++;
}

static int push_unit_background(shell_script_t *s, int background)
{
    shell_pipeline_t *p = &s->units[s->n_units - 1].p;
    int last = p->n_cmds - 1;
    if (p->n_cmds == 0) return 0; /* 空段：丢弃，不新开单元 */
    if (cmd_is_empty(&p->cmds[last]) && p->n_cmds > 1) return -1; /* 空命令段（如 "a | ; ..."）：语法错误 */
    /* 丢弃末尾空命令 */
    if (cmd_is_empty(&p->cmds[last])) p->n_cmds--;
    s->units[s->n_units - 1].background = background;
    if (s->n_units >= SHELL_MAX_UNITS) return -1;
    s->n_units++;
    s->units[s->n_units - 1].p.n_cmds = 0;
    s->units[s->n_units - 1].background = 0;
    return 0;
}

/* 解析入口 */
int shell_parse_line(mem_arena_t *arena, const char *line, shell_script_t *out)
{
    cursor_t c = { line, 0, arena };
    int expect = 0; /* 0=无，1=<，2=>，3=>>，4=2>，5=2>> */

    out->n_units = 1;
    out->units[0].p.n_cmds = 0;
    out->units[0].background = 0;
    new_cmd(out);

    for (;;) {
        skip_ws_and_comments(&c);
        char ch = peek(&c);
        if (ch == '\0' || ch == '\n') break;

        if (ch == '2') {
            /* 2>  2>>  2>&1 */
            if (c.line[c.i + 1] == '>') {
                c.i += 2;
                if (peek(&c) == '&') { /* 2>&1 */
                    c.i++;
                    if (peek(&c) == '1') c.i++;
                    else return -1; /* 目前仅支持 2>&1 */
                    cur_cmd(out)->err_to_out = 1;
                    continue;
                }
                if (peek(&c) == '>') { c.i++; expect = 5; }
                else                 {           expect = 4; }
                continue;
            }
        }

        switch (ch) {
        case '|':
            if (expect) return -1;
            if (cmd_is_empty(cur_cmd(out))) return -1; /* 空命令后接 |（如 "| a" / "a | | b" 的中间段） */
            get(&c);
            new_cmd(out);
            continue;
        case ';':
            if (expect) return -1;
            get(&c);
            if (push_unit_background(out, 0) != 0) return -1;
            continue;
        case '&':
            if (expect) return -1;
            get(&c);
            if (push_unit_background(out, 1) != 0) return -1;
            continue;
        case '<':
            if (expect) return -1;
            expect = 1;
            get(&c);
            continue;
        case '>':
            if (expect) return -1;
            get(&c);
            if (peek(&c) == '>') { get(&c); expect = 3; }
            else                 {          expect = 2; }
            continue;
        default:
            break;
        }

        /* token */
        char *tok = parse_token(&c);
        if (!tok) return -1;
        if (expect) {
            shell_cmd_t *cmd = cur_cmd(out);
            switch (expect) {
            case 1: cmd->redir_in = tok; break;
            case 2: cmd->redir_out = tok; break;
            case 3: cmd->redir_append = tok; break;
            case 4: cmd->redir_err = tok; break;
            case 5: cmd->redir_err_append = tok; break;
            }
            expect = 0;
            continue;
        }
        {
            shell_cmd_t *cmd = cur_cmd(out);
            if (cmd->argc >= SHELL_MAX_ARGS) return -1;
            cmd->argv[cmd->argc++] = tok;
            cmd->argv[cmd->argc] = 0;
        }
    }

    if (expect) return -1; /* 结尾悬空重定向 */
    {
        shell_pipeline_t *p = &out->units[out->n_units - 1].p;
        shell_cmd_t *last = &p->cmds[p->n_cmds - 1];
        if (cmd_is_empty(last)) {
            if (p->n_cmds > 1) return -1; /* 尾随 |，如 "echo a |" */
            p->n_cmds = 0;                 /* 整行为空白/注释 */
        }
        if (p->n_cmds == 0) out->n_units--;
    }
    return 0;
}
