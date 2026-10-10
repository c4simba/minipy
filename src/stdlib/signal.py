"""Signals (CPython's signal module): the table of handlers.

No signal of the system reaches a minipy program: raise_signal(sig) calls the handler set for
sig (SIGINT's default one raises KeyboardInterrupt, SIG_IGN does nothing, SIG_DFL ends the program
for the others). Compiled programs: SIG_DFL and SIG_IGN are functions (as handlers are), and a
handler is a function (signum: int, frame: None) -> None."""
import sys

if sys.platform == "darwin":
    SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGEMT, SIGFPE = 1, 2, 3, 4, 5, 6, 7, 8
    SIGKILL, SIGBUS, SIGSEGV, SIGSYS, SIGPIPE, SIGALRM, SIGTERM, SIGURG = 9, 10, 11, 12, 13, 14, 15, 16
    SIGSTOP, SIGTSTP, SIGCONT, SIGCHLD, SIGTTIN, SIGTTOU, SIGIO, SIGXCPU = 17, 18, 19, 20, 21, 22, 23, 24
    SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGINFO, SIGUSR1, SIGUSR2 = 25, 26, 27, 28, 29, 30, 31
    NSIG = 32
else:
    SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS, SIGFPE = 1, 2, 3, 4, 5, 6, 7, 8
    SIGKILL, SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE, SIGALRM, SIGTERM, SIGSTKFLT = 9, 10, 11, 12, 13, 14, 15, 16
    SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG, SIGXCPU = 17, 18, 19, 20, 21, 22, 23, 24
    SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGIO, SIGPWR, SIGSYS = 25, 26, 27, 28, 29, 30, 31
    SIGPOLL = SIGIO
    NSIG = 65
SIGIOT = SIGABRT
ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF = 0, 1, 2
SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK = 0, 1, 2

_NAMES = {SIGHUP: "SIGHUP", SIGINT: "SIGINT", SIGQUIT: "SIGQUIT", SIGILL: "SIGILL", SIGTRAP: "SIGTRAP",
          SIGABRT: "SIGABRT", SIGBUS: "SIGBUS", SIGFPE: "SIGFPE", SIGKILL: "SIGKILL", SIGUSR1: "SIGUSR1",
          SIGSEGV: "SIGSEGV", SIGUSR2: "SIGUSR2", SIGPIPE: "SIGPIPE", SIGALRM: "SIGALRM", SIGTERM: "SIGTERM",
          SIGCHLD: "SIGCHLD", SIGCONT: "SIGCONT", SIGSTOP: "SIGSTOP", SIGTSTP: "SIGTSTP", SIGTTIN: "SIGTTIN",
          SIGTTOU: "SIGTTOU", SIGURG: "SIGURG", SIGXCPU: "SIGXCPU", SIGXFSZ: "SIGXFSZ", SIGVTALRM: "SIGVTALRM",
          SIGPROF: "SIGPROF", SIGWINCH: "SIGWINCH", SIGIO: "SIGIO", SIGSYS: "SIGSYS"}
_DESCR = {SIGHUP: "Hangup", SIGINT: "Interrupt", SIGQUIT: "Quit", SIGILL: "Illegal instruction",
          SIGTRAP: "Trace/BPT trap", SIGABRT: "Abort trap", SIGBUS: "Bus error", SIGFPE: "Floating point exception",
          SIGKILL: "Killed", SIGUSR1: "User defined signal 1", SIGSEGV: "Segmentation fault",
          SIGUSR2: "User defined signal 2", SIGPIPE: "Broken pipe", SIGALRM: "Alarm clock", SIGTERM: "Terminated",
          SIGCHLD: "Child exited", SIGCONT: "Continued", SIGSTOP: "Suspended (signal)", SIGTSTP: "Suspended",
          SIGTTIN: "Stopped (tty input)", SIGTTOU: "Stopped (tty output)", SIGURG: "Urgent I/O condition",
          SIGXCPU: "Cputime limit exceeded", SIGXFSZ: "Filesize limit exceeded",
          SIGVTALRM: "Virtual timer expired", SIGPROF: "Profiling timer expired",
          SIGWINCH: "Window size changes", SIGIO: "I/O possible", SIGSYS: "Bad system call"}


