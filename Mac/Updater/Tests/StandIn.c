// The game, for the updater's tests: copied into a bundle as
// "Baldur's Gate 3" and started with the loader inserted, it says when its
// main() begins, then waits (when asked) for a file to appear, as a game
// would go on running while the update is fetched.
//
//   Baldur's Gate 3 [wait <path> <seconds>]

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    printf("main\n");
    fflush(stdout);
    if (argc == 4 && strcmp(argv[1], "wait") == 0) {
        int tenths = atoi(argv[3]) * 10;
        struct stat st;
        for (int i = 0; i < tenths && stat(argv[2], &st) != 0; i++) usleep(100000);
        printf("%s\n", stat(argv[2], &st) == 0 ? "appeared" : "not there");
        fflush(stdout);
    }
    return 0;
}
