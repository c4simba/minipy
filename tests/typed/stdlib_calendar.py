import calendar
import time
print(calendar.month(2024, 2))
print(calendar.month(2025, 12, 3, 2))
print(calendar.calendar(2024))
print(calendar.isleap(2000), calendar.isleap(1900), calendar.leapdays(1900, 2025), int(calendar.weekday(2024, 7, 4)))
d1, n = calendar.monthrange(2024, 2)
print(int(d1), n, calendar.monthcalendar(2024, 9), calendar.month_name[1], calendar.day_abbr[6], len(calendar.month_name))
print(calendar.timegm(time.gmtime(1700000000)), calendar.timegm((2000, 1, 1, 0, 0, 0)), calendar.weekheader(3))
cal = calendar.TextCalendar(calendar.SUNDAY)
print(cal.formatmonth(2023, 1))
print(list(cal.iterweekdays()), cal.firstweekday, list(cal.itermonthdays2(2024, 2))[:5])
print(calendar.HTMLCalendar().formatmonth(2024, 1))
print([str(d) for d in calendar.Calendar().itermonthdates(2024, 3)][:3])
try:
    calendar.month(2024, 13)
except calendar.IllegalMonthError as e:
    print("IllegalMonthError:", e)
try:
    calendar.setfirstweekday(9)
except calendar.IllegalWeekdayError as e:
    print("IllegalWeekdayError:", e)
calendar.setfirstweekday(6)
print(calendar.firstweekday(), calendar.month(2024, 6))
