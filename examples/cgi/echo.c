#include <stdio.h>

int main(void)
{
    fputs("Content-Type: application/octet-stream\r\n\r\n", stdout);
    char buffer[4096];
    size_t length;
    while ((length = fread(buffer, 1, sizeof buffer, stdin)) != 0)
        if (fwrite(buffer, 1, length, stdout) != length)
            return 1;
    return ferror(stdin) || fflush(stdout) != 0;
}
