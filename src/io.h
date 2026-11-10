#ifndef IO_H
#define IO_H
#include "config.h"
#include <stddef.h>

typedef struct { int fd; } Output;
int io_write_all(int fd, const char *text, size_t size);
int io_text(int fd, const char *text);
int io_error(const char *context);
int output_open(Output *output, const Config *config);
int output_event(void *context, uint64_t time, const char *event,
                 size_t flower, size_t gardener, uint64_t request, const char *detail);
int output_summary(Output *output, const char *summary);
int output_close(Output *output);
#endif
