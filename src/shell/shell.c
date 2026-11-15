/*
 * shell.c — 救援 shell 主循环（§2 / §7 / §9）
 * 交互：lineedit_read（cbreak per line，children 恢复 normal termios）
 * 脚本：逐行从 fd 0 读。
 * 一行 → 多个 unit（; / & 分隔），每 unit 一个 pipeline：
 *   - 单个命令且为 in_process 内置（cd/exit/exec）→ 进程内执行
 *   - 其余 → fork pipeline：内置/applet 子进程内执行，或 PATH 搜索 execve
 * SIGINT：shell 装 handler（lineedit 借 EINTR 打断），子进程 DFL —— Ctrl-C 杀子进程不杀 shell。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "shell_internal.h"
#include "syscall_shim.h"
#include "ttyutil.h"
#include "util.h"

static char *child_env[72];

static volatile int aether_shell_sigint;

static void shell_sigint_handler(int sig)
{
    (void)sig;
    aether_shell_sigint = 1;
}

static size_t build_child_env(char **envp)
{
    int has_path = 0, n = 0;
    if (envp) {
        for (int i = 0; envp[i] && n < 71; i++) {
            const char *e = envp[i];
            if (e[0] == 'P' && e[1] == 'A' && e[2] == 'T' && e[3] == 'H' && e[4] == '=')
                has_path = 1;
            child_env[n++] = (char *)e;
        }
    }
    if (!has_path && n < 71)
        child_env[n++] = (char *)"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    child_env[n] = 0;
    return (size_t)n;
}

static int shell_tty_is(int fd)
{
    aether_termios_t t;
    return sys_ioctl(fd, TTY_IOC_TCGETS, (unsigned long)&t) == 0;
}

/* —— 重定向 —— */

