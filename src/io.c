#include "io.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int io_write_all(int fd, const char *text, size_t size)
{
    while (size) {
        ssize_t n = write(fd, text, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        text += (size_t)n;
        size -= (size_t)n;
    }
    return 0;
}
int io_text(int fd, const char *text) { return io_write_all(fd, text, strlen(text)); }

int io_error(const char *context)
{
    int saved = errno;
    char message[512];
    int n = snprintf(message, sizeof(message), "Ошибка: %s: %s\n", context, strerror(saved));
    if (n > 0 && (size_t)n < sizeof(message)) io_text(2, message);
    errno = saved;
    return -1;
}

int output_open(Output *out, const Config *config)
{
    struct stat log, input;
    do { out->fd = open(config->log_path, O_WRONLY | O_CREAT | O_NONBLOCK, 0644); }
    while (out->fd < 0 && errno == EINTR);
    if (out->fd < 0 || fstat(out->fd, &log) < 0) return io_error("открытие журнала");
    if (!S_ISREG(log.st_mode)) { errno = EINVAL; return io_error("журнал должен быть обычным файлом"); }
    if (config->config_path) {
        if (stat(config->config_path, &input) < 0) return io_error("проверка конфигурации");
        if (log.st_dev == input.st_dev && log.st_ino == input.st_ino) {
            errno = EINVAL;
            return io_error("журнал совпадает с конфигурацией");
        }
    }
    return ftruncate(out->fd, 0) < 0 ? io_error("очистка журнала") : 0;
}

int output_event(void *context, uint64_t time, const char *event,
                 size_t flower, size_t gardener, uint64_t request, const char *detail)
{
    Output *out = context;
    char line[1024];
    int n = snprintf(line, sizeof(line),
        "t=%" PRIu64 " event=%s flower=%zu gardener=%zu request=%" PRIu64 " detail=%s\n",
        time, event, flower, gardener, request, detail);
    if (n < 0 || (size_t)n >= sizeof(line)) { errno = EOVERFLOW; return -1; }
    if (io_write_all(out->fd, line, (size_t)n) < 0) return -1;
    return io_write_all(1, line, (size_t)n);
}

int output_summary(Output *out, const char *summary)
{
    int result = 0;
    if (io_text(out->fd, summary) < 0) result = io_error("сохранение итогов");
    if (io_text(1, summary) < 0) result = io_error("вывод итогов");
    return result;
}

int output_close(Output *out)
{
    int result = out->fd < 0 ? 0 : close(out->fd);
    out->fd = -1;
    return result < 0 ? io_error("закрытие журнала") : 0;
}

