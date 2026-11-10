#include "ipc.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MODE
#error Select the IPC implementation in the Makefile
#endif

enum Role { COORDINATOR, FLOWER, GARDENER };
volatile sig_atomic_t interrupted;
static void on_signal(int sig) { interrupted = sig; }

/* Сигнал остановки не теряется между проверкой флага и засыпанием. */
static void rest(uint64_t ms)
{
    sigset_t mask, old;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &old);
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    if (!interrupted) pselect(0, NULL, NULL, NULL, &t, &old);
    sigprocmask(SIG_SETMASK, &old, NULL);
}

static int worker(Ipc *ipc, int flower, unsigned id)
{
    Request q = {.action = flower ? JOIN_FLOWER : JOIN_GARDENER, .id = id, .pid = getpid()};
    Reply r = {0};
    sigset_t mask, old;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &old);
    int joined = ipc_call(ipc, &q, &r);
    pid_t server = r.coordinator;
    sigprocmask(SIG_SETMASK, &old, NULL);
    if (joined == 0 && !server) {
        io_text(2, "Участник с таким номером не ожидается или уже запущен.\n");
        return 2;
    }
    q.action = flower ? FLOWER_TICK : GARDENER_CHOOSE;
    int result = joined;
    while (!interrupted && result == 0) {
        result = ipc_call(ipc, &q, &r);
        if (result < 0) break;
        if (flower) {
            q.action = flower_action(&r);
            if (q.action != FLOWER_TICK) {
                result = ipc_call(ipc, &q, &r);
                if (result < 0) break;
            }
            q.action = FLOWER_TICK;
        }
        if (r.inactive) {
            while (!interrupted) rest(60000);
            break;
        }
        if (!flower) {
            if (q.action == GARDENER_CHOOSE && r.request) {
                q.request = r.request;
                q.action = GARDENER_CLAIM;
            } else if ((q.action == GARDENER_CLAIM || q.action == GARDENER_FINISH) && r.wait_ms) {
                q.action = GARDENER_FINISH;
            } else {
                q.action = GARDENER_CHOOSE;
                q.request = 0;
            }
        }
        rest(r.wait_ms ? r.wait_ms : 1);
    }
    if (interrupted == SIGTERM) return 0;  /* Координатор закончил сеанс. */
    if (result < 0 && !interrupted) io_error("обмен с координатором");
    if (server > 0) kill(server, interrupted == SIGINT ? SIGINT : SIGTERM);
    return interrupted == SIGINT ? 130 : 1;
}

static int64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int serve(Ipc *ipc, Model *model, const Config *c, pid_t *members, const char **reason, uint64_t *tick)
{
    size_t joined = 0;
    int64_t start = 0;
    while (!interrupted) {
        if (model_all_inactive(model)) { *reason = "all_inactive"; return 0; }
        if (!c->unlimited && *tick >= c->duration) { *reason = "time_limit"; return 0; }
        Request q;
        int got = ipc_receive(ipc, &q);
        if (got < 0) {
            if (interrupted) break;
            return io_error("получение команды");
        }
        if (start) {
            int64_t now = now_ms();
            if (now < 0) return io_error("часы");
            *tick = (uint64_t)(now - start) / (c->delay_ms ? c->delay_ms : 10);
            if (!c->unlimited && *tick >= c->duration) {
                *tick = c->duration; *reason = "time_limit"; return 0;
            }
        }
        if (!got) continue;
        int flower = q.action == JOIN_FLOWER || q.action == FLOWER_TICK || q.action == FLOWER_REQUEST ||
                     q.action == FLOWER_DRY || q.action == FLOWER_RECOVER;
        int joining = q.action == JOIN_FLOWER || q.action == JOIN_GARDENER;
        Reply r = {0};
        if (q.pid > 1 && q.id && q.id <= (flower ? c->flowers : c->gardeners)) {
            size_t slot = (flower ? 0 : c->flowers) + q.id - 1;
            if (joining && !members[slot]) {
                members[slot] = q.pid;
                r.coordinator = getpid();
                if (++joined == c->flowers + c->gardeners) {
                    start = now_ms();
                    if (start < 0) return io_error("часы");
                }
            } else if (!joining && members[slot] == q.pid && start) {
                if (model_handle(model, &q, *tick, &r) < 0) return io_error("обработка команды");
            }
        }
        if (ipc_reply(ipc, &r) < 0) return io_error("ответ участнику");
    }
    *reason = interrupted == SIGINT ? "interrupted" : "error";
    return interrupted == SIGINT ? 130 : 1;
}

