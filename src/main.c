/* main.c - executable entry point; everything else lives in the library (see include/omc.h). */
#include "../include/omc.h"

int main(int argc, char **argv) {
#ifdef _WIN32
    omc_win_init_exe(); /* plat/win/posix.h: the console and stdio of the omc process */
#endif
    return omc_main(argc, argv);
}
