/* 웹 애플리케이션 */

#include "app.h"
#include "http.h"
#include "webserver.h"

#include <stdio.h>

int main(void)
{
    return webserver_run();
}

const char *app_handle_data(const char *data, size_t data_length)
{
    printf("Received %zu bytes:\n", data_length);
    fwrite(data, 1, data_length, stdout);
    putchar('\n');
    fflush(stdout);

    return HTTP_OK_RESPONSE;
}
