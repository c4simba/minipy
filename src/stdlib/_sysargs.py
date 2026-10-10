"""sys.argv in compiled programs (the compiler reads sys.argv as __mpy_argv; the
interpreter has its own). Linux: the program's arguments as the system passed
them. KolibriOS passes the program's path and one line of parameters: words
separated by spaces, double quotes keep spaces in one."""
import sys


def _split(params: str) -> list[str]:
    out: list[str] = []
    cur = ""
    have = False
    quoted = False
    for ch in params:
        if ch == '"':
            quoted = not quoted
            have = True
        elif (ch == " " or ch == "\t") and not quoted:
            if have:
                out.append(cur)
                cur = ""
                have = False
        else:
            cur += ch
            have = True
    if have:
        out.append(cur)
    return out


def _make() -> list[str]:
    raw = sys._rawargs(0)
    if sys.platform == "kolibrios":
        return [raw[0]] + _split(raw[1])
    return raw


__mpy_argv = _make()
