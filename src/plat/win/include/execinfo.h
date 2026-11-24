/* execinfo.h for Windows: backtrace over CaptureStackBackTrace; symbols are module+offset lines */
#ifndef OMC_WIN_EXECINFO_H
#define OMC_WIN_EXECINFO_H
#define backtrace omc_win_backtrace
#define backtrace_symbols_fd omc_win_backtrace_symbols_fd
int omc_win_backtrace(void **buf, int n);
void omc_win_backtrace_symbols_fd(void *const *buf, int n, int fd);
#endif
