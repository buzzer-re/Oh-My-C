/* plat/win/fs.c - the file system with POSIX semantics on Windows: UTF-8 paths ('/' or '\'), stat with symlinks and
 * nanosecond times, d_type directories, files opened with full sharing so they can be renamed over and deleted while
 * open (as POSIX allows), atomic rename-replace, advisory locks, temp files, links, mmap and glob. */
#include "plat/win/win.h"

#include <glob.h>
#include <sys/random.h>
#include <sys/mman.h>
#include <winioctl.h>

#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)

#ifndef ERROR_DIRECTORY_NOT_SUPPORTED
#define ERROR_DIRECTORY_NOT_SUPPORTED 336L
#endif
#ifndef ERROR_CANT_RESOLVE_FILENAME
#define ERROR_CANT_RESOLVE_FILENAME 1921L
#endif
#ifndef FILE_DISPOSITION_FLAG_DELETE
#define FILE_DISPOSITION_FLAG_DELETE 0x1
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x2
#define FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE 0x10
#endif
typedef struct {
    ULONG Flags;
} disposition_ex_t;
#define FILE_DISPOSITION_INFO_EX disposition_ex_t
#define FileDispositionInfoEx ((FILE_INFO_BY_HANDLE_CLASS)21)

/* ---------- reparse points ---------- */

typedef struct {
    ULONG tag;
    USHORT len, reserved;
    union {
        struct {
            USHORT sub_off, sub_len, print_off, print_len;
            ULONG flags;
            WCHAR buf[1];
        } sym;
        struct {
            USHORT sub_off, sub_len, print_off, print_len;
            WCHAR buf[1];
        } mount;
    };
} reparse_t;

static bool is_link_tag(DWORD tag) { return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT; }

/* ---------- stat ---------- */

static void ft_to_ts(const FILETIME *ft, struct timespec *ts) {
    unsigned long long t = (unsigned long long)ft->dwHighDateTime << 32 | ft->dwLowDateTime;
    if (t < 116444736000000000ull) {
        ts->tv_sec = 0;
        ts->tv_nsec = 0;
        return;
    }
    t -= 116444736000000000ull; /* 1601 -> 1970 */
    ts->tv_sec = (time_t)(t / 10000000ull);
    ts->tv_nsec = (long)(t % 10000000ull) * 100;
}

static bool has_exec_ext(const wchar_t *p) {
    const wchar_t *dot = wcsrchr(p, L'.'), *sep = wcsrchr(p, L'\\');
    if (!dot || (sep && dot < sep)) return false;
    return !_wcsicmp(dot, L".exe") || !_wcsicmp(dot, L".com") || !_wcsicmp(dot, L".bat") || !_wcsicmp(dot, L".cmd");
}

static unsigned mode_from(DWORD attrs, bool link, const wchar_t *path) {
    if (link) return S_IFLNK | 0777;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return S_IFDIR | 0755;
    unsigned m = S_IFREG | ((attrs & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
    if (path && has_exec_ext(path)) m |= 0111;
    return m;
}

static int fill_from_handle(HANDLE h, struct omc_win_stat *st, const wchar_t *path, bool nofollow) {
    memset(st, 0, sizeof *st);
    DWORD type = GetFileType(h);
    if (type == FILE_TYPE_PIPE) {
        st->st_mode = S_IFIFO | 0600;
        st->st_nlink = 1;
        return 0;
    }
    if (type == FILE_TYPE_CHAR) {
        st->st_mode = S_IFCHR | 0666;
        st->st_nlink = 1;
        return 0;
    }
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi)) return win_fail(GetLastError());
    bool link = false;
    if (nofollow && (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        FILE_ATTRIBUTE_TAG_INFO ti;
        if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof ti) && is_link_tag(ti.ReparseTag)) link = true;
    }
    st->st_dev = fi.dwVolumeSerialNumber;
    st->st_ino = (unsigned long long)fi.nFileIndexHigh << 32 | fi.nFileIndexLow;
    st->st_mode = mode_from(fi.dwFileAttributes, link, path);
    st->st_nlink = fi.nNumberOfLinks;
    st->st_size = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : (long long)((unsigned long long)fi.nFileSizeHigh << 32 | fi.nFileSizeLow);
    ft_to_ts(&fi.ftLastAccessTime, &st->st_atim);
    ft_to_ts(&fi.ftLastWriteTime, &st->st_mtim);
    ft_to_ts(&fi.ftLastWriteTime, &st->st_ctim);
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    return 0;
}

static int stat_w(const wchar_t *wp, struct omc_win_stat *st, bool nofollow) {
    HANDLE h = CreateFileW(wp, FILE_READ_ATTRIBUTES, SHARE_ALL, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | (nofollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0), NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        /* files the handle can't be opened on (in use by the pagefile, app execution aliases): the directory entry */
        WIN32_FILE_ATTRIBUTE_DATA ad;
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND && GetFileAttributesExW(wp, GetFileExInfoStandard, &ad)) {
            memset(st, 0, sizeof *st);
            st->st_mode = mode_from(ad.dwFileAttributes, false, wp);
            st->st_nlink = 1;
            st->st_size = (long long)((unsigned long long)ad.nFileSizeHigh << 32 | ad.nFileSizeLow);
            ft_to_ts(&ad.ftLastAccessTime, &st->st_atim);
            ft_to_ts(&ad.ftLastWriteTime, &st->st_mtim);
            st->st_ctim = st->st_mtim;
            return 0;
        }
        return win_fail(err);
    }
    int r = fill_from_handle(h, st, wp, nofollow);
    CloseHandle(h);
    return r;
}

