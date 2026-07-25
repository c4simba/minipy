/* KolibriOS threading backend.
 *   fn 51.1: create thread   -> ecx = entry EIP, edx = stack top; eax = TID
 *   fn 68.1: yield the rest of this thread's time slice
 *   fn -1  : terminate the calling thread
 * A KolibriOS thread starts at a bare address with no argument, so entry/arg
 * are handed off through a single lock-guarded slot: the spawner fills it, the
 * new thread copies it out and flags that the slot is free to reuse.
 */

#include "platform/platform.h"

#define KOS_TSTACK (64*1024)

static mpy_lock kos_spawn_lock;
static int kos_spawn_lock_ready = 0;
static void (* volatile kos_pending_entry)(void *);
static void * volatile kos_pending_arg;
static volatile int kos_handoff_taken;

static void kos_thread_start(void){
    void (*entry)(void *) = kos_pending_entry;
    void *arg = kos_pending_arg;
    __atomic_store_n(&kos_handoff_taken, 1, __ATOMIC_RELEASE);   /* slot copied: spawner may proceed */
    entry(arg);
    __asm__ __volatile__("int $0x40" :: "a"(-1) : "memory");     /* terminate this thread */
}

int mpy_thread_spawn(void (*entry)(void *), void *arg){
    if(!kos_spawn_lock_ready){ mpy_lock_init(&kos_spawn_lock); kos_spawn_lock_ready = 1; }
    char *stack = (char*)malloc(KOS_TSTACK);
    if(!stack) return -1;

    mpy_lock_acquire(&kos_spawn_lock);          /* only one handoff in flight at a time */
    kos_pending_entry = entry;
    kos_pending_arg = arg;
    __atomic_store_n(&kos_handoff_taken, 0, __ATOMIC_RELEASE);

    int tid;
    __asm__ __volatile__("int $0x40"
        : "=a"(tid)
        : "a"(51), "b"(1), "c"(kos_thread_start), "d"(stack + KOS_TSTACK)
        : "memory");
    if(tid < 0){ mpy_lock_release(&kos_spawn_lock); free(stack); return -1; }

    while(!__atomic_load_n(&kos_handoff_taken, __ATOMIC_ACQUIRE)) mpy_thread_yield();
    mpy_lock_release(&kos_spawn_lock);
    return 0;   /* NB: `stack` is intentionally leaked until join lands in phase 2 */
}

void mpy_thread_yield(void){
    __asm__ __volatile__("int $0x40" :: "a"(68), "b"(1) : "memory");   /* fn 68.1 */
}

void mpy_thread_sleep_ms(int ms){
    int cs = ms/10; if(cs < 1) cs = 1;                                  /* fn 5: delay in 1/100 s */
    __asm__ __volatile__("int $0x40" :: "a"(5), "b"(cs) : "memory");
}
