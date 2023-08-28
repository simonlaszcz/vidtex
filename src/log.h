#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <string.h>
#include <errno.h>

#define log_err() fprintf(stderr, \
    "Error: %s (%d)\n\tat %s line %d\n", strerror(errno), errno, __FILE__, __LINE__)

#endif
