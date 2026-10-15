# queue in compiled programs: generic Queue[T], LifoQueue, PriorityQueue, SimpleQueue with threads
import queue
import threading

q: queue.Queue[int] = queue.Queue()
out: list[int] = []


def producer() -> None:
    for i in range(10):
        q.put(i)
    q.put(-1)


def consumer() -> None:
    while True:
        x = q.get()
        if x < 0:
            q.task_done()
            break
        out.append(x * x)
        q.task_done()


p = threading.Thread(target=producer)
c = threading.Thread(target=consumer)
p.start()
c.start()
q.join()
p.join()
c.join()
print(out, q.empty(), q.qsize())
pq: queue.PriorityQueue[tuple[int, str]] = queue.PriorityQueue()
for x in [(5, "e"), (1, "a"), (4, "d"), (2, "b")]:
    pq.put(x)
print([pq.get() for _ in range(4)])
lq: queue.LifoQueue[str] = queue.LifoQueue()
lq.put("x")
lq.put("y")
print(lq.get(), lq.get())
try:
    queue.Queue[str]().get_nowait()
except queue.Empty:
    print("Empty")
bq: queue.Queue[int] = queue.Queue(1)
bq.put(1)
try:
    bq.put(2, timeout=0.01)
except queue.Full:
    print("Full")
sq: queue.SimpleQueue[float] = queue.SimpleQueue()
sq.put(1.5)
print(sq.get(), sq.empty())
q2 = queue.Queue()
q2.put("a")
q2.put("b")
print(q2.get(), q2.qsize())
w = queue.Queue(maxsize=2)
w.put(1.5)
print(w.full(), w.get())
