"""Time access and conversions (CPython's time).

The clocks are the system's (clock_gettime; on KolibriOS its clock and the
1/100 s counter). Local time follows the TZ variable or /etc/localtime (zoneinfo
files and POSIX TZ rules); KolibriOS keeps no time zone, its clock is taken as UTC."""
import sys
import _os

_NS = 1000000000

if sys.platform == "darwin":
    CLOCK_REALTIME = 0
    CLOCK_MONOTONIC_RAW = 4
    CLOCK_MONOTONIC = 6
    CLOCK_UPTIME_RAW = 8
    CLOCK_PROCESS_CPUTIME_ID = 12
    CLOCK_THREAD_CPUTIME_ID = 16
else:
    CLOCK_REALTIME = 0
    CLOCK_MONOTONIC = 1
    CLOCK_PROCESS_CPUTIME_ID = 2
    CLOCK_THREAD_CPUTIME_ID = 3
    CLOCK_MONOTONIC_RAW = 4
    CLOCK_BOOTTIME = 7
    CLOCK_TAI = 11


# ---------------------------------------------------------------- calendar arithmetic

def _days_from_civil(y: int, m: int, d: int) -> int:
    """Days from 1970-01-01 to a date (proleptic Gregorian)."""
    if m <= 2:
        y -= 1
        m += 12
    era = y // 400
    yoe = y - era * 400
    doy = (153 * (m - 3) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def _civil(days: int) -> tuple[int, int, int]:
    """(year, month, day) of a day counted from 1970-01-01."""
    z = days + 719468
    era = z // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    y = yoe + era * 400
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + 3 if mp < 10 else mp - 9
    if m <= 2:
        y += 1
    return (y, m, d)


def _leap(y: int) -> bool:
    return y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)


def _month_days(y: int, m: int) -> int:
    if m == 2:
        return 29 if _leap(y) else 28
    return 30 if m in (4, 6, 9, 11) else 31


# ---------------------------------------------------------------- clocks

def _bcd(x: int) -> int:
    return (x >> 4) * 10 + (x & 15)


def _kwall() -> int:
    """KolibriOS's clock (fn 3 and fn 29): seconds since 1970."""
    d = sys.syscall(29)[0]
    t = sys.syscall(3)[0]
    if sys.syscall(29)[0] != d:
        d = sys.syscall(29)[0]
        t = sys.syscall(3)[0]
    days = _days_from_civil(2000 + _bcd(d & 255), _bcd((d >> 8) & 255), _bcd((d >> 16) & 255))
    return ((days * 24 + _bcd(t & 255)) * 60 + _bcd((t >> 8) & 255)) * 60 + _bcd((t >> 16) & 255)


_koff: list[int] = []