def _check(signalnum: int) -> None:
    if signalnum < 1 or signalnum >= NSIG:
        raise ValueError("signal number out of range")


def strsignal(signalnum: int) -> str | None:
    """The description of the signal (None: not one known)."""
    _check(signalnum)
    return _DESCR.get(signalnum)


def valid_signals() -> set[int]:
    return set(range(1, NSIG))


def alarm(seconds: int) -> int:
    """(no alarm is delivered here) The seconds left of a previous alarm: 0."""
    return 0


def pause() -> None:
    raise OSError(38, "Function not implemented")


def siginterrupt(signalnum: int, flag: bool) -> None:
    _check(signalnum)


def set_wakeup_fd(fd: int, *, warn_on_full_buffer: bool = True) -> int:
    return -1


if not sys._compiled:
    import enum as _enum

    class Handlers(_enum.IntEnum):
        SIG_DFL = 0
        SIG_IGN = 1

    SIG_DFL = Handlers.SIG_DFL
    SIG_IGN = Handlers.SIG_IGN

    def default_int_handler(signalnum, frame):
        """The default handler for SIGINT installed by Python: raises KeyboardInterrupt."""
        raise KeyboardInterrupt

    default_int_handler = sys._builtin(default_int_handler)

    _handlers = {SIGINT: default_int_handler}

    def signal(signalnum, handler):
        """Set the action for the given signal; the previous one is returned."""
        _check(signalnum)
        if signalnum in (SIGKILL, SIGSTOP):
            raise OSError(22, "Invalid argument")
        if not callable(handler) and handler not in (SIG_DFL, SIG_IGN):
            raise TypeError("signal handler must be signal.SIG_IGN, signal.SIG_DFL, or a callable object")
        old = _handlers.get(signalnum, SIG_DFL)
        _handlers[signalnum] = handler
        return old

    def getsignal(signalnum):
        """The current action for the given signal (SIG_IGN, SIG_DFL, None or a callable)."""
        _check(signalnum)
        return _handlers.get(signalnum, SIG_DFL)

    def raise_signal(signalnum):
        """Send a signal to the executing process: its handler is called."""
        _check(signalnum)
        h = _handlers.get(signalnum, SIG_DFL)
        if h == SIG_IGN:
            return
        if h == SIG_DFL:
            if signalnum in (SIGCHLD, SIGURG, SIGWINCH, SIGCONT, SIGIO):
                return
            sys.stdout.flush()
            raise SystemExit(128 + signalnum)
        h(signalnum, None)

    signal = sys._builtin(signal)
    getsignal = sys._builtin(getsignal)
    raise_signal = sys._builtin(raise_signal)

if sys._compiled:
    def SIG_DFL(signalnum: int, frame: None) -> None:
        """The default action of a signal: SIGINT raises KeyboardInterrupt, others end the program."""
        if signalnum == SIGINT:
            raise KeyboardInterrupt
        if signalnum in (SIGCHLD, SIGURG, SIGWINCH, SIGCONT, SIGIO):
            return
        raise SystemExit(128 + signalnum)

    def SIG_IGN(signalnum: int, frame: None) -> None:
        """Ignore the signal."""
        pass

    def default_int_handler(signalnum: int, frame: None) -> None:
        """The default handler for SIGINT: raises KeyboardInterrupt."""
        raise KeyboardInterrupt

    _handlers = {SIGINT: default_int_handler}

    def signal(signalnum: int, handler):
        """Set the action for the given signal; the previous one is returned."""
        _check(signalnum)
        if signalnum == SIGKILL or signalnum == SIGSTOP:
            raise OSError(22, "Invalid argument")
        old = _handlers.get(signalnum, SIG_DFL)
        _handlers[signalnum] = handler
        return old

    def getsignal(signalnum: int):
        """The current action for the given signal."""
        _check(signalnum)
        return _handlers.get(signalnum, SIG_DFL)

    def raise_signal(signalnum: int) -> None:
        """Send a signal to the executing process: its handler is called."""
        _check(signalnum)
        _handlers.get(signalnum, SIG_DFL)(signalnum, None)