static int stat_path(const char *path, struct omc_win_stat *st, bool nofollow) {
    if (!path || !*path) {
        errno = ENOENT;
        return -1;
    }
    wchar_t *wp = win_path(path);
    if (!wp) {
        errno = ENOMEM;
        return -1;
    }
    int r = stat_w(wp, st, nofollow);
    free(wp);
    if (r == 0 && S_ISLNK(st->st_mode)) {
        char buf[4096];
        ssize_t n = omc_win_readlink(path, buf, sizeof buf);
        st->st_size = n > 0 ? n : 0;
    }
    return r;
}

int omc_win_stat(const char *path, struct omc_win_stat *st) { return stat_path(path, st, false); }
int omc_win_lstat(const char *path, struct omc_win_stat *st) { return stat_path(path, st, true); }

int omc_win_fstat(int fd, struct omc_win_stat *st) {
    int k = win_fd_kind(fd);
    if (k == FDK_SOCKET) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFSOCK | 0600;
        st->st_nlink = 1;
        return 0;
    }
    HANDLE h = win_fd_get_handle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    return fill_from_handle(h, st, NULL, false);
}

/* the path of an open directory fd (owned, forward slashes) */
static char *fd_path(int fd) {
    HANDLE h = win_fd_get_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    wchar_t buf[32768];
    DWORD n = GetFinalPathNameByHandleW(h, buf, ARRAYSIZE(buf), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= ARRAYSIZE(buf)) return NULL;
    const wchar_t *p = buf;
    if (!wcsncmp(p, L"\\\\?\\UNC\\", 8)) {
        p += 6;
        buf[6] = L'\\';
    } else if (!wcsncmp(p, L"\\\\?\\", 4)) {
        p += 4;
    }
    char *u = win_utf8(p);
    win_slashes(u);
    return u;
}

static bool is_abs_path(const char *p) {
    return (p[0] == '/' || p[0] == '\\') || (isalpha((unsigned char)p[0]) && p[1] == ':');
}

/* dirfd-relative path -> a path the plain calls take (owned) */
static char *at_path(int dirfd, const char *path) {
    if (dirfd == AT_FDCWD || is_abs_path(path)) return strdup(path);
    char *d = fd_path(dirfd);
    if (!d) {
        errno = EBADF;
        return NULL;
    }
    size_t a = strlen(d), b = strlen(path);
    char *r = malloc(a + b + 2);
    if (r) {
        memcpy(r, d, a);
        r[a] = '/';
        memcpy(r + a + 1, path, b + 1);
    }
    free(d);
    return r;
}

int omc_win_fstatat(int dirfd, const char *path, struct omc_win_stat *st, int flags) {
    char *p = at_path(dirfd, path);
    if (!p) return -1;
    int r = stat_path(p, st, (flags & AT_SYMLINK_NOFOLLOW) != 0);
    free(p);
    return r;
}

/* ---------- open ---------- */

static HANDLE open_handle(const char *path, int flags, int mode, DWORD *err) {
    wchar_t *wp = win_path(path);
    if (!wp) {
        *err = ERROR_NOT_ENOUGH_MEMORY;
        return INVALID_HANDLE_VALUE;
    }
    int acc = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    DWORD access = acc == O_WRONLY ? GENERIC_WRITE : acc == O_RDWR ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    if (flags & O_APPEND) access = (access & ~(DWORD)GENERIC_WRITE) | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES;
    DWORD disp;
    if ((flags & O_CREAT) && (flags & O_EXCL)) disp = CREATE_NEW;
    else if ((flags & O_CREAT) && (flags & O_TRUNC)) disp = CREATE_ALWAYS;
    else if (flags & O_CREAT) disp = OPEN_ALWAYS;
    else if (flags & O_TRUNC) disp = TRUNCATE_EXISTING;
    else disp = OPEN_EXISTING;
    DWORD attrs = FILE_ATTRIBUTE_NORMAL;
    if ((flags & O_CREAT) && !(mode & 0200)) attrs = FILE_ATTRIBUTE_READONLY;
    /* directories open read-only (POSIX open(dir, O_RDONLY) works; reads then fail) */
    attrs |= FILE_FLAG_BACKUP_SEMANTICS;
    if (flags & O_NOFOLLOW) attrs |= FILE_FLAG_OPEN_REPARSE_POINT;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, FALSE};
    HANDLE h = CreateFileW(wp, access, SHARE_ALL, &sa, disp, attrs, NULL);
    *err = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    /* TRUNCATE_EXISTING needs GENERIC_WRITE; O_RDONLY|O_TRUNC is unspecified in POSIX anyway */
    if (h != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION fi;
        bool dir = GetFileInformationByHandle(h, &fi) && (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if ((flags & O_DIRECTORY) && !dir) {
            CloseHandle(h);
            *err = ERROR_DIRECTORY;
            h = INVALID_HANDLE_VALUE;
        } else if (dir && acc != O_RDONLY) {
            CloseHandle(h);
            *err = ERROR_DIRECTORY_NOT_SUPPORTED;
            h = INVALID_HANDLE_VALUE;
        }
    }
    free(wp);
    return h;
}