def _clock(clk: int) -> tuple[int, int]:
    """(seconds, nanoseconds) of a clock (Linux's numbers): 0 real time, 1 monotonic, 2 the process's CPU time, 3 the thread's."""
    if sys.platform == "kolibrios":
        ticks = sys.syscall(26, 9)[0]
        if clk == 0:                            # the clock's seconds, in steps of the 1/100 s counter
            wall = _kwall() * 100
            if not _koff or abs(_koff[0] + ticks - wall) > 150:
                del _koff[:]
                _koff.append(wall - ticks)
            ticks += _koff[0]
        return (ticks // 100, ticks % 100 * 10000000)
    b = sys.buffer(8)
    sys.syscall(265, clk, b)
    return (sys.peek(b, 0, 4), sys.peek(b, 4, 4))


def _linux_clock(clk_id: int) -> int:
    if sys.platform == "darwin":
        if clk_id == 6 or clk_id == 4 or clk_id == 8:
            return 1
        if clk_id == 12:
            return 2
        if clk_id == 16:
            return 3
        if clk_id != 0:
            raise OSError(22, "Invalid argument")
        return 0
    if clk_id < 0 or clk_id > 11:
        raise OSError(22, "Invalid argument")
    return clk_id


def time() -> float:
    """Seconds since the epoch (1970-01-01 UTC)."""
    s, ns = _clock(0)
    return s + ns / 1e9


def time_ns() -> int:
    s, ns = _clock(0)
    return s * _NS + ns


def monotonic() -> float:
    """A clock that never goes back (seconds; only differences mean anything)."""
    s, ns = _clock(1)
    return s + ns / 1e9


def monotonic_ns() -> int:
    s, ns = _clock(1)
    return s * _NS + ns


def perf_counter() -> float:
    s, ns = _clock(1)
    return s + ns / 1e9


def perf_counter_ns() -> int:
    s, ns = _clock(1)
    return s * _NS + ns


def process_time() -> float:
    """CPU time of the process (seconds)."""
    s, ns = _clock(2)
    return s + ns / 1e9


def process_time_ns() -> int:
    s, ns = _clock(2)
    return s * _NS + ns


def thread_time() -> float:
    s, ns = _clock(3)
    return s + ns / 1e9


def thread_time_ns() -> int:
    s, ns = _clock(3)
    return s * _NS + ns


def clock_gettime(clk_id: int) -> float:
    s, ns = _clock(_linux_clock(clk_id))
    return s + ns / 1e9


def clock_gettime_ns(clk_id: int) -> int:
    s, ns = _clock(_linux_clock(clk_id))
    return s * _NS + ns


def clock_getres(clk_id: int) -> float:
    _linux_clock(clk_id)
    if sys.platform == "kolibrios":
        return 0.01
    return 1e-09


def sleep(secs: float) -> None:
    """Wait secs seconds."""
    if secs != secs:
        raise ValueError("Invalid value NaN (not a number)")
    if secs < 0:
        raise ValueError("sleep length must be non-negative")
    sys._sleep(secs)


class _ClockInfo:
    def __init__(self, implementation: str, monotonic: bool, adjustable: bool, resolution: float):
        self.implementation = implementation
        self.monotonic = monotonic
        self.adjustable = adjustable
        self.resolution = resolution

    def __repr__(self) -> str:
        return ("namespace(implementation=" + repr(self.implementation) + ", monotonic=" + repr(self.monotonic)
                + ", adjustable=" + repr(self.adjustable) + ", resolution=" + repr(self.resolution) + ")")


def get_clock_info(name: str) -> _ClockInfo:
    """How clock name ('time', 'monotonic', 'perf_counter', 'process_time', 'thread_time') is made."""
    if sys.platform == "kolibrios":
        if name == "time":
            return _ClockInfo("KolibriOS clock (fn 3, fn 29)", False, True, 1.0)
        if name in ("monotonic", "perf_counter", "process_time", "thread_time"):
            return _ClockInfo("KolibriOS counter (fn 26.9)", True, False, 0.01)
    else:
        if name == "time":
            return _ClockInfo("clock_gettime(CLOCK_REALTIME)", False, True, 1e-09)
        if name == "monotonic" or name == "perf_counter":
            return _ClockInfo("clock_gettime(CLOCK_MONOTONIC)", True, False, 1e-09)
        if name == "process_time":
            return _ClockInfo("clock_gettime(CLOCK_PROCESS_CPUTIME_ID)", True, False, 1e-09)
        if name == "thread_time":
            return _ClockInfo("clock_gettime(CLOCK_THREAD_CPUTIME_ID)", True, False, 1e-09)
    raise ValueError("unknown clock")


# ---------------------------------------------------------------- struct_time

class struct_time:
    """A date and time of day: a 9-sequence (year, month, day, hour, minute, second, weekday
    (Monday 0), day of the year, DST flag) with tm_zone and tm_gmtoff besides."""
    n_sequence_fields = 9
    n_fields = 11
    n_unnamed_fields = 0

    def __init__(self, seq: tuple[int, int, int, int, int, int, int, int, int], d=None):
        self.tm_year = seq[0]
        self.tm_mon = seq[1]
        self.tm_mday = seq[2]
        self.tm_hour = seq[3]
        self.tm_min = seq[4]
        self.tm_sec = seq[5]
        self.tm_wday = seq[6]
        self.tm_yday = seq[7]
        self.tm_isdst = seq[8]
        self.tm_zone: str | None = None
        self.tm_gmtoff: int | None = None
        if not sys._compiled:
            self._rest(seq)

    if not sys._compiled:
        def _rest(self, seq):
            n = len(seq)
            if n < 9:
                raise TypeError("time.struct_time() takes an at least 9-sequence (" + str(n) + "-sequence given)")
            if n > 11:
                raise TypeError("time.struct_time() takes an at most 11-sequence (" + str(n) + "-sequence given)")
            if n > 9:
                self.tm_zone = seq[9]
            if n > 10:
                self.tm_gmtoff = seq[10]

    def _tuple(self) -> tuple[int, int, int, int, int, int, int, int, int]:
        return (self.tm_year, self.tm_mon, self.tm_mday, self.tm_hour, self.tm_min, self.tm_sec, self.tm_wday,
                self.tm_yday, self.tm_isdst)

    def __getitem__(self, i: int) -> int:
        if not sys._compiled:
            if not isinstance(i, int):
                return self._tuple()[i]
        k = i + 9 if i < 0 else i
        if k == 0:
            return self.tm_year
        if k == 1:
            return self.tm_mon
        if k == 2:
            return self.tm_mday
        if k == 3:
            return self.tm_hour
        if k == 4:
            return self.tm_min
        if k == 5:
            return self.tm_sec
        if k == 6:
            return self.tm_wday
        if k == 7:
            return self.tm_yday
        if k == 8:
            return self.tm_isdst
        raise IndexError("tuple index out of range")

    def __len__(self) -> int:
        return 9

    def __iter__(self):
        for k in range(9):
            yield self[k]

    def __eq__(self, other: "struct_time") -> bool:
        if not sys._compiled:
            if isinstance(other, tuple):
                return self._tuple() == other
            if not isinstance(other, struct_time):
                return NotImplemented
        return self._tuple() == other._tuple()

    def __ne__(self, other: "struct_time") -> bool:
        return not self == other

    def __lt__(self, other: "struct_time") -> bool:
        return self._tuple() < other._tuple()

    def __le__(self, other: "struct_time") -> bool:
        return self._tuple() <= other._tuple()

    def __gt__(self, other: "struct_time") -> bool:
        return self._tuple() > other._tuple()

    def __ge__(self, other: "struct_time") -> bool:
        return self._tuple() >= other._tuple()

    def __hash__(self) -> int:
        return hash(self._tuple())

    def __repr__(self) -> str:
        return ("time.struct_time(tm_year=" + str(self.tm_year) + ", tm_mon=" + str(self.tm_mon) + ", tm_mday="
                + str(self.tm_mday) + ", tm_hour=" + str(self.tm_hour) + ", tm_min=" + str(self.tm_min) + ", tm_sec="
                + str(self.tm_sec) + ", tm_wday=" + str(self.tm_wday) + ", tm_yday=" + str(self.tm_yday)
                + ", tm_isdst=" + str(self.tm_isdst) + ")")


def _make(t: int, off: int, isdst: int, zone: str) -> struct_time:
    """The struct_time of time t (seconds since 1970 UTC) at UTC offset off."""
    lt = t + off
    days = lt // 86400
    rem = lt - days * 86400
    y, m, d = _civil(days)
    st = struct_time((y, m, d, rem // 3600, rem // 60 % 60, rem % 60, (days + 3) % 7,
                      days - _days_from_civil(y, 1, 1) + 1, isdst))
    st.tm_zone = zone
    st.tm_gmtoff = off
    return st


def _whole(secs) -> int:
    """A timestamp's whole seconds (rounded down)."""
    if secs != secs:
        raise ValueError("Invalid value NaN (not a number)")
    if secs - secs != 0:
        raise OverflowError("timestamp out of range for platform time_t")
    return int(secs // 1)


def gmtime(secs=None) -> struct_time:
    """The UTC date and time of secs seconds since the epoch (now without it)."""
    if secs is None:
        return _make(_clock(0)[0], 0, 0, "UTC")
    else:
        return _make(_whole(secs), 0, 0, "UTC")


def localtime(secs=None) -> struct_time:
    """The local date and time of secs seconds since the epoch (now without it)."""
    if secs is None:
        t = _clock(0)[0]
    else:
        t = _whole(secs)
    off, dst, zone = _info(t)
    return _make(t, off, dst, zone)


def _local_secs(y: int, mo: int, d: int, h: int, mi: int, s: int, isdst: int) -> int:
    """The time (seconds since 1970 UTC) of a local date and time (fields out of range carry over)."""
    y += (mo - 1) // 12
    mo = (mo - 1) % 12 + 1
    wall = ((_days_from_civil(y, mo, 1) + d - 1) * 24 + h) * 3600 + mi * 60 + s
    off = _info(wall - _info(wall)[0])[0]
    t = wall - off
    o2, dst2, z2 = _info(t)
    if o2 != off:                              # (in a gap or an overlap of the clock)
        t2 = wall - o2
        if _info(t2)[0] == o2:
            t = t2
    elif isdst >= 0 and dst2 != (1 if isdst > 0 else 0):
        for other in (off + 3600, off - 3600):
            t3 = wall - other
            o3, d3, z3 = _info(t3)
            if o3 == other and d3 == (1 if isdst > 0 else 0):
                t = t3
    return t


def mktime(t: struct_time) -> float:
    """The seconds since the epoch of local time t."""
    if not sys._compiled:
        if not isinstance(t, struct_time):
            t = struct_time(t)
    return float(_local_secs(t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, t.tm_isdst))


_DAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
_FULL_DAYS = ("Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday")
_MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
_FULL_MONTHS = ("January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
                "November", "December")


def _2(n: int) -> str:
    return "0" + str(n) if 0 <= n < 10 else str(n)


def _sp2(n: int) -> str:
    return " " + str(n) if 0 <= n < 10 else str(n)


def _asc(t: struct_time) -> str:
    _check(t)
    return (_DAYS[t.tm_wday % 7] + " " + _MONTHS[(t.tm_mon - 1) % 12] + " " + _sp2(t.tm_mday) + " " + _2(t.tm_hour) + ":"
            + _2(t.tm_min) + ":" + _2(t.tm_sec) + " " + str(t.tm_year))


def asctime(t=None) -> str:
    """'Sun Jun 20 23:21:05 1993' of struct_time t (now, local, without it)."""
    if t is None:
        return _asc(localtime())
    else:
        if not sys._compiled:
            if not isinstance(t, struct_time):
                return _asc(struct_time(t))
        return _asc(t)


def ctime(secs=None) -> str:
    """asctime(localtime(secs))."""
    return _asc(localtime(secs))


def _check(t: struct_time) -> None:
    if t.tm_mon != 0 and (t.tm_mon < 1 or t.tm_mon > 12):
        raise ValueError("month out of range")
    if t.tm_mday < 0 or t.tm_mday > 31:
        raise ValueError("day of month out of range")
    if t.tm_hour < 0 or t.tm_hour > 23:
        raise ValueError("hour out of range")
    if t.tm_min < 0 or t.tm_min > 59:
        raise ValueError("minute out of range")
    if t.tm_sec < 0 or t.tm_sec > 61:
        raise ValueError("seconds out of range")
    if t.tm_wday < -1:
        raise ValueError("day of week out of range")
    if t.tm_yday != 0 and (t.tm_yday < 1 or t.tm_yday > 366):
        raise ValueError("day of year out of range")


def _iso_weeks(y: int) -> int:
    p = (y + y // 4 - y // 100 + y // 400) % 7
    q = (y - 1 + (y - 1) // 4 - (y - 1) // 100 + (y - 1) // 400) % 7
    return 53 if p == 4 or q == 3 else 52


def _iso(t: struct_time) -> tuple[int, int]:
    """(ISO year, ISO week) of t."""
    y = t.tm_year
    wk = (t.tm_yday - (t.tm_wday % 7 + 1) + 10) // 7
    if wk < 1:
        y -= 1
        wk = _iso_weeks(y)
    elif wk > _iso_weeks(y):
        y += 1
        wk = 1
    return (y, wk)


def _gmtoff_text(off: int) -> str:
    sign = "-" if off < 0 else "+"
    a = -off if off < 0 else off
    return sign + _2(a // 3600) + _2(a // 60 % 60)


def _field(c: str, t: struct_time) -> str:
    """What %c stands for in strftime (the C locale)."""
    wd = t.tm_wday % 7
    mon = t.tm_mon if t.tm_mon != 0 else 1
    mday = t.tm_mday if t.tm_mday != 0 else 1
    yday = t.tm_yday if t.tm_yday != 0 else 1
    if c == "a":
        return _DAYS[wd]
    if c == "A":
        return _FULL_DAYS[wd]
    if c == "b" or c == "h":
        return _MONTHS[mon - 1]
    if c == "B":
        return _FULL_MONTHS[mon - 1]
    if c == "c":
        return _strftime("%a %b %e %H:%M:%S %Y", t)
    if c == "C":
        return _2(t.tm_year // 100)
    if c == "d":
        return _2(mday)
    if c == "D" or c == "x":
        return _strftime("%m/%d/%y", t)
    if c == "e":
        return _sp2(mday)
    if c == "F":
        return _strftime("%Y-%m-%d", t)
    if c == "G":
        return str(_iso(t)[0])
    if c == "g":
        return _2(_iso(t)[0] % 100)
    if c == "V":
        return _2(_iso(t)[1])
    if c == "H":
        return _2(t.tm_hour)
    if c == "I":
        return _2(t.tm_hour % 12 if t.tm_hour % 12 else 12)
    if c == "j":
        return ("00" if yday < 10 else "0" if yday < 100 else "") + str(yday)
    if c == "k":
        return _sp2(t.tm_hour)
    if c == "l":
        return _sp2(t.tm_hour % 12 if t.tm_hour % 12 else 12)
    if c == "m":
        return _2(mon)
    if c == "M":
        return _2(t.tm_min)
    if c == "n":
        return "\n"
    if c == "p":
        return "PM" if t.tm_hour >= 12 else "AM"
    if c == "r":
        return _strftime("%I:%M:%S %p", t)
    if c == "R":
        return _strftime("%H:%M", t)
    if c == "s":
        return str(_local_secs(t.tm_year, mon, mday, t.tm_hour, t.tm_min, t.tm_sec, t.tm_isdst))
    if c == "S":
        return _2(t.tm_sec)
    if c == "t":
        return "\t"
    if c == "T" or c == "X":
        return _strftime("%H:%M:%S", t)
    if c == "u":
        return str(wd + 1)
    if c == "U":
        return _2((yday - 1 + 7 - (wd + 1) % 7) // 7)
    if c == "w":
        return str((wd + 1) % 7)
    if c == "W":
        return _2((yday - 1 + 7 - wd) // 7)
    if c == "y":
        return _2(t.tm_year % 100)
    if c == "Y":
        y = t.tm_year
        if 0 <= y < 1000:
            return "0" * (4 - len(str(y))) + str(y)
        return str(y)
    if c == "z":
        if sys.platform == "darwin" or t.tm_gmtoff is None:
            return _gmtoff_text(_info(_local_secs(t.tm_year, mon, mday, t.tm_hour, t.tm_min, t.tm_sec, t.tm_isdst))[0])
        return _gmtoff_text(t.tm_gmtoff)
    if c == "Z":
        z = t.tm_zone
        if z is None:
            return _lazy_tzname()[1] if t.tm_isdst > 0 else _lazy_tzname()[0]
        return z
    if c == "+":
        return _strftime("%a %b %e %H:%M:%S %Z %Y", t)
    if c == "%":
        return "%"
    if sys.platform == "darwin":
        return c
    return "%" + c


def _strftime(format: str, t: struct_time) -> str:
    out: list[str] = []
    i = 0
    n = len(format)
    while i < n:
        c = format[i]
        if c != "%" or i + 1 >= n:
            out.append(c)
            i += 1
            continue
        out.append(_field(format[i + 1], t))
        i += 2
    return "".join(out)


def strftime(format: str, t=None) -> str:
    """t (now, local, without it) as format says: %Y-%m-%d %H:%M:%S, %a %b %e, ..."""
    if t is None:
        st = localtime()
    else:
        if not sys._compiled:
            if not isinstance(t, struct_time):
                t = struct_time(t)
        st = t
    _check(st)
    return _strftime(format, st)


# ---------------------------------------------------------------- strptime

_D = "0123456789"
_D19 = "123456789"
_NUM = {"d": [["3", "01"], ["12", _D], ["0", _D19], [_D19], [" ", _D19]],
        "H": [["2", "0123"], ["01", _D], [_D]],
        "I": [["1", "012"], ["0", _D19], [_D19], [" ", _D19]],
        "G": [[_D, _D, _D, _D]],
        "j": [["3", "6", "0123456"], ["3", "012345", _D], ["12", _D, _D], ["0", _D19, _D], ["0", "0", _D19],
              [_D19, _D], ["0", _D19], [_D19]],
        "m": [["1", "012"], ["0", _D19], [_D19]],
        "M": [["012345", _D], [_D]],
        "S": [["6", "01"], ["012345", _D], [_D]],
        "U": [["5", "0123"], ["01234", _D], [_D]],
        "W": [["5", "0123"], ["01234", _D], [_D]],
        "w": [["0123456"]],
        "u": [["1234567"]],
        "V": [["5", "0123"], ["0", _D19], ["1234", _D], [_D]],
        "y": [[_D, _D]],
        "Y": [[_D, _D, _D, _D]],
        "f": [[_D, _D, _D, _D, _D, _D], [_D, _D, _D, _D, _D], [_D, _D, _D, _D], [_D, _D, _D], [_D, _D], [_D]]}


def _by_length(names: list[str]) -> list[str]:
    """Longer names first (as CPython's alternation tries them)."""
    out: list[str] = []
    for k in range(max(len(x) for x in names), 0, -1):
        for x in names:
            if len(x) == k:
                out.append(x)
    return out


def _zone_names() -> list[str]:
    tzname = _lazy_tzname()
    names = ["utc", "gmt", tzname[0].lower()]
    if _lazy_daylight():
        names.append(tzname[1].lower())
    return names


def _names(c: str) -> list[str]:
    if c == "a":
        return _by_length([x.lower() for x in _DAYS])
    if c == "A":
        return _by_length([x.lower() for x in _FULL_DAYS])
    if c == "b":
        return _by_length([x.lower() for x in _MONTHS])
    if c == "B":
        return _by_length([x.lower() for x in _FULL_MONTHS])
    if c == "p":
        return ["am", "pm"]
    return _by_length(_zone_names())


def _isdigit(s: str, i: int) -> bool:
    return i < len(s) and s[i] in _D


def _zone_lengths(s: str, i: int) -> list[int]:
    """The lengths %z can match at s[i:] (+hh[:]mm[[:]ss[.ffffff]] or Z), in the order a regex tries them."""
    out: list[int] = []
    if i < len(s) and s[i] == "Z":
        out.append(1)
    if i >= len(s) or s[i] not in "+-" or not _isdigit(s, i + 1) or not _isdigit(s, i + 2):
        return out
    for c1 in (1, 0):
        p = i + 3
        if c1:
            if p >= len(s) or s[p] != ":":
                continue
            p += 1
        if p >= len(s) or s[p] not in "012345" or not _isdigit(s, p + 1):
            continue
        p += 2
        for c2 in (1, 0):
            q = p
            if c2:
                if q >= len(s) or s[q] != ":":
                    continue
                q += 1
            if q < len(s) and s[q] in "012345" and _isdigit(s, q + 1):
                q += 2
                if q < len(s) and s[q] == "." and _isdigit(s, q + 1):
                    k = q + 1
                    while k < len(s) and k < q + 7 and s[k] in _D:
                        k += 1
                    for e in range(k, q + 1, -1):
                        out.append(e - i)
                out.append(q - i)
        out.append(p - i)
    return out


def _lengths(c: str, s: str, i: int) -> list[int]:
    """The lengths directive %c can match at s[i:], in the order a regex tries them."""
    out: list[int] = []
    if c == "z":
        return _zone_lengths(s, i)
    if c in "aAbBpZ":
        for name in _names(c):
            if s[i:i + len(name)].lower() == name:
                out.append(len(name))
        return out
    for alt in _NUM[c]:
        ok = i + len(alt) <= len(s)
        k = 0
        while ok and k < len(alt):
            if s[i + k] not in alt[k]:
                ok = False
            k += 1
        if ok:
            out.append(len(alt))
    return out


def _tokens(format: str) -> list[str]:
    """format as tokens: '%x' a directive, ' ' a run of whitespace, else one character."""
    out: list[str] = []
    i = 0
    n = len(format)
    while i < n:
        c = format[i]
        if c.isspace():
            while i < n and format[i].isspace():
                i += 1
            out.append(" ")
            continue
        if c != "%":
            out.append(c)
            i += 1
            continue
        if i + 1 >= n:
            raise ValueError("stray % in format '" + format + "'")
        d = format[i + 1]
        i += 2
        if d == "c":
            out.extend(_tokens("%a %b %d %H:%M:%S %Y"))
        elif d == "x":
            out.extend(_tokens("%m/%d/%y"))
        elif d == "X":
            out.extend(_tokens("%H:%M:%S"))
        elif d == "%":
            out.append("%")
        elif d in "aAbBpZz" or d in _NUM:
            out.append("%" + d)
        else:
            raise ValueError("'" + d + "' is a bad directive in format '" + format + "'")
    return out


def _match(toks: list[str], k: int, s: str, i: int, found: dict[str, str]) -> int:
    """Where the match of toks[k:] at s[i:] ends (the first one a regex finds), or -1."""
    if k == len(toks):
        return i
    tok = toks[k]
    if tok == " ":
        j = i
        while j < len(s) and s[j].isspace():
            j += 1
        while j > i:
            r = _match(toks, k + 1, s, j, found)
            if r >= 0:
                return r
            j -= 1
        return -1
    if len(tok) == 2 and tok[0] == "%":
        c = tok[1]
        for n in _lengths(c, s, i):
            found[c] = s[i:i + n]
            r = _match(toks, k + 1, s, i + n, found)
            if r >= 0:
                return r
        if c in found:
            del found[c]
        return -1
    if i < len(s) and s[i].lower() == tok.lower():
        return _match(toks, k + 1, s, i + 1, found)
    return -1


def _weekday(y: int, m: int, d: int) -> int:
    return (_days_from_civil(y, m, d) + 3) % 7


def _check_date(y: int, m: int, d: int) -> None:
    if y < 1 or y > 9999:
        raise ValueError("year " + str(y) + " is out of range")
    if m < 1 or m > 12:
        raise ValueError("month must be in 1..12")
    if d < 1 or d > _month_days(y, m):
        raise ValueError("day " + str(d) + " must be in range 1.." + str(_month_days(y, m)) + " for month " + str(m)
                         + " in year " + str(y))


def _julian_from_week(year: int, week: int, weekday: int, monday_first: bool) -> int:
    first = _weekday(year, 1, 1)
    if not monday_first:
        first = (first + 1) % 7
        weekday = (weekday + 1) % 7
    week0 = (7 - first) % 7
    if week == 0:
        return 1 + weekday - first
    return 1 + week0 + 7 * (week - 1) + weekday


def strptime(data_string: str, format: str = "%a %b %d %H:%M:%S %Y") -> struct_time:
    """The struct_time data_string stands for, read as format says."""
    return _strptime(data_string, format)[0]


def _strptime(data_string: str, format: str) -> tuple[struct_time, int, int]:
    """strptime, and the microseconds of %f and of %z's fraction (for datetime)."""
    toks = _tokens(format)
    found: dict[str, str] = {}
    end = _match(toks, 0, data_string, 0, found)
    if end < 0:
        raise ValueError("time data " + repr(data_string) + " does not match format " + repr(format))
    if end != len(data_string):
        raise ValueError("unconverted data remains: " + data_string[end:])
    year = -1
    iso_year = -1
    iso_week = -1
    month = 1
    day = 1
    hour = 0
    minute = 0
    second = 0
    tz = -1
    gmtoff = 0
    has_gmtoff = False
    week_of_year = -1
    week_start = -1
    weekday = -1
    julian = -1
    fraction = 0
    gmtoff_fraction = 0
    for key in found:
        v = found[key]
        if key == "y":
            year = int(v)
            year += 2000 if year <= 68 else 1900
        elif key == "Y":
            year = int(v)
        elif key == "G":
            iso_year = int(v)
        elif key == "m":
            month = int(v)
        elif key == "B":
            month = [x.lower() for x in _FULL_MONTHS].index(v.lower()) + 1
        elif key == "b":
            month = [x.lower() for x in _MONTHS].index(v.lower()) + 1
        elif key == "d":
            day = int(v)
        elif key == "H":
            hour = int(v)
        elif key == "I":
            hour = int(v)
            ampm = found["p"].lower() if "p" in found else ""
            if ampm == "" or ampm == "am":
                if hour == 12:
                    hour = 0
            elif hour != 12:
                hour += 12
        elif key == "M":
            minute = int(v)
        elif key == "S":
            second = int(v)
        elif key == "A":
            weekday = [x.lower() for x in _FULL_DAYS].index(v.lower())
        elif key == "a":
            weekday = [x.lower() for x in _DAYS].index(v.lower())
        elif key == "w":
            weekday = int(v)
            weekday = 6 if weekday == 0 else weekday - 1
        elif key == "u":
            weekday = int(v) - 1
        elif key == "j":
            julian = int(v)
        elif key == "U" or key == "W":
            week_of_year = int(v)
            week_start = 6 if key == "U" else 0
        elif key == "V":
            iso_week = int(v)
        elif key == "z":
            z = v
            if z == "Z":
                gmtoff = 0
            else:
                if z[3] == ":":
                    z = z[:3] + z[4:]
                    if len(z) > 5:
                        if z[5] != ":":
                            raise ValueError("Inconsistent use of : in " + v)
                        z = z[:5] + z[6:]
                gmtoff = int(z[1:3]) * 3600 + int(z[3:5]) * 60 + (int(z[5:7]) if len(z) > 5 else 0)
                rest = z[8:]
                gmtoff_fraction = int(rest + "0" * (6 - len(rest)))
                if z[0] == "-":
                    gmtoff = -gmtoff
                    gmtoff_fraction = -gmtoff_fraction
            has_gmtoff = True
        elif key == "f":
            fraction = int(v + "0" * (6 - len(v)))
        elif key == "Z":
            tzname = _lazy_tzname()
            daylight = _lazy_daylight()
            name = v.lower()
            if name == "utc" or name == "gmt":
                tz = 0
            elif name == tzname[0].lower() and not (tzname[0] == tzname[1] and daylight):
                tz = 0
            elif daylight and name == tzname[1].lower() and tzname[0] != tzname[1]:
                tz = 1
    if year < 0 and iso_year >= 0:
        if iso_week < 0 or weekday < 0:
            raise ValueError("ISO year directive '%G' must be used with the ISO week directive '%V' and a weekday "
                             "directive ('%A', '%a', '%w', or '%u').")
        if julian >= 0:
            raise ValueError("Day of the year directive '%j' is not compatible with ISO year directive '%G'. "
                             "Use '%Y' instead.")
    elif iso_week >= 0:
        if year < 0 or weekday < 0:
            raise ValueError("ISO week directive '%V' must be used with the ISO year directive '%G' and a weekday "
                             "directive ('%A', '%a', '%w', or '%u').")
        raise ValueError("ISO week directive '%V' is incompatible with the year directive '%Y'. "
                         "Use the ISO year '%G' instead.")
    leap_fix = False
    if year < 0 and month == 2 and day == 29:
        year = 1904
        leap_fix = True
    elif year < 0 and iso_year < 0:
        year = 1900
    if julian < 0 and weekday >= 0:
        if week_of_year >= 0:
            julian = _julian_from_week(year, week_of_year, weekday, week_start == 0)
        elif iso_year >= 0 and iso_week >= 0:
            jan4 = _days_from_civil(iso_year, 1, 4)
            first_monday = jan4 - (jan4 + 3) % 7
            days = first_monday + (iso_week - 1) * 7 + weekday
            year, month, day = _civil(days)
            julian = days - _days_from_civil(year, 1, 1) + 1
        if julian != -1 and julian <= 0:
            year -= 1
            julian += 366 if _leap(year) else 365
    if julian < 0:
        _check_date(year, month, day)
        julian = _days_from_civil(year, month, day) - _days_from_civil(year, 1, 1) + 1
    else:
        year, month, day = _civil(julian - 1 + _days_from_civil(year, 1, 1))
    if weekday < 0:
        weekday = _weekday(year, month, day)
    if leap_fix:
        year = 1900
    st = struct_time((year, month, day, hour, minute, second, weekday, julian, tz))
    if "Z" in found:
        st.tm_zone = found["Z"]
    if has_gmtoff:
        st.tm_gmtoff = gmtoff
    return (st, fraction, gmtoff_fraction)


# ---------------------------------------------------------------- time zones

class _Rule:
    """A POSIX TZ rule: std offset [dst [offset] [,start[/time],end[/time]]]."""

    def __init__(self) -> None:
        self.std = "UTC"
        self.std_off = 0
        self.dst = ""
        self.dst_off = 0
        self.start: list[int] = [2, 3, 2, 0, 7200]     # (kind: 0 Jn, 1 n, 2 Mm.w.d), m/n, w, d, seconds
        self.end: list[int] = [2, 11, 1, 0, 7200]


class _Zone:
    def __init__(self) -> None:
        self.trans: list[int] = []
        self.idx: list[int] = []
        self.off: list[int] = []
        self.isdst: list[int] = []
        self.abbr: list[str] = []
        self.rules: list[_Rule] = []


class _Text:
    def __init__(self, s: str) -> None:
        self.s = s
        self.i = 0

    def peek(self) -> str:
        return self.s[self.i] if self.i < len(self.s) else ""

    def name(self) -> str:
        if self.peek() == "<":
            j = self.s.find(">", self.i)
            if j < 0:
                return ""
            r = self.s[self.i + 1:j]
            self.i = j + 1
            return r
        j = self.i
        while j < len(self.s) and self.s[j].isalpha():
            j += 1
        r = self.s[self.i:j]
        self.i = j
        return r if len(r) >= 3 else ""

    def number(self) -> int:
        j = self.i
        while j < len(self.s) and self.s[j] in _D:
            j += 1
        if j == self.i:
            return -1
        r = int(self.s[self.i:j])
        self.i = j
        return r

    def hms(self) -> int:
        """[+-]hh[:mm[:ss]] as seconds (-1000000: none)."""
        sign = 1
        if self.peek() in ("+", "-"):
            sign = -1 if self.peek() == "-" else 1
            self.i += 1
        h = self.number()
        if h < 0:
            return -1000000
        secs = h * 3600
        if self.peek() == ":":
            self.i += 1
            secs += max(self.number(), 0) * 60
            if self.peek() == ":":
                self.i += 1
                secs += max(self.number(), 0)
        return sign * secs

    def date(self) -> list[int]:
        """Jn, n or Mm.w.d [/time]."""
        r = [1, 0, 0, 0, 7200]
        c = self.peek()
        if c == "J":
            self.i += 1
            r[0] = 0
            r[1] = self.number()
        elif c == "M":
            self.i += 1
            r[0] = 2
            r[1] = self.number()
            if self.peek() == ".":
                self.i += 1
            r[2] = self.number()
            if self.peek() == ".":
                self.i += 1
            r[3] = self.number()
        else:
            r[1] = self.number()
        if self.peek() == "/":
            self.i += 1
            r[4] = self.hms()
        return r


def _parse_rule(s: str) -> _Rule | None:
    p = _Text(s)
    r = _Rule()
    r.std = p.name()
    if not r.std:
        return None
    off = p.hms()
    if off == -1000000:
        return None
    r.std_off = -off
    r.dst = p.name()
    r.dst_off = r.std_off + 3600
    if r.dst:
        if p.peek() not in (",", ""):
            o = p.hms()
            if o != -1000000:
                r.dst_off = -o
        if p.peek() == ",":
            p.i += 1
            r.start = p.date()
            if p.peek() == ",":
                p.i += 1
                r.end = p.date()
    return r


def _rule_day(d: list[int], y: int) -> int:
    """The day (counted from 1970) of rule date d in year y."""
    if d[0] == 0:
        n = d[1] - 1
        if _leap(y) and d[1] >= 60:
            n += 1
        return _days_from_civil(y, 1, 1) + n
    if d[0] == 1:
        return _days_from_civil(y, 1, 1) + d[1]
    m = d[1]
    first = _days_from_civil(y, m, 1)
    day = (d[3] - (first + 4) % 7) % 7 + 1 + (d[2] - 1) * 7
    while day > _month_days(y, m):
        day -= 7
    return first + day - 1


def _rule_info(r: _Rule, t: int) -> tuple[int, int, str]:
    if not r.dst:
        return (r.std_off, 0, r.std)
    y = _civil((t + r.std_off) // 86400)[0]
    start = _rule_day(r.start, y) * 86400 + r.start[4] - r.std_off
    end = _rule_day(r.end, y) * 86400 + r.end[4] - r.dst_off
    if y < 1970 or (y == 1970 and t < min(start, end)):     # (as tzcode: the rule's changes from 1970 on)
        return (r.std_off, 0, r.std)
    if start < end:
        dst = start <= t < end
    else:
        dst = not (end <= t < start)
    if dst:
        return (r.dst_off, 1, r.dst)
    return (r.std_off, 0, r.std)


def _be(b: bytes, off: int, n: int) -> int:
    """A signed big-endian number of n bytes."""
    v = b[off]
    if v >= 128:
        v -= 256
    for k in range(1, n):
        v = (v << 8) | b[off + k]
    return v


def _tzif(data: bytes, z: _Zone) -> bool:
    """Read a TZif file (RFC 8536) into z."""
    if len(data) < 44 or data[0:4] != b"TZif":
        return False
    p = 0
    size = 4
    if data[4] >= 50:
        p = 44 + _be(data, 32, 4) * 5 + _be(data, 36, 4) * 6 + _be(data, 40, 4) + _be(data, 28, 4) * 8 \
            + _be(data, 24, 4) + _be(data, 20, 4)
        size = 8
        if len(data) < p + 44 or data[p:p + 4] != b"TZif":
            return False
    isut = _be(data, p + 20, 4)
    isstd = _be(data, p + 24, 4)
    leap = _be(data, p + 28, 4)
    timecnt = _be(data, p + 32, 4)
    typecnt = _be(data, p + 36, 4)
    charcnt = _be(data, p + 40, 4)
    q = p + 44
    for k in range(timecnt):
        z.trans.append(_be(data, q + k * size, size))
    q += timecnt * size
    for k in range(timecnt):
        z.idx.append(data[q + k])
    q += timecnt
    chars = q + typecnt * 6
    for k in range(typecnt):
        z.off.append(_be(data, q + k * 6, 4))
        z.isdst.append(data[q + k * 6 + 4])
        a = chars + data[q + k * 6 + 5]
        e = a
        while e < len(data) and data[e] != 0:
            e += 1
        z.abbr.append(data[a:e].decode("ascii", "replace"))
    q = chars + charcnt + leap * (size + 4) + isstd + isut
    if size == 8 and q < len(data) and data[q] == 10:
        e = data.find(b"\n", q + 1)
        if e > q + 1:
            r = _parse_rule(data[q + 1:e].decode("ascii", "replace"))
            if r is not None:
                z.rules.append(r)
    return True


def _read(path: str) -> bytes:
    """The bytes of file path (b'' when it cannot be read)."""
    try:
        fd = _os.open(path, 0)
    except OSError:
        return b""
    parts: list[bytes] = []
    while True:
        try:
            b = _os.read(fd, 65536)
        except OSError:
            break
        if not b:
            break
        parts.append(b)
    _os.close(fd)
    return b"".join(parts)


def _load() -> _Zone:
    z = _Zone()
    name = _os.environ.get("TZ")
    if sys.platform == "kolibrios":                 # (no zoneinfo files: a POSIX rule or UTC)
        if name is not None:
            r = _parse_rule(name[1:] if name.startswith(":") else name)
            if r is not None:
                z.rules.append(r)
        return z
    path = "/etc/localtime"
    if name is not None:
        if name.startswith(":"):
            name = name[1:]
        if name == "":
            return z
        path = name if name.startswith("/") else "/usr/share/zoneinfo/" + name
    if _tzif(_read(path), z):
        return z
    if name is not None:
        r = _parse_rule(name)
        if r is not None:
            z.rules.append(r)
    return z


_zone: list[_Zone] = []


def _info(t: int) -> tuple[int, int, str]:
    """(UTC offset, DST flag, zone name) of local time at time t (seconds since 1970 UTC)."""
    if not _zone:
        _zone.append(_load())
    z = _zone[0]
    n = len(z.trans)
    if n == 0 or t >= z.trans[n - 1]:
        if z.rules:
            return _rule_info(z.rules[0], t)
        if n == 0:
            if not z.off:
                return (0, 0, "UTC")
            return (z.off[0], z.isdst[0], z.abbr[0])
    if t < z.trans[0]:
        k = 0
        for j in range(len(z.off)):
            if not z.isdst[j]:
                k = j
                break
        return (z.off[k], z.isdst[k], z.abbr[k])
    lo = 0
    hi = n
    while lo < hi:
        mid = (lo + hi) // 2
        if z.trans[mid] <= t:
            lo = mid + 1
        else:
            hi = mid
    k = z.idx[lo - 1]
    return (z.off[k], z.isdst[k], z.abbr[k])


def _zones() -> tuple[int, int, int, tuple[str, str]]:
    """(timezone, altzone, daylight, tzname) as CPython works them out: January and July of this year."""
    year = (365 * 24 + 6) * 3600
    t = _clock(0)[0] // year * year
    jo, jd, jn = _info(t)
    uo, ud, un = _info(t + year // 2)
    if -jo < -uo:
        return (-uo, -jo, 1 if jo != uo else 0, (un, jn))
    return (-jo, -uo, 1 if jo != uo else 0, (jn, un))


_zinfo: list[tuple[int, int, int, tuple[str, str]]] = []


def _zone_values() -> tuple[int, int, int, tuple[str, str]]:
    if not _zinfo:
        _zinfo.append(_zones())
    return _zinfo[0]


# timezone, altzone, daylight and tzname: worked out when first used (the compiler makes
# time.tzname a call of _lazy_tzname(); the interpreter asks __getattr__)
def _lazy_timezone() -> int:
    return _zone_values()[0]


def _lazy_altzone() -> int:
    return _zone_values()[1]


def _lazy_daylight() -> int:
    return _zone_values()[2]


def _lazy_tzname() -> tuple[str, str]:
    return _zone_values()[3]


if not sys._compiled:
    def __getattr__(name):
        if name == "timezone":
            return _lazy_timezone()
        if name == "altzone":
            return _lazy_altzone()
        if name == "daylight":
            return _lazy_daylight()
        if name == "tzname":
            return _lazy_tzname()
        raise AttributeError("module 'time' has no attribute '" + name + "'")


def tzset() -> None:
    """Read the TZ variable again."""
    del _zone[:]
    del _zinfo[:]


if not sys._compiled:                   # (CPython's are C functions: Formatter.converter = time.gmtime is not a method)
    for _f in (time, time_ns, monotonic, monotonic_ns, perf_counter, perf_counter_ns, process_time, process_time_ns,
               thread_time, thread_time_ns, clock_gettime, clock_gettime_ns, clock_getres, sleep, get_clock_info,
               gmtime, localtime, mktime, asctime, ctime, strftime, strptime, tzset):
        sys._builtin(_f)
    del _f
