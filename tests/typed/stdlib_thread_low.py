import _thread
import time
lock = _thread.allocate_lock()
done: list[int] = []
def work(n: int, tag: str) -> None:
    with lock:
        done.append(n)
for i in range(3):
    _thread.start_new_thread(work, (i, "t"))
while len(done) < 3:
    time.sleep(0.01)
print(sorted(done), lock.locked(), _thread.get_ident() == _thread.get_ident(), _thread.error is RuntimeError)
