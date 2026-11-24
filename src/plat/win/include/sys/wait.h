/* sys/wait.h for Windows: waitpid over the children plat/win/proc.c started (pid -> process handle). A child's exit
 * code is its status; a child terminated by omc_win_kill reports the signal (WIFSIGNALED). */
#ifndef OMC_WIN_SYS_WAIT_H
#define OMC_WIN_SYS_WAIT_H
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s) ((s) & 0x7f)
#define WIFSTOPPED(s) 0
#define WSTOPSIG(s) 0
#define WCOREDUMP(s) 0
#define waitpid omc_win_waitpid
int omc_win_waitpid(int pid, int *status, int options);
#define wait(s) omc_win_waitpid(-1, (s), 0)
#endif
