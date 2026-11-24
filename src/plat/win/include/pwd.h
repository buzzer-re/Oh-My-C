/* pwd.h for Windows: the current user only (name from GetUserNameW, home from USERPROFILE) */
#ifndef OMC_WIN_PWD_H
#define OMC_WIN_PWD_H
struct passwd {
    char *pw_name, *pw_passwd;
    unsigned pw_uid, pw_gid;
    char *pw_gecos, *pw_dir, *pw_shell;
};
#define getpwuid omc_win_getpwuid
#define getpwnam(n) omc_win_getpwuid(0)
struct passwd *omc_win_getpwuid(unsigned uid);
#endif
