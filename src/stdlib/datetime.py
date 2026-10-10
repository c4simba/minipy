"""Dates, times and durations (CPython's datetime): date, time, datetime, timedelta,
tzinfo, timezone; strftime/strptime, isoformat/fromisoformat, arithmetic.

The proleptic Gregorian calendar, years 1..9999; days are counted as ordinals
(0001-01-01 is day 1). Local time comes from the time module (TZ, /etc/localtime).

In compiled programs datetime is not a subclass of date (both derive from _YMD), so
a datetime is not accepted where a date is wanted; tzinfo subclasses work."""
import sys
import time as _time
from typing import TypeVar

_T = TypeVar("_T")
_U = TypeVar("_U")

__all__ = ["date", "datetime", "time", "timedelta", "timezone", "tzinfo", "MINYEAR", "MAXYEAR", "UTC"]

MINYEAR = 1
MAXYEAR = 9999
_MAXORDINAL = 3652059

_DAYS_IN_MONTH = [-1, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
_DAYS_BEFORE_MONTH = [-1, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334]
_MONTHNAMES = ["", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
_DAYNAMES = ["", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
_DI400Y = 146097
_DI100Y = 36524
_DI4Y = 1461


def _is_leap(year: int) -> bool:
    return year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)


def _days_before_year(year: int) -> int:
    y = year - 1
    return y * 365 + y // 4 - y // 100 + y // 400


def _days_in_month(year: int, month: int) -> int:
    if month == 2 and _is_leap(year):
        return 29
    return _DAYS_IN_MONTH[month]


def _days_before_month(year: int, month: int) -> int:
    return _DAYS_BEFORE_MONTH[month] + (1 if month > 2 and _is_leap(year) else 0)


def _ymd2ord(year: int, month: int, day: int) -> int:
    return _days_before_year(year) + _days_before_month(year, month) + day


def _ord2ymd(n: int) -> tuple[int, int, int]:
    """The (year, month, day) of ordinal n."""
    n -= 1
    n400 = n // _DI400Y
    n = n % _DI400Y
    year = n400 * 400 + 1
    n100 = n // _DI100Y
    n = n % _DI100Y
    n4 = n // _DI4Y
    n = n % _DI4Y
    n1 = n // 365
    n = n % 365
    year += n100 * 100 + n4 * 4 + n1
    if n1 == 4 or n100 == 4:
        return (year - 1, 12, 31)
    leapyear = n1 == 3 and (n4 != 24 or n100 == 3)
    month = (n + 50) >> 5
    preceding = _DAYS_BEFORE_MONTH[month] + (1 if month > 2 and leapyear else 0)
    if preceding > n:
        month -= 1
        preceding -= _DAYS_IN_MONTH[month] + (1 if month == 2 and leapyear else 0)
    n -= preceding
    return (year, month, n + 1)


def _isoweek1monday(year: int) -> int:
    """The ordinal of the Monday of ISO week 1 of year."""
    firstday = _ymd2ord(year, 1, 1)
    firstweekday = (firstday + 6) % 7
    week1monday = firstday - firstweekday
    if firstweekday > 3:
        week1monday += 7
    return week1monday


def _isoweek_to_gregorian(year: int, week: int, day: int) -> tuple[int, int, int]:
    if not MINYEAR <= year <= MAXYEAR:
        raise ValueError("year must be in " + str(MINYEAR) + ".." + str(MAXYEAR) + ", not " + str(year))
    if not 0 < week < 53:
        out_of_range = True
        if week == 53:
            first_weekday = _ymd2ord(year, 1, 1) % 7
            if first_weekday == 4 or (first_weekday == 3 and _is_leap(year)):
                out_of_range = False
        if out_of_range:
            raise ValueError("Invalid week: " + str(week))
    if not 0 < day < 8:
        raise ValueError("Invalid weekday: " + str(day) + " (range is [1, 7])")
    return _ord2ymd(_isoweek1monday(year) + (week - 1) * 7 + (day - 1))


def _pad(n: int, width: int) -> str:
    s = str(n)
    if len(s) < width:
        return "0" * (width - len(s)) + s
    return s


def _check_date_fields(year: int, month: int, day: int) -> None:
    if not MINYEAR <= year <= MAXYEAR:
        raise ValueError("year must be in " + str(MINYEAR) + ".." + str(MAXYEAR) + ", not " + str(year))
    if not 1 <= month <= 12:
        raise ValueError("month must be in 1..12, not " + str(month))
    dim = _days_in_month(year, month)
    if not 1 <= day <= dim:
        raise ValueError("day " + str(day) + " must be in range 1.." + str(dim) + " for month " + str(month) +
                         " in year " + str(year))


def _check_time_fields(hour: int, minute: int, second: int, microsecond: int, fold: int) -> None:
    if not 0 <= hour <= 23:
        raise ValueError("hour must be in 0..23, not " + str(hour))
    if not 0 <= minute <= 59:
        raise ValueError("minute must be in 0..59, not " + str(minute))
    if not 0 <= second <= 59:
        raise ValueError("second must be in 0..59, not " + str(second))
    if not 0 <= microsecond <= 999999:
        raise ValueError("microsecond must be in 0..999999, not " + str(microsecond))
    if fold != 0 and fold != 1:
        raise ValueError("fold must be either 0 or 1, not " + str(fold))


def _format_time(hh: int, mm: int, ss: int, us: int, timespec: str) -> str:
    if timespec == "auto":
        timespec = "microseconds" if us else "seconds"
    if timespec == "hours":
        return _pad(hh, 2)
    if timespec == "minutes":
        return _pad(hh, 2) + ":" + _pad(mm, 2)
    if timespec == "seconds":
        return _pad(hh, 2) + ":" + _pad(mm, 2) + ":" + _pad(ss, 2)
    if timespec == "milliseconds":
        return _pad(hh, 2) + ":" + _pad(mm, 2) + ":" + _pad(ss, 2) + "." + _pad(us // 1000, 3)
    if timespec == "microseconds":
        return _pad(hh, 2) + ":" + _pad(mm, 2) + ":" + _pad(ss, 2) + "." + _pad(us, 6)
    raise ValueError("Unknown timespec value")


def _tn(x: _T) -> str:
    """The type name CPython's messages give for x."""
    if isinstance(x, bool):
        return "bool"
    elif isinstance(x, int):
        return "int"
    elif isinstance(x, float):
        return "float"
    elif isinstance(x, str):
        return "str"
    elif isinstance(x, timedelta):
        return "datetime.timedelta"
    elif isinstance(x, datetime):
        return "datetime.datetime"
    elif isinstance(x, date):
        return "datetime.date"
    elif isinstance(x, time):
        return "datetime.time"
    else:
        return type(x).__name__


def _optype(op: str, a: _T, b: _U) -> TypeError:
    return TypeError("unsupported operand type(s) for " + op + ": '" + _tn(a) + "' and '" + _tn(b) + "'")


def _cmperror(op: str, a: _T, b: _U) -> TypeError:
    return TypeError("'" + op + "' not supported between instances of '" + _tn(a) + "' and '" + _tn(b) + "'")


def _sign(x: int) -> int:
    return (x > 0) - (x < 0)


class timedelta:
    """A duration: days, seconds (0..86399) and microseconds (0..999999), the
    arguments (weeks, days, hours, minutes, seconds, milliseconds, microseconds;
    numbers, maybe negative or fractional) summed up."""

    def __init__(self, days: float = 0, seconds: float = 0, microseconds: float = 0, milliseconds: float = 0,
                 minutes: float = 0, hours: float = 0, weeks: float = 0):
        d = days + weeks * 7
        s = seconds + minutes * 60 + hours * 3600
        us = microseconds + milliseconds * 1000
        di = int(d)
        si = int(s)
        ui = int(us)
        frac = (d - di) * 86400000000.0 + (s - si) * 1000000.0 + (us - ui)
        total = ui
        if frac != 0:
            total = round(ui + frac)
        carry = total // 1000000
        self._microseconds = total - carry * 1000000
        si += carry
        carry = si // 86400
        self._seconds = si - carry * 86400
        self._days = di + carry
        if abs(self._days) > 999999999:
            raise OverflowError("days=" + str(self._days) + "; must have magnitude <= 999999999")

    @property
    def days(self) -> int:
        return self._days

    @property
    def seconds(self) -> int:
        return self._seconds

    @property
    def microseconds(self) -> int:
        return self._microseconds

    def total_seconds(self) -> float:
        """The duration in seconds."""
        return self._to_microseconds() / 10 ** 6

    def _to_microseconds(self) -> int:
        return (self._days * 86400 + self._seconds) * 1000000 + self._microseconds

    def __repr__(self) -> str:
        args: list[str] = []
        if self._days:
            args.append("days=" + str(self._days))
        if self._seconds:
            args.append("seconds=" + str(self._seconds))
        if self._microseconds:
            args.append("microseconds=" + str(self._microseconds))
        if not args:
            args.append("0")
        return "datetime.timedelta(" + ", ".join(args) + ")"

    def __str__(self) -> str:
        mm = self._seconds // 60
        ss = self._seconds % 60
        hh = mm // 60
        mm = mm % 60
        s = str(hh) + ":" + _pad(mm, 2) + ":" + _pad(ss, 2)
        if self._days:
            s = str(self._days) + " day" + ("s" if abs(self._days) != 1 else "") + ", " + s
        if self._microseconds:
            s = s + "." + _pad(self._microseconds, 6)
        return s

    def __add__(self, other: _T):
        if isinstance(other, timedelta):
            return timedelta(self._days + other._days, self._seconds + other._seconds,
                             self._microseconds + other._microseconds)
        elif isinstance(other, datetime):
            return other + self
        elif isinstance(other, date):
            return other + self
        else:
            raise _optype("+", self, other)

    def __radd__(self, other: _T):
        return self + other

    def __sub__(self, other: _T) -> "timedelta":
        if isinstance(other, timedelta):
            return timedelta(self._days - other._days, self._seconds - other._seconds,
                             self._microseconds - other._microseconds)
        else:
            raise _optype("-", self, other)

    def __neg__(self) -> "timedelta":
        return timedelta(-self._days, -self._seconds, -self._microseconds)

    def __pos__(self) -> "timedelta":
        return self

    def __abs__(self) -> "timedelta":
        if self._days < 0:
            return -self
        return self

    def __mul__(self, other: _T) -> "timedelta":
        if isinstance(other, bool):
            return _from_us(self._to_microseconds() * int(other))
        elif isinstance(other, int):
            return timedelta(self._days * other, self._seconds * other, self._microseconds * other)
        elif isinstance(other, float):
            return _from_us(round(self._to_microseconds() * other))
        else:
            raise _optype("*", self, other)

    def __rmul__(self, other: _T) -> "timedelta":
        return self * other

    def __floordiv__(self, other: _T):
        if isinstance(other, timedelta):
            return self._to_microseconds() // other._to_microseconds()
        elif isinstance(other, int):
            return _from_us(self._to_microseconds() // other)
        else:
            raise _optype("//", self, other)

    def __truediv__(self, other: _T):
        if isinstance(other, timedelta):
            return self._to_microseconds() / other._to_microseconds()
        elif isinstance(other, int):
            return _from_us(_divide_and_round(self._to_microseconds(), other))
        elif isinstance(other, float):
            return _from_us(round(self._to_microseconds() / other))
        else:
            raise _optype("/", self, other)

    def __mod__(self, other: "timedelta") -> "timedelta":
        return _from_us(self._to_microseconds() % other._to_microseconds())

    def __divmod__(self, other: "timedelta") -> tuple[int, "timedelta"]:
        a = self._to_microseconds()
        b = other._to_microseconds()
        return (a // b, _from_us(a % b))

    def _cmp(self, other: "timedelta") -> int:
        if self._days != other._days:
            return -1 if self._days < other._days else 1
        if self._seconds != other._seconds:
            return -1 if self._seconds < other._seconds else 1
        if self._microseconds != other._microseconds:
            return -1 if self._microseconds < other._microseconds else 1
        return 0

    def __eq__(self, other: _T) -> bool:
        if isinstance(other, timedelta):
            return self._cmp(other) == 0
        else:
            return False

    def __ne__(self, other: _T) -> bool:
        return not self == other

    def __lt__(self, other: _T) -> bool:
        if isinstance(other, timedelta):
            return self._cmp(other) < 0
        else:
            raise _cmperror("<", self, other)

    def __le__(self, other: _T) -> bool:
        if isinstance(other, timedelta):
            return self._cmp(other) <= 0
        else:
            raise _cmperror("<=", self, other)

    def __gt__(self, other: _T) -> bool:
        if isinstance(other, timedelta):
            return self._cmp(other) > 0
        else:
            raise _cmperror(">", self, other)

    def __ge__(self, other: _T) -> bool:
        if isinstance(other, timedelta):
            return self._cmp(other) >= 0
        else:
            raise _cmperror(">=", self, other)

    def __hash__(self) -> int:
        return hash((self._days, self._seconds, self._microseconds))

    def __bool__(self) -> bool:
        return self._days != 0 or self._seconds != 0 or self._microseconds != 0


def _from_us(us: int) -> timedelta:
    """The timedelta of us microseconds."""
    d = us // 86400000000
    us -= d * 86400000000
    s = us // 1000000
    return timedelta(d, s, us - s * 1000000)


def _divide_and_round(a: int, b: int) -> int:
    """a / b rounded to the nearest integer (halves to even)."""
    q = a // b
    r = a - q * b
    r *= 2
    greater_than_half = r > b if b > 0 else r < b
    if greater_than_half or (r == b and q % 2 == 1):
        q += 1
    return q


timedelta.min = timedelta(-999999999)
timedelta.max = timedelta(days=999999999, hours=23, minutes=59, seconds=59, microseconds=999999)
timedelta.resolution = timedelta(microseconds=1)


def _format_offset(off: timedelta | None, sep: str) -> str:
    if off is None:
        return ""
    sign = "+"
    if off.days < 0:
        sign = "-"
        off = -off
    secs = off.days * 86400 + off.seconds
    s = sign + _pad(secs // 3600, 2) + sep + _pad(secs // 60 % 60, 2)
    if secs % 60 or off.microseconds:
        s += sep + _pad(secs % 60, 2)
        if off.microseconds:
            s += "." + _pad(off.microseconds, 6)
    return s


def _wrap_strftime(format: str, year: int, microsecond: int, off: timedelta | None, zone: str | None,
                   tt: _time.struct_time) -> str:
    """strftime with %f, %z, %:z and %Z done here (from the object), the rest by the time module."""
    out: list[str] = []
    i = 0
    n = len(format)
    while i < n:
        ch = format[i]
        i += 1
        if ch != "%":
            out.append(ch)
            continue
        if i >= n:
            out.append("%")
            continue
        ch = format[i]
        i += 1
        if ch == "f":
            out.append(_pad(microsecond, 6))
        elif ch == "z":
            out.append(_format_offset(off, ""))
        elif ch == ":":
            if i < n and format[i] == "z":
                i += 1
                out.append(_format_offset(off, ":"))
            elif i < n:
                out.append("%:" + format[i])
                i += 1
            else:
                out.append("%:")
        elif ch == "Z":
            if zone is not None:
                out.append(zone.replace("%", "%%"))
        elif (ch == "Y" or ch == "G" or ch == "F" or ch == "C") and year < 1000:
            y = year
            if ch == "G":
                y = int(_time._strftime("%G", tt))
            if ch == "C":
                out.append(_pad(y // 100, 2))
            else:
                out.append(_pad(y, 4))
                if ch == "F":
                    out.append("-" + _pad(tt.tm_mon, 2) + "-" + _pad(tt.tm_mday, 2))
        else:
            out.append("%" + ch)
    return _time._strftime("".join(out), tt)


def _struct(y: int, m: int, d: int, hh: int, mm: int, ss: int, dst: int) -> _time.struct_time:
    wday = (_ymd2ord(y, m, d) + 6) % 7
    return _time.struct_time((y, m, d, hh, mm, ss, wday, _days_before_month(y, m) + d, dst))


class IsoCalendarDate:
    """date.isocalendar(): year, week (1..53) and weekday (1..7, Monday 1); a 3-sequence."""

    def __init__(self, year: int, week: int, weekday: int):
        self.year = year
        self.week = week
        self.weekday = weekday

    def __getitem__(self, i: int) -> int:
        k = i + 3 if i < 0 else i
        if k == 0:
            return self.year
        if k == 1:
            return self.week
        if k == 2:
            return self.weekday
        raise IndexError("tuple index out of range")

    def __len__(self) -> int:
        return 3

    def __iter__(self):
        yield self.year
        yield self.week
        yield self.weekday

    def __eq__(self, other: _T) -> bool:
        if isinstance(other, IsoCalendarDate):
            return self.year == other.year and self.week == other.week and self.weekday == other.weekday
        elif isinstance(other, tuple):
            return (self.year, self.week, self.weekday) == other
        else:
            return False

    def __repr__(self) -> str:
        return ("datetime.IsoCalendarDate(year=" + str(self.year) + ", week=" + str(self.week) + ", weekday=" +
                str(self.weekday) + ")")


class _YMD:
    """What date and datetime share: year, month, day and the calendar."""

    def __init__(self, year: int, month: int, day: int):
        _check_date_fields(year, month, day)
        self._year = year
        self._month = month
        self._day = day

    @property
    def year(self) -> int:
        return self._year

    @property
    def month(self) -> int:
        return self._month

    @property
    def day(self) -> int:
        return self._day

    def toordinal(self) -> int:
        """The day number (0001-01-01 is 1)."""
        return _ymd2ord(self._year, self._month, self._day)

    def weekday(self) -> int:
        """Monday 0 ... Sunday 6."""
        return (self.toordinal() + 6) % 7

    def isoweekday(self) -> int:
        """Monday 1 ... Sunday 7."""
        return self.toordinal() % 7 or 7

    def isocalendar(self) -> IsoCalendarDate:
        """The ISO year, week number and weekday."""
        year = self._year
        week1monday = _isoweek1monday(year)
        today = _ymd2ord(self._year, self._month, self._day)
        week = (today - week1monday) // 7
        day = (today - week1monday) % 7
        if week < 0:
            year -= 1
            week1monday = _isoweek1monday(year)
            week = (today - week1monday) // 7
            day = (today - week1monday) % 7
        elif week >= 52:
            if today >= _isoweek1monday(year + 1):
                year += 1
                week = 0
        return IsoCalendarDate(year, week + 1, day + 1)

    def _date_iso(self) -> str:
        return _pad(self._year, 4) + "-" + _pad(self._month, 2) + "-" + _pad(self._day, 2)


class date(_YMD):
    """A day: year, month, day."""

    def __init__(self, year: int, month: int, day: int):
        super().__init__(year, month, day)

    @staticmethod
    def today() -> "date":
        """The local date now."""
        return date.fromtimestamp(_time.time())

    @staticmethod
    def fromtimestamp(t: float) -> "date":
        """The local date at POSIX timestamp t."""
        st = _time.localtime(t)
        return date(st.tm_year, st.tm_mon, st.tm_mday)

    @staticmethod
    def fromordinal(n: int) -> "date":
        """The date of day number n."""
        if n < 1:
            raise ValueError("ordinal must be >= 1")
        if n > _MAXORDINAL:
            raise ValueError("year " + str(_ord2ymd(n)[0]) + " is out of range")
        y, m, d = _ord2ymd(n)
        return date(y, m, d)

    @staticmethod
    def fromisoformat(date_string: str) -> "date":
        """The date of YYYY-MM-DD (or YYYYMMDD, YYYY-Www-D, ...)."""
        if len(date_string) not in (7, 8, 10):
            raise ValueError("Invalid isoformat string: " + repr(date_string))
        ymd = _parse_isoformat_date(date_string, date_string)
        return date(ymd[0], ymd[1], ymd[2])

    @staticmethod
    def fromisocalendar(year: int, week: int, day: int) -> "date":
        """The date of an ISO year, week and weekday."""
        y, m, d = _isoweek_to_gregorian(year, week, day)
        return date(y, m, d)

    @staticmethod
    def strptime(date_string: str, format: str) -> "date":
        """The date date_string stands for, read as format says."""
        st = _time._strptime(date_string, format)[0]
        return date(st.tm_year, st.tm_mon, st.tm_mday)

    def __repr__(self) -> str:
        return "datetime.date(" + str(self._year) + ", " + str(self._month) + ", " + str(self._day) + ")"

    def isoformat(self) -> str:
        """YYYY-MM-DD."""
        return self._date_iso()

    def __str__(self) -> str:
        return self._date_iso()

    def ctime(self) -> str:
        weekday = self.toordinal() % 7 or 7
        return (_DAYNAMES[weekday] + " " + _MONTHNAMES[self._month] + " " + ("%2d" % self._day) + " 00:00:00 " +
                _pad(self._year, 4))

    def timetuple(self) -> _time.struct_time:
        """The time.struct_time of the day's start (isdst -1)."""
        return _struct(self._year, self._month, self._day, 0, 0, 0, -1)

    def strftime(self, format: str) -> str:
        """The date as format says (time.strftime's directives; the time of day is 0)."""
        return _wrap_strftime(format, self._year, 0, None, None, self.timetuple())

    def __format__(self, fmt: str) -> str:
        if fmt:
            return self.strftime(fmt)
        return str(self)

    def replace(self, year: int | None = None, month: int | None = None, day: int | None = None) -> "date":
        """A copy with some fields changed."""
        return date(self._year if year is None else year, self._month if month is None else month,
                    self._day if day is None else day)

    def _cmp(self, other: "date") -> int:
        a = (self._year, self._month, self._day)
        b = (other._year, other._month, other._day)
        return 0 if a == b else 1 if a > b else -1

    def __eq__(self, other: _T) -> bool:
        if isinstance(other, datetime):
            return False
        elif isinstance(other, date):
            return self._cmp(other) == 0
        else:
            return False

    def __ne__(self, other: _T) -> bool:
        return not self == other

    def __lt__(self, other: _T) -> bool:
        if isinstance(other, datetime):
            raise _cmperror("<", self, other)
        elif isinstance(other, date):
            return self._cmp(other) < 0
        else:
            raise _cmperror("<", self, other)

    def __le__(self, other: _T) -> bool:
        if isinstance(other, datetime):
            raise _cmperror("<=", self, other)
        elif isinstance(other, date):
            return self._cmp(other) <= 0
        else:
            raise _cmperror("<=", self, other)

    def __gt__(self, other: _T) -> bool:
        if isinstance(other, datetime):
            raise _cmperror(">", self, other)
        elif isinstance(other, date):
            return self._cmp(other) > 0
        else:
            raise _cmperror(">", self, other)

    def __ge__(self, other: _T) -> bool:
        if isinstance(other, datetime):
            raise _cmperror(">=", self, other)
        elif isinstance(other, date):
            return self._cmp(other) >= 0
        else:
            raise _cmperror(">=", self, other)

    def __hash__(self) -> int:
        return hash((self._year, self._month, self._day))

    def __add__(self, other: _T) -> "date":
        if isinstance(other, timedelta):
            o = self.toordinal() + other.days
            if 0 < o <= _MAXORDINAL:
                return date.fromordinal(o)
            raise OverflowError("date value out of range")
        else:
            raise _optype("+", self, other)

    def __radd__(self, other: _T) -> "date":
        return self + other

    def __sub__(self, other: _T):
        if isinstance(other, timedelta):
            return self + timedelta(-other.days)
        elif isinstance(other, datetime):
            raise _optype("-", self, other)
        elif isinstance(other, date):
            return timedelta(self.toordinal() - other.toordinal())
        else:
            raise _optype("-", self, other)


date.min = date(1, 1, 1)
date.max = date(9999, 12, 31)
date.resolution = timedelta(days=1)


def _parse_isoformat_date(s: str, whole: str) -> list[int]:
    """[year, month, day] of YYYY-MM-DD, YYYYMMDD, YYYY-Www[-D], YYYYWww[D]."""
    if not s.isascii() or len(s) < 7:
        raise ValueError("Invalid isoformat string: " + repr(whole))
    has_sep = s[4] == "-"
    pos = 5 if has_sep else 4
    if not _digits(s[0:4]):
        raise ValueError("Invalid isoformat string: " + repr(whole))
    year = int(s[0:4])
    if s[pos:pos + 1] == "W":
        pos += 1
        if not _digits(s[pos:pos + 2]):
            raise ValueError("Invalid isoformat string: " + repr(whole))
        weekno = int(s[pos:pos + 2])
        pos += 2
        dayno = 1
        if len(s) > pos:
            if (s[pos:pos + 1] == "-") != has_sep:
                raise ValueError("Invalid isoformat string: " + repr(whole))
            if has_sep:
                pos += 1
            if not _digits(s[pos:pos + 1]) or len(s) != pos + 1:
                raise ValueError("Invalid isoformat string: " + repr(whole))
            dayno = int(s[pos:pos + 1])
        y, m, d = _isoweek_to_gregorian(year, weekno, dayno)
        return [y, m, d]
    if not _digits(s[pos:pos + 2]):
        raise ValueError("Invalid isoformat string: " + repr(whole))
    month = int(s[pos:pos + 2])
    pos += 2
    if (s[pos:pos + 1] == "-") != has_sep:
        raise ValueError("Invalid isoformat string: " + repr(whole))
    if has_sep:
        pos += 1
    if not _digits(s[pos:pos + 2]) or len(s) != pos + 2:
        raise ValueError("Invalid isoformat string: " + repr(whole))
    return [year, month, int(s[pos:pos + 2])]


def _digits(s: str) -> bool:
    if not s:
        return False
    for c in s:
        if c < "0" or c > "9":
            return False
    return True


def _parse_hh_mm_ss_ff(tstr: str, whole: str) -> list[int]:
    """[hour, minute, second, microsecond] of HH[:MM[:SS[.ffffff]]] (or without colons)."""
    len_str = len(tstr)
    comps = [0, 0, 0, 0]
    pos = 0
    has_sep = False
    for comp in range(3):
        if len_str - pos < 2 or not _digits(tstr[pos:pos + 2]):
            raise ValueError("Invalid isoformat string: " + repr(whole))
        comps[comp] = int(tstr[pos:pos + 2])
        pos += 2
        next_char = tstr[pos:pos + 1]
        if comp == 0:
            has_sep = next_char == ":"
        if not next_char or comp >= 2:
            break
        if has_sep and next_char != ":":
            raise ValueError("Invalid isoformat string: " + repr(whole))
        if has_sep:
            pos += 1
    if pos < len_str:
        if tstr[pos] not in ".,":
            raise ValueError("Invalid isoformat string: " + repr(whole))
        pos += 1
        if not _digits(tstr[pos:]):
            raise ValueError("Invalid isoformat string: " + repr(whole))
        frac = tstr[pos:pos + 6]
        comps[3] = int(frac + "0" * (6 - len(frac)))
    return comps


class _ParsedTime:
    def __init__(self, comps: list[int], tz: "tzinfo | None", next_day: bool, bad24: bool):
        self.comps = comps
        self.tz = tz
        self.next_day = next_day
        self.bad24 = bad24


def _parse_isoformat_time(tstr: str, whole: str) -> _ParsedTime:
    """HH[:MM[:SS[.fff[fff]]]][+HH:MM[:SS[.ffffff]]] or ...Z."""
    len_str = len(tstr)
    if len_str < 2:
        raise ValueError("Invalid isoformat string: " + repr(whole))
    tz_pos = tstr.find("-") + 1 or tstr.find("+") + 1 or tstr.find("Z") + 1
    timestr = tstr[:tz_pos - 1] if tz_pos > 0 else tstr
    comps = _parse_hh_mm_ss_ff(timestr, whole)
    next_day = False
    bad24 = False
    if comps[0] == 24:
        if comps[1] == 0 and comps[2] == 0 and comps[3] == 0:
            comps[0] = 0
            next_day = True
        else:
            bad24 = True
    tzi: tzinfo | None = None
    if tz_pos == len_str and tstr[-1] == "Z":
        tzi = timezone.utc
    elif tz_pos > 0:
        tzstr = tstr[tz_pos:]
        if len(tzstr) in (0, 1, 3) or tstr[tz_pos - 1] == "Z":
            raise ValueError("Invalid isoformat string: " + repr(whole))
        tzc = _parse_hh_mm_ss_ff(tzstr, whole)
        if tzc[0] == 0 and tzc[1] == 0 and tzc[2] == 0 and tzc[3] == 0:
            tzi = timezone.utc
        else:
            td = timedelta(hours=tzc[0], minutes=tzc[1], seconds=tzc[2], microseconds=tzc[3])
            if tstr[tz_pos - 1] == "-":
                td = -td
            tzi = timezone(td)
    return _ParsedTime(comps, tzi, next_day, bad24)


class tzinfo:
    """A time zone: its offset from UTC (utcoffset), DST adjustment (dst) and name
    (tzname) at a datetime. Subclasses define them; timezone is a fixed offset."""

    def tzname(self, dt: "datetime | None") -> str | None:
        """The name of the zone at dt."""
        raise NotImplementedError("tzinfo subclass must override tzname()")

    def utcoffset(self, dt: "datetime | None") -> timedelta | None:
        """The local time minus UTC at dt."""
        raise NotImplementedError("tzinfo subclass must override utcoffset()")

    def dst(self, dt: "datetime | None") -> timedelta | None:
        """The DST adjustment at dt (included in utcoffset)."""
        raise NotImplementedError("tzinfo subclass must override dst()")

    def fromutc(self, dt: "datetime") -> "datetime":
        """dt (UTC, with this tzinfo) as local time."""
        dtoff = dt.utcoffset()
        if dtoff is None:
            raise ValueError("fromutc() requires a non-None utcoffset() result")
        dtdst = dt.dst()
        if dtdst is None:
            raise ValueError("fromutc() requires a non-None dst() result")
        delta = dtoff - dtdst
        if delta:
            dt = dt + delta
            dtdst = dt.dst()
            if dtdst is None:
                raise ValueError("fromutc(): dt.dst gave inconsistent results; cannot convert")
        return dt + dtdst


class timezone(tzinfo):
    """A fixed offset from UTC (strictly between -24 and 24 hours), maybe with a name."""

    def __init__(self, offset: timedelta, name: str | None = None):
        if not timedelta(hours=-24) < offset < timedelta(hours=24):
            raise ValueError("offset must be a timedelta strictly between -timedelta(hours=24) and "
                             "timedelta(hours=24), not " + repr(offset))
        self._offset = offset
        self._name = name

    def __eq__(self, other: _T) -> bool:
        if isinstance(other, timezone):
            return self._offset == other._offset
        else:
            return False

    def __hash__(self) -> int:
        return hash(self._offset)

    def __repr__(self) -> str:
        if self is timezone.utc:
            return "datetime.timezone.utc"
        if self._name is None:
            return "datetime.timezone(" + repr(self._offset) + ")"
        return "datetime.timezone(" + repr(self._offset) + ", " + repr(self._name) + ")"

    def __str__(self) -> str:
        return self._tzname()

    def utcoffset(self, dt: "datetime | None") -> timedelta | None:
        return self._offset

    def tzname(self, dt: "datetime | None") -> str | None:
        return self._tzname()

    def _tzname(self) -> str:
        name = self._name
        if name is None:
            return _name_from_offset(self._offset)
        return name

    def dst(self, dt: "datetime | None") -> timedelta | None:
        return None

    def fromutc(self, dt: "datetime") -> "datetime":
        if dt.tzinfo is not self:
            raise ValueError("fromutc: dt.tzinfo is not self")
        return dt + self._offset


def _name_from_offset(delta: timedelta) -> str:
    if not delta:
        return "UTC"
    sign = "+"
    if delta < timedelta(0):
        sign = "-"
        delta = -delta
    secs = delta.days * 86400 + delta.seconds
    s = "UTC" + sign + _pad(secs // 3600, 2) + ":" + _pad(secs // 60 % 60, 2)
    if secs % 60 or delta.microseconds:
        s += ":" + _pad(secs % 60, 2)
        if delta.microseconds:
            s += "." + _pad(delta.microseconds, 6)
    return s


timezone.utc = timezone(timedelta(0))
timezone.min = timezone(-timedelta(hours=23, minutes=59))
timezone.max = timezone(timedelta(hours=23, minutes=59))
UTC = timezone.utc


def _check_utc_offset(name: str, offset: timedelta | None) -> None:
    if offset is not None and not -timedelta(1) < offset < timedelta(1):
        raise ValueError("offset must be a timedelta strictly between -timedelta(hours=24) and "
                         "timedelta(hours=24), not " + repr(offset))


def _check_tzinfo_arg(tz: _T) -> None:
    if not sys._compiled:
        if tz is not None and not isinstance(tz, tzinfo):
            raise TypeError("tzinfo argument must be None or of a tzinfo subclass, not " + repr(type(tz).__name__))


class time:
    """A time of day: hour, minute, second, microsecond, tzinfo (or None), fold."""

    def __init__(self, hour: int = 0, minute: int = 0, second: int = 0, microsecond: int = 0,
                 tzinfo: "tzinfo | None" = None, *, fold: int = 0):
        _check_time_fields(hour, minute, second, microsecond, fold)
        _check_tzinfo_arg(tzinfo)
        self._hour = hour
        self._minute = minute
        self._second = second
        self._microsecond = microsecond
        self._tzinfo = tzinfo
        self._fold = fold

    @property
    def hour(self) -> int:
        return self._hour

    @property
    def minute(self) -> int:
        return self._minute

    @property
    def second(self) -> int:
        return self._second

    @property
    def microsecond(self) -> int:
        return self._microsecond

    @property
    def tzinfo(self) -> "tzinfo | None":
        return self._tzinfo

    @property
    def fold(self) -> int:
        return self._fold

    @staticmethod
    def fromisoformat(time_string: str) -> "time":
        """The time of HH[:MM[:SS[.ffffff]]][+HH:MM] (a leading T is allowed)."""
        s = time_string[1:] if time_string.startswith("T") else time_string
        p = _parse_isoformat_time(s, s)
        if p.bad24:
            raise ValueError("Invalid isoformat string: " + repr(s))
        return time(p.comps[0], p.comps[1], p.comps[2], p.comps[3], p.tz)

    @staticmethod
    def strptime(time_string: str, format: str) -> "time":
        """The time time_string stands for, read as format says."""
        r = _time._strptime(time_string, format)
        st = r[0]
        return time(st.tm_hour, st.tm_min, st.tm_sec, r[1], _strptime_tz(st, r[2]))

    def utcoffset(self) -> timedelta | None:
        if self._tzinfo is None:
            return None
        off = self._tzinfo.utcoffset(None)
        _check_utc_offset("utcoffset", off)
        return off

    def dst(self) -> timedelta | None:
        if self._tzinfo is None:
            return None
        off = self._tzinfo.dst(None)
        _check_utc_offset("dst", off)
        return off

    def tzname(self) -> str | None:
        if self._tzinfo is None:
            return None
        return self._tzinfo.tzname(None)

    def _key(self) -> tuple[int, int, int, int]:
        return (self._hour, self._minute, self._second, self._microsecond)

    def _cmp(self, other: "time", op: str) -> int:
        mytz = self._tzinfo
        ottz = other._tzinfo
        myoff: timedelta | None = None
        otoff: timedelta | None = None
        base_compare = mytz is ottz
        if not base_compare:
            myoff = self.utcoffset()
            otoff = other.utcoffset()
            base_compare = myoff == otoff
        if base_compare:
            a = self._key()
            b = other._key()
            return 0 if a == b else 1 if a > b else -1
        if myoff is None or otoff is None:
            if op == "==":
                return 2
            raise TypeError("can't compare offset-naive and offset-aware times")
        myhhmm = self._hour * 60 + self._minute - myoff // timedelta(minutes=1)
        othhmm = other._hour * 60 + other._minute - otoff // timedelta(minutes=1)
        a2 = (myhhmm, self._second, self._microsecond)
        b2 = (othhmm, other._second, other._microsecond)
        return 0 if a2 == b2 else 1 if a2 > b2 else -1

    def __eq__(self, other: _T) -> bool:
        if isinstance(other, time):
            return self._cmp(other, "==") == 0
        else:
            return False

    def __ne__(self, other: _T) -> bool:
        return not self == other

    def __lt__(self, other: "time") -> bool:
        return self._cmp(other, "<") < 0

    def __le__(self, other: "time") -> bool:
        return self._cmp(other, "<=") <= 0

    def __gt__(self, other: "time") -> bool:
        return self._cmp(other, ">") > 0

    def __ge__(self, other: "time") -> bool:
        return self._cmp(other, ">=") >= 0

    def __hash__(self) -> int:
        return hash(self._key())

    def __repr__(self) -> str:
        s = ""
        if self._microsecond != 0:
            s = ", " + str(self._second) + ", " + str(self._microsecond)
        elif self._second != 0:
            s = ", " + str(self._second)
        s = "datetime.time(" + str(self._hour) + ", " + str(self._minute) + s + ")"
        if self._tzinfo is not None:
            s = s[:-1] + ", tzinfo=" + repr(self._tzinfo) + ")"
        if self._fold:
            s = s[:-1] + ", fold=1)"
        return s

    def isoformat(self, timespec: str = "auto") -> str:
        """HH:MM:SS[.ffffff][+HH:MM] (timespec: auto, hours, minutes, seconds, milliseconds, microseconds)."""
        s = _format_time(self._hour, self._minute, self._second, self._microsecond, timespec)
        return s + _format_offset(self.utcoffset(), ":")

    def __str__(self) -> str:
        return self.isoformat()

    def strftime(self, format: str) -> str:
        """The time as format says (the date is 1900-01-01)."""
        tt = _time.struct_time((1900, 1, 1, self._hour, self._minute, self._second, 0, 1, -1))
        return _wrap_strftime(format, 1900, self._microsecond, self.utcoffset(), self.tzname(), tt)

    def __format__(self, fmt: str) -> str:
        if fmt:
            return self.strftime(fmt)
        return str(self)

    def replace(self, hour: int | None = None, minute: int | None = None, second: int | None = None,
                microsecond: int | None = None, tzinfo: _T = True, *, fold: int | None = None) -> "time":
        """A copy with some fields changed."""
        if isinstance(tzinfo, bool):
            tz = self._tzinfo
        else:
            tz = tzinfo
        return time(self._hour if hour is None else hour, self._minute if minute is None else minute,
                    self._second if second is None else second,
                    self._microsecond if microsecond is None else microsecond, tz,
                    fold=self._fold if fold is None else fold)


time.min = time(0, 0, 0)
time.max = time(23, 59, 59, 999999)
time.resolution = timedelta(microseconds=1)


def _strptime_tz(st: _time.struct_time, gmtoff_fraction: int) -> tzinfo | None:
    """The timezone of a parsed %z (named by %Z when there is one)."""
    off = st.tm_gmtoff
    if off is None:
        return None
    td = timedelta(seconds=off, microseconds=gmtoff_fraction)
    name = st.tm_zone
    if name:
        return timezone(td, name)
    return timezone(td)


if sys._compiled:
    _DateParent = _YMD
else:
    _DateParent = date


class datetime(_DateParent):
    """A date and a time of day: year, month, day, hour, minute, second, microsecond,
    tzinfo (None: naive, local time) and fold."""

    def __init__(self, year: int, month: int, day: int, hour: int = 0, minute: int = 0, second: int = 0,
                 microsecond: int = 0, tzinfo: "tzinfo | None" = None, *, fold: int = 0):
        _YMD.__init__(self, year, month, day)
        _check_time_fields(hour, minute, second, microsecond, fold)
        _check_tzinfo_arg(tzinfo)
        self._hour = hour
        self._minute = minute
        self._second = second
        self._microsecond = microsecond
        self._tzinfo = tzinfo
        self._fold = fold

    @property
    def hour(self) -> int:
        return self._hour

    @property
    def minute(self) -> int:
        return self._minute

    @property
    def second(self) -> int:
        return self._second

    @property
    def microsecond(self) -> int:
        return self._microsecond

    @property
    def tzinfo(self) -> "tzinfo | None":
        return self._tzinfo

    @property
    def fold(self) -> int:
        return self._fold

    @staticmethod
    def _fromtimestamp(t: float, utc: bool, tz: "tzinfo | None") -> "datetime":
        secs = int(t)
        if secs > t:
            secs -= 1
        us = round((t - secs) * 1e6)
        if us >= 1000000:
            secs += 1
            us -= 1000000
        st = _time.gmtime(secs) if utc else _time.localtime(secs)
        result = datetime(st.tm_year, st.tm_mon, st.tm_mday, st.tm_hour, st.tm_min, min(st.tm_sec, 59), us, tz)
        if tz is not None:
            result = tz.fromutc(result)
        return result

    @staticmethod
    def fromtimestamp(t: float, tz: "tzinfo | None" = None) -> "datetime":
        """The datetime at POSIX timestamp t: local (naive) without tz, else in tz."""
        return datetime._fromtimestamp(t, tz is not None, tz)

    @staticmethod
    def utcfromtimestamp(t: float) -> "datetime":
        """The naive UTC datetime at timestamp t (deprecated: use fromtimestamp(t, timezone.utc))."""
        return datetime._fromtimestamp(t, True, None)

    @staticmethod
    def now(tz: "tzinfo | None" = None) -> "datetime":
        """The current local datetime (naive), or the time in tz."""
        return datetime.fromtimestamp(_time.time(), tz)

    @staticmethod
    def utcnow() -> "datetime":
        """The current UTC datetime, naive (deprecated: use now(timezone.utc))."""
        return datetime.utcfromtimestamp(_time.time())

    @staticmethod
    def today() -> "datetime":
        """The current local datetime."""
        return datetime.fromtimestamp(_time.time())

    @staticmethod
    def fromordinal(n: int) -> "datetime":
        """Midnight of day number n."""
        d = date.fromordinal(n)
        return datetime(d.year, d.month, d.day)

    @staticmethod
    def fromisocalendar(year: int, week: int, day: int) -> "datetime":
        y, m, d = _isoweek_to_gregorian(year, week, day)
        return datetime(y, m, d)

    @staticmethod
    def combine(date: "date", time: "time", tzinfo: _T = True) -> "datetime":
        """The datetime of a date and a time (with time's tzinfo, unless tzinfo is given)."""
        if isinstance(tzinfo, bool):
            tz = time.tzinfo
        else:
            tz = tzinfo
        return datetime(date.year, date.month, date.day, time.hour, time.minute, time.second, time.microsecond, tz,
                        fold=time.fold)

    @staticmethod
    def fromisoformat(date_string: str) -> "datetime":
        """The datetime of an ISO 8601 text: YYYY-MM-DD[*HH[:MM[:SS[.fff[fff]]]][+HH:MM[:SS[.ffffff]]]]."""
        if len(date_string) < 7:
            raise ValueError("Invalid isoformat string: " + repr(date_string))
        sep = _find_isoformat_datetime_separator(date_string)
        dstr = date_string[0:sep]
        tstr = date_string[sep + 1:]
        dc = _parse_isoformat_date(dstr, date_string)
        if len(date_string) > sep and not tstr:
            raise ValueError("Invalid isoformat string: " + repr(date_string))
        if tstr:
            p = _parse_isoformat_time(tstr, date_string)
            if p.bad24:
                raise ValueError("minute, second, and microsecond must be 0 when hour is 24")
            year = dc[0]
            month = dc[1]
            day = dc[2]
            if p.next_day and month <= 12 and day <= _days_in_month(year, month):
                day += 1
                if day > _days_in_month(year, month):
                    day = 1
                    month += 1
                    if month > 12:
                        month = 1
                        year += 1
            return datetime(year, month, day, p.comps[0], p.comps[1], p.comps[2], p.comps[3], p.tz)
        return datetime(dc[0], dc[1], dc[2])

    @staticmethod
    def strptime(date_string: str, format: str) -> "datetime":
        """The datetime date_string stands for, read as format says (%f microseconds, %z offset)."""
        r = _time._strptime(date_string, format)
        st = r[0]
        return datetime(st.tm_year, st.tm_mon, st.tm_mday, st.tm_hour, st.tm_min, st.tm_sec, r[1],
                        _strptime_tz(st, r[2]))

    def timetuple(self) -> _time.struct_time:
        """The time.struct_time (isdst from dst(); -1 when naive)."""
        dst = self.dst()
        flag = -1
        if dst is not None:
            flag = 1 if dst else 0
        return _struct(self._year, self._month, self._day, self._hour, self._minute, self._second, flag)

    def _mktime(self) -> int:
        """The POSIX timestamp of this naive (local) datetime."""
        st = _struct(self._year, self._month, self._day, self._hour, self._minute, self._second, -1)
        return int(_time.mktime(st))

    def timestamp(self) -> float:
        """The POSIX timestamp (a naive datetime is local time)."""
        if self._tzinfo is None:
            return self._mktime() + self._microsecond / 1e6
        return (self - _EPOCH).total_seconds()

    def utctimetuple(self) -> _time.struct_time:
        """The time.struct_time in UTC (isdst 0)."""
        offset = self.utcoffset()
        dt = self
        if offset:
            dt = self - offset
        return _struct(dt._year, dt._month, dt._day, dt._hour, dt._minute, dt._second, 0)

    def date(self) -> "date":
        return date(self._year, self._month, self._day)

    def time(self) -> "time":
        return time(self._hour, self._minute, self._second, self._microsecond, fold=self._fold)

    def timetz(self) -> "time":
        return time(self._hour, self._minute, self._second, self._microsecond, self._tzinfo, fold=self._fold)

    def replace(self, year: int | None = None, month: int | None = None, day: int | None = None,
                hour: int | None = None, minute: int | None = None, second: int | None = None,
                microsecond: int | None = None, tzinfo: _T = True, *, fold: int | None = None) -> "datetime":
        """A copy with some fields changed."""
        if isinstance(tzinfo, bool):
            tz = self._tzinfo
        else:
            tz = tzinfo
        return datetime(self._year if year is None else year, self._month if month is None else month,
                        self._day if day is None else day, self._hour if hour is None else hour,
                        self._minute if minute is None else minute, self._second if second is None else second,
                        self._microsecond if microsecond is None else microsecond, tz,
                        fold=self._fold if fold is None else fold)

    def _local_timezone(self) -> tzinfo:
        ts = 0
        if self._tzinfo is None:
            ts = self._mktime()
        else:
            ts = (self - _EPOCH) // timedelta(seconds=1)
        st = _time.localtime(ts)
        off = st.tm_gmtoff
        zone = st.tm_zone
        return timezone(timedelta(seconds=0 if off is None else off), zone)

    def astimezone(self, tz: "tzinfo | None" = None) -> "datetime":
        """The same moment in tz (by default the local zone)."""
        if tz is None:
            tz = self._local_timezone()
        mytz = self._tzinfo
        myoffset: timedelta | None = None
        if mytz is None:
            mytz = self._local_timezone()
            myoffset = mytz.utcoffset(self)
        else:
            myoffset = mytz.utcoffset(self)
            if myoffset is None:
                mytz = self.replace(tzinfo=None)._local_timezone()
                myoffset = mytz.utcoffset(self)
        if tz is mytz:
            return self
        if myoffset is None:
            raise ValueError("astimezone() cannot be applied to a naive datetime")
        utc = (self - myoffset).replace(tzinfo=tz)
        return tz.fromutc(utc)

    def ctime(self) -> str:
        weekday = self.toordinal() % 7 or 7
        return (_DAYNAMES[weekday] + " " + _MONTHNAMES[self._month] + " " + ("%2d" % self._day) + " " +
                _pad(self._hour, 2) + ":" + _pad(self._minute, 2) + ":" + _pad(self._second, 2) + " " +
                _pad(self._year, 4))

    def isoformat(self, sep: str = "T", timespec: str = "auto") -> str:
        """YYYY-MM-DDTHH:MM:SS[.ffffff][+HH:MM]."""
        s = (self._date_iso() + sep +
             _format_time(self._hour, self._minute, self._second, self._microsecond, timespec))
        return s + _format_offset(self.utcoffset(), ":")

    def __str__(self) -> str:
        return self.isoformat(" ")

    def __repr__(self) -> str:
        parts = [self._year, self._month, self._day, self._hour, self._minute, self._second, self._microsecond]
        if parts[-1] == 0:
            del parts[-1]
            if parts[-1] == 0:
                del parts[-1]
        s = "datetime.datetime(" + ", ".join([str(x) for x in parts]) + ")"
        if self._tzinfo is not None:
            s = s[:-1] + ", tzinfo=" + repr(self._tzinfo) + ")"
        if self._fold:
            s = s[:-1] + ", fold=1)"
        return s

    def strftime(self, format: str) -> str:
        """The datetime as format says (time.strftime's directives, %f microseconds, %z, %:z, %Z)."""
        return _wrap_strftime(format, self._year, self._microsecond, self.utcoffset(), self.tzname(),
                              self.timetuple())

    def __format__(self, fmt: str) -> str:
        if fmt:
            return self.strftime(fmt)
        return str(self)

    def utcoffset(self) -> timedelta | None:
        if self._tzinfo is None:
            return None
        off = self._tzinfo.utcoffset(self)
        _check_utc_offset("utcoffset", off)
        return off

    def tzname(self) -> str | None:
        if self._tzinfo is None:
            return None
        return self._tzinfo.tzname(self)

    def dst(self) -> timedelta | None:
        if self._tzinfo is None:
            return None
        off = self._tzinfo.dst(self)
        _check_utc_offset("dst", off)
        return off

    def _key(self) -> tuple[int, int, int, int, int, int, int]:
        return (self._year, self._month, self._day, self._hour, self._minute, self._second, self._microsecond)

    def _cmp(self, other: "datetime", op: str) -> int:
        mytz = self._tzinfo
        ottz = other._tzinfo
        myoff: timedelta | None = None
        otoff: timedelta | None = None
        base_compare = mytz is ottz
        if not base_compare:
            myoff = self.utcoffset()
            otoff = other.utcoffset()
            base_compare = myoff == otoff
        if base_compare:
            a = self._key()
            b = other._key()
            return 0 if a == b else 1 if a > b else -1
        if myoff is None or otoff is None:
            if op == "==":
                return 2
            raise TypeError("can't compare offset-naive and offset-aware datetimes")
        diff = self - other
        if diff.days < 0:
            return -1
        return 1 if diff else 0

    def __eq__(self, other: _T) -> bool:
        if isinstance(other, datetime):
            return self._cmp(other, "==") == 0
        else:
            return False

    def __ne__(self, other: _T) -> bool:
        return not self == other

    def __lt__(self, other: _T) -> bool:
        if isinstance(other, datetime):
            return self._cmp(other, "<") < 0
        else:
            raise _cmperror("<", self, other)

    def __le__(self, other: _T) -> bool:
        if isinstance(other, datetime):
            return self._cmp(other, "<=") <= 0
        else:
            raise _cmperror("<=", self, other)

    def __gt__(self, other: _T) -> bool:
        if isinstance(other, datetime):
            return self._cmp(other, ">") > 0
        else:
            raise _cmperror(">", self, other)

    def __ge__(self, other: _T) -> bool:
        if isinstance(other, datetime):
            return self._cmp(other, ">=") >= 0
        else:
            raise _cmperror(">=", self, other)

    def __hash__(self) -> int:
        off = self.utcoffset()
        if off is None:
            return hash(self._key())
        t = self - off
        return hash(t._key())

    def __add__(self, other: _T) -> "datetime":
        if isinstance(other, timedelta):
            days = self.toordinal() + other.days
            secs = self._hour * 3600 + self._minute * 60 + self._second + other.seconds
            us = self._microsecond + other.microseconds
            secs += us // 1000000
            us = us % 1000000
            days += secs // 86400
            secs = secs % 86400
            if 0 < days <= _MAXORDINAL:
                y, m, d = _ord2ymd(days)
                return datetime(y, m, d, secs // 3600, secs // 60 % 60, secs % 60, us, self._tzinfo)
            raise OverflowError("date value out of range")
        else:
            raise _optype("+", self, other)

    def __radd__(self, other: _T) -> "datetime":
        return self + other

    def __sub__(self, other: _T):
        if isinstance(other, timedelta):
            return self + -other
        elif isinstance(other, datetime):
            days1 = self.toordinal()
            days2 = other.toordinal()
            secs1 = self._second + self._minute * 60 + self._hour * 3600
            secs2 = other._second + other._minute * 60 + other._hour * 3600
            base = timedelta(days1 - days2, secs1 - secs2, self._microsecond - other._microsecond)
            if self._tzinfo is other._tzinfo:
                return base
            myoff = self.utcoffset()
            otoff = other.utcoffset()
            if myoff == otoff:
                return base
            if myoff is None or otoff is None:
                raise TypeError("can't subtract offset-naive and offset-aware datetimes")
            return base + otoff - myoff
        else:
            raise _optype("-", self, other)


def _find_isoformat_datetime_separator(dtstr: str) -> int:
    len_dtstr = len(dtstr)
    if len_dtstr == 7:
        return 7
    if dtstr[4] == "-":
        if dtstr[5] == "W":
            if len_dtstr < 8:
                raise ValueError("Invalid isoformat string: " + repr(dtstr))
            if len_dtstr > 8 and dtstr[8] == "-":
                if len_dtstr == 9:
                    raise ValueError("Invalid isoformat string: " + repr(dtstr))
                if len_dtstr > 10 and dtstr[10] in "0123456789":
                    return 8
                return 10
            return 8
        return 10
    if dtstr[4] == "W":
        idx = 7
        while idx < len_dtstr:
            if dtstr[idx] not in "0123456789":
                break
            idx += 1
        if idx < 9:
            return idx
        if idx % 2 == 0:
            return 7
        return 8
    return 8


datetime.min = datetime(1, 1, 1)
datetime.max = datetime(9999, 12, 31, 23, 59, 59, 999999)
datetime.resolution = timedelta(microseconds=1)
_EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)
