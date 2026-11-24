/* sys/mman.h for Windows: read-only private file mappings (CreateFileMapping) */
#ifndef OMC_WIN_SYS_MMAN_H
#define OMC_WIN_SYS_MMAN_H
#include <stddef.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FAILED ((void *)-1)
#define mmap omc_win_mmap
#define munmap omc_win_munmap
#define madvise(a, l, f) 0
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
void *omc_win_mmap(void *addr, size_t len, int prot, int flags, int fd, long long off);
int omc_win_munmap(void *addr, size_t len);
#endif
