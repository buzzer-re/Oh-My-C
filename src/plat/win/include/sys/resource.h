/* sys/resource.h for Windows: getrusage(RUSAGE_SELF) from GetProcessTimes / GetProcessMemoryInfo */
#ifndef OMC_WIN_SYS_RESOURCE_H
#define OMC_WIN_SYS_RESOURCE_H
#include <sys/time.h>
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)
struct rusage {
    struct timeval ru_utime, ru_stime;
    long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap, ru_inblock, ru_oublock, ru_msgsnd,
        ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};
#define getrusage omc_win_getrusage
int omc_win_getrusage(int who, struct rusage *ru);
#endif
