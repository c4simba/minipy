# threading (Thread, locks, Condition, Event, Semaphore, Barrier, Timer, local), queue, atexit
import threading, queue, time, atexit
results = []
lock = threading.Lock()
def worker(n):
    for i in range(3):
        with lock:
            results.append((n, i))
        time.sleep(0.001)
ts = [threading.Thread(target=worker, args=(k,)) for k in range(4)]
print([t.name for t in ts], ts[0].is_alive(), repr(ts[0]).startswith("<Thread(Thread-"))
for t in ts: t.start()
for t in ts: t.join()
print(sorted(results) == sorted((n, i) for n in range(4) for i in range(3)), len(results), all(not t.is_alive() for t in ts))
q = queue.Queue()
def producer():
    for i in range(10):
        q.put(i)
    q.put(None)
out = []
def consumer():
    while True:
        x = q.get()
        if x is None:
            q.task_done(); break
        out.append(x * x)
        q.task_done()
p = threading.Thread(target=producer); c = threading.Thread(target=consumer)
p.start(); c.start(); q.join(); p.join(); c.join()
print(out, q.empty(), q.qsize())
pq = queue.PriorityQueue()
for x in [5, 1, 4, 2]: pq.put(x)
print([pq.get() for _ in range(4)])
lq = queue.LifoQueue(); lq.put(1); lq.put(2); print(lq.get(), lq.get())
try:
    queue.Queue().get_nowait()
except queue.Empty:
    print("Empty")
bq = queue.Queue(1); bq.put(1)
try:
    bq.put(2, timeout=0.01)
except queue.Full:
    print("Full")
ev = threading.Event()
def setter():
    time.sleep(0.01); ev.set()
threading.Thread(target=setter).start()
print(ev.wait(2), ev.is_set(), ev.wait(0))
cond = threading.Condition()
items = []
def cons():
    with cond:
        cond.wait_for(lambda: items)
        items.append("got")
tc = threading.Thread(target=cons); tc.start()
time.sleep(0.01)
with cond:
    items.append("x"); cond.notify()
tc.join(); print(items)
sem = threading.Semaphore(2); print(sem.acquire(), sem.acquire(), sem.acquire(False)); sem.release(); print(sem.acquire(timeout=0.01))
bs = threading.BoundedSemaphore(1)
try:
    bs.release()
except ValueError as e:
    print("ValueError", e)
r = threading.RLock()
with r:
    with r:
        print("reentrant", r._is_owned())
try:
    r.release()
except RuntimeError as e:
    print("RuntimeError", e)
print(threading.current_thread().name, threading.main_thread() is threading.current_thread(), threading.active_count())
loc = threading.local(); loc.x = 1
seen = []
def lw():
    seen.append(hasattr(loc, "x")); loc.x = 2; seen.append(loc.x)
t = threading.Thread(target=lw); t.start(); t.join(); print(seen, loc.x)
b = threading.Barrier(3)
order = []
def bw(i):
    order.append(b.wait() >= 0)
bt = [threading.Thread(target=bw, args=(i,)) for i in range(3)]
for t in bt: t.start()
for t in bt: t.join()
print(order)
tm = threading.Timer(0.01, lambda: print("timer fired")); tm.start(); tm.join()
tm2 = threading.Timer(5, lambda: print("never")); tm2.start(); tm2.cancel(); tm2.join()
def bad():
    raise ValueError("boom in thread")
caught = []
threading.excepthook = lambda args: caught.append((args.exc_type.__name__, str(args.exc_value), args.thread.name))
tb = threading.Thread(target=bad, name="Bad"); tb.start(); tb.join()
print(caught)
threading.excepthook = threading.__excepthook__
try:
    tb.start()
except RuntimeError as e:
    print("RuntimeError", e)
try:
    threading.Thread().join()
except RuntimeError as e:
    print("RuntimeError", e)
lk = threading.Lock(); print(lk.acquire(), lk.acquire(False), lk.acquire(timeout=0.01), lk.locked()); lk.release()
try:
    lk.release()
except RuntimeError as e:
    print("RuntimeError", e)
atexit.register(print, "atexit ran", 1)
def late():
    time.sleep(0.02); print("non-daemon thread finished")
threading.Thread(target=late).start()
print("main done")
