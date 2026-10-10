"""Calendar printing functions (CPython's calendar).

Note when comparing these calendars to the ones printed by cal(1): By default, these calendars
have Monday as the first day of the week, and Sunday as the last (the European convention).
Use setfirstweekday() to set the first day of the week (0=Monday, 6=Sunday).

Names are English (the C locale's). In compiled programs MONDAY ... SUNDAY and JANUARY ...
DECEMBER are ints (CPython's are IntEnum members: Day, Month), month_name / day_name take
an index (not a slice), and LocaleTextCalendar / LocaleHTMLCalendar use the English names."""
import sys
import datetime
from typing import Iterator, TypeVar

_T = TypeVar("_T")

__all__ = ["IllegalMonthError", "IllegalWeekdayError", "setfirstweekday", "firstweekday", "isleap", "leapdays",
           "weekday", "monthrange", "monthcalendar", "prmonth", "month", "prcal", "calendar", "timegm",
           "month_name", "month_abbr", "day_name", "day_abbr", "Calendar", "TextCalendar", "HTMLCalendar",
           "LocaleTextCalendar", "LocaleHTMLCalendar", "weekheader", "Day", "Month", "JANUARY", "FEBRUARY", "MARCH",
           "APRIL", "MAY", "JUNE", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER", "MONDAY",
           "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY", "SUNDAY"]

error = ValueError


if not sys._compiled:
    class IllegalMonthError(ValueError, IndexError):
        def __init__(self, month):
            self.month = month

        def __str__(self):
            return "bad month number " + repr(self.month) + "; must be 1-12"

if sys._compiled:
    class IllegalMonthError(ValueError):            # (compiled: one base class)
        def __init__(self, month: int) -> None:
            self.month = month

        def __str__(self) -> str:
            return "bad month number " + repr(self.month) + "; must be 1-12"


class IllegalWeekdayError(ValueError):
    def __init__(self, weekday: int) -> None:
        self.weekday = weekday

    def __str__(self) -> str:
        return "bad weekday number " + repr(self.weekday) + "; must be 0 (Monday) to 6 (Sunday)"


_MONTHS = ["", "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
           "November", "December"]
_DAYS = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]

if not sys._compiled:
    from enum import IntEnum, global_enum

    @global_enum
    class Month(IntEnum):
        JANUARY = 1
        FEBRUARY = 2
        MARCH = 3
        APRIL = 4
        MAY = 5
        JUNE = 6
        JULY = 7
        AUGUST = 8
        SEPTEMBER = 9
        OCTOBER = 10
        NOVEMBER = 11
        DECEMBER = 12

    @global_enum
    class Day(IntEnum):
        MONDAY = 0
        TUESDAY = 1
        WEDNESDAY = 2
        THURSDAY = 3
        FRIDAY = 4
        SATURDAY = 5
        SUNDAY = 6

    class _localized_month:
        def __init__(self, format):
            self.format = format

        def __getitem__(self, i):
            names = [("" if not k else _MONTHS[k] if self.format == "%B" else _MONTHS[k][:3]) for k in range(13)]
            return names[i]

        def __len__(self):
            return 13

    class _localized_day:
        def __init__(self, format):
            self.format = format

        def __getitem__(self, i):
            names = [(d if self.format == "%A" else d[:3]) for d in _DAYS]
            return names[i]

        def __len__(self):
            return 7

    def _day(n):
        return Day(n)

if sys._compiled:
    from enum import IntEnum

    class Month(IntEnum):
        JANUARY = 1
        FEBRUARY = 2
        MARCH = 3
        APRIL = 4
        MAY = 5
        JUNE = 6
        JULY = 7
        AUGUST = 8
        SEPTEMBER = 9
        OCTOBER = 10
        NOVEMBER = 11
        DECEMBER = 12

    class Day(IntEnum):
        MONDAY = 0
        TUESDAY = 1
        WEDNESDAY = 2
        THURSDAY = 3
        FRIDAY = 4
        SATURDAY = 5
        SUNDAY = 6

    JANUARY = 1
    FEBRUARY = 2
    MARCH = 3
    APRIL = 4
    MAY = 5
    JUNE = 6
    JULY = 7
    AUGUST = 8
    SEPTEMBER = 9
    OCTOBER = 10
    NOVEMBER = 11
    DECEMBER = 12
    MONDAY = 0
    TUESDAY = 1
    WEDNESDAY = 2
    THURSDAY = 3
    FRIDAY = 4
    SATURDAY = 5
    SUNDAY = 6

    class _localized_month:
        def __init__(self, format: str) -> None:
            self.format = format

        def __getitem__(self, i: int) -> str:
            k = i % 13
            if not k:
                return ""
            return _MONTHS[k] if self.format == "%B" else _MONTHS[k][:3]

        def __len__(self) -> int:
            return 13

    class _localized_day:
        def __init__(self, format: str) -> None:
            self.format = format

        def __getitem__(self, i: int) -> str:
            d = _DAYS[i % 7]
            return d if self.format == "%A" else d[:3]

        def __len__(self) -> int:
            return 7

    def _day(n: int) -> int:
        return n