int omc_win_open(const char *path, int flags, ...) {
    int mode = 0666;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!path) {
        errno = EFAULT;
        return -1;
    }
    DWORD err;
    HANDLE h = open_handle(path, flags, mode, &err);
    if (h == INVALID_HANDLE_VALUE) {
        if (err == ERROR_DIRECTORY && (flags & O_DIRECTORY)) errno = ENOTDIR;
        else if (err == ERROR_DIRECTORY_NOT_SUPPORTED) errno = EISDIR;
        else if (err == ERROR_FILE_EXISTS) errno = EEXIST;
        else errno = win_errno_from(err);
        return -1;
    }
    int crt = _O_BINARY | ((flags & O_APPEND) ? _O_APPEND : 0);
    int acc = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    if (acc == O_RDONLY) crt |= _O_RDONLY;
    int fd = _open_osfhandle((intptr_t)h, crt);
    if (fd < 0) {
        CloseHandle(h);
        errno = EMFILE;
        return -1;
    }
    if (flags & O_NONBLOCK) win_fd_set_nonblock(fd, true);
    return fd;
}

int omc_win_openat(int dirfd, const char *path, int flags, ...) {
    int mode = 0666;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    char *p = at_path(dirfd, path);
    if (!p) return -1;
    int fd = omc_win_open(p, flags, mode);
    free(p);
    return fd;
}

/* fopen modes: r w a, +, b/t ignored (always binary), x exclusive, e close-on-exec */
static int mode_flags(const char *mode, int *crt_mode_ok) {
    int flags;
    switch (mode[0]) {
    case 'r': flags = O_RDONLY; break;
    case 'w': flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': flags = O_WRONLY | O_CREAT | O_APPEND; break;
    default: *crt_mode_ok = 0; return 0;
    }
    *crt_mode_ok = 1;
    for (const char *m = mode + 1; *m; m++) {
        if (*m == '+') flags = (flags & ~(O_RDONLY | O_WRONLY)) | O_RDWR;
        else if (*m == 'x') flags |= O_EXCL;
        else if (*m == 'e') flags |= O_CLOEXEC;
    }
    return flags;
}

static void crt_mode(const char *mode, char out[8]) {
    int n = 0;
    out[n++] = mode[0];
    if (strchr(mode, '+')) out[n++] = '+';
    out[n++] = 'b';
    out[n] = 0;
}

FILE *omc_win_fopen(const char *path, const char *mode) {
    int ok;
    int flags = mode_flags(mode, &ok);
    if (!ok) {
        errno = EINVAL;
        return NULL;
    }
    int fd = omc_win_open(path, flags, 0666);
    if (fd < 0) return NULL;
    char m[8];
    crt_mode(mode, m);
    FILE *f = _fdopen(fd, m);
    if (!f) {
        int e = errno;
        _close(fd);
        errno = e;
    }
    return f;
}

FILE *omc_win_freopen(const char *path, const char *mode, FILE *f) {
    if (!path) return f;
    int ok;
    int flags = mode_flags(mode, &ok);
    if (!ok) {
        errno = EINVAL;
        return NULL;
    }
    int fd = omc_win_open(path, flags, 0666);
    if (fd < 0) return NULL;
    fflush(f);
    if (_dup2(fd, _fileno(f)) != 0) {
        _close(fd);
        return NULL;
    }
    _close(fd);
    return f;
}

FILE *omc_win_fdopen(int fd, const char *mode) {
    char m[8];
    crt_mode(mode, m);
    return _fdopen(fd, m);
}

int omc_win_fileno(FILE *f) { return _fileno(f); }

/* ---------- directories ---------- */

struct omc_win_DIR {
    HANDLE find;
    WIN32_FIND_DATAW data;
    bool have, first;
    wchar_t *pattern;
    int fd; /* dirfd(): opened on demand */
    char *path;
    struct omc_win_dirent ent;
};

static bool dir_start(omc_win_DIR *d) {
    d->find = FindFirstFileExW(d->pattern, FindExInfoBasic, &d->data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (d->find == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        d->have = false;
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_MORE_FILES; /* an empty directory (never, with . and ..) */
    }
    d->have = true;
    return true;
}

omc_win_DIR *omc_win_opendir(const char *path) {
    struct omc_win_stat st;
    if (omc_win_stat(path, &st) != 0) return NULL;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }
    omc_win_DIR *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    size_t n = strlen(path);
    char *pat = malloc(n + 3);
    memcpy(pat, path, n);
    size_t k = n;
    if (k && pat[k - 1] != '/' && pat[k - 1] != '\\') pat[k++] = '/';
    pat[k++] = '*';
    pat[k] = 0;
    d->pattern = win_path(pat);
    free(pat);
    d->path = strdup(path);
    d->fd = -1;
    if (!dir_start(d)) {
        int e = win_errno_from(GetLastError());
        free(d->pattern);
        free(d->path);
        free(d);
        errno = e;
        return NULL;
    }
    return d;
}

omc_win_DIR *omc_win_fdopendir(int fd) {
    char *p = fd_path(fd);
    if (!p) {
        errno = EBADF;
        return NULL;
    }
    omc_win_DIR *d = omc_win_opendir(p);
    free(p);
    if (d) d->fd = fd; /* closedir closes it, as POSIX says */
    return d;
}

