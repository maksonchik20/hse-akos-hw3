#ifndef IPC_H
#define IPC_H

#include "model.h"
#include <signal.h>

enum Mode { FORK_NAMED, FORK_UNNAMED, INDEPENDENT_SHM, INDEPENDENT_MQ };
typedef struct Ipc Ipc;
extern volatile sig_atomic_t interrupted;

Ipc *ipc_open(enum Mode mode, int owner);
int ipc_call(Ipc *ipc, const Request *request, Reply *reply);
/* 1 — запрос принят, 0 — за время ожидания ничего не пришло, -1 — ошибка. */
int ipc_receive(Ipc *ipc, Request *request);
int ipc_reply(Ipc *ipc, const Reply *reply);
void ipc_close(Ipc *ipc);

#endif
