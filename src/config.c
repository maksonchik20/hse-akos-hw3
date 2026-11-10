#include "config.h"
#include "io.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *name, *value;
    uint64_t *target, minimum, maximum;
} Option;

static int invalid(const char *name)
{
    io_text(2, "Ошибка параметра: ");
    io_text(2, name);
    io_text(2, ". Проверьте значение и диапазон в --help.\n");
    return -1;
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) text[--n] = '\0';
    return text;
}

static int find_option(Option *options, size_t count, const char *name)
{
    for (size_t i = 0; i < count; ++i)
        if (strcmp(options[i].name, name) == 0) return (int)i;
    return invalid(name);
}

static int load_config(Config *c, Option *options, size_t count)
{
    int fd;
    do { fd = open(c->config_path, O_RDONLY); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return io_error("открытие конфигурации");
    size_t used = 0;
    int result = 0;
    while (used < sizeof(c->text) - 1) {
        ssize_t n = read(fd, c->text + used, sizeof(c->text) - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { result = io_error("чтение конфигурации"); break; }
        if (!n) break;
        if (memchr(c->text + used, 0, (size_t)n)) { result = invalid("config: нулевой байт"); break; }
        used += (size_t)n;
    }
    if (used == sizeof(c->text) - 1) result = invalid("config: файл слишком большой");
    if (close(fd) < 0) result = io_error("закрытие конфигурации");
    if (result < 0) return -1;
    c->text[used] = '\0';
    char *line = c->text;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *key = trim(line);
        if (*key) {
            char *value = strchr(key, '=');
            if (!value) return invalid("config: ожидается key=value");
            *value++ = '\0';
            int index = find_option(options, count, trim(key));
            if (index < 0) return -1;
            options[index].value = trim(value);
        }
        line = next;
    }
    return 0;
}

static int number(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *value)
{
    char *end;
    errno = 0;
    if (!isdigit((unsigned char)*text)) return -1;
    unsigned long long n = strtoull(text, &end, 10);
    if (errno || *end || n < minimum || n > maximum) return -1;
    *value = (uint64_t)n;
    return 0;
}

static int list(const char *text, uint64_t count, uint64_t *values)
{
    size_t n = 0;
    for (;;) {
        while (isspace((unsigned char)*text)) ++text;
        if (n >= (count ? count : 1) || !isdigit((unsigned char)*text)) return -1;
        char *end;
        errno = 0;
        unsigned long long value = strtoull(text, &end, 10);
        if (errno || value < 1 || value > 1000000) return -1;
        values[n++] = (uint64_t)value;
        while (isspace((unsigned char)*end)) ++end;
        if (!*end) break;
        if (*end != ',') return -1;
        text = end + 1;
    }
    if (n != 1 && n != count) return -1;
    if (n == 1)
        for (size_t i = 1; i < count; ++i) values[i] = values[0];
    return 0;
}

int config_parse(Config *c, int argc, char **argv)
{
    memset(c, 0, sizeof(*c));
    Option options[] = {
        {"flowers", "3", &c->flowers, 0, MAX_ENTITIES},
        {"gardeners", "2", &c->gardeners, 0, MAX_ENTITIES},
        {"request-threshold", "10", &c->request_threshold, 1, 1000000},
        {"death-threshold", "30", &c->death_threshold, 1, 1000000},
        {"water-duration", "2", &c->water_duration, 1, 1000000},
        {"recovery-duration", "3", &c->recovery_duration, 1, 1000000},
        {"protection-interval", "5", &c->protection_interval, 0, 1000000},
        {"duration", "40", &c->duration, 0, 1000000},
        {"delay-ms", "0", &c->delay_ms, 0, 60000},
        {"unlimited", "false", NULL, 0, 0},
        {"dry-rates", "1", NULL, 0, 0},
        {"speeds", "1", NULL, 0, 0},
        {"log", "events.log", NULL, 0, 0}
    };
    const size_t count = sizeof(options) / sizeof(options[0]);
    const char *cli[sizeof(options) / sizeof(options[0])] = {0};
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            if (io_text(1, "Умные цветы. Процессы POSIX, стратегия FIFO.\n"
                "Параметры: --имя значение. Файл: --config FILE.\n"
                "flowers/gardeners: 0..1000 (3/2); dry-rates/speeds: список 1..1000000 (1).\n"
                "request-threshold/death-threshold: 1..1000000 (10/30), первый меньше второго.\n"
                "water-duration/recovery-duration: 1..1000000 (2/3).\n"
                "protection-interval/duration: 0..1000000 (5/40); delay-ms: 0..60000 (0).\n"
                "delay-ms задаёт длительность такта; 0 означает 10 мс.\n"
                "unlimited: true|false (false); log: путь (events.log, перезапись).\n"
                "Список: одно число для всех или по одному на участника через запятую.\n"
                "Конфигурация: key=value. Приоритет: defaults < config < CLI. Ctrl+C сохраняет итог.\n") < 0) return -1;
            return 1;
        }
        if (strncmp(argv[i], "--", 2) || i + 1 == argc) return invalid(argv[i]);
        const char *name = argv[i] + 2, *value = argv[++i];
        if (strcmp(name, "config") == 0) {
            if (c->config_path || !*value) return invalid(name);
            c->config_path = value;
        } else {
            int index = find_option(options, count, name);
            if (index < 0) return -1;
            cli[index] = value;
        }
    }
    if (c->config_path && load_config(c, options, count) < 0) return -1;
    for (size_t i = 0; i < count; ++i) {
        Option *option = &options[i];
        if (cli[i]) option->value = cli[i];
        if (option->target) {
            if (number(option->value, option->minimum, option->maximum, option->target) < 0)
                return invalid(option->name);
        } else if (strcmp(option->name, "unlimited") == 0) {
            if (strcmp(option->value, "true") && strcmp(option->value, "false")) return invalid(option->name);
            c->unlimited = strcmp(option->value, "true") == 0;
        } else if (strcmp(option->name, "dry-rates") == 0) {
            if (list(option->value, c->flowers, c->dry_rates) < 0) return invalid(option->name);
        } else if (strcmp(option->name, "speeds") == 0) {
            if (list(option->value, c->gardeners, c->speeds) < 0) return invalid(option->name);
        } else {
            if (!*option->value) return invalid(option->name);
            c->log_path = option->value;
        }
    }
    return c->request_threshold < c->death_threshold ? 0 : invalid("пороги заявки и засыхания");
}