struct omc_win_dirent *omc_win_readdir(omc_win_DIR *d) {
    for (;;) {
        if (!d->have) return NULL;
        WIN32_FIND_DATAW *f = &d->data;
        char *name = win_utf8(f->cFileName);
        if (!name) return NULL;
        size_t n = strlen(name);
        if (n >= sizeof d->ent.d_name) n = sizeof d->ent.d_name - 1;
        memcpy(d->ent.d_name, name, n);
        d->ent.d_name[n] = 0;
        free(name);
        DWORD a = f->dwFileAttributes;
        if ((a & FILE_ATTRIBUTE_REPARSE_POINT) && is_link_tag(f->dwReserved0)) d->ent.d_type = DT_LNK;
        else if (a & FILE_ATTRIBUTE_DIRECTORY) d->ent.d_type = DT_DIR;
        else d->ent.d_type = DT_REG;
        d->ent.d_ino = 0;
        if (!FindNextFileW(d->find, &d->data)) d->have = false;
        return &d->ent;
    }
}

int omc_win_closedir(omc_win_DIR *d) {
    if (!d) return -1;
    if (d->find != INVALID_HANDLE_VALUE && d->find) FindClose(d->find);
    if (d->fd >= 0) omc_win_close(d->fd);
    free(d->pattern);
    free(d->path);
    free(d);
    return 0;
}

void omc_win_rewinddir(omc_win_DIR *d) {
    if (d->find != INVALID_HANDLE_VALUE && d->find) FindClose(d->find);
    dir_start(d);
}

int omc_win_dirfd(omc_win_DIR *d) {
    if (d->fd < 0) d->fd = omc_win_open(d->path, O_RDONLY | O_DIRECTORY);
    return d->fd;
}

/* ---------- names ---------- */

int omc_win_mkdir(const char *path, int mode) {
    (void)mode;
    wchar_t *wp = win_path(path);
    BOOL ok = CreateDirectoryW(wp, NULL);
    DWORD e = GetLastError();
    free(wp);
    return ok ? 0 : win_fail(e);
}

int omc_win_rmdir(const char *path) {
    wchar_t *wp = win_path(path);
    BOOL ok = RemoveDirectoryW(wp);
    DWORD e = GetLastError();
    if (!ok && e == ERROR_DIRECTORY) e = ERROR_PATH_NOT_FOUND; /* not a directory */
    free(wp);
    if (!ok && e == ERROR_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }
    return ok ? 0 : win_fail(e);
}

/* delete with POSIX semantics where the file system has them: the name goes now, even with handles open */
static BOOL delete_w(const wchar_t *wp, bool dir, DWORD *err) {
    HANDLE h = CreateFileW(wp, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, SHARE_ALL, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return FALSE;
    }
    FILE_ATTRIBUTE_TAG_INFO ti;
    BY_HANDLE_FILE_INFORMATION fi;
    if (GetFileInformationByHandle(h, &fi)) {
        bool is_dir = fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        bool link = (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof ti) && is_link_tag(ti.ReparseTag);
        if (is_dir && !dir && !link) {
            CloseHandle(h);
            *err = ERROR_DIRECTORY_NOT_SUPPORTED;
            return FALSE;
        }
    }
    FILE_DISPOSITION_INFO_EX dx = {FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                   FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    BOOL ok = SetFileInformationByHandle(h, FileDispositionInfoEx, &dx, sizeof dx);
    if (!ok) {
        FILE_DISPOSITION_INFO d = {TRUE};
        ok = SetFileInformationByHandle(h, FileDispositionInfo, &d, sizeof d);
    }
    *err = ok ? 0 : GetLastError();
    CloseHandle(h);
    return ok;
}

int omc_win_unlink(const char *path) {
    wchar_t *wp = win_path(path);
    DWORD e;
    BOOL ok = delete_w(wp, false, &e);
    if (!ok && e == ERROR_ACCESS_DENIED) {
        /* read-only files: clear the attribute and retry with the plain call */
        DWORD a = GetFileAttributesW(wp);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(wp, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
            ok = DeleteFileW(wp);
            e = GetLastError();
        }
    }
    free(wp);
    if (!ok && e == ERROR_DIRECTORY_NOT_SUPPORTED) {
        errno = EISDIR;
        return -1;
    }
    return ok ? 0 : win_fail(e);
}

int omc_win_unlinkat(int dirfd, const char *path, int flags) {
    char *p = at_path(dirfd, path);
    if (!p) return -1;
    int r = (flags & AT_REMOVEDIR) ? omc_win_rmdir(p) : omc_win_unlink(p);
    free(p);
    return r;
}

int omc_win_remove(const char *path) {
    struct omc_win_stat st;
    if (omc_win_lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) return omc_win_rmdir(path);
    return omc_win_unlink(path);
}

int omc_win_rename(const char *from, const char *to) {
    wchar_t *wf = win_path(from), *wt = win_path(to);
    BOOL ok = MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    DWORD e = ok ? 0 : GetLastError();
    if (!ok && e == ERROR_ACCESS_DENIED) {
        /* the target is open somewhere: POSIX-semantics rename replaces it anyway (NTFS, Windows 10 1709+) */
        HANDLE h = CreateFileW(wf, DELETE | SYNCHRONIZE, SHARE_ALL, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            size_t tl = wcslen(wt);
            size_t sz = sizeof(FILE_RENAME_INFO) + tl * sizeof(wchar_t);
            FILE_RENAME_INFO *ri = calloc(1, sz);
            if (ri) {
                ri->Flags = 0x1 /* REPLACE_IF_EXISTS */ | 0x2 /* POSIX_SEMANTICS */ | 0x40 /* IGNORE_READONLY_ATTRIBUTE */;
                ri->FileNameLength = (DWORD)(tl * sizeof(wchar_t));
                memcpy(ri->FileName, wt, tl * sizeof(wchar_t));
                ok = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)22 /* FileRenameInfoEx */, ri, (DWORD)sz);
                e = ok ? 0 : GetLastError();
                free(ri);
            }
            CloseHandle(h);
        }
    }
    free(wf);
    free(wt);
    return ok ? 0 : win_fail(e);
}

