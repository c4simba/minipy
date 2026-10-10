"""time: clocks, sleep, gmtime/localtime/mktime/strftime/strptime/asctime/ctime, struct_time,
and time zones (TZ: zoneinfo files and POSIX rules, switched with tzset)."""
import os
import time

t = time.time()
print(type(t).__name__, t > 1.7e9, type(time.time_ns()).__name__, time.time_ns() // 1000000000 >= int(t) - 1)
m = time.monotonic()
time.sleep(0.02)
print(time.monotonic() - m >= 0.015, time.perf_counter_ns() > 0, time.process_time() >= 0, time.thread_time() >= 0)
try:
    time.sleep(-1)
except ValueError as e:
    print("ValueError", e)

for secs in [0, 1700000000, -1, 951782400, 4102444800, -2000000000, 1234567890]:
    g = time.gmtime(secs)
    print(g, g.tm_zone, g.tm_gmtoff)
    print(time.strftime("%Y-%m-%d %H:%M:%S %a %A %b %B %j %U %W %w %u %G-W%V %g %y %C %e %I %p %D %T %R %F %h %%", g))
    print(time.asctime(g), time.mktime(g) == time.mktime(g))
print(time.gmtime(1e9 + 0.75), time.gmtime(-1.5).tm_sec)
try:
    time.gmtime(float("nan"))
except ValueError as e:
    print("ValueError", e)

st = time.struct_time((2024, 2, 29, 13, 5, 9, 3, 60, 0))
print(st, st[0], st[-1], len(st), st.tm_mday, st.tm_zone, st.tm_gmtoff)
print(st == time.struct_time((2024, 2, 29, 13, 5, 9, 3, 60, 0)), st < time.struct_time((2024, 3, 1, 0, 0, 0, 4, 61, 0)))
print(list(st), hash(st) == hash(time.struct_time((2024, 2, 29, 13, 5, 9, 3, 60, 0))))
print(time.strftime("%c|%x|%X|%r", st), time.asctime(st))
print(time.strftime("%Y %m %d %j", time.struct_time((2024, 0, 0, 0, 0, 0, 0, 0, 0))))
for bad in [(2024, 13, 1, 0, 0, 0, 0, 1, 0), (2024, 1, 32, 0, 0, 0, 0, 1, 0), (2024, 1, 1, 24, 0, 0, 0, 1, 0), (2024, 1, 1, 0, 0, 0, -2, 1, 0)]:
    try:
        print(time.strftime("%Y", time.struct_time(bad)))
    except ValueError as e:
        print("ValueError", e)

for s, f in [("2024-03-05", "%Y-%m-%d"), ("  tuesday   MARCH 5 2024 10:00:00 pm +05:30 UTC", " %A %B %d %Y %I:%M:%S %p %z %Z"),
             ("2024 10 3", "%Y %W %w"), ("2024 01 1", "%G %V %u"), ("2023 52 0", "%Y %U %w"), ("1112 2024", "%d%m %Y"),
             ("123456", "%H%M%S"), ("Tue Mar  5 10:00:00 2024", "%a %b %d %H:%M:%S %Y"), ("5.123", "%S.%f"),
             ("2024-366", "%Y-%j"), ("+0530", "%z"), ("-05:30:15.25", "%z"), ("Z", "%z"), ("99 12 31", "%y %m %d"),
             ("03/05/24", "%x"), ("Tue Mar  5 10:00:00 2024", "%c"), ("12 AM", "%I %p"), ("12 pm", "%I %p")]:
    p = time.strptime(s, f)
    print(repr(s), p, p.tm_zone, p.tm_gmtoff)
for s, f in [("1900-02-30", "%Y-%m-%d"), ("2024-13-01", "%Y-%m-%d"), ("2024", "%Y %Q"), ("2024", "%Y %"),
             ("2024-01-01x", "%Y-%m-%d"), ("2024 01", "%G %V"), ("2024 01 1", "%Y %V %u")]:
    try:
        print(time.strptime(s, f))
    except ValueError as e:
        print("ValueError", e)

for zone in ["UTC", "Europe/Berlin", "America/New_York", "Australia/Sydney", "Asia/Kolkata", "EST5EDT,M3.2.0,M11.1.0",
             "<+0330>-3:30", "CET-1CEST,M3.5.0,M10.5.0/3", "NZST-12NZDT,M9.5.0,M4.1.0/3"]:
    os.environ["TZ"] = zone
    time.tzset()
    print(zone, time.tzname, time.timezone, time.altzone, time.daylight)
    for secs in [1700000000, 1720000000, 1711846799, 1711846800, 1729990799, 1729990800, 0]:
        lt = time.localtime(secs)
        print(" ", lt, lt.tm_zone, lt.tm_gmtoff, time.mktime(lt) == secs, time.strftime("%Y-%m-%d %H:%M:%S %Z", lt), time.ctime(secs))
    print(" ", time.mktime(time.struct_time((2024, 1, 15, 12, 0, 0, 0, 0, -1))), time.mktime(time.struct_time((2024, 7, 15, 12, 0, 0, 0, 0, -1))),
          time.mktime(time.struct_time((2024, 13, 35, 25, 61, 61, 0, 0, -1))))

os.environ["TZ"] = "<+0330>-3:30"
time.tzset()
from time import tzname, timezone
print(tzname, timezone, time.localtime(0).tm_hour)
