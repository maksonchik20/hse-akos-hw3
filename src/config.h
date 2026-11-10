#ifndef CONFIG_H
#define CONFIG_H
#include <stdint.h>
#define MAX_ENTITIES 1000

typedef struct {
    uint64_t flowers, gardeners;
    uint64_t dry_rates[MAX_ENTITIES], speeds[MAX_ENTITIES];
    uint64_t request_threshold, death_threshold, water_duration;
    uint64_t recovery_duration, protection_interval, duration, delay_ms;
    uint64_t unlimited;
    const char *log_path, *config_path;
    char text[65536];
} Config;

int config_parse(Config *config, int argc, char **argv);
#endif
