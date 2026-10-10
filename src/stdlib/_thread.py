"""_thread in compiled programs (the interpreter's is native): threading's cooperative threads,
started without being waited for at exit (as CPython's _thread threads)."""
import sys
import threading as _threading
from typing import TypeVar

_F = TypeVar("_F")
_A = TypeVar("_A")
_K = TypeVar("_K")

error = RuntimeError
TIMEOUT_MAX = _threading.TIMEOUT_MAX


def allocate_lock() -> _threading.Lock:
    """A new lock."""
    return _threading.Lock()


def allocate() -> _threading.Lock:
    return _threading.Lock()


def RLock() -> _threading.RLock:
    return _threading.RLock()


def get_ident() -> int:
    """The running thread's identifier."""
    return _threading.get_ident()


def get_native_id() -> int:
    return _threading.get_native_id()


def start_new_thread(function: _F, args: _A, kwargs: dict[str, _K] | None = None) -> int:
    """Runs function(*args, **kwargs) in a new thread: its identifier."""
    t = _threading.Thread(target=function, args=args, kwargs=kwargs, daemon=True)
    t.start()
    i = t.ident
    return i if i is not None else 0


def start_new(function: _F, args: _A) -> int:
    return start_new_thread(function, args)


def stack_size(size: int = 0) -> int:
    return 0


def exit() -> None:
    """Ends the running thread."""
    raise SystemExit


def _count() -> int:
    return _threading.active_count() - 1