int omc_win_access(const char *path, int mode) {
    wchar_t *wp = win_path(path);
    DWORD a = GetFileAttributesW(wp);
    DWORD e = GetLastError();
    if (a == INVALID_FILE_ATTRIBUTES) {
        free(wp);
        return win_fail(e);
    }
    int r = 0;
    if ((mode & W_OK) && (a & FILE_ATTRIBUTE_READONLY) && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        errno = EACCES;
        r = -1;
    }
    if (!r && (mode & X_OK) && !(a & FILE_ATTRIBUTE_DIRECTORY) && !has_exec_ext(wp)) {
        errno = EACCES;
        r = -1;
    }
    free(wp);
    return r;
}

int omc_win_chdir(const char *path) {
    wchar_t *wp = win_path(path);
    BOOL ok = SetCurrentDirectoryW(wp);
    DWORD e = GetLastError();
    free(wp);
    return ok ? 0 : win_fail(e);
}

char *omc_win_getcwd(char *buf, size_t size) {
    DWORD n = GetCurrentDirectoryW(0, NULL);
    wchar_t *w = malloc((n + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    GetCurrentDirectoryW(n + 1, w);
    char *u = win_utf8(w);
    free(w);
    if (!u) return NULL;
    win_slashes(u);
    size_t l = strlen(u);
    /* "C:/" keeps its slash; nothing else ends in one */
    if (!buf) {
        if (size && l + 1 > size) {
            free(u);
            errno = ERANGE;
            return NULL;
        }
        return u;
    }
    if (l + 1 > size) {
        free(u);
        errno = ERANGE;
        return NULL;
    }
    memcpy(buf, u, l + 1);
    free(u);
    return buf;
}

/* "\\?\C:\x" -> "C:/x", "\\?\UNC\srv\share" -> "//srv/share" (owned) */
static char *final_to_utf8(const wchar_t *p) {
    char *u;
    if (!wcsncmp(p, L"\\\\?\\UNC\\", 8)) {
        char *t = win_utf8(p + 8);
        size_t n = strlen(t);
        u = malloc(n + 3);
        u[0] = u[1] = '/';
        memcpy(u + 2, t, n + 1);
        free(t);
    } else {
        u = win_utf8(!wcsncmp(p, L"\\\\?\\", 4) ? p + 4 : p);
    }
    win_slashes(u);
    if (u && isalpha((unsigned char)u[0]) && u[1] == ':') u[0] = (char)toupper((unsigned char)u[0]);
    return u;
}

char *omc_win_realpath(const char *path, char *resolved) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    wchar_t *wp = win_path(path);
    HANDLE h = CreateFileW(wp, FILE_READ_ATTRIBUTES, SHARE_ALL, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD e = GetLastError();
    free(wp);
    if (h == INVALID_HANDLE_VALUE) {
        errno = win_errno_from(e);
        return NULL;
    }
    wchar_t buf[32768];
    DWORD n = GetFinalPathNameByHandleW(h, buf, ARRAYSIZE(buf), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= ARRAYSIZE(buf)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    char *u = final_to_utf8(buf);
    if (!u) return NULL;
    if (!resolved) return u;
    size_t l = strlen(u);
    if (l >= PATH_MAX) {
        free(u);
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(resolved, u, l + 1);
    free(u);
    return resolved;
}

ssize_t omc_win_readlink(const char *path, char *buf, size_t size) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (!strcmp(path, "/proc/self/exe")) {
        /* the process's executable, as Linux's link names it */
        wchar_t w[32768];
        DWORD n = GetModuleFileNameW(NULL, w, ARRAYSIZE(w));
        if (!n || n >= ARRAYSIZE(w)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        char *u = win_utf8(w);
        if (!u) return -1;
        win_slashes(u);
        size_t l = strlen(u);
        if (l > size) l = size;
        memcpy(buf, u, l);
        free(u);
        return (ssize_t)l;
    }
    wchar_t *wp = win_path(path);
    HANDLE h = CreateFileW(wp, FILE_READ_ATTRIBUTES, SHARE_ALL, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD e = GetLastError();
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return win_fail(e);
    union {
        reparse_t r;
        char b[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    } u;
    DWORD got;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, &u, sizeof u, &got, NULL);
    e = GetLastError();
    CloseHandle(h);
    if (!ok) {
        errno = e == ERROR_NOT_A_REPARSE_POINT ? EINVAL : win_errno_from(e);
        return -1;
    }
    const WCHAR *t;
    USHORT tl;
    bool relative = false;
    if (u.r.tag == IO_REPARSE_TAG_SYMLINK) {
        t = u.r.sym.buf + u.r.sym.sub_off / 2;
        tl = u.r.sym.sub_len / 2;
        relative = u.r.sym.flags & 1; /* SYMLINK_FLAG_RELATIVE */
    } else if (u.r.tag == IO_REPARSE_TAG_MOUNT_POINT) {
        t = u.r.mount.buf + u.r.mount.sub_off / 2;
        tl = u.r.mount.sub_len / 2;
    } else {
        errno = EINVAL;
        return -1;
    }
    /* \??\C:\x -> C:\x */
    if (!relative && tl >= 4 && !wcsncmp(t, L"\\??\\", 4)) {
        t += 4;
        tl -= 4;
    }
    char *s = win_utf8_n(t, tl);
    if (!s) return -1;
    win_slashes(s);
    size_t n = strlen(s);
    if (n > size) n = size;
    memcpy(buf, s, n);
    free(s);
    return (ssize_t)n;
}

int omc_win_symlink(const char *target, const char *linkpath) {
    /* a directory link needs the flag; a relative target is relative to the link's directory */
    struct omc_win_stat st;
    bool dir = false;
    if (is_abs_path(target)) {
        dir = omc_win_stat(target, &st) == 0 && S_ISDIR(st.st_mode);
    } else {
        const char *sl = strrchr(linkpath, '/'), *bs = strrchr(linkpath, '\\');
        if (bs > sl) sl = bs;
        size_t dl = sl ? (size_t)(sl - linkpath) + 1 : 0, tl = strlen(target);
        char *full = malloc(dl + tl + 1);
        memcpy(full, linkpath, dl);
        memcpy(full + dl, target, tl + 1);
        dir = omc_win_stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        free(full);
    }
    wchar_t *wt = win_path(target), *wl = win_path(linkpath);
    DWORD flags = (dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) | 0x2 /* ALLOW_UNPRIVILEGED_CREATE */;
    BOOL ok = CreateSymbolicLinkW(wl, wt, flags);
    DWORD e = GetLastError();
    if (!ok && e == ERROR_INVALID_PARAMETER) ok = CreateSymbolicLinkW(wl, wt, flags & ~0x2u), e = GetLastError();
    free(wt);
    free(wl);
    if (!ok && e == ERROR_PRIVILEGE_NOT_HELD) {
        errno = EPERM;
        return -1;
    }
    return ok ? 0 : win_fail(e);
}

int omc_win_link(const char *from, const char *to) {
    wchar_t *wf = win_path(from), *wt = win_path(to);
    BOOL ok = CreateHardLinkW(wt, wf, NULL);
    DWORD e = GetLastError();
    free(wf);
    free(wt);
    return ok ? 0 : win_fail(e);
}

int omc_win_chmod(const char *path, int mode) {
    wchar_t *wp = win_path(path);
    DWORD a = GetFileAttributesW(wp);
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        free(wp);
        return win_fail(e);
    }
    DWORD na = (mode & 0200) ? (a & ~(DWORD)FILE_ATTRIBUTE_READONLY) : (a | FILE_ATTRIBUTE_READONLY);
    if (a & FILE_ATTRIBUTE_DIRECTORY) na = a;
    BOOL ok = na == a || SetFileAttributesW(wp, na);
    DWORD e = GetLastError();
    free(wp);
    return ok ? 0 : win_fail(e);
}

int omc_win_fchmod(int fd, int mode) {
    (void)fd;
    (void)mode;
    return 0;
}

int omc_win_umask(int mask) {
    (void)mask;
    return 022;
}

static void ts_to_ft(const struct timespec *ts, FILETIME *ft) {
    unsigned long long t = (unsigned long long)ts->tv_sec * 10000000ull + (unsigned long long)ts->tv_nsec / 100 + 116444736000000000ull;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

static int set_times(HANDLE h, const struct timespec ts[2]) {
    FILETIME now, a, m;
    GetSystemTimeAsFileTime(&now);
    FILETIME *pa = &a, *pm = &m;
    if (!ts) {
        a = m = now;
    } else {
        if (ts[0].tv_nsec == UTIME_OMIT) pa = NULL;
        else if (ts[0].tv_nsec == UTIME_NOW) a = now;
        else ts_to_ft(&ts[0], &a);
        if (ts[1].tv_nsec == UTIME_OMIT) pm = NULL;
        else if (ts[1].tv_nsec == UTIME_NOW) m = now;
        else ts_to_ft(&ts[1], &m);
    }
    return SetFileTime(h, NULL, pa, pm) ? 0 : win_fail(GetLastError());
}

int omc_win_utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags) {
    char *p = at_path(dirfd, path);
    if (!p) return -1;
    wchar_t *wp = win_path(p);
    free(p);
    HANDLE h = CreateFileW(wp, FILE_WRITE_ATTRIBUTES, SHARE_ALL, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | ((flags & AT_SYMLINK_NOFOLLOW) ? FILE_FLAG_OPEN_REPARSE_POINT : 0), NULL);
    DWORD e = GetLastError();
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return win_fail(e);
    int r = set_times(h, ts);
    CloseHandle(h);
    return r;
}

int omc_win_futimens(int fd, const struct timespec ts[2]) {
    HANDLE h = win_fd_get_handle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    return set_times(h, ts);
}

int omc_win_truncate(const char *path, long long len) {
    int fd = omc_win_open(path, O_WRONLY);
    if (fd < 0) return -1;
    int r = omc_win_ftruncate(fd, len);
    int e = errno;
    omc_win_close(fd);
    errno = e;
    return r;
}

int omc_win_ftruncate(int fd, long long len) {
    HANDLE h = win_fd_get_handle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    FILE_END_OF_FILE_INFO ei = {.EndOfFile.QuadPart = len};
    return SetFileInformationByHandle(h, FileEndOfFileInfo, &ei, sizeof ei) ? 0 : win_fail(GetLastError());
}

int omc_win_fsync(int fd) {
    HANDLE h = win_fd_get_handle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (GetFileType(h) != FILE_TYPE_DISK) return 0;
    if (!FlushFileBuffers(h)) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) return 0; /* a read-only handle (directories): nothing of ours to flush */
        return win_fail(e);
    }
    return 0;
}

/* ---------- temp names ---------- */

static void fill_random(char *x, size_t n) {
    static const char cs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned char r[64];
    omc_win_getrandom(r, n < sizeof r ? n : sizeof r, 0);
    for (size_t i = 0; i < n; i++) x[i] = cs[r[i % sizeof r] % (sizeof cs - 1)];
}

static char *template_x(char *tmpl, int suffixlen) {
    size_t n = strlen(tmpl);
    if (n < (size_t)suffixlen + 6) return NULL;
    char *x = tmpl + n - suffixlen - 6;
    for (int i = 0; i < 6; i++)
        if (x[i] != 'X') return NULL;
    return x;
}

char *omc_win_mkdtemp(char *tmpl) {
    char *x = template_x(tmpl, 0);
    if (!x) {
        errno = EINVAL;
        return NULL;
    }
    for (int tries = 0; tries < 100; tries++) {
        fill_random(x, 6);
        if (omc_win_mkdir(tmpl, 0700) == 0) return tmpl;
        if (errno != EEXIST) return NULL;
    }
    errno = EEXIST;
    return NULL;
}

int omc_win_mkstemps(char *tmpl, int suffixlen) {
    char *x = template_x(tmpl, suffixlen);
    if (!x) {
        errno = EINVAL;
        return -1;
    }
    for (int tries = 0; tries < 100; tries++) {
        fill_random(x, 6);
        int fd = omc_win_open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

int omc_win_mkstemp(char *tmpl) { return omc_win_mkstemps(tmpl, 0); }

int omc_win_mkfifo(const char *path, int mode) {
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

/* ---------- locks: one byte far past any file's end, so they stay advisory (Windows byte locks block I/O) ---------- */

#define LOCK_OFF_LO 0xFFFFFFF0u
#define LOCK_OFF_HI 0x7FFFFFFFu

static int lock_handle(HANDLE h, bool exclusive, bool wait) {
    OVERLAPPED ov = {0};
    ov.Offset = LOCK_OFF_LO;
    ov.OffsetHigh = LOCK_OFF_HI;
    DWORD f = (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
    if (LockFileEx(h, f, 0, 1, 0, &ov)) return 0;
    DWORD e = GetLastError();
    errno = (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) ? EWOULDBLOCK : win_errno_from(e);
    return -1;
}

static int unlock_handle(HANDLE h) {
    OVERLAPPED ov = {0};
    ov.Offset = LOCK_OFF_LO;
    ov.OffsetHigh = LOCK_OFF_HI;
    if (UnlockFileEx(h, 0, 1, 0, &ov)) return 0;
    return GetLastError() == ERROR_NOT_LOCKED ? 0 : win_fail(GetLastError());
}

int omc_win_flock(int fd, int op) {
    HANDLE h = win_fd_get_handle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (op & LOCK_UN) return unlock_handle(h);
    /* flock converts an existing lock: drop ours first (Windows would stack them) */
    unlock_handle(h);
    return lock_handle(h, (op & LOCK_EX) != 0, !(op & LOCK_NB));
}

int omc_win_fcntl_lock(int fd, int cmd, struct omc_win_flock *fl) {
    HANDLE h = win_fd_get_handle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (cmd == F_GETLK) {
        if (lock_handle(h, fl->l_type == F_WRLCK, false) == 0) {
            unlock_handle(h);
            fl->l_type = F_UNLCK;
        } else {
            fl->l_type = F_WRLCK;
            fl->l_pid = 0;
        }
        return 0;
    }
    if (fl->l_type == F_UNLCK) return unlock_handle(h);
    unlock_handle(h);
    int r = lock_handle(h, fl->l_type == F_WRLCK, cmd == F_SETLKW);
    if (r != 0 && errno == EWOULDBLOCK) errno = EAGAIN;
    return r;
}

/* ---------- mmap (read-only views) ---------- */

void *omc_win_mmap(void *addr, size_t len, int prot, int flags, int fd, long long off) {
    (void)addr;
    (void)flags;
    if (prot & PROT_WRITE) {
        errno = ENOTSUP;
        return MAP_FAILED;
    }
    HANDLE h = win_fd_get_handle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return MAP_FAILED;
    }
    HANDLE m = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m) {
        errno = win_errno_from(GetLastError());
        return MAP_FAILED;
    }
    void *p = MapViewOfFile(m, FILE_MAP_READ, (DWORD)((unsigned long long)off >> 32), (DWORD)off, len);
    DWORD e = GetLastError();
    CloseHandle(m); /* the view keeps the mapping alive */
    if (!p) {
        errno = win_errno_from(e);
        return MAP_FAILED;
    }
    return p;
}

int omc_win_munmap(void *addr, size_t len) {
    (void)len;
    return UnmapViewOfFile(addr) ? 0 : win_fail(GetLastError());
}

/* ---------- glob ---------- */

static bool wild_match(const char *p, const char *s) {
    for (;; p++, s++) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return true;
            for (; *s; s++)
                if (wild_match(p, s)) return true;
            return false;
        }
        if (!*s) return !*p;
        if (*p == '?') continue;
        if (*p == '[') {
            const char *q = p + 1;
            bool neg = *q == '!' || *q == '^';
            if (neg) q++;
            bool hit = false;
            bool first = true;
            for (; *q && (first || *q != ']'); q++, first = false) {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if ((unsigned char)*s >= (unsigned char)q[0] && (unsigned char)*s <= (unsigned char)q[2]) hit = true;
                    q += 2;
                } else if (tolower((unsigned char)*q) == tolower((unsigned char)*s)) {
                    hit = true;
                }
            }
            if (*q != ']' || hit == neg) return false;
            p = q;
            continue;
        }
        if (*p == '\\' && p[1]) p++;
        if (tolower((unsigned char)*p) != tolower((unsigned char)*s)) return false; /* NTFS names fold case */
    }
}

static bool has_wild(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] == '*' || s[i] == '?' || s[i] == '[') return true;
    return false;
}

