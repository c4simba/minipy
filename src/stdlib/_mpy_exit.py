"""SystemExit in compiled programs: SystemExit(code) / sys.exit(code) keep the code (an int, or
None; a message: printed, status 1), and an uncaught one ends the program with that status."""
import sys


def __mpy_sysexit_int(code: int) -> SystemExit:
    e = SystemExit(str(code))
    e.code = code
    return e


def __mpy_sysexit_str(msg: str) -> SystemExit:
    e = SystemExit(msg)
    e._msgexit = True
    return e


def __mpy_sysexit_none() -> SystemExit:
    return SystemExit()


def __mpy_uncaught(e: BaseException) -> int:
    """The exit status of an uncaught exception (-1: show it as usual)."""
    if isinstance(e, SystemExit):
        c = e.code
        if c is not None:
            return c & 0xFF
        if e._msgexit:
            print(str(e), file=sys.stderr)
            return 1
        return 0
    return -1
