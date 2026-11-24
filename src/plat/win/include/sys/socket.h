/* sys/socket.h for Windows: Winsock behind CRT fds. A socket fd is a CRT fd that fd.c maps to its SOCKET, so the
 * POSIX calls below (and read/write/close/poll/fcntl) take it as they would on Linux. */
#ifndef OMC_WIN_SYS_SOCKET_H
#define OMC_WIN_SYS_SOCKET_H
#pragma push_macro("gethostname")
#undef gethostname
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#pragma pop_macro("gethostname")
#include <poll.h>

#define SOCK_CLOEXEC 0x10000000
#define SOCK_NONBLOCK 0x20000000
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#endif
#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0
#endif

#define getaddrinfo omc_win_getaddrinfo
int omc_win_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
#define socket omc_win_socket
#define connect omc_win_connect
#define bind omc_win_bind
#define listen omc_win_listen
#define accept omc_win_accept
#define accept4 omc_win_accept4
#define send omc_win_send
#define recv omc_win_recv
#define sendto omc_win_sendto
#define recvfrom omc_win_recvfrom
#define setsockopt omc_win_setsockopt
#define getsockopt omc_win_getsockopt
#define shutdown omc_win_shutdown
#define getsockname omc_win_getsockname
#define getpeername omc_win_getpeername
int omc_win_socket(int domain, int type, int proto);
int omc_win_connect(int fd, const struct sockaddr *sa, socklen_t len);
int omc_win_bind(int fd, const struct sockaddr *sa, socklen_t len);
int omc_win_listen(int fd, int backlog);
int omc_win_accept(int fd, struct sockaddr *sa, socklen_t *len);
int omc_win_accept4(int fd, struct sockaddr *sa, socklen_t *len, int flags);
ssize_t omc_win_send(int fd, const void *buf, size_t n, int flags);
ssize_t omc_win_recv(int fd, void *buf, size_t n, int flags);
ssize_t omc_win_sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *sa, socklen_t len);
ssize_t omc_win_recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *sa, socklen_t *len);
int omc_win_setsockopt(int fd, int level, int opt, const void *val, socklen_t len);
int omc_win_getsockopt(int fd, int level, int opt, void *val, socklen_t *len);
int omc_win_shutdown(int fd, int how);
int omc_win_getsockname(int fd, struct sockaddr *sa, socklen_t *len);
int omc_win_getpeername(int fd, struct sockaddr *sa, socklen_t *len);
#endif
