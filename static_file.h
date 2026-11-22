#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include <stddef.h>
#include <sys/stat.h>

int static_file_open(int root_fd, const char *relative,
                     struct stat *info, char *resolved, size_t capacity);

#endif
