#include "ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { LOCK, REQUEST_READY, REPLY_READY, SEM_COUNT };
typedef union { Request request; Reply reply; } Message;
typedef struct {
    Message message;
    sem_t sem[SEM_COUNT];
} Mailbox;

struct Ipc {
    enum Mode mode;
    int owner, created_data;
    char names[SEM_COUNT + 1][80];
    Mailbox *box;
    mqd_t queue;
    sem_t *sem[SEM_COUNT];
};

void ipc_close(Ipc *ipc)
{
    if (!ipc) return;
    for (int i = 0; i < SEM_COUNT; ++i) {
        if (!ipc->sem[i]) continue;
        if (ipc->mode == FORK_UNNAMED) {
            if (ipc->owner) sem_destroy(ipc->sem[i]);
        } else {
            if (ipc->owner) sem_unlink(ipc->names[i]);
            sem_close(ipc->sem[i]);
        }
    }
    if (ipc->queue != (mqd_t)-1) mq_close(ipc->queue);
    if (ipc->box) munmap(ipc->box, sizeof(*ipc->box));
    if (ipc->created_data) {
        if (ipc->mode == INDEPENDENT_MQ) mq_unlink(ipc->names[SEM_COUNT]);
        else shm_unlink(ipc->names[SEM_COUNT]);
    }
    free(ipc);
}

static int open_data(Ipc *ipc, int flags)
{
    const char *name = ipc->names[SEM_COUNT];
    if (ipc->mode == INDEPENDENT_MQ) {
        struct mq_attr attr = {.mq_maxmsg = 1, .mq_msgsize = sizeof(Message)};
        ipc->queue = mq_open(name, O_RDWR | O_NONBLOCK | flags, 0600, &attr);
        if (ipc->queue == (mqd_t)-1) return -1;
        ipc->created_data = ipc->owner;
        return 0;
    }
    int fd = shm_open(name, O_RDWR | flags, 0600);
    if (fd < 0) return -1;
    ipc->created_data = ipc->owner;
    int rc = 0;
    if (ipc->owner) {
        rc = ftruncate(fd, sizeof(Mailbox));
    } else {
        struct stat st;
        rc = fstat(fd, &st);
        if (!rc && st.st_size != (off_t)sizeof(Mailbox)) { errno = EAGAIN; rc = -1; }
    }
    if (!rc) {
        void *address = mmap(NULL, sizeof(Mailbox), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (address == MAP_FAILED) rc = -1;
        else ipc->box = address;
    }
    int saved = errno;
    if (close(fd) < 0 && !rc) return -1;
    errno = saved;
    return rc;
}

Ipc *ipc_open(enum Mode mode, int owner)
{
    const char *session = getenv("HSE_GARDEN");
    if (!session) session = "default";
    const char *allowed = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-";
    size_t n = strlen(session);
    if (!n || n > 32 || strspn(session, allowed) != n) { errno = EINVAL; return NULL; }

    Ipc *ipc = calloc(1, sizeof(*ipc));
    if (!ipc) return NULL;
    ipc->mode = mode;
    ipc->owner = owner;
    ipc->queue = (mqd_t)-1;
    const char *suffix[] = {"lock", "request", "reply", "data"};
    for (int i = 0; i <= SEM_COUNT; ++i)
        snprintf(ipc->names[i], sizeof(ipc->names[i]), "/hse14_%lu_%d_%s_%s",
                 (unsigned long)getuid(), (int)mode, session, suffix[i]);
    int flags = owner ? O_CREAT | O_EXCL : 0;
    if (open_data(ipc, flags) < 0) goto fail;
    for (int i = 0; i < SEM_COUNT; ++i) {
        if (mode == FORK_UNNAMED) {
            sem_t *sem = &ipc->box->sem[i];
            if (owner && sem_init(sem, 1, i == LOCK) < 0) goto fail;
            ipc->sem[i] = sem;
        } else {
            sem_t *sem = sem_open(ipc->names[i], flags, 0600, (unsigned)(i == LOCK));
            if (sem == SEM_FAILED) goto fail;
            ipc->sem[i] = sem;
        }
    }
    return ipc;
fail: {
        int error = errno;
        ipc_close(ipc);
        errno = error;
        return NULL;
    }
}

static int wait_for(sem_t *sem, unsigned attempts)
{
    for (unsigned i = 0; i < attempts && !interrupted; ++i) {
        struct timespec until;
        if (clock_gettime(CLOCK_REALTIME, &until) < 0) return -1;
        until.tv_nsec += 10000000L;
        if (until.tv_nsec >= 1000000000L) { ++until.tv_sec; until.tv_nsec -= 1000000000L; }
        if (sem_timedwait(sem, &until) == 0) return 1;
        if (errno != ETIMEDOUT && errno != EINTR) return -1;
    }
    errno = interrupted ? EINTR : ETIMEDOUT;
    return interrupted ? -1 : 0;
}

static int send_message(Ipc *ipc, const Message *msg, int ready)
{
    if (ipc->mode == INDEPENDENT_MQ) {
        if (mq_send(ipc->queue, (const char *)msg, sizeof(*msg), 0) < 0) return -1;
    } else {
        ipc->box->message = *msg;
    }
    return sem_post(ipc->sem[ready]);
}

static int read_message(Ipc *ipc, Message *msg)
{
    if (ipc->mode != INDEPENDENT_MQ) {
        *msg = ipc->box->message;
        return 0;
    }
    ssize_t n = mq_receive(ipc->queue, (char *)msg, sizeof(*msg), NULL);
    if (n < 0) return -1;
    if ((size_t)n != sizeof(*msg)) { errno = EPROTO; return -1; }
    return 0;
}

int ipc_call(Ipc *ipc, const Request *request, Reply *reply)
{
    Message msg = {.request = *request};
    if (wait_for(ipc->sem[LOCK], 500) != 1) return -1;
    /* Одна пара запрос–ответ: соседний процесс не заберёт наш ответ. */
    if (send_message(ipc, &msg, REQUEST_READY) < 0 ||
        wait_for(ipc->sem[REPLY_READY], 500) != 1 || read_message(ipc, &msg) < 0)
        return -1;
    *reply = msg.reply;
    return sem_post(ipc->sem[LOCK]);
}

int ipc_receive(Ipc *ipc, Request *request)
{
    int ready = wait_for(ipc->sem[REQUEST_READY], 1);
    if (ready != 1) return ready;
    Message msg;
    if (read_message(ipc, &msg) < 0) return -1;
    *request = msg.request;
    return 1;
}

int ipc_reply(Ipc *ipc, const Reply *reply)
{
    Message msg = {.reply = *reply};
    return send_message(ipc, &msg, REPLY_READY);
}