typedef struct {
    char **v;
    size_t n, cap;
} gl_vec_t;

static void gl_push(gl_vec_t *g, char *s) {
    if (g->n + 1 >= g->cap) {
        g->cap = g->cap ? g->cap * 2 : 16;
        g->v = realloc(g->v, g->cap * sizeof *g->v);
    }
    g->v[g->n++] = s;
}

/* expand `rest` (components separated by '/') under `base` (which ends with a separator or is "") */
static void gl_expand(const char *base, const char *rest, int flags, gl_vec_t *out) {
    while (*rest == '/') rest++;
    if (!*rest) {
        struct omc_win_stat st;
        if (omc_win_stat(*base ? base : ".", &st) == 0) gl_push(out, strdup(base));
        return;
    }
    size_t cl = strcspn(rest, "/");
    if (!has_wild(rest, cl)) {
        size_t bl = strlen(base);
        char *nb = malloc(bl + cl + 2);
        memcpy(nb, base, bl);
        memcpy(nb + bl, rest, cl);
        nb[bl + cl] = 0;
        if (rest[cl]) {
            strcat(nb, "/");
            gl_expand(nb, rest + cl, flags, out);
            free(nb);
        } else {
            struct omc_win_stat st;
            if (omc_win_lstat(nb, &st) == 0) gl_push(out, nb);
            else free(nb);
        }
        return;
    }
    char *comp = strndup(rest, cl);
    omc_win_DIR *d = omc_win_opendir(*base ? base : ".");
    if (d) {
        struct omc_win_dirent *e;
        while ((e = omc_win_readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (e->d_name[0] == '.' && comp[0] != '.') continue;
            if (!wild_match(comp, e->d_name)) continue;
            size_t bl = strlen(base), nl = strlen(e->d_name);
            char *nb = malloc(bl + nl + 2);
            memcpy(nb, base, bl);
            memcpy(nb + bl, e->d_name, nl + 1);
            if (rest[cl]) {
                strcat(nb, "/");
                gl_expand(nb, rest + cl, flags, out);
                free(nb);
            } else {
                gl_push(out, nb);
            }
        }
        omc_win_closedir(d);
    }
    free(comp);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int omc_win_glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g) {
    (void)errfunc;
    memset(g, 0, sizeof *g);
    char *pat = strdup(pattern);
    win_slashes(pat);
    if ((flags & GLOB_TILDE) && pat[0] == '~' && (pat[1] == '/' || !pat[1])) {
        const char *home = getenv("HOME");
        if (!home || !*home) home = getenv("USERPROFILE");
        if (home) {
            size_t hl = strlen(home), pl = strlen(pat);
            char *np = malloc(hl + pl);
            memcpy(np, home, hl);
            memcpy(np + hl, pat + 1, pl);
            free(pat);
            pat = np;
            win_slashes(pat);
        }
    }
    gl_vec_t out = {0};
    /* the root part: "C:/", "//srv/share/", "/" */
    const char *rest = pat;
    char root[16] = "";
    if (isalpha((unsigned char)pat[0]) && pat[1] == ':') {
        root[0] = pat[0];
        root[1] = ':';
        root[2] = '/';
        root[3] = 0;
        rest = pat + 2;
    } else if (pat[0] == '/') {
        strcpy(root, "/");
    }
    gl_expand(root, rest, flags, &out);
    if (!out.n) {
        if (flags & GLOB_NOCHECK) {
            gl_push(&out, strdup(pattern));
        } else {
            free(pat);
            free(out.v);
            return GLOB_NOMATCH;
        }
    } else if (!(flags & GLOB_NOSORT)) {
        qsort(out.v, out.n, sizeof *out.v, cmp_str);
    }
    gl_push(&out, NULL);
    out.n--;
    g->gl_pathc = out.n;
    g->gl_pathv = out.v;
    free(pat);
    return 0;
}

void omc_win_globfree(glob_t *g) {
    for (size_t i = 0; i < g->gl_pathc; i++) free(g->gl_pathv[i]);
    free(g->gl_pathv);
    memset(g, 0, sizeof *g);
}
