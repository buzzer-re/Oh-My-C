/* fake_omc.c - the "new release" binary of the Windows `omc update` check (tests/windows/run-tests.ps1, packed by
 * tests/e2e/mock_release.py): reports omc/<FAKE_VERSION> for --version, like the sh script the Linux check uses. */
#include <stdio.h>
#include <string.h>

#ifndef FAKE_VERSION
#define FAKE_VERSION "99.0.0"
#endif

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        puts("omc/" FAKE_VERSION);
        return 0;
    }
    puts("fake omc " FAKE_VERSION);
    return 0;
}
