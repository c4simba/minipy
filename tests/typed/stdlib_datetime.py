# datetime: timedelta, date, time, datetime, timezone, a tzinfo subclass; formatting, parsing, arithmetic, errors
from datetime import date, datetime, time, timedelta, timezone, tzinfo, MINYEAR, MAXYEAR
import datetime as D

# timedelta
td = timedelta(days=-1, seconds=5, microseconds=7)
print(repr(td), td, repr(timedelta()), timedelta(days=2, hours=3), repr(timedelta(hours=1.5)))
print(timedelta(milliseconds=1.5), timedelta(microseconds=0.5), timedelta(microseconds=1.5), timedelta(weeks=1, minutes=-1))
print(timedelta(seconds=10) / 3, timedelta(seconds=10) // timedelta(seconds=3), divmod(timedelta(seconds=10), timedelta(seconds=3)))
print(timedelta(seconds=10) * 1.5, 2 * timedelta(seconds=10), timedelta(seconds=10) * 3, abs(timedelta(-1)), -timedelta(hours=1))
print(timedelta(hours=-1).total_seconds(), timedelta(seconds=7) % timedelta(seconds=3), timedelta(seconds=1) / timedelta(milliseconds=250))
print(timedelta(days=1) == timedelta(hours=24), timedelta(1) < timedelta(2), timedelta(1) > timedelta(0, 86401), bool(timedelta()), bool(timedelta(microseconds=1)))
print(timedelta.max, timedelta.min, timedelta.resolution, timedelta(days=1).days, timedelta(hours=25).seconds)
print(timedelta(days=1, seconds=1) + timedelta(seconds=-2), timedelta(1) - timedelta(seconds=1), timedelta(seconds=10) // 4, timedelta(seconds=10) / 4)

# date
d = date(2024, 5, 17)
print(repr(d), d, d.year, d.month, d.day, d.weekday(), d.isoweekday(), d.isocalendar(), d.toordinal(), d.ctime())
print(d + timedelta(days=30), d - timedelta(days=200), d - date(2024, 1, 1), date(2024, 3, 1) - date(2024, 2, 1))
print(d.replace(day=1), d.replace(year=2023, month=2, day=28), d.timetuple(), d.isoformat(), d.strftime("%A %d %B %Y %j %U %W %a %b"))
print(date.fromordinal(738000), date.fromisoformat("2024-05-17"), date.fromisoformat("20240517"), date.fromisoformat("2024-W20-5"), date.fromisocalendar(2024, 1, 1))
print(date(2024, 5, 17) == date(2024, 5, 17), date(2024, 5, 17) < date(2024, 5, 18), date(2024, 1, 1) >= date(2023, 12, 31), date.min, date.max, date.resolution)
print(date(2024, 12, 30).isocalendar(), date(2021, 1, 3).isocalendar(), date(2020, 12, 31).isocalendar(), date(99, 1, 1).strftime("%Y %G %F %C"))
print(format(d, "%d.%m"), f"{d:%Y/%m}", f"{d}", str(d), date.strptime("2024-05-17", "%Y-%m-%d"), MINYEAR, MAXYEAR)
y, w, wd = d.isocalendar()
print(y, w, wd, d.isocalendar()[1], d.isocalendar().week, d.isocalendar() == (2024, 20, 5))

# time
t = time(13, 5, 7, 123456)
print(repr(t), t, repr(time(12, 30)), time(1, 2, 3, 4), time(), t.isoformat("milliseconds"), time(13, 5).isoformat("minutes"))
print(t.hour, t.minute, t.second, t.microsecond, t.tzinfo, t.replace(hour=1), time.fromisoformat("10:30:15.5"), time.fromisoformat("T0130"))
print(t.strftime("%H:%M:%S.%f %p %I"), time(10) < time(11), time(10) == time(10, 0), time.min, time.max, time.strptime("10:30", "%H:%M"))
tz3 = timezone(timedelta(hours=3))
print(repr(time(10, tzinfo=tz3)), time(10, tzinfo=tz3), time(10, tzinfo=tz3).utcoffset(), time(10, tzinfo=timezone.utc).tzname())

# timezone
print(repr(timezone.utc), timezone.utc, repr(timezone(timedelta(hours=-5, minutes=-30))), timezone(timedelta(hours=5), "X"))
print(repr(timezone(timedelta(hours=5), "X")), timezone(timedelta(hours=5)), timezone(timedelta(seconds=90)), timezone.min, timezone.max, D.UTC)
print(timezone(timedelta(hours=1)) == timezone(timedelta(hours=1), "A"), tz3.utcoffset(None), tz3.dst(None), tz3.tzname(None))

# datetime
dt = datetime(2024, 5, 17, 13, 5, 7, 123456)
print(repr(dt), dt, repr(datetime(2024, 5, 17, 12, 30)), repr(datetime(2024, 5, 17)), dt.date(), dt.time(), dt.timetz())
print(dt.isoformat(), dt.isoformat(" ", "seconds"), dt.isoformat(timespec="milliseconds"), dt.ctime(), dt.timetuple())
print(dt + timedelta(hours=11), dt - timedelta(days=17), dt - datetime(2024, 1, 1), datetime(2024, 3, 1) - timedelta(days=1))
print(dt.replace(microsecond=0), dt.replace(day=1, tzinfo=timezone.utc), datetime.combine(date(2024, 1, 2), time(3, 4)))
print(dt.strftime("%Y-%m-%d %H:%M:%S.%f %j %a %A %b %B %p %I %y %%"), dt.strftime("%f %z %Z %:z"))
aw = datetime(2024, 5, 17, 12, tzinfo=timezone.utc)
print(repr(aw), aw, aw.isoformat(), aw.utcoffset(), aw.tzname(), aw.dst(), aw.timestamp(), aw.strftime("%z %Z %:z"))
brt = datetime(2024, 5, 17, tzinfo=timezone(timedelta(hours=-3), "BRT"))
print(repr(brt), brt.strftime("%z %Z %:z"), brt.astimezone(timezone.utc), aw.astimezone(tz3), aw.astimezone(tz3).isoformat())
print(aw == datetime(2024, 5, 17, 15, tzinfo=tz3), aw < datetime(2024, 5, 17, 16, tzinfo=tz3), aw - datetime(2024, 5, 17, 12, tzinfo=tz3))
print(datetime.fromisoformat("2024-05-17T10:30:15"), repr(datetime.fromisoformat("2024-05-17T10:30:15.5+05:30")), repr(datetime.fromisoformat("20240517T103015Z")))
print(datetime.fromisoformat("2024-05-17T24:00"), datetime.fromisoformat("2024-05-17 10:30"), datetime.fromisoformat("2024-05-17"), datetime.fromisoformat("2024-W20-5T01"))
print(repr(datetime.strptime("2024-05-17 10:30:15.25 +0130", "%Y-%m-%d %H:%M:%S.%f %z")), datetime.strptime("17/05/24 1:2", "%d/%m/%y %H:%M"))
print(repr(datetime.strptime("Fri May 17 10:30:00 2024", "%a %b %d %H:%M:%S %Y")), repr(datetime.strptime("10 UTC", "%H %Z")))
print(datetime.fromtimestamp(0, timezone.utc), datetime.fromtimestamp(1715947507.25, timezone.utc), repr(datetime.fromtimestamp(86400.5, tz3)))
print(datetime(2024, 5, 17) == datetime(2024, 5, 17, 0), datetime(2024, 5, 17) < datetime(2024, 5, 17, 0, 0, 1), datetime.min, datetime.max)
print(datetime.fromordinal(738000), datetime.fromisocalendar(2024, 20, 5), dt.weekday(), dt.isocalendar(), dt.toordinal(), dt.year, dt.hour, dt.fold)
print(f"{dt:%H:%M}", format(dt, ""), datetime(2024, 1, 1, tzinfo=timezone.utc).timestamp(), datetime(1970, 1, 2, tzinfo=timezone.utc).timestamp())
print(sorted([date(2024, 1, 3), date(2023, 5, 1), date(2024, 1, 1)]), max(datetime(2024, 1, 1), datetime(2023, 1, 1)))
print({date(2024, 1, 1): "a"}[date(2024, 1, 1)], len({datetime(2024, 1, 1), datetime(2024, 1, 1, 0)}), timedelta(days=1) in [timedelta(hours=24)])


class FixedTZ(tzinfo):
    def __init__(self, hours: int, name: str):
        self.off = timedelta(hours=hours)
        self.name = name

    def utcoffset(self, dt: datetime | None) -> timedelta | None:
        return self.off

    def dst(self, dt: datetime | None) -> timedelta | None:
        return timedelta(0)

    def tzname(self, dt: datetime | None) -> str | None:
        return self.name


ft = FixedTZ(2, "EET")
x = datetime(2024, 5, 17, 12, tzinfo=ft)
print(x.isoformat(), x.utcoffset(), x.tzname(), x.astimezone(timezone.utc), datetime.fromtimestamp(0, ft), x.strftime("%H %Z %z"))

for bad in ["2024-13-01", "2024-02-30", "xx", "2024-1-1"]:
    try:
        print(date.fromisoformat(bad))
    except ValueError as e:
        print("ValueError", e)
for bad2 in ["2024-01-01T25:00", "2024-01-01T10:61", "2024-01-01T", "2024-01-01T1"]:
    try:
        print(datetime.fromisoformat(bad2))
    except ValueError as e:
        print("ValueError", e)
try:
    date(2023, 2, 29)
except ValueError as e:
    print("ValueError", e)
try:
    time(24)
except ValueError as e:
    print("ValueError", e)
try:
    timedelta(days=1e9)
except OverflowError as e:
    print("OverflowError", e)
try:
    print(datetime(2024, 1, 1) - datetime(2024, 1, 1, tzinfo=timezone.utc))
except TypeError as e:
    print("TypeError", e)
try:
    print(datetime(2024, 1, 1) < datetime(2024, 1, 1, tzinfo=timezone.utc))
except TypeError as e:
    print("TypeError", e)
try:
    print(datetime.strptime("2024", "%Y-%m"))
except ValueError as e:
    print("ValueError", e)
try:
    print(timezone(timedelta(hours=25)))
except ValueError as e:
    print("ValueError", e)
try:
    print(date.fromordinal(0))
except ValueError as e:
    print("ValueError", e)
try:
    print(date.max + timedelta(days=1))
except OverflowError as e:
    print("OverflowError", e)

import time as _t
now = datetime.now()
print(abs(now - datetime.fromtimestamp(_t.time())) < timedelta(seconds=5), date.today() == now.date() or now.hour == 0)
print(abs(datetime.now(timezone.utc) - datetime.fromtimestamp(_t.time(), timezone.utc)) < timedelta(seconds=5))
print(abs(datetime.now(timezone.utc).timestamp() - _t.time()) < 5, now.tzinfo is None, datetime.now(timezone.utc).tzinfo is timezone.utc)
loc = now.astimezone()
print(loc.tzinfo is not None, abs(loc.timestamp() - now.timestamp()) < 1, abs(now.timestamp() - _t.time()) < 5)
print(datetime.fromtimestamp(86400 * 365).timestamp() == 86400 * 365)