mdays = [0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]

day_name = _localized_day('%A')
day_abbr = _localized_day('%a')
month_name = _localized_month('%B')
month_abbr = _localized_month('%b')


def isleap(year: int) -> bool:
    """Return True for leap years, False for non-leap years."""
    return year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)


def leapdays(y1: int, y2: int) -> int:
    """Return number of leap years in range [y1, y2). Assume y1 <= y2."""
    y1 -= 1
    y2 -= 1
    return (y2 // 4 - y1 // 4) - (y2 // 100 - y1 // 100) + (y2 // 400 - y1 // 400)


def weekday(year: int, month: int, day: int) -> int:
    """Return weekday (0-6 ~ Mon-Sun) for year, month (1-12), day (1-31)."""
    if not datetime.MINYEAR <= year <= datetime.MAXYEAR:
        year = 2000 + year % 400
    return _day(datetime.date(year, month, day).weekday())


def _validate_month(month: int) -> None:
    if not 1 <= month <= 12:
        raise IllegalMonthError(month)


def monthrange(year: int, month: int) -> tuple[int, int]:
    """Return weekday of first day of month (0-6 ~ Mon-Sun) and number of days (28-31) for year, month."""
    _validate_month(month)
    day1 = weekday(year, month, 1)
    ndays = mdays[month] + (1 if month == 2 and isleap(year) else 0)
    return day1, ndays


def _monthlen(year: int, month: int) -> int:
    return mdays[month] + (1 if month == 2 and isleap(year) else 0)


def _prevmonth(year: int, month: int) -> tuple[int, int]:
    if month == 1:
        return year - 1, 12
    return year, month - 1


def _nextmonth(year: int, month: int) -> tuple[int, int]:
    if month == 12:
        return year + 1, 1
    return year, month + 1


class Calendar:
    """Base calendar class. This class doesn't do any formatting. It simply provides data to subclasses."""

    def __init__(self, firstweekday: int = 0) -> None:
        self._firstweekday = firstweekday          # 0 = Monday, 6 = Sunday

    def getfirstweekday(self) -> int:
        return self._firstweekday % 7

    def setfirstweekday(self, firstweekday: int) -> None:
        self._firstweekday = firstweekday

    @property
    def firstweekday(self) -> int:
        return self._firstweekday % 7

    @firstweekday.setter
    def firstweekday(self, value: int) -> None:
        self._firstweekday = value

    def iterweekdays(self) -> Iterator[int]:
        """An iterator for one week of weekday numbers starting with the configured first one."""
        for i in range(self.firstweekday, self.firstweekday + 7):
            yield i % 7

    def itermonthdates(self, year: int, month: int) -> Iterator[datetime.date]:
        """An iterator for one month's dates (complete weeks: dates outside the month too)."""
        for y, m, d in self.itermonthdays3(year, month):
            yield datetime.date(y, m, d)

    def itermonthdays(self, year: int, month: int) -> Iterator[int]:
        """Like itermonthdates(), but day numbers (0 for days outside the month)."""
        day1, ndays = monthrange(year, month)
        days_before = (day1 - self.firstweekday) % 7
        for _ in range(days_before):
            yield 0
        for d in range(1, ndays + 1):
            yield d
        days_after = (self.firstweekday - day1 - ndays) % 7
        for _ in range(days_after):
            yield 0

    def itermonthdays2(self, year: int, month: int) -> Iterator[tuple[int, int]]:
        """Like itermonthdates(), but (day number, weekday number) tuples."""
        i = self.firstweekday
        for d in self.itermonthdays(year, month):
            yield d, i % 7
            i += 1

    def itermonthdays3(self, year: int, month: int) -> Iterator[tuple[int, int, int]]:
        """Like itermonthdates(), but (year, month, day) tuples (outside datetime.date's range too)."""
        day1, ndays = monthrange(year, month)
        days_before = (day1 - self.firstweekday) % 7
        days_after = (self.firstweekday - day1 - ndays) % 7
        y, m = _prevmonth(year, month)
        end = _monthlen(y, m) + 1
        for d in range(end - days_before, end):
            yield y, m, d
        for d in range(1, ndays + 1):
            yield year, month, d
        y, m = _nextmonth(year, month)
        for d in range(1, days_after + 1):
            yield y, m, d

    def itermonthdays4(self, year: int, month: int) -> Iterator[tuple[int, int, int, int]]:
        """Like itermonthdates(), but (year, month, day, day_of_week) tuples."""
        i = 0
        for y, m, d in self.itermonthdays3(year, month):
            yield y, m, d, (self.firstweekday + i) % 7
            i += 1

    def monthdatescalendar(self, year: int, month: int) -> list[list[datetime.date]]:
        """A month's calendar: a list of weeks, each a list of datetime.date values."""
        dates = list(self.itermonthdates(year, month))
        return [dates[i:i + 7] for i in range(0, len(dates), 7)]

    def monthdays2calendar(self, year: int, month: int) -> list[list[tuple[int, int]]]:
        """A month's calendar: weeks of (day number, weekday number) tuples."""
        days = list(self.itermonthdays2(year, month))
        return [days[i:i + 7] for i in range(0, len(days), 7)]

    def monthdayscalendar(self, year: int, month: int) -> list[list[int]]:
        """A month's calendar: weeks of day numbers (0 outside this month)."""
        days = list(self.itermonthdays(year, month))
        return [days[i:i + 7] for i in range(0, len(days), 7)]

    def yeardatescalendar(self, year: int, width: int = 3) -> list[list[list[list[datetime.date]]]]:
        months = [self.monthdatescalendar(year, m) for m in range(1, 13)]
        return [months[i:i + width] for i in range(0, len(months), width)]

    def yeardays2calendar(self, year: int, width: int = 3) -> list[list[list[list[tuple[int, int]]]]]:
        months = [self.monthdays2calendar(year, m) for m in range(1, 13)]
        return [months[i:i + width] for i in range(0, len(months), width)]

    def yeardayscalendar(self, year: int, width: int = 3) -> list[list[list[list[int]]]]:
        months = [self.monthdayscalendar(year, m) for m in range(1, 13)]
        return [months[i:i + width] for i in range(0, len(months), width)]


class TextCalendar(Calendar):
    """A calendar as plain text, like the UNIX program cal."""

    def prweek(self, theweek: list[tuple[int, int]], width: int) -> None:
        """Print a single week (no newline)."""
        print(self.formatweek(theweek, width), end='')

    def formatday(self, day: int, weekday: int, width: int) -> str:
        """A formatted day."""
        if day == 0:
            s = ''
        else:
            s = '%2i' % day
        return s.center(width)

    def formatweek(self, theweek: list[tuple[int, int]], width: int) -> str:
        """A single week in a string (no newline)."""
        return ' '.join([self.formatday(d, wd, width) for (d, wd) in theweek])

    def formatweekday(self, day: int, width: int) -> str:
        """A formatted week day name."""
        if width >= 9:
            name = day_name[day]
        else:
            name = day_abbr[day]
        return name[:width].center(width)

    def formatweekheader(self, width: int) -> str:
        """A header for a week."""
        return ' '.join([self.formatweekday(i, width) for i in self.iterweekdays()])

    def formatmonthname(self, theyear: int, themonth: int, width: int, withyear: bool = True) -> str:
        """A formatted month name."""
        _validate_month(themonth)
        s = month_name[themonth]
        if withyear:
            s = s + " " + repr(theyear)
        return s.center(width)

    def prmonth(self, theyear: int, themonth: int, w: int = 0, l: int = 0) -> None:
        """Print a month's calendar."""
        print(self.formatmonth(theyear, themonth, w, l), end='')

    def formatmonth(self, theyear: int, themonth: int, w: int = 0, l: int = 0) -> str:
        """A month's calendar string (multi-line)."""
        w = max(2, w)
        l = max(1, l)
        s = self.formatmonthname(theyear, themonth, 7 * (w + 1) - 1)
        s = s.rstrip()
        s += '\n' * l
        s += self.formatweekheader(w).rstrip()
        s += '\n' * l
        for week in self.monthdays2calendar(theyear, themonth):
            s += self.formatweek(week, w).rstrip()
            s += '\n' * l
        return s

    def formatyear(self, theyear: int, w: int = 2, l: int = 1, c: int = 6, m: int = 3) -> str:
        """A year's calendar as a multi-line string."""
        w = max(2, w)
        l = max(1, l)
        c = max(2, c)
        colwidth = (w + 1) * 7 - 1
        v: list[str] = []
        v.append(repr(theyear).center(colwidth * m + c * (m - 1)).rstrip())
        v.append('\n' * l)
        header = self.formatweekheader(w)
        i = 0
        for row in self.yeardays2calendar(theyear, m):
            months = range(m * i + 1, min(m * (i + 1) + 1, 13))
            v.append('\n' * l)
            names = [self.formatmonthname(theyear, k, colwidth, False) for k in months]
            v.append(formatstring(names, colwidth, c).rstrip())
            v.append('\n' * l)
            headers = [header for k in months]
            v.append(formatstring(headers, colwidth, c).rstrip())
            v.append('\n' * l)
            height = max([len(cal) for cal in row])
            for j in range(height):
                weeks: list[str] = []
                for cal in row:
                    if j >= len(cal):
                        weeks.append('')
                    else:
                        weeks.append(self.formatweek(cal[j], w))
                v.append(formatstring(weeks, colwidth, c).rstrip())
                v.append('\n' * l)
            i += 1
        return ''.join(v)

    def pryear(self, theyear: int, w: int = 0, l: int = 0, c: int = 6, m: int = 3) -> None:
        """Print a year's calendar."""
        print(self.formatyear(theyear, w, l, c, m), end='')


class HTMLCalendar(Calendar):
    """A calendar as HTML."""

    def __init__(self, firstweekday: int = 0) -> None:
        Calendar.__init__(self, firstweekday)
        self.cssclasses = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]
        self.cssclasses_weekday_head = self.cssclasses
        self.cssclass_noday = "noday"
        self.cssclass_month_head = "month"
        self.cssclass_month = "month"
        self.cssclass_year_head = "year"
        self.cssclass_year = "year"

    def formatday(self, day: int, weekday: int) -> str:
        """A day as a table cell."""
        if day == 0:
            return '<td class="' + self.cssclass_noday + '">&nbsp;</td>'
        return '<td class="' + self.cssclasses[weekday] + '">' + str(day) + '</td>'

    def formatweek(self, theweek: list[tuple[int, int]]) -> str:
        """A complete week as a table row."""
        return '<tr>' + ''.join([self.formatday(d, wd) for (d, wd) in theweek]) + '</tr>'

    def formatweekday(self, day: int) -> str:
        """A weekday name as a table header."""
        return '<th class="' + self.cssclasses_weekday_head[day] + '">' + day_abbr[day] + '</th>'

    def formatweekheader(self) -> str:
        """A header for a week as a table row."""
        return '<tr>' + ''.join([self.formatweekday(i) for i in self.iterweekdays()]) + '</tr>'

    def formatmonthname(self, theyear: int, themonth: int, withyear: bool = True) -> str:
        """A month name as a table row."""
        _validate_month(themonth)
        if withyear:
            s = month_name[themonth] + " " + str(theyear)
        else:
            s = month_name[themonth]
        return '<tr><th colspan="7" class="' + self.cssclass_month_head + '">' + s + '</th></tr>'

    def formatmonth(self, theyear: int, themonth: int, withyear: bool = True) -> str:
        """A formatted month as a table."""
        v: list[str] = []
        v.append('<table border="0" cellpadding="0" cellspacing="0" class="' + self.cssclass_month + '">')
        v.append('\n')
        v.append(self.formatmonthname(theyear, themonth, withyear=withyear))
        v.append('\n')
        v.append(self.formatweekheader())
        v.append('\n')
        for week in self.monthdays2calendar(theyear, themonth):
            v.append(self.formatweek(week))
            v.append('\n')
        v.append('</table>')
        v.append('\n')
        return ''.join(v)

    def formatyear(self, theyear: int, width: int = 3) -> str:
        """A formatted year as a table of tables."""
        v: list[str] = []
        width = max(width, 1)
        v.append('<table border="0" cellpadding="0" cellspacing="0" class="' + self.cssclass_year + '">')
        v.append('\n')
        v.append('<tr><th colspan="' + str(width) + '" class="' + self.cssclass_year_head + '">' + str(theyear) + '</th></tr>')
        for i in range(1, 13, width):
            v.append('<tr>')
            for m in range(i, min(i + width, 13)):
                v.append('<td>')
                v.append(self.formatmonth(theyear, m, withyear=False))
                v.append('</td>')
            v.append('</tr>')
        v.append('</table>')
        return ''.join(v)

    def formatyearpage(self, theyear: int, width: int = 3, css: str | None = 'calendar.css',
                       encoding: str | None = None) -> bytes:
        """A formatted year as a complete HTML page."""
        if encoding is None:
            encoding = "utf-8"
        v: list[str] = []
        v.append('<?xml version="1.0" encoding="' + encoding + '"?>\n')
        v.append('<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Strict//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd">\n')
        v.append('<html>\n')
        v.append('<head>\n')
        v.append('<meta http-equiv="Content-Type" content="text/html; charset=' + encoding + '" />\n')
        if css is not None:
            v.append('<link rel="stylesheet" type="text/css" href="' + css + '" />\n')
        v.append('<title>Calendar for ' + str(theyear) + '</title>\n')
        v.append('</head>\n')
        v.append('<body>\n')
        v.append(self.formatyear(theyear, width))
        v.append('</body>\n')
        v.append('</html>\n')
        return ''.join(v).encode(encoding, "xmlcharrefreplace")


