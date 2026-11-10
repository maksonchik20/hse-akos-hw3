#include "model.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    enum State state;
    unsigned gardener;
    uint64_t request, due, last_water, normal_since;
} Flower;

struct Model {
    const Config *config;
    Output *output;
    Flower flowers[MAX_ENTITIES + 1];
    unsigned watering[MAX_ENTITIES + 1]; // Номер цветка; 0 — садовник свободен.
    uint64_t tick, created, completed, cancelled, waterings, conflicts, alive;
};

static int event(Model *m, const char *code, unsigned flower, unsigned gardener,
                 uint64_t request, const char *detail)
{
    return output_event(m->output, m->tick, code, flower, gardener, request, detail);
}

static int state_event(Model *m, unsigned id)
{
    static const char *const names[] = {
        "NORMAL; Нормальное развитие", "WILTING; Цветок увядает", "WATERING; Цветок поливается",
        "RECOVERING; Цветок восстанавливается", "DRIED; Цветок засох", "DEAD_OVERWATER; Цветок погиб от перелива"
    };
    Flower *f = &m->flowers[id];
    return event(m, "STATE", id, f->gardener, f->request, names[f->state]);
}

Model *model_create(const Config *config, Output *output)
{
    Model *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->config = config;
    m->output = output;
    m->alive = config->flowers;
    for (unsigned id = 1; id <= config->flowers; ++id) {
        if (state_event(m, id) < 0) { free(m); return NULL; }
    }
    return m;
}

enum Action flower_action(const Reply *view)
{
    if (!view->dry_after) return FLOWER_TICK;
    if ((view->state == NORMAL || view->state == WILTING) && view->age >= view->dry_after) return FLOWER_DRY;
    if (view->state == NORMAL && view->age >= view->wilt_after) return FLOWER_REQUEST;
    if (view->state == RECOVERING && !view->wait_ms) return FLOWER_RECOVER;
    return FLOWER_TICK;
}

static int update_flower(Model *m, unsigned id, enum Action action)
{
    Flower *f = &m->flowers[id];
    if (action == FLOWER_RECOVER && f->state == RECOVERING && m->tick >= f->due) {
        f->state = NORMAL;
        f->normal_since = m->tick;
        f->due = 0;
        if (state_event(m, id) < 0) return -1;
        return event(m, "RECOVERY_END", id, 0, 0, "Цветок восстановился");
    }
    if (action == FLOWER_DRY && (f->state == NORMAL || f->state == WILTING)) {
        uint64_t request = f->request;
        f->state = DRIED;
        f->request = 0;
        --m->alive;
        if (request) ++m->cancelled;
        if (state_event(m, id) < 0) return -1;
        if (request && event(m, "CANCEL", id, 0, request, "Заявка снята после засыхания") < 0) return -1;
        return event(m, "DRIED", id, 0, request, "Цветок прекратил активность");
    }
    if (action == FLOWER_REQUEST && f->state == NORMAL) {
        f->state = WILTING;
        f->request = ++m->created;
        if (state_event(m, id) < 0) return -1;
        return event(m, "REQUEST", id, 0, f->request, "Цветок отправил новую заявку");
    }
    return 0;
}

static int finish_watering(Model *m, unsigned id, unsigned gardener)
{
    Flower *f = &m->flowers[id];
    uint64_t request = f->request;
    f->state = RECOVERING;
    f->due = m->tick + m->config->recovery_duration;
    f->request = 0;
    f->gardener = m->watering[gardener] = 0;
    ++m->completed;
    if (event(m, "WATER_END", id, gardener, request, "Садовник закончил полив") < 0 ||
        event(m, "COMPLETE", id, gardener, request, "Выполненная заявка снята") < 0 ||
        state_event(m, id) < 0) return -1;
    return event(m, "RECOVERY_START", id, 0, 0, "Началось восстановление");
}

static int choose(Model *m, unsigned gardener, Reply *reply)
{
    if (m->watering[gardener]) return 0;
    unsigned oldest = 0;
    for (unsigned id = 1; id <= m->config->flowers; ++id) {
        const Flower *f = &m->flowers[id];
        if (f->state == WILTING && (!oldest || f->request < m->flowers[oldest].request)) oldest = id;
    }
    if (!oldest) return 0;
    reply->request = m->flowers[oldest].request;
    return event(m, "CHOICE", oldest, gardener, reply->request, "Выбрана самая старая заявка FIFO");
}

