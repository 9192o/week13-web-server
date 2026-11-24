#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    signal(SIGTERM, SIG_IGN);
    sleep(5);
    printf("Content-Type: text/plain\r\n\r\nlate\n");
    return 0;
}
