#include <stdio.h>
#include <unistd.h>

int main(void) {
    sleep(2);
    printf("Content-Type: text/plain\r\n\r\nslept\n");
    return 0;
}