static int setup_redirects(const shell_cmd_t *cmd)
{
    if (cmd->redir_in) {
        long fd = sys_open(cmd->redir_in, O_RDONLY, 0);
        if (AETHER_IS_ERR(fd)) { elog_err("aether: %s: %d", cmd->redir_in, AETHER_ERR_CODE(fd)); return -1; }
        sys_dup2((int)fd, 0);
        sys_close((int)fd);
    }
    if (cmd->redir_out) {
        long fd = sys_open(cmd->redir_out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (AETHER_IS_ERR(fd)) { elog_err("aether: %s: %d", cmd->redir_out, AETHER_ERR_CODE(fd)); return -1; }
        sys_dup2((int)fd, 1);
        sys_close((int)fd);
    }
    if (cmd->redir_append) {
        long fd = sys_open(cmd->redir_append, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (AETHER_IS_ERR(fd)) { elog_err("aether: %s: %d", cmd->redir_append, AETHER_ERR_CODE(fd)); return -1; }
        sys_dup2((int)fd, 1);
        sys_close((int)fd);
    }
    if (cmd->redir_err) {
        long fd = sys_open(cmd->redir_err, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (AETHER_IS_ERR(fd)) { elog_err("aether: %s: %d", cmd->redir_err, AETHER_ERR_CODE(fd)); return -1; }
        sys_dup2((int)fd, 2);
        sys_close((int)fd);
    }
    if (cmd->redir_err_append) {
        long fd = sys_open(cmd->redir_err_append, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (AETHER_IS_ERR(fd)) { elog_err("aether: %s: %d", cmd->redir_err_append, AETHER_ERR_CODE(fd)); return -1; }
        sys_dup2((int)fd, 2);
        sys_close((int)fd);
    }
    if (cmd->err_to_out)
        sys_dup2(1, 2);
    return 0;
}

/* —— 子进程：找内置 / applet / PATH，对应地 execve 或直接调用 —— */

static void child_exec_or_builtin(int argc, char **argv)
{
    const char *name = argv[0];
    const shell_builtin_t *bi = shell_builtin_find(name);

    /* 作业实现必要：子进程恢复默认信号处置（Ctrl-C/Ctrl-Z 生效） */
    shell_child_reset_signals();

    if (bi)
        aether_exit(bi->fn(argc, argv, child_env));

    {
        int rc = aether_applet_run(name, argc, argv, child_env);
        if (rc >= 0)
            aether_exit(rc);
    }

    if (shell_exec_search(argc, argv, child_env) == 0)
        aether_exit(127); /* 不可达：shell_exec_search 成功即 exec 成功 */
    elog_err("aether: %s: command not found", name);
    aether_exit(127);
}

/* —— 管道的 unit 里 exec 一个命令 —— */

/* 从 pipeline 重建一行展示用命令行（作业表 jobs 显示用） */
static void build_cmdline(const shell_pipeline_t *p, char *out, size_t cap)
{
    size_t d = 0;
    if (cap == 0) return;
    out[0] = '\0';
    for (int i = 0; i < p->n_cmds; i++) {
        if (i > 0 && d + 3 < cap) { out[d++] = ' '; out[d++] = '|'; out[d++] = ' '; }
        for (int j = 0; j < p->cmds[i].argc; j++) {
            if (j > 0 && d + 1 < cap) out[d++] = ' ';
            for (const char *w = p->cmds[i].argv[j]; *w && d + 1 < cap; w++) out[d++] = *w;
        }
    }
    out[d] = '\0';
}

static int run_pipeline(const shell_pipeline_t *p, int background)
{
    int n = p->n_cmds;
    int prev_read = -1;
    int pids[SHELL_MAX_CMDS];
    int pgid = -1;
    int i;
    int interactive = shell_jobs_interactive();

    for (i = 0; i < n; i++) pids[i] = -1;

    for (i = 0; i < n; i++) {
        int fds[2] = { -1, -1 };
        int pid;
        if (i < n - 1) {
            if (sys_pipe(fds) != 0) {
                elog_err("aether: pipe failed");
                pids[i] = 0;
                goto foreground_wait;
            }
        }
        pid = (int)sys_fork();
        if (pid < 0 || AETHER_IS_ERR((long)pid)) {
            elog_err("aether: fork failed");
            if (i < n - 1) { sys_close(fds[0]); sys_close(fds[1]); }
            pids[i] = 0;
            goto foreground_wait;
        }
        pids[i] = pid;
        if (pid == 0) {
            /* 子进程：加入作业组（首个成员自己成组） */
            if (interactive) shell_job_child_enter(i == 0 ? -1 : pgid);
            /* 子进程 */
            if (i > 0) {
                if (prev_read >= 0) sys_dup2(prev_read, 0);
                sys_close(prev_read);
            }
            if (i < n - 1) {
                sys_close(fds[0]);
                sys_dup2(fds[1], 1);
                sys_close(fds[1]);
            }
            if (setup_redirects(&p->cmds[i]) != 0)
                aether_exit(1);
            /* argv 内容不被修改，const 仅提示用途 */
            child_exec_or_builtin(p->cmds[i].argc, (char **)p->cmds[i].argv);
            aether_exit(127); /* 不可达 */
        }
        /* 父进程 */
        if (interactive && i == 0 && pid > 0) pgid = pid;      /* 组长 pid */
        if (interactive && pid > 0 && i > 0) sys_setpgid(pid, pgid); /* 防竞争 */
        if (i > 0 && prev_read >= 0) sys_close(prev_read);
        prev_read = (i < n - 1) ? fds[0] : -1;
        if (i < n - 1 && fds[1] >= 0) sys_close(fds[1]);
    }

    /* 前台作业：终端交给作业进程组 */
    if (interactive && !background && pgid > 0) tty_setpgrp(0, pgid);

    if (background) {
        if (interactive) {
            char cmdline[600];
            build_cmdline(p, cmdline, sizeof(cmdline));
            {
                int job_id = shell_job_add(pgid > 0 ? pgid : pids[0], pids, n, cmdline, JOB_RUN);
                if (job_id > 0) elog_info("[%d] %d", job_id, pgid > 0 ? pgid : pids[0]);
            }
        }
        return 0;
    }

foreground_wait:
    /* 前台等待：归还终端 + 登记 stopped 作业 */
    {
        char cmdline[600];
        build_cmdline(p, cmdline, sizeof(cmdline));
        int fg_result = shell_job_wait_foreground(pgid, pids, n);
        if (interactive) tty_setpgrp(0, shell_jobs_self_pgid());
        if (interactive && (fg_result & 0x100)) {
            int job_id = shell_job_add(pgid > 0 ? pgid : pids[0], pids, n, cmdline, JOB_STOP);
            if (job_id > 0) elog_info("[%d] Stopped\t%s", job_id, cmdline);
        }
        return fg_result & 0xff;
    }
}

/* —— unit（; / & 分开的段）执行 —— */

static int run_unit(const shell_unit_t *u)
{
    const shell_pipeline_t *p = &u->p;

    if (p->n_cmds == 0) return 0;

    /* in_process 内置（cd/exit/exec）：单命令段时在 shell 里直接执行 */
    if (p->n_cmds == 1) {
        const shell_cmd_t *cmd = &p->cmds[0];
        const shell_builtin_t *bi = shell_builtin_find(cmd->argv[0]);
        if (bi && bi->in_process && !u->background)
            return bi->fn(cmd->argc, (char **)cmd->argv, child_env); /* exit/exec 不会返回 */
    }

    return run_pipeline(p, u->background);
}

static void reap_zombies(void)
{
    /* 收割已退出的作业成员（打印 [id] Done）；shell 本身保持干净 */
    shell_job_reap();
}

/* 提示符：aether <cwd> # */
static void build_prompt(char *buf, size_t cap)
{
    size_t d = 0;
    char cwd[512];
    const char *pre = "aether ";
    long r = sys_getcwd(cwd, sizeof(cwd));

    if (cap < 2) { buf[0] = '\0'; return; }
    for (size_t k = 0; pre[k] && d + 1 < cap; k++) buf[d++] = pre[k];
    if (AETHER_IS_ERR(r) || r <= 0) {
        const char *root = "/";
        for (size_t k = 0; root[k] && d + 1 < cap; k++) buf[d++] = root[k];
    } else {
        long slen = r - 1; /* 内核 getcwd 原始返回含尾 NUL，slen = strlen */
        if (slen < 0) slen = 0;
        for (long k = 0; k < slen && d + 1 < cap; k++) buf[d++] = cwd[k];
    }
    for (size_t k = 0; "# "[k] && d + 1 < cap; k++) buf[d++] = "# "[k];
    buf[d] = '\0';
}

/* —— 主入口 —— */

int shell_main(int argc, char **argv, char **envp)
{
    char line[SHELL_MAX_LINE];
    mem_arena_t *arena;
    int status = 0;
    int interactive;

    (void)argc; (void)argv;

    interactive = shell_tty_is(0);
    shell_jobs_init(interactive);
    build_child_env(envp);

    aether_signal(SIGINT, shell_sigint_handler);
    /* 关键：shell 必须忽略 SIGTSTP/SIGTTIN/SIGTTOU —— 我们的进程组是孤儿进程组
     * （initramfs 里 PID 1 同理），否则 tcsetpgrp/TCSETS 会 EIO/ENOTTY（Linux 内核
     * 对孤儿进程组的后台进程不发 SIGTTOU 也不放行）。对齐 bash 交互模式行为。 */
    aether_signal(SIGTSTP, SIG_IGN);
    aether_signal(SIGTTIN, SIG_IGN);
    aether_signal(SIGTTOU, SIG_IGN);

    arena = mem_arena_create();
    if (!arena) {
        elog_err("aether: out of memory");
        return 1;
    }

    for (;;) {
        shell_script_t script;
        int n;

        reap_zombies();
        mem_arena_reset(arena);

        if (interactive) {
            char prompt[600];
            build_prompt(prompt, sizeof(prompt));
            n = lineedit_read(prompt, line, sizeof(line));
        } else {
            n = shell_read_fd0_line(line, sizeof(line));
        }

        if (n == LINEEDIT_CTRL_C) continue; /* Ctrl-C：放弃本行 */
        if (n < 0) break;                        /* EOF */

        if (shell_parse_line(arena, line, &script) != 0) {
            elog_err("aether: syntax error");
            status = 2;
            continue;
        }
        for (int i = 0; i < script.n_units; i++)
            status = run_unit(&script.units[i]);
    }

    mem_arena_destroy(arena);
    return status;
}
