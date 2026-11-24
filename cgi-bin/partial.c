#include <stdio.h>

int main(void) {
    printf("Content-Type: text/plain\r\n\r\npartial");
    fflush(stdout);
    return 1;
}
