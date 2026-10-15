# System calls: the clocks and the calendar in a loop - time.time(),
# time.monotonic() and time.perf_counter() (a system call each), today's date
# and time from datetime.now(), broken down by time.localtime() and formatted
# by strftime(). What is printed does not depend on when it runs.
import time
import datetime

N = 7_500_000


def main() -> None:
    ok = 0
    backwards = 0
    last = time.monotonic()
    for i in range(N):
        now = time.monotonic()
        if now < last:
            backwards += 1
        last = now
        t = time.time()
        p = time.perf_counter()
        if t > 1.0e9 and p >= 0.0:
            ok += 1
        if i % 10 == 0:
            d = datetime.datetime.now()
            lt = time.localtime()
            text = d.strftime("%Y-%m-%d %H:%M:%S")
            if 2000 < d.year < 3000 and 1 <= lt.tm_mon <= 12 and len(text) == 19:
                ok += 1
    print("clock", ok, backwards)


main()
