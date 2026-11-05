/* main_twice.c - a host that calls omc_main twice in one process (tests/e2e/main_twice_tui.py), as an embedder that
 * opens the agent UI more than once does. `--sigpipe-check` instead checks that a SIGPIPE handler the host installed
 * survives the library. */
#include "omc.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static void host_handler(int sig) { (void)sig; }

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--sigpipe-check")) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = host_handler;
        sigaction(SIGPIPE, &sa, NULL);
        char *args[] = {argv[0], "--version", NULL};
        int rc = omc_main(2, args);
        struct sigaction now;
        sigaction(SIGPIPE, NULL, &now);
        puts(rc == 0 && now.sa_handler == host_handler ? "host handler kept" : "host handler replaced");
        return 0;
    }
    int first = omc_main(argc, argv);
    int second = omc_main(argc, argv);
    fprintf(stderr, "\nmain twice rc %d %d\n", first, second);
    return first || second;
}
