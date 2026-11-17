#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>
#include <unistd.h>
int main(int argc,char **argv) {
    if (argc != 2 || argv[1][0] != 'x') return 9;
    for (int i=0;i<12;++i) {
        struct timespec t={0,100000000}; nanosleep(&t,0);
        puts("HOSTILE read(0) = 1 <99999.9>");
        fputs("HOSTILE write(1) = 1 <99999.9>\n",stderr);
    }
    return 0;
}
