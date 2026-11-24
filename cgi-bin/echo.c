#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const char *method = getenv("REQUEST_METHOD");
    const char *query = getenv("QUERY_STRING");
    const char *content_type = getenv("CONTENT_TYPE");
    const char *content_length = getenv("CONTENT_LENGTH");
    const char *script_name = getenv("SCRIPT_NAME");
    const char *protocol = getenv("SERVER_PROTOCOL");
    const char *remote_addr = getenv("REMOTE_ADDR");
    const char *path = getenv("PATH");
    const char *home = getenv("HOME");

    printf("Content-Type: text/plain\r\n\r\n");
    printf("METHOD=%s\n", method ? method : "");
    printf("QUERY=%s\n", query ? query : "");
    printf("CONTENT_TYPE=%s\n", content_type ? content_type : "");
    printf("CONTENT_LENGTH=%s\n", content_length ? content_length : "");
    printf("SCRIPT_NAME=%s\n", script_name ? script_name : "");
    printf("SERVER_PROTOCOL=%s\n", protocol ? protocol : "");
    printf("REMOTE_ADDR=%s\n", remote_addr ? remote_addr : "");
    printf("PATH=%s\n", path ? path : "");
    printf("HOME=[%s]\n", home ? home : "");
    printf("BODY=");

    int c;
    while ((c = getchar()) != EOF)
        putchar(c);

    printf("\n");
    return 0;
}
