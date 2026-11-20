#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

_Static_assert(__STDC_VERSION__ >= 201710L, "C17 is required");

int main(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 1;
    }

    unsigned char *bytes = malloc(256);
    if (bytes == NULL) {
        return 2;
    }

    uint32_t sum = 0;
    for (size_t i = 0; i < 256; ++i) {
        bytes[i] = (unsigned char)i;
        sum += bytes[i];
    }
    free(bytes);

    return sum == 32640 ? 0 : 3;
}
