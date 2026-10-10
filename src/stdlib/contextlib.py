"""Context manager helpers (CPython's contextlib): contextmanager, closing, suppress,
nullcontext, redirect_stdout/stderr, chdir, ExitStack, ContextDecorator and their
async forms.

In compiled programs @contextmanager functions are generators the compiler wraps
(with f(...) as x: works, for functions and methods); closing, suppress, nullcontext
and chdir are available; ExitStack, redirect_stdout/stderr, the async forms and the
decorator forms (ContextDecorator) are interpreter only."""
import sys
from typing import TypeVar, Generic

_T = TypeVar("_T")
_E = TypeVar("_E")

__all__ = ["asynccontextmanager", "contextmanager", "closing", "nullcontext", "AbstractContextManager",
           "AbstractAsyncContextManager", "AsyncExitStack", "ContextDecorator", "ExitStack", "redirect_stdout",
           "redirect_stderr", "suppress", "aclosing", "chdir"]


class _GeneratorCM(Generic[_T]):
    """What a @contextmanager function's call is: its generator's first value, then the rest."""

    def __init__(self, gen):
        self.gen = gen

    def __enter__(self):
        try:
            return next(self.gen)
        except StopIteration:
            raise RuntimeError("generator didn't yield")

    def __exit__(self, typ: type[BaseException] | None, value: BaseException | None, traceback) -> bool:
        if value is None:
            try:
                next(self.gen)
            except StopIteration:
                return False
            try:
                raise RuntimeError("generator didn't stop")
            finally:
                self.gen.close()
        try:
            self.gen.throw(value)
        except StopIteration as exc:
            return exc is not value
        except BaseException as exc:
            if exc is not value:
                raise
            return False
        try:
            raise RuntimeError("generator didn't stop after throw()")
        finally:
            self.gen.close()


def contextmanager(func: _T) -> _T:
    """@contextmanager def f(...): ... yield x ...: with f(...) as x: runs the code before the
    yield, the with body, then the code after it. (Compiled: the compiler wraps the calls.)"""
    return func


class closing(Generic[_T]):
    """with closing(thing) as t: ... calls thing.close() at the end."""

    def __init__(self, thing: _T):
        self.thing = thing

    def __enter__(self) -> _T:
        return self.thing

    def __exit__(self, typ: type[BaseException] | None, value: BaseException | None, traceback) -> bool:
        self.thing.close()
        return False


class nullcontext(Generic[_T]):
    """A context manager doing nothing: with nullcontext(x) as y gives y = x."""

    def __init__(self, enter_result: _T = None):
        self.enter_result = enter_result

    def __enter__(self) -> _T:
        return self.enter_result

    def __exit__(self, typ: type[BaseException] | None, value: BaseException | None, traceback) -> bool:
        return False


class suppress:
    """with suppress(E1, E2): ... ends quietly on those exceptions."""

    def __init__(self, *exceptions: type[BaseException]):
        self._exceptions = exceptions

    def __enter__(self) -> None:
        pass

    def __exit__(self, typ: type[BaseException] | None, value: BaseException | None, traceback) -> bool:
        if value is None:
            return False
        for e in self._exceptions:
            if isinstance(value, e):
                return True
        return False


class chdir:
    """with chdir(path): ... runs in that directory, then goes back."""

    def __init__(self, path: str):
        self.path = path
        self._old_cwd: list[str] = []

    def __enter__(self) -> None:
        import os
        self._old_cwd.append(os.getcwd())
        os.chdir(self.path)

    def __exit__(self, typ: type[BaseException] | None, value: BaseException | None, traceback) -> bool:
        import os
        os.chdir(self._old_cwd.pop())
        return False


