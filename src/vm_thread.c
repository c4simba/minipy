/* ========================= Threading: the `thread` module =========================
   Real OS threads (pthreads / KolibriOS fn 51) under a GIL. Each thread gets its
   own VM execution state (see vm.h: `vm` = the GIL holder's state); the object
   heap, builtins and modules are shared. A thread runs one Python callable to
   completion, then unregisters and exits.

   All entry points here run under the GIL (they are natives called from
   bytecode). Where a native must block (sleep, lock, join) it releases the GIL
   first so other threads can run, and restores `mpy_cur_vm` on return. */

#include "vm.h"
#include "gc.h"
#include "containers.h"

#define MPY_MAX_PY_THREADS 64
#define MPY_MAX_LOCKS      64

typedef struct { VM tvm; volatile int done; } PyThread;

static PyThread *py_threads[MPY_MAX_PY_THREADS];   /* handle -> thread */
static mpy_lock  user_locks[MPY_MAX_LOCKS];
static int       lock_used[MPY_MAX_LOCKS];

/* Run the GIL release/reacquire dance around a blocking C operation, keeping
   mpy_cur_vm pinned to the caller across the window. */
#define WITH_GIL_RELEASED(body) do{ VM *self=mpy_cur_vm; mpy_lock_release(&mpy_gil); body; mpy_lock_acquire(&mpy_gil); mpy_cur_vm=self; }while(0)

/* OS-thread entry: acquire the GIL, adopt our own VM, run the callable. */
static void py_thread_entry(void *p){
    PyThread *pt = (PyThread*)p;
    mpy_lock_acquire(&mpy_gil);
    VM *self = &pt->tvm;
    mpy_cur_vm = self;
    int stack_anchor; gc_set_stack_base(&stack_anchor);   /* this thread's C-stack base */

    Value callable = self->stack[0];
    Value args     = self->stack[1];
    self->sp = 0;                                         /* bootstrap roots consumed */

    if(setjmp(self->panic) == 0){                        /* per-thread top-level catch */
        if(is_obj(args, O_TUPLE)){ List *l=&args.as.obj->as.tuple; call_value(callable, l->count, l->items); }
        else call_value(callable, 0, NULL);
    } else {
        print_traceback(is_obj(self->pending_exception,O_EXCEPTION) ? self->pending_exception
                        : exceptionv("RuntimeError", self->error_msg?self->error_msg:"error", nonev()));
    }

    pt->done = 1;
    mpy_vm_thread_unregister(self);
    mpy_lock_release(&mpy_gil);                           /* never touch VM state after this */
}

/* thread.start(callable [, args_tuple]) -> handle */
static Value native_thread_start(int argc, Value *argv){
    if(argc < 1 || argc > 2) runtime_error("thread.start(callable[, args]) expects 1 or 2 arguments");
    Value callable = argv[0];
    Value args     = (argc == 2) ? argv[1] : nonev();
    if(argc == 2 && !is_obj(args, O_TUPLE)) runtime_error("thread.start args must be a tuple");

    int h = -1; for(int i=0;i<MPY_MAX_PY_THREADS;i++) if(!py_threads[i]){ h=i; break; }
    if(h < 0) runtime_error("too many threads");

    PyThread *pt = (PyThread*)malloc(sizeof(PyThread));
    if(!pt) runtime_error("out of memory");
    memset(pt, 0, sizeof(PyThread));
    pt->tvm.builtins = vm.builtins;                      /* share heap namespaces */
    pt->tvm.modules  = vm.modules;
    pt->tvm.stack[0] = callable;                         /* keep them alive across the handoff */
    pt->tvm.stack[1] = args;
    pt->tvm.sp = 2;
    py_threads[h] = pt;
    mpy_vm_thread_register(&pt->tvm);                    /* GC now roots callable/args */

    if(mpy_thread_spawn(py_thread_entry, pt) != 0){
        mpy_vm_thread_unregister(&pt->tvm); py_threads[h]=NULL; free(pt);
        runtime_error("could not start thread");
    }
    return intv(h);
}

/* thread.join(handle) -> None : block until that thread finishes. */
static Value native_thread_join(int argc, Value *argv){
    if(argc != 1) runtime_error("thread.join(handle) expects 1 argument");
    int h = (int)as_int(argv[0]);
    if(h < 0 || h >= MPY_MAX_PY_THREADS || !py_threads[h]) runtime_error("invalid thread handle");
    PyThread *pt = py_threads[h];
    while(!pt->done) WITH_GIL_RELEASED( mpy_thread_sleep_ms(1) );
    py_threads[h] = NULL;
    free(pt);                                            /* child no longer touches it */
    return nonev();
}

/* thread.sleep(ms) -> None : sleep without holding the GIL. */
static Value native_thread_sleep(int argc, Value *argv){
    if(argc != 1) runtime_error("thread.sleep(ms) expects 1 argument");
    int ms = (int)as_int(argv[0]);
    WITH_GIL_RELEASED( mpy_thread_sleep_ms(ms) );
    return nonev();
}

/* thread.lock() -> handle */
static Value native_thread_lock(int argc, Value *argv){
    (void)argv; if(argc != 0) runtime_error("thread.lock() takes no arguments");
    int h = -1; for(int i=0;i<MPY_MAX_LOCKS;i++) if(!lock_used[i]){ h=i; break; }
    if(h < 0) runtime_error("too many locks");
    mpy_lock_init(&user_locks[h]); lock_used[h] = 1;
    return intv(h);
}

/* thread.acquire(lock) -> None : block on the lock with the GIL released. */
static Value native_thread_acquire(int argc, Value *argv){
    if(argc != 1) runtime_error("thread.acquire(lock) expects 1 argument");
    int h = (int)as_int(argv[0]);
    if(h < 0 || h >= MPY_MAX_LOCKS || !lock_used[h]) runtime_error("invalid lock handle");
    WITH_GIL_RELEASED( mpy_lock_acquire(&user_locks[h]) );
    return nonev();
}

/* thread.release(lock) -> None */
static Value native_thread_release(int argc, Value *argv){
    if(argc != 1) runtime_error("thread.release(lock) expects 1 argument");
    int h = (int)as_int(argv[0]);
    if(h < 0 || h >= MPY_MAX_LOCKS || !lock_used[h]) runtime_error("invalid lock handle");
    mpy_lock_release(&user_locks[h]);
    return nonev();
}

Native N_THREAD_START  = {"start",  -1, native_thread_start};
Native N_THREAD_JOIN   = {"join",    1, native_thread_join};
Native N_THREAD_SLEEP  = {"sleep",   1, native_thread_sleep};
Native N_THREAD_LOCK   = {"lock",    0, native_thread_lock};
Native N_THREAD_ACQUIRE= {"acquire", 1, native_thread_acquire};
Native N_THREAD_RELEASE= {"release", 1, native_thread_release};
