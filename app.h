#ifndef APP_H
#define APP_H

#include "http.h"

/* All slices are borrowed and valid only for the duration of this call. */
const char *app_handle_request(const struct http_request *request);

#endif
