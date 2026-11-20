/* 웹 애플리케이션 */

#include "app.h"
#include "http.h"
#include "webserver.h"

#include <stdio.h>

int main(void)
{
    return webserver_run();
}

const char *app_handle_request(const struct http_request *request)
{
    printf("Parsed request: headers=%zu body=%zu bytes\n",
           request->headers.count, request->body.length);
    fflush(stdout);

    return HTTP_OK_RESPONSE;
}
