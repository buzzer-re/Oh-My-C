/* sys/random.h for Windows: getrandom over BCryptGenRandom */
#ifndef OMC_WIN_SYS_RANDOM_H
#define OMC_WIN_SYS_RANDOM_H
#include <stddef.h>
#define GRND_NONBLOCK 1
#define GRND_RANDOM 2
#define getrandom omc_win_getrandom
ssize_t omc_win_getrandom(void *buf, size_t len, unsigned flags);
#endif