static int claim(Model *m, unsigned gardener, uint64_t request)
{
    if (!request) return 0;
    unsigned id = 1;
    while (id <= m->config->flowers && m->flowers[id].request != request) ++id;
    if (id > m->config->flowers) return 0; // Пока садовник отвечал, заявку могли снять.
    Flower *f = &m->flowers[id];
    if (f->state == WATERING) {
        char detail[96];
        snprintf(detail, sizeof(detail), "winner=%u loser=%u; Заявка уже занята", f->gardener, gardener);
        ++m->conflicts;
        return event(m, "CONFLICT", id, gardener, request, detail);
    }
    if (event(m, "ACCEPT", id, gardener, request, "Садовник принял заявку") < 0) return -1;
    if (f->last_water && m->tick - f->last_water < m->config->protection_interval) {
        f->state = DEAD_OVERWATER;
        f->request = 0;
        ++m->cancelled;
        --m->alive;
        if (event(m, "OVERWATER", id, gardener, request, "Ранний повторный полив: цветок погиб") < 0 ||
            state_event(m, id) < 0) return -1;
        return event(m, "CANCEL", id, gardener, request, "Заявка снята после перелива");
    }
    uint64_t speed = m->config->speeds[gardener - 1];
    f->state = WATERING;
    f->gardener = gardener;
    m->watering[gardener] = id;
    f->last_water = m->tick;
    f->due = m->tick + (m->config->water_duration + speed - 1) / speed;
    ++m->waterings;
    if (state_event(m, id) < 0) return -1;
    return event(m, "WATER_START", id, gardener, request, "Начался полив");
}

int model_handle(Model *m, const Request *r, uint64_t tick, Reply *reply)
{
    uint64_t tick_ms = m->config->delay_ms ? m->config->delay_ms : 10;
    m->tick = tick;
    *reply = (Reply){0};
    switch (r->action) {
    case FLOWER_TICK: case FLOWER_REQUEST: case FLOWER_DRY: case FLOWER_RECOVER: {
        Flower *f = &m->flowers[r->id];
        if (update_flower(m, r->id, r->action) < 0) return -1;
        uint64_t rate = m->config->dry_rates[r->id - 1];
        reply->state = f->state;
        reply->age = tick - f->normal_since;
        reply->wilt_after = (m->config->request_threshold + rate - 1) / rate;
        reply->dry_after = (m->config->death_threshold + rate - 1) / rate;
        reply->inactive = f->state == DRIED || f->state == DEAD_OVERWATER;
        reply->wait_ms = tick_ms;
        if (f->state == RECOVERING) reply->wait_ms = (f->due > tick ? f->due - tick : 0) * tick_ms;
        return 0;
    }
    case GARDENER_CHOOSE: return choose(m, r->id, reply);
    case GARDENER_CLAIM:
        if (m->watering[r->id]) return 0;
        if (claim(m, r->id, r->request) < 0) return -1;
        if (m->watering[r->id]) reply->wait_ms = (m->flowers[m->watering[r->id]].due - tick) * tick_ms;
        return 0;
    case GARDENER_FINISH: {
        unsigned flower = m->watering[r->id];
        if (!flower) return 0;
        if (tick >= m->flowers[flower].due) return finish_watering(m, flower, r->id);
        reply->wait_ms = (m->flowers[flower].due - tick) * tick_ms;
        return 0;
    }
    default: errno = EINVAL; return -1;
    }
}

int model_all_inactive(const Model *m) { return m->alive == 0; }

int model_summary(Model *m, uint64_t tick, const char *reason)
{
    unsigned pending = 0, assigned = 0, dried = 0, overwater = 0;
    int ok = 1;
    for (unsigned id = 1; id <= m->config->flowers; ++id) {
        const Flower *f = &m->flowers[id];
        pending += f->state == WILTING;
        assigned += f->state == WATERING;
        dried += f->state == DRIED;
        overwater += f->state == DEAD_OVERWATER;
        if ((f->state == WILTING || f->state == WATERING) != (f->request != 0) ||
            (f->state == WATERING) != (f->gardener != 0) ||
            (f->gardener && m->watering[f->gardener] != id)) ok = 0;
    }
    for (unsigned id = 1; id <= m->config->gardeners; ++id)
        if (m->watering[id] && m->flowers[m->watering[id]].gardener != id) ok = 0;
    if (m->created != m->completed + m->cancelled + pending + assigned ||
        m->waterings != m->completed + assigned || m->alive + dried + overwater != m->config->flowers) ok = 0;
    char text[512];
    snprintf(text, sizeof(text),
        "SUMMARY reason=%s time=%" PRIu64 " created=%" PRIu64 " completed=%" PRIu64
        " cancelled=%" PRIu64 " pending=%u assigned=%u waterings=%" PRIu64
        " conflicts=%" PRIu64 " overwater=%u dried=%u alive=%" PRIu64 " invariants=%s\n",
        reason, tick, m->created, m->completed, m->cancelled, pending, assigned, m->waterings,
        m->conflicts, overwater, dried, m->alive, ok ? "ok" : "FAILED");
    if (output_summary(m->output, text) < 0) return -1;
    if (!ok) { errno = EINVAL; return -1; }
    return 0;
}

void model_destroy(Model *m) { free(m); }