static int coordinator(const Config *c)
{
    Ipc *ipc = ipc_open(MODE, 1);
    if (!ipc) { io_error("создание IPC"); return 1; }
    Output out = {-1};
    Model *model = NULL;
    const char *reason = "error";
    uint64_t tick = 0;
    int code = 1;
    size_t count = (size_t)(c->flowers + c->gardeners), spawned = 0;
    pid_t members[2 * MAX_ENTITIES] = {0}, children[2 * MAX_ENTITIES];
    if (output_open(&out, c) < 0) goto cleanup;
    model = model_create(c, &out);
    if (!model) { io_error("создание модели"); goto cleanup; }
    if (MODE == FORK_NAMED || MODE == FORK_UNNAMED) {
        while (!interrupted && c->flowers && (c->unlimited || c->duration) && spawned < count) {
            int flower = spawned < c->flowers;
            unsigned id = (unsigned)(flower ? spawned + 1 : spawned - c->flowers + 1);
            pid_t pid = fork();
            if (pid < 0) { io_error("fork"); goto finish; }
            if (!pid) {
                close(out.fd);
                _exit(worker(ipc, flower, id));
            }
            children[spawned++] = pid;
        }
    } else if (io_text(1, "READY\n") < 0) { io_error("объявление готовности"); goto finish; }
    code = serve(ipc, model, c, members, &reason, &tick);
    if (code < 0) code = 1;
finish:
    if (MODE == FORK_NAMED || MODE == FORK_UNNAMED) {
        for (size_t i = 0; i < spawned; ++i) kill(children[i], SIGTERM);
        for (size_t i = 0; i < spawned; ++i) {
            pid_t waited;
            do { waited = waitpid(children[i], NULL, 0); } while (waited < 0 && errno == EINTR);
            if (waited < 0) { io_error("waitpid"); reason = "error"; code = 1; }
        }
    } else {
        for (size_t i = 0; i < count; ++i)
            if (members[i]) kill(members[i], SIGTERM);
    }
    if (model_summary(model, tick, reason) < 0) code = 1;
cleanup:
    model_destroy(model);
    if (output_close(&out) < 0) code = 1;
    ipc_close(ipc);
    return code;
}

int main(int argc, char **argv)
{
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = on_signal;
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return 1;
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL)) return 1;
    if (ROLE != COORDINATOR) {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            io_text(1, "Запустите участника с его номером (от 1). Сначала запустите server.\n");
            return 0;
        }
        if (argc != 2 || argv[1][0] < '0' || argv[1][0] > '9') {
            io_text(2, "Ожидается один аргумент: номер участника.\n"); return 2;
        }
        char *end;
        unsigned long id = strtoul(argv[1], &end, 10);
        if (*end || !id || id > MAX_ENTITIES) { io_text(2, "Неверный номер участника\n"); return 2; }
        Ipc *ipc = ipc_open(MODE, 0);
        if (!ipc) { io_error("подключение к координатору"); return 1; }
        int code = worker(ipc, ROLE == FLOWER, (unsigned)id);
        ipc_close(ipc);
        return code;
    }
    Config cfg;
    int parsed = config_parse(&cfg, argc, argv);
    return parsed ? (parsed > 0 ? 0 : 2) : coordinator(&cfg);
}
