import locale
print(locale.getencoding(), locale.setlocale(locale.LC_ALL, "C"), locale.setlocale(locale.LC_NUMERIC, None), locale.getpreferredencoding())
print(locale.localeconv()["decimal_point"], repr(locale.localeconv()["thousands_sep"]), locale.CHAR_MAX)
print(locale.atof("3.5"), locale.atoi("42"), locale.str(1.25), locale.str(1 / 3), locale.format_string("%d items", 5), locale.format_string("%.2f", 2.0 / 3))
print(locale.strcoll("a", "b"), locale.strcoll("b", "a"), locale.strcoll("x", "x"), locale.strxfrm("abc"), locale.normalize("C"))
try:
    locale.setlocale(locale.LC_ALL, "xx_YY")
except locale.Error as e:
    print("Error:", e)
try:
    locale.currency(1.5)
except ValueError as e:
    print("ValueError:", e)