class LocaleTextCalendar(TextCalendar):
    """(Here: the English names, whatever the locale.)"""

    def __init__(self, firstweekday: int = 0, locale: str | None = None) -> None:
        TextCalendar.__init__(self, firstweekday)
        self.locale = locale


class LocaleHTMLCalendar(HTMLCalendar):
    """(Here: the English names, whatever the locale.)"""

    def __init__(self, firstweekday: int = 0, locale: str | None = None) -> None:
        HTMLCalendar.__init__(self, firstweekday)
        self.locale = locale


c = TextCalendar()
_tc = c


def firstweekday() -> int:
    return c.getfirstweekday()


def setfirstweekday(firstweekday: int) -> None:
    if not 0 <= firstweekday <= 6:
        raise IllegalWeekdayError(firstweekday)
    c.firstweekday = firstweekday


def monthcalendar(year: int, month: int) -> list[list[int]]:
    return c.monthdayscalendar(year, month)


def prweek(theweek: list[tuple[int, int]], width: int) -> None:
    c.prweek(theweek, width)


def week(theweek: list[tuple[int, int]], width: int) -> str:
    return c.formatweek(theweek, width)


def weekheader(width: int) -> str:
    return c.formatweekheader(width)


def prmonth(theyear: int, themonth: int, w: int = 0, l: int = 0) -> None:
    c.prmonth(theyear, themonth, w, l)


