# concurrent.futures: ThreadPoolExecutor (submit with arguments of any types, map, shutdown), Future
# (result, exception, callbacks, cancel, states), wait, as_completed
import concurrent.futures
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed, wait, FIRST_COMPLETED, ALL_COMPLETED


def noaddr(s: str) -> str:
    i = s.find(" at 0x")
    if i < 0:
        return s
    j = i + 6
    while j < len(s) and s[j] in "0123456789abcdef":
        j += 1
    return s[:i] + " at 0x..." + s[j:]


def square(x: int) -> int:
    time.sleep(0.01 * (5 - x))
    return x * x


def fail(msg: str) -> int:
    raise ValueError(msg)


def describe(name: str, n: int, sep: str) -> str:
    return sep.join([name] * n)


log: list[str] = []


def note(word: str) -> None:
    log.append(word)


with ThreadPoolExecutor(max_workers=3) as ex:
    futs = [ex.submit(square, i) for i in range(5)]
    print([f.result() for f in futs])
    print(sorted(f.result() for f in as_completed(futs)))
    print(list(ex.map(square, [1, 2, 3])))
    print(list(ex.map(describe, ["a", "b"], [2, 3], ["-", "+"])))
    print(ex.submit(describe, "x", 3, "/").result())
    bad = ex.submit(fail, "boom")
    try:
        bad.result()
    except ValueError as e:
        print("ValueError:", e, bad.done(), bad.cancelled(), type(bad.exception()).__name__)
    done, not_done = wait(futs)
    print(len(done), len(not_done))
    r = wait(futs, timeout=1, return_when=ALL_COMPLETED)
    print(len(r.done), len(r.not_done))
    p = ex.submit(note, "proc")
    print(p.result(), log)
    got: list[int] = []
    f3 = ex.submit(square, 4)
    f3.add_done_callback(lambda f: got.append(f.result()))
    f3.result()
    time.sleep(0.05)
    print(got)

ev = threading.Event()


def blocked() -> str:
    ev.wait()
    return "late"


with ThreadPoolExecutor(max_workers=1) as one:
    first = one.submit(blocked)
    second = one.submit(square, 3)
    print(second.cancel(), second.cancelled(), second.done())
    d, nd = wait([first], timeout=0.05, return_when=FIRST_COMPLETED)
    print(len(d), len(nd))
    ev.set()
    print(first.result())
    try:
        second.result()
    except concurrent.futures.CancelledError:
        print("CancelledError")
try:
    one.submit(square, 1)
except RuntimeError as e:
    print("RuntimeError:", e)

names: list[str] = []


def init(tag: str) -> None:
    names.append(tag + threading.current_thread().name[:7])


with ThreadPoolExecutor(max_workers=2, thread_name_prefix="worker", initializer=init, initargs=("w:",)) as pool:
    print(pool.submit(square, 2).result())
print(names[:1])

fut: concurrent.futures.Future[int] = concurrent.futures.Future()
print(fut.done(), fut.running(), noaddr(repr(fut)))
fut.set_result(7)
print(fut.result(), fut.done(), noaddr(repr(fut)))
try:
    fut.set_result(8)
except concurrent.futures.InvalidStateError as e:
    print("InvalidStateError:", noaddr(str(e)))
print(concurrent.futures.CancelledError.__name__, concurrent.futures.TimeoutError is TimeoutError)
pending: concurrent.futures.Future[int] = concurrent.futures.Future()
try:
    pending.result(timeout=0.01)
except TimeoutError:
    print("TimeoutError")
