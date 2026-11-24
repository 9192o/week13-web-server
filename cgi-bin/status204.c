#include <stdio.h>

int main(void) {
    printf("Status: 204 No Content\r\n");
    printf("Content-Type: text/plain\r\n\r\n");
    printf("body\n");
    return 0;
}
