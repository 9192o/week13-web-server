#ifndef STATIC_FILE_INTERNAL_H
#define STATIC_FILE_INTERNAL_H

#include "static_file.h"

/* Internal helper, exposed for unit tests. */
int send_file(int connfd, int filefd, size_t size);

#endif
