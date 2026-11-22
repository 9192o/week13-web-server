#include <stdio.h>
#include <time.h>

int main(void) {
    time_t now = time(NULL);

    printf("Content-Type: text/plain\r\n\r\n");
    printf("time: %ld\n", (long)now);
    return 0;
}
