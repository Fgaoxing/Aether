/*
 * shell_internal.h — shell 模块内部声明（§7 / §9）
 */
#ifndef AETHER_SHELL_INTERNAL_H
#define AETHER_SHELL_INTERNAL_H

#include <stddef.h>
#include <stdarg.h>

#include "mem.h"

#define SHELL_MAX_ARGS    128
#define SHELL_MAX_CMDS    16    /* 一个 pipeline 最多命令数 */
#define SHELL_MAX_UNITS   16    /* 一行里 ;/& 分成的 pipeline 段数 */
#define SHELL_MAX_LINE    4096
#define SHELL_REDIR_MAX   8

typedef struct {
    char *argv[SHELL_MAX_ARGS + 1];
    int   argc;

    /* 重定向（未设置则为 0） */
    const char *redir_in;          /* < */
    const char *redir_out;         /* > */
    const char *redir_append;      /* >> */
    const char *redir_err;         /* 2>  */
    const char *redir_err_append;  /* 2>> */
    int         err_to_out;        /* 2>&1 */
} shell_cmd_t;

typedef struct {
    shell_cmd_t cmds[SHELL_MAX_CMDS];
    int         n_cmds;
} shell_pipeline_t;

typedef struct {
    shell_pipeline_t p;
    int              background;   /* 段尾以 & 结束 */
} shell_unit_t;

typedef struct {
    shell_unit_t units[SHELL_MAX_UNITS];
    int          n_units;
} shell_script_t;

/* pda.c：解析一行（不含 \n）；成功返回 0；失败打印错误并返回 -1 */
int  shell_parse_line(mem_arena_t *arena, const char *line, shell_script_t *out);

/* lineedit.c：交互式读一行；EOF 返回 -1；Ctrl-C 返回 -2（用户放弃本行） */
#define LINEEDIT_CTRL_C (-2)
int  lineedit_read(const char *prompt, char *buf, size_t cap);
/* 直接从 fd 0 读一行（非交互/脚本模式）；EOF 返回 -1 */
int  shell_read_fd0_line(char *buf, size_t cap);

/* builtin.c */
int  shell_run_builtin_child(int argc, char **argv, char **envp);   /* 在 fork 出的子进程里执行 */
int  shell_is_builtin(const char *name);                            /* 名字是否注册为内置 */
const char *shell_builtin_name(size_t i);                           /* 枚举用 */

/* telnet 模式：shell 输入流按行处理并剥离 IAC（telnetd 用） */
extern int aether_telnet_mode;

/* 内置表（shell_builtin_t 供 shell.c 调度：name/fn/in_process/doc） */
typedef struct {
    const char *name;
    int (*fn)(int argc, char **argv, char **envp);
    int in_process; /* 1 = shell 进程内执行 */
    const char *doc;
} shell_builtin_t;

const shell_builtin_t *shell_builtin_find(const char *name);   /* 未找到返回 0 */
size_t shell_builtin_count(void);
const shell_builtin_t *shell_builtin_at(size_t i);

/* PATH 搜索 + execve（成功不返回；失败 -1） */
int shell_exec_search(int argc, char **argv, char **envp);

/* jobs.c — 作业控制（§7.7）：进程组 + 终端归属 + jobs/fg/bg */
#define SHELL_JOB_MAX 16
#define JOB_RUN 1
#define JOB_STOP 2
typedef struct {
    int id, pgid, n_cmds, n_left, state;
    int pids[SHELL_MAX_CMDS];
    char cmdline[600];
} shell_job_t;

void shell_jobs_init(int interactive);
int  shell_jobs_interactive(void);
int  shell_jobs_self_pgid(void);
void shell_child_reset_signals(void);         /* 子进程：SIGINT/SIGQUIT/SIGTSTP/SIGCHLD → DFL */
void shell_job_child_enter(int pgid);         /* 子进程加入作业组（pgid<0=自己成组） */
int  shell_job_wait_foreground(int pgid, const int *pids, int n); /* 前台等待（外区段：调用方归还终端）；返回 bit8=有停止，低位=最后状态 */
int  shell_job_add(int pgid, const int *pids, int n, const char *cmdline, int state);
shell_job_t *shell_job_find(int id);
shell_job_t *shell_job_latest(void);
void shell_job_list_print(void);
void shell_job_reap(void);                    /* WNOHANG 收割；打印 [id] Done */
int  shell_job_continue_fg(shell_job_t *job); /* fg：终端交给作业 + SIGCONT + 等待 */
int  shell_job_continue_bg(shell_job_t *job); /* bg：SIGCONT（终端留 shell） */

/* 内置表（exec 分支要用） */

#endif /* AETHER_SHELL_INTERNAL_H */