def month(theyear: int, themonth: int, w: int = 0, l: int = 0) -> str:
    return c.formatmonth(theyear, themonth, w, l)


def calendar(theyear: int, w: int = 2, l: int = 1, c: int = 6, m: int = 3) -> str:
    return _tc.formatyear(theyear, w, l, c, m)


def prcal(theyear: int, w: int = 0, l: int = 0, c: int = 6, m: int = 3) -> None:
    _tc.pryear(theyear, w, l, c, m)


_colwidth = 7 * 3 - 1           # Amount printed by prweek()
_spacing = 6                    # Number of spaces between columns


def format(cols: list[str], colwidth: int = _colwidth, spacing: int = _spacing) -> None:
    """Prints multi-column formatting for year calendars."""
    print(formatstring(cols, colwidth, spacing))


def formatstring(cols: list[str], colwidth: int = _colwidth, spacing: int = _spacing) -> str:
    """Returns a string formatted from n strings, centered within n columns."""
    sp = ' ' * spacing
    return sp.join([col.center(colwidth) for col in cols])


EPOCH = 1970
_EPOCH_ORD = datetime.date(EPOCH, 1, 1).toordinal()


def timegm(tuple: _T) -> int:
    """Unrelated but handy function to calculate Unix timestamp from GMT."""
    year = tuple[0]
    month = tuple[1]
    day = tuple[2]
    hour = tuple[3]
    minute = tuple[4]
    second = tuple[5]
    days = datetime.date(year, month, 1).toordinal() - _EPOCH_ORD + day - 1
    hours = days * 24 + hour
    minutes = hours * 60 + minute
    seconds = minutes * 60 + second
    return seconds
