/* poll.h for Windows: struct pollfd and POLL* come from Winsock; omc_win_poll (fd.c) waits on pipes, the console,
 * files and socket fds alike */
#ifndef OMC_WIN_POLL_H
#define OMC_WIN_POLL_H
#pragma push_macro("gethostname")
#undef gethostname
#include <winsock2.h>
#pragma pop_macro("gethostname")
#undef poll
#define poll omc_win_poll
int omc_win_poll(struct pollfd *fds, nfds_t n, int timeout_ms);
#endif
