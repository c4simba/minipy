# threading in compiled programs: cooperative threads (Thread, Timer, locks, Event, Condition, Semaphore); waiting for non-daemon threads at the end
import threading
import time

log: list[str] = []
lock = threading.Lock()


def worker(n: int, tag: str) -> None:
    for i in range(3):
        with lock:
            log.append(tag + str(n) + ":" + str(i))
        time.sleep(0.001)


class Counter(threading.Thread):
    def __init__(self, k: int) -> None:
        super().__init__()
        self.k = k
        self.total = 0

    def run(self) -> None:
        for i in range(self.k):
            self.total += i


ts = [threading.Thread(target=worker, args=(k, "w")) for k in range(3)]
for t in ts:
    t.start()
for t in ts:
    t.join()
print(len(log), sorted(log)[:3], all(not t.is_alive() for t in ts))
c = Counter(10)
c.start()
c.join()
print(c.total, c.name, threading.current_thread().name, threading.main_thread().name)
ev = threading.Event()


def setter() -> None:
    time.sleep(0.01)
    ev.set()


threading.Thread(target=setter).start()
print(ev.wait(2), ev.is_set())
sem = threading.Semaphore(2)
print(sem.acquire(), sem.acquire(), sem.acquire(False))
r = threading.RLock()
with r:
    with r:
        print("reentrant")
cond = threading.Condition()
items: list[int] = []


def consumer() -> None:
    with cond:
        cond.wait_for(lambda: len(items) > 0)
        items.append(99)


tc = threading.Thread(target=consumer)
tc.start()
time.sleep(0.01)
with cond:
    items.append(1)
    cond.notify()
tc.join()
print(items)
tm = threading.Timer(0.01, worker, args=(7, "t"))
tm.start()
tm.join()
print(log[-1])


def late() -> None:
    time.sleep(0.02)
    print("late thread finished")


threading.Thread(target=late).start()
print("main done")
