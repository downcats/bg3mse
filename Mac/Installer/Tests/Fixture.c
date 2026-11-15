// A stand-in for the game's executable in the installer's tests: a universal
// (arm64 and x86_64) program, signed ad hoc as the game is.

#include <stdio.h>

int main(void)
{
    printf("fixture build 1\n");
    return 0;
}
