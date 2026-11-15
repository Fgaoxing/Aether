/*
 * jobs.c — 作业控制（§7.7）：进程组 + 终端归属 + jobs/fg/bg
 *
 * 模型：每个 pipeline（作业）一个进程组，组长为第一个子进程；
 * 交互模式下 shell 自己有独立进程组并拥有终端（tcsetpgrp），
 * 前台作业启动时 shell 把终端交给作业的进程组，作业全部结束/停止后归还。
 * fg：抢回终端 + SIGCONT；bg：SIGCONT（终端留给 shell）。
 * 作业表记录成员 pid；WUNTRACED 只在前台等待时消费，reaper 用 WNOHANG 防止死循环。
 */
#include <stddef.h>
#include <stdarg.h>

#include "shell_internal.h"
#include "elog.h"
#include "syscall_shim.h"
#include "ttyutil.h"
#include "util.h"

#define SIGCONT_C 18
#define WIFSTOPPED_SX 0x7f

static shell_job_t jobs[SHELL_JOB_MAX];
static int jobs_interactive = 0;
static int shell_pgid = -1;
static int job_next_id = 1;

/* ---------- 初始化 ---------- */

void shell_jobs_init(int interactive)
{
    jobs_interactive = interactive;
    if (!interactive) { shell_pgid = -1; return; }
    long pid = sys_getpid();
    sys_setpgid((int)pid, (int)pid); /* 失败（如 PID 1 已是组长）忽略 */
    shell_pgid = (int)pid;
    /* 抢走终端：shell 成为前台进程组（不抢也不致命，作业启动时再交一次） */
    tty_setpgrp(0, shell_pgid);
}

int shell_jobs_interactive(void) { return jobs_interactive; }
int shell_jobs_self_pgid(void) { return shell_pgid; }

/* ---------- 子进程 ---------- */

void shell_child_reset_signals(void)
{
    /* 作业里的子进程：恢复默认处置（可被 Ctrl-C / Ctrl-\ / Ctrl-Z 直接终止或停止） */
    aether_signal(SIGINT, SIG_DFL);
    aether_signal(SIGQUIT, SIG_DFL);
    aether_signal(SIGTSTP, SIG_DFL);
    aether_signal(SIGTTIN, SIG_DFL);
    aether_signal(SIGTTOU, SIG_DFL);
    aether_signal(SIGCHLD, SIG_DFL);
}

void shell_job_child_enter(int pgid)
{
    /* 加入作业组；leader 组长（pgid<0）则自己成组 */
    if (pgid < 0) sys_setpgid(0, 0);
    else          sys_setpgid(0, pgid);
}

/* ---------- 作业表 ---------- */

static shell_job_t *job_alloc(void)
{
    for (int i = 0; i < SHELL_JOB_MAX; i++)
        if (jobs[i].state == 0) return &jobs[i];
    return 0;
}

int shell_job_add(int pgid, const int *pids, int n, const char *cmdline, int state)
{
    shell_job_t *j = job_alloc();
    if (!j) return -1;
    j->pgid = pgid;
    j->id = job_next_id++;
    j->state = state;
    j->n_cmds = 0;
    j->n_left = 0;
    for (int i = 0; i < n && i < SHELL_MAX_CMDS; i++) {
        if (pids[i] > 0) {
            j->pids[j->n_left++] = pids[i];
            j->n_cmds = j->n_left;
        }
    }
    size_t d = 0;
    if (cmdline) {
        while (cmdline[d] && d + 1 < sizeof(j->cmdline)) { j->cmdline[d] = cmdline[d]; d++; }
    }
    j->cmdline[d] = '\0';
    return j->id;
}

shell_job_t *shell_job_find(int id)
{
    for (int i = 0; i < SHELL_JOB_MAX; i++)
        if (jobs[i].state != 0 && jobs[i].id == id) return &jobs[i];
    return 0;
}

shell_job_t *shell_job_latest(void)
{
    shell_job_t *best = 0;
    for (int i = 0; i < SHELL_JOB_MAX; i++) {
        if (jobs[i].state != 0 && (!best || jobs[i].id > best->id)) best = &jobs[i];
    }
    return best;
}