if not sys._compiled:
    import abc

    class AbstractContextManager(abc.ABC):
        """A class with __enter__ (returning self) and __exit__."""

        __class_getitem__ = classmethod(lambda cls, item: cls)

        def __enter__(self):
            return self

        @abc.abstractmethod
        def __exit__(self, exc_type, exc_value, traceback):
            return None

        @classmethod
        def __subclasshook__(cls, C):
            if cls is AbstractContextManager:
                if any("__enter__" in B.__dict__ for B in C.__mro__) and any("__exit__" in B.__dict__ for B in C.__mro__):
                    return True
            return NotImplemented

    class AbstractAsyncContextManager(abc.ABC):
        """A class with __aenter__ (returning self) and __aexit__."""

        __class_getitem__ = classmethod(lambda cls, item: cls)

        async def __aenter__(self):
            return self

        @abc.abstractmethod
        async def __aexit__(self, exc_type, exc_value, traceback):
            return None

        @classmethod
        def __subclasshook__(cls, C):
            if cls is AbstractAsyncContextManager:
                if any("__aenter__" in B.__dict__ for B in C.__mro__) and any("__aexit__" in B.__dict__ for B in C.__mro__):
                    return True
            return NotImplemented

    class ContextDecorator(object):
        """A context manager that is also a decorator: @cm() runs the function inside it."""

        def _recreate_cm(self):
            return self

        def __call__(self, func):
            def inner(*args, **kwds):
                with self._recreate_cm():
                    return func(*args, **kwds)
            inner.__name__ = getattr(func, "__name__", "inner")
            inner.__qualname__ = getattr(func, "__qualname__", "inner")
            inner.__doc__ = getattr(func, "__doc__", None)
            inner.__wrapped__ = func
            return inner

    class AsyncContextDecorator(object):
        """An async context manager that is also a decorator of coroutine functions."""

        def _recreate_cm(self):
            return self

        def __call__(self, func):
            async def inner(*args, **kwds):
                async with self._recreate_cm():
                    return await func(*args, **kwds)
            inner.__name__ = getattr(func, "__name__", "inner")
            inner.__wrapped__ = func
            return inner

    class _GeneratorContextManager(ContextDecorator, AbstractContextManager):
        """Helper for @contextmanager."""

        def __init__(self, func, args, kwds):
            self.gen = func(*args, **kwds)
            self.func, self.args, self.kwds = func, args, kwds
            doc = getattr(func, "__doc__", None)
            if doc is None:
                doc = type(self).__doc__
            self.__doc__ = doc

        def _recreate_cm(self):
            return self.__class__(self.func, self.args, self.kwds)

        def __enter__(self):
            del self.args, self.kwds, self.func
            try:
                return next(self.gen)
            except StopIteration:
                raise RuntimeError("generator didn't yield") from None

        def __exit__(self, typ, value, traceback):
            if typ is None:
                try:
                    next(self.gen)
                except StopIteration:
                    return False
                else:
                    try:
                        raise RuntimeError("generator didn't stop")
                    finally:
                        self.gen.close()
            else:
                if value is None:
                    value = typ()
                try:
                    self.gen.throw(value)
                except StopIteration as exc:
                    return exc is not value
                except RuntimeError as exc:
                    if exc is value:
                        exc.__traceback__ = traceback
                        return False
                    if isinstance(value, StopIteration) and exc.__cause__ is value:
                        value.__traceback__ = traceback
                        return False
                    raise
                except BaseException as exc:
                    if exc is not value:
                        raise
                    exc.__traceback__ = traceback
                    return False
                try:
                    raise RuntimeError("generator didn't stop after throw()")
                finally:
                    self.gen.close()

    class _AsyncGeneratorContextManager(AsyncContextDecorator, AbstractAsyncContextManager):
        """Helper for @asynccontextmanager."""

        def __init__(self, func, args, kwds):
            self.gen = func(*args, **kwds)
            self.func, self.args, self.kwds = func, args, kwds
            doc = getattr(func, "__doc__", None)
            if doc is None:
                doc = type(self).__doc__
            self.__doc__ = doc

        def _recreate_cm(self):
            return self.__class__(self.func, self.args, self.kwds)

        async def __aenter__(self):
            del self.args, self.kwds, self.func
            try:
                return await anext(self.gen)
            except StopAsyncIteration:
                raise RuntimeError("generator didn't yield") from None

        async def __aexit__(self, typ, value, traceback):
            if typ is None:
                try:
                    await anext(self.gen)
                except StopAsyncIteration:
                    return False
                else:
                    try:
                        raise RuntimeError("generator didn't stop")
                    finally:
                        await self.gen.aclose()
            else:
                if value is None:
                    value = typ()
                try:
                    await self.gen.athrow(value)
                except StopAsyncIteration as exc:
                    return exc is not value
                except RuntimeError as exc:
                    if exc is value:
                        exc.__traceback__ = traceback
                        return False
                    if isinstance(value, (StopIteration, StopAsyncIteration)) and exc.__cause__ is value:
                        value.__traceback__ = traceback
                        return False
                    raise
                except BaseException as exc:
                    if exc is not value:
                        raise
                    exc.__traceback__ = traceback
                    return False
                try:
                    raise RuntimeError("generator didn't stop after athrow()")
                finally:
                    await self.gen.aclose()

    def contextmanager(func):
        """@contextmanager def f(...): ... yield x ...: with f(...) as x: runs the code
        before the yield, the with body, then the code after it."""
        def helper(*args, **kwds):
            return _GeneratorContextManager(func, args, kwds)
        helper.__name__ = getattr(func, "__name__", "helper")
        helper.__qualname__ = getattr(func, "__qualname__", "helper")
        helper.__doc__ = getattr(func, "__doc__", None)
        helper.__wrapped__ = func
        return helper

    def asynccontextmanager(func):
        """@contextmanager for async generator functions (async with f(...) as x)."""
        def helper(*args, **kwds):
            return _AsyncGeneratorContextManager(func, args, kwds)
        helper.__name__ = getattr(func, "__name__", "helper")
        helper.__qualname__ = getattr(func, "__qualname__", "helper")
        helper.__doc__ = getattr(func, "__doc__", None)
        helper.__wrapped__ = func
        return helper

    class closing(AbstractContextManager):
        """with closing(thing) as t: ... calls thing.close() at the end."""

        def __init__(self, thing):
            self.thing = thing

        def __enter__(self):
            return self.thing

        def __exit__(self, *exc_info):
            self.thing.close()

    class aclosing(AbstractAsyncContextManager):
        """async with aclosing(thing) as t: ... awaits thing.aclose() at the end."""

        def __init__(self, thing):
            self.thing = thing

        async def __aenter__(self):
            return self.thing

        async def __aexit__(self, *exc_info):
            await self.thing.aclose()

    class _RedirectStream(AbstractContextManager):
        _stream = None

        def __init__(self, new_target):
            self._new_target = new_target
            self._old_targets = []

        def __enter__(self):
            self._old_targets.append(getattr(sys, self._stream))
            setattr(sys, self._stream, self._new_target)
            return self._new_target

        def __exit__(self, exctype, excinst, exctb):
            setattr(sys, self._stream, self._old_targets.pop())

    class redirect_stdout(_RedirectStream):
        """with redirect_stdout(f): print() writes to f."""
        _stream = "stdout"

    class redirect_stderr(_RedirectStream):
        """with redirect_stderr(f): sys.stderr is f."""
        _stream = "stderr"

    class suppress(AbstractContextManager):
        """with suppress(E1, E2): ... ends quietly on those exceptions."""

        def __init__(self, *exceptions):
            self._exceptions = exceptions

        def __enter__(self):
            pass

        def __exit__(self, exctype, excinst, exctb):
            if exctype is None:
                return
            if issubclass(exctype, self._exceptions):
                return True
            if issubclass(exctype, BaseExceptionGroup):
                match, rest = excinst.split(self._exceptions)
                if rest is None:
                    return True
                raise rest
            return False

    class _BaseExitStack:
        """Exit callbacks run in reverse order (LIFO)."""

        @staticmethod
        def _create_exit_wrapper(cm, cm_exit):
            return lambda exc_type, exc, tb: cm_exit(cm, exc_type, exc, tb)

        @staticmethod
        def _create_cb_wrapper(callback, /, *args, **kwds):
            def _exit_wrapper(exc_type, exc, tb):
                callback(*args, **kwds)
            return _exit_wrapper

        def __init__(self):
            self._exit_callbacks = []

        def pop_all(self):
            """A new stack with this one's callbacks (this one is left empty)."""
            new_stack = type(self)()
            new_stack._exit_callbacks = self._exit_callbacks
            self._exit_callbacks = []
            return new_stack

        def push(self, exit):
            """An exit callback (or an object's __exit__) to run at the end."""
            _cb_type = type(exit)
            try:
                exit_method = _cb_type.__exit__
            except AttributeError:
                self._push_exit_callback(exit)
            else:
                self._push_cm_exit(exit, exit_method)
            return exit

        def enter_context(self, cm):
            """cm entered now and exited at the end: what its __enter__ gives."""
            cls = type(cm)
            try:
                _enter = cls.__enter__
                _exit = cls.__exit__
            except AttributeError:
                raise TypeError("'" + cls.__module__ + "." + cls.__qualname__ +
                                "' object does not support the context manager protocol") from None
            result = _enter(cm)
            self._push_cm_exit(cm, _exit)
            return result

        def callback(self, callback, /, *args, **kwds):
            """callback(*args, **kwds) run at the end."""
            _exit_wrapper = self._create_cb_wrapper(callback, *args, **kwds)
            _exit_wrapper.__wrapped__ = callback
            self._push_exit_callback(_exit_wrapper)
            return callback

        def _push_cm_exit(self, cm, cm_exit):
            _exit_wrapper = self._create_exit_wrapper(cm, cm_exit)
            self._push_exit_callback(_exit_wrapper, True)

        def _push_exit_callback(self, callback, is_sync=True):
            self._exit_callbacks.append((is_sync, callback))

    class ExitStack(_BaseExitStack, AbstractContextManager):
        """with ExitStack() as stack: stack.enter_context(cm) ... - exited in reverse order."""

        def __enter__(self):
            return self

        def __exit__(self, *exc_details):
            exc = exc_details[1]
            received_exc = exc is not None
            frame_exc = sys.exception()

            def _fix_exception_context(new_exc, old_exc):
                while 1:
                    exc_context = new_exc.__context__
                    if exc_context is None or exc_context is old_exc:
                        return
                    if exc_context is frame_exc:
                        break
                    new_exc = exc_context
                new_exc.__context__ = old_exc

            suppressed_exc = False
            pending_raise = False
            while self._exit_callbacks:
                is_sync, cb = self._exit_callbacks.pop()
                try:
                    if exc is None:
                        exc_details = None, None, None
                    else:
                        exc_details = type(exc), exc, exc.__traceback__
                    if cb(*exc_details):
                        suppressed_exc = True
                        pending_raise = False
                        exc = None
                except BaseException as new_exc:
                    _fix_exception_context(new_exc, exc)
                    pending_raise = True
                    exc = new_exc
            if pending_raise:
                try:
                    fixed_ctx = exc.__context__
                    raise exc
                except BaseException:
                    exc.__context__ = fixed_ctx
                    raise
            return received_exc and suppressed_exc

        def close(self):
            """Runs the callbacks now."""
            self.__exit__(None, None, None)

    class AsyncExitStack(_BaseExitStack, AbstractAsyncContextManager):
        """ExitStack for async context managers (enter_async_context, push_async_callback)."""

        @staticmethod
        def _create_async_exit_wrapper(cm, cm_exit):
            return lambda exc_type, exc, tb: cm_exit(cm, exc_type, exc, tb)

        @staticmethod
        def _create_async_cb_wrapper(callback, /, *args, **kwds):
            async def _exit_wrapper(exc_type, exc, tb):
                await callback(*args, **kwds)
            return _exit_wrapper

        async def enter_async_context(self, cm):
            cls = type(cm)
            try:
                _enter = cls.__aenter__
                _exit = cls.__aexit__
            except AttributeError:
                raise TypeError("'" + cls.__module__ + "." + cls.__qualname__ +
                                "' object does not support the asynchronous context manager protocol") from None
            result = await _enter(cm)
            self._push_async_cm_exit(cm, _exit)
            return result

        def push_async_exit(self, exit):
            _cb_type = type(exit)
            try:
                exit_method = _cb_type.__aexit__
            except AttributeError:
                self._push_exit_callback(exit, False)
            else:
                self._push_async_cm_exit(exit, exit_method)
            return exit

        def push_async_callback(self, callback, /, *args, **kwds):
            _exit_wrapper = self._create_async_cb_wrapper(callback, *args, **kwds)
            _exit_wrapper.__wrapped__ = callback
            self._push_exit_callback(_exit_wrapper, False)
            return callback

        async def aclose(self):
            await self.__aexit__(None, None, None)

        def _push_async_cm_exit(self, cm, cm_exit):
            _exit_wrapper = self._create_async_exit_wrapper(cm, cm_exit)
            self._push_exit_callback(_exit_wrapper, False)

        async def __aenter__(self):
            return self

        async def __aexit__(self, *exc_details):
            exc = exc_details[1]
            received_exc = exc is not None
            suppressed_exc = False
            pending_raise = False
            while self._exit_callbacks:
                is_sync, cb = self._exit_callbacks.pop()
                try:
                    if exc is None:
                        cb_details = None, None, None
                    else:
                        cb_details = type(exc), exc, exc.__traceback__
                    if is_sync:
                        cb_suppress = cb(*cb_details)
                    else:
                        cb_suppress = await cb(*cb_details)
                    if cb_suppress:
                        suppressed_exc = True
                        pending_raise = False
                        exc = None
                except BaseException as new_exc:
                    pending_raise = True
                    exc = new_exc
            if pending_raise:
                raise exc
            return received_exc and suppressed_exc

    class nullcontext(AbstractContextManager, AbstractAsyncContextManager):
        """A context manager doing nothing: with nullcontext(x) as y gives y = x."""

        def __init__(self, enter_result=None):
            self.enter_result = enter_result

        def __enter__(self):
            return self.enter_result

        def __exit__(self, *excinfo):
            pass

        async def __aenter__(self):
            return self.enter_result

        async def __aexit__(self, *excinfo):
            pass

    class chdir(AbstractContextManager):
        """with chdir(path): ... runs in that directory, then goes back."""

        def __init__(self, path):
            self.path = path
            self._old_cwd = []

        def __enter__(self):
            import os
            self._old_cwd.append(os.getcwd())
            os.chdir(self.path)

        def __exit__(self, *excinfo):
            import os
            os.chdir(self._old_cwd.pop())
