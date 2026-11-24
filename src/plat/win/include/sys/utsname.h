/* sys/utsname.h for Windows: sysname "Windows_NT" (Node's os.type()), release "10.0.<build>", machine "x86_64" */
#ifndef OMC_WIN_SYS_UTSNAME_H
#define OMC_WIN_SYS_UTSNAME_H
struct utsname {
    char sysname[65], nodename[257], release[65], version[65], machine[65];
};
#define uname omc_win_uname
int omc_win_uname(struct utsname *u);
#endif
