"""Functions to run when the program ends (CPython's atexit): register, unregister."""
import sys

if not sys._compiled:

    _exithandlers = []


    def register(func, *args, **kwargs):
        """Calls func(*args, **kwargs) at exit (the last registered first); func."""
        if not callable(func):
            raise TypeError("the first argument must be callable")
        _exithandlers.append((func, args, kwargs))
        return func


    def unregister(func):
        """Forgets func (every registration of it)."""
        _exithandlers[:] = [h for h in _exithandlers if h[0] != func]


    def _clear():
        del _exithandlers[:]


    def _ncallbacks():
        return len(_exithandlers)


    def _run_exitfuncs():
        """(At exit: the handlers, last first; an exception is printed, the others still run.)"""
        import sys
        while _exithandlers:
            func, args, kwargs = _exithandlers.pop()
            try:
                func(*args, **kwargs)
            except SystemExit:
                pass
            except BaseException as e:
                print("Exception ignored in atexit callback " + repr(func) + ":", file=sys.stderr)
                sys.stderr.write(sys._format_exception(e))


if sys._compiled:
    # Compiled programs: the same, typed (register's *args: of one type; no keyword arguments).
    from typing import Callable, TypeVar

    _F = TypeVar("_F")
    _A = TypeVar("_A")

    class _Exit:
        def __init__(self, key: int, name: str, call: Callable[[], None]) -> None:
            self.key = key
            self.name = name
            self.call = call

    _exithandlers: list[_Exit] = []

    def register(func: _F, *args: _A) -> _F:
        """Calls func(*args) at exit (the last registered first); func."""
        _exithandlers.append(_Exit(id(func), repr(func), lambda: func(*args)))
        return func

    def unregister(func: _F) -> None:
        """Forgets func (every registration of it)."""
        k = id(func)
        _exithandlers[:] = [h for h in _exithandlers if h.key != k]

    def _clear() -> None:
        del _exithandlers[:]

    def _ncallbacks() -> int:
        return len(_exithandlers)

    def _run_exitfuncs() -> None:
        """(At exit: the handlers, last first; an exception is printed, the others still run.)"""
        while len(_exithandlers) > 0:
            h = _exithandlers.pop()
            try:
                h.call()
            except SystemExit:
                pass
            except BaseException as e:
                s = str(e)
                print("Exception ignored in atexit callback " + h.name + ":", file=sys.stderr)
                print(type(e).__qualname__ + (": " + s if s else ""), file=sys.stderr)
