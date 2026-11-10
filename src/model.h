#ifndef MODEL_H
#define MODEL_H
#include "config.h"
#include "io.h"
#include <sys/types.h>

enum Action { JOIN_FLOWER, JOIN_GARDENER, FLOWER_TICK, FLOWER_REQUEST, FLOWER_DRY, FLOWER_RECOVER,
              GARDENER_CHOOSE, GARDENER_CLAIM, GARDENER_FINISH };
enum State { NORMAL, WILTING, WATERING, RECOVERING, DRIED, DEAD_OVERWATER };
typedef struct { enum Action action; unsigned id; uint64_t request; pid_t pid; } Request;
typedef struct {
    uint64_t request, wait_ms, age, wilt_after, dry_after;
    enum State state;
    int inactive;
    pid_t coordinator;
} Reply;

typedef struct Model Model;
enum Action flower_action(const Reply *view);
Model *model_create(const Config *config, Output *output);
void model_destroy(Model *model);
int model_handle(Model *model, const Request *request, uint64_t tick, Reply *reply);
int model_all_inactive(const Model *model);
int model_summary(Model *model, uint64_t tick, const char *reason);
#endif
