/*
 * steamlink-ps5 - pthread backend for ihslib's thread abstraction.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ihslib ships an SDL2 backend; the PS5 has no SDL. Mutexes are recursive to
 * match SDL's semantics, which ihslib relies on. Threads get an explicit stack:
 * a title's default thread stack is not what a desktop library expects.
 */

#include "ihs_thread.h"

#include <pthread.h>
#include <stdlib.h>

#define IHS_PS5_THREAD_STACK_SIZE (512 * 1024)

struct IHS_Thread {
    pthread_t tid;
    IHS_ThreadFunction *function;
    void *context;
};

struct IHS_Mutex {
    pthread_mutex_t mutex;
};

struct IHS_Cond {
    pthread_cond_t cond;
};

static void *ThreadEntry(void *arg) {
    IHS_Thread *thread = arg;
    thread->function(thread->context);
    return NULL;
}

IHS_Thread *IHS_ThreadCreate(IHS_ThreadFunction *function, const char *name, void *context) {
    (void) name;
    IHS_Thread *thread = calloc(1, sizeof(IHS_Thread));
    if (thread == NULL) {
        return NULL;
    }
    thread->function = function;
    thread->context = context;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, IHS_PS5_THREAD_STACK_SIZE);
    int result = pthread_create(&thread->tid, &attr, ThreadEntry, thread);
    pthread_attr_destroy(&attr);
    if (result != 0) {
        free(thread);
        return NULL;
    }
    return thread;
}

void IHS_ThreadJoin(IHS_Thread *thread) {
    if (thread == NULL) {
        return;
    }
    pthread_join(thread->tid, NULL);
    free(thread);
}

IHS_Mutex *IHS_MutexCreate() {
    IHS_Mutex *mutex = calloc(1, sizeof(IHS_Mutex));
    if (mutex == NULL) {
        return NULL;
    }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    return mutex;
}

void IHS_MutexDestroy(IHS_Mutex *mutex) {
    if (mutex == NULL) {
        return;
    }
    pthread_mutex_destroy(&mutex->mutex);
    free(mutex);
}

bool IHS_MutexLock(IHS_Mutex *mutex) {
    return pthread_mutex_lock(&mutex->mutex) == 0;
}

bool IHS_MutexUnlock(IHS_Mutex *mutex) {
    return pthread_mutex_unlock(&mutex->mutex) == 0;
}

IHS_Cond *IHS_CondCreate() {
    IHS_Cond *cond = calloc(1, sizeof(IHS_Cond));
    if (cond == NULL) {
        return NULL;
    }
    pthread_cond_init(&cond->cond, NULL);
    return cond;
}

void IHS_CondDestroy(IHS_Cond *cond) {
    if (cond == NULL) {
        return;
    }
    pthread_cond_destroy(&cond->cond);
    free(cond);
}

void IHS_CondSignal(IHS_Cond *cond) {
    pthread_cond_signal(&cond->cond);
}

bool IHS_CondWait(IHS_Cond *cond, IHS_Mutex *mutex) {
    return pthread_cond_wait(&cond->cond, &mutex->mutex) == 0;
}