void shell_job_list_print(void)
{
    for (int i = 0; i < SHELL_JOB_MAX; i++) {
        shell_job_t *j = &jobs[i];
        if (j->state == 0) continue;
        elog_info("[%d] %s\t%s", j->id, j->state == JOB_RUN ? "Running" : "Stopped",
                  j->cmdline[0] ? j->cmdline : "?");
    }
}

/* ---------- 前台等待 ---------- */

static int wif_stopped(int status) { return (status & 0x7f) == WIFSTOPPED_SX; }
static int wexit_status(int status) { return (status >> 8) & 0xff; }

/*
 * 前台作业等待：按成员顺序 wait4（WUNTRACED）。
 * 终端归属已由调用方交给作业 pgid；返回后调用方归还终端。
 * 返回：bit0=有成员停止（stopped），低 8 位为最后退出成员的退出码（近似）。
 */
int shell_job_wait_foreground(int pgid, const int *pids, int n)
{
    int any_stopped = 0;
    int last_status = 0;
    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0) continue;
        for (;;) {
            int status = 0;
            long r = sys_wait4(pids[i], &status, WUNTRACED, 0);
            if (AETHER_IS_ERR(r)) {
                if (AETHER_ERR_CODE(r) == 4) continue; /* EINTR */
                break;
            }
            if (wif_stopped(status)) any_stopped = 1;
            else last_status = wexit_status(status);
            break;
        }
    }
    return (any_stopped ? 0x100 : 0) | (last_status & 0xff);
}

/* ---------- 后台收割 ---------- */

/* 收割已退出的作业成员（不打印停止类通知——前台已在 wait 里报告过） */
void shell_job_reap(void)
{
    for (;;) {
        int status = 0;
        long r = sys_wait4(-1, &status, WNOHANG, 0);
        if (r <= 0 || AETHER_IS_ERR(r)) break;
        int pid = (int)r;
        for (int i = 0; i < SHELL_JOB_MAX; i++) {
            shell_job_t *j = &jobs[i];
            if (j->state == 0) continue;
            int found = 0;
            for (int k = 0; k < j->n_left; k++)
                if (j->pids[k] == pid) { found = 1; break; }
            if (!found) continue;
            /* 退出成员：从表移除 */
            for (int k = 0; k < j->n_left; k++)
                if (j->pids[k] == pid) {
                    for (int m = k; m + 1 < j->n_left; m++) j->pids[m] = j->pids[m + 1];
                    j->n_left--;
                    break;
                }
            if (j->n_left == 0) {
                elog_info("[%d] Done\t%s", j->id, j->cmdline);
                j->state = 0;
                j->n_cmds = 0;
            }
            break;
        }
    }
}

/* ---------- fg / bg ---------- */

int shell_job_continue_fg(shell_job_t *job)
{
    int stopped = 0, termed = 0;
    if (shell_jobs_interactive()) tty_setpgrp(0, job->pgid);
    sys_kill(-job->pgid, SIGCONT_C);
    job->state = JOB_RUN;
    /* 逐成员阻塞等待（每个成员本次调用只等一次，避免重复消费 stop 通知死循环） */
    int new_left = 0;
    int new_pids[SHELL_MAX_CMDS];
    for (int i = 0; i < job->n_left; i++) {
        int status = 0;
        long r = sys_wait4(job->pids[i], &status, WUNTRACED, 0);
        if (AETHER_IS_ERR(r)) {
            /* ECHILD：成员已被回收，直接丢弃 */
            continue;
        }
        if (wif_stopped(status)) { stopped = 1; new_pids[new_left++] = job->pids[i]; }
        else if ((status & 0x7f) != 0) { termed = 1; }
        /* 正常退出：丢弃 */
    }
    job->n_left = new_left;
    for (int k = 0; k < new_left; k++) job->pids[k] = new_pids[k];
    if (job->n_left == 0) {
        elog_info("[%d] %s\t%s", job->id, termed ? "Terminated" : "Done", job->cmdline);
        job->state = 0;
    } else if (stopped) job->state = JOB_STOP;
    if (shell_jobs_interactive()) tty_setpgrp(0, shell_jobs_self_pgid());
    if (stopped) {
        elog_info("[%d] Stopped\t%s", job->id, job->cmdline);
        return -2; /* 停止 */
    }
    return 0;
}

int shell_job_continue_bg(shell_job_t *job)
{
    sys_kill(-job->pgid, SIGCONT_C);
    job->state = JOB_RUN;
    return 0;
}
