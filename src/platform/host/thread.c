/* Host threading backend: POSIX threads. */

#include "platform/platform.h"
#include <pthread.h>
#include <sched.h>
#include <time.h>

typedef struct { void (*entry)(void *); void *arg; } mpy_thunk;

static void *mpy_trampoline(void *p){
    mpy_thunk t = *(mpy_thunk*)p;
    free(p);
    t.entry(t.arg);
    return NULL;
}

int mpy_thread_spawn(void (*entry)(void *), void *arg){
    mpy_thunk *t = (mpy_thunk*)malloc(sizeof *t);
    if(!t) return -1;
    t->entry = entry; t->arg = arg;
    pthread_t th;
    if(pthread_create(&th, NULL, mpy_trampoline, t) != 0){ free(t); return -1; }
    pthread_detach(th);
    return 0;
}

void mpy_thread_yield(void){ sched_yield(); }

void mpy_thread_sleep_ms(int ms){
    if(ms < 0) ms = 0;
    struct timespec ts = { ms/1000, (long)(ms%1000) * 1000000L };
    nanosleep(&ts, NULL);
}
