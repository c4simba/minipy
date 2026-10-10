# Copyright 2009 Brian Quinlan. All Rights Reserved.
# Licensed to PSF under a Contributor Agreement.

"""Execute computations asynchronously using threads (CPython's concurrent.futures; no process pools).

Compiled programs: ThreadPoolExecutor's threads are cooperative ones (as threading's); submit(fn, *args)
takes arguments of any types (a typed copy per call), a Future is typed by fn's result (None: None)."""
import sys

if not sys._compiled:
    __author__ = 'Brian Quinlan (brian@sweetapp.com)'

    from concurrent.futures._base import (FIRST_COMPLETED,
                                          FIRST_EXCEPTION,
                                          ALL_COMPLETED,
                                          CancelledError,
                                          TimeoutError,
                                          InvalidStateError,
                                          BrokenExecutor,
                                          Future,
                                          Executor,
                                          wait,
                                          as_completed)

    __all__ = [
        'FIRST_COMPLETED',
        'FIRST_EXCEPTION',
        'ALL_COMPLETED',
        'CancelledError',
        'TimeoutError',
        'InvalidStateError',
        'BrokenExecutor',
        'Future',
        'Executor',
        'wait',
        'as_completed',
        'ProcessPoolExecutor',
        'ThreadPoolExecutor',
    ]


    try:
        import _interpreters
    except ImportError:
        _interpreters = None

    if _interpreters:
        __all__.append('InterpreterPoolExecutor')


    def __dir__():
        return __all__ + ['__author__', '__doc__']


    def __getattr__(name):
        global ProcessPoolExecutor, ThreadPoolExecutor, InterpreterPoolExecutor

        if name == 'ProcessPoolExecutor':
            from .process import ProcessPoolExecutor
            return ProcessPoolExecutor

        if name == 'ThreadPoolExecutor':
            from .thread import ThreadPoolExecutor
            return ThreadPoolExecutor

        if _interpreters and name == 'InterpreterPoolExecutor':
            from .interpreter import InterpreterPoolExecutor
            return InterpreterPoolExecutor

        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


if sys._compiled:
    import threading as _threading
    import time as _time
    from typing import Callable, Generic, TypeVar

    _T = TypeVar("_T")
    _F = TypeVar("_F")
    _A = TypeVar("_A")

    __all__ = ['FIRST_COMPLETED', 'FIRST_EXCEPTION', 'ALL_COMPLETED', 'CancelledError', 'TimeoutError',
               'InvalidStateError', 'BrokenExecutor', 'Future', 'Executor', 'wait', 'as_completed',
               'ThreadPoolExecutor']

    FIRST_COMPLETED = 'FIRST_COMPLETED'
    FIRST_EXCEPTION = 'FIRST_EXCEPTION'
    ALL_COMPLETED = 'ALL_COMPLETED'
    _AS_COMPLETED = '_AS_COMPLETED'

    # Possible future states (for internal use by the futures package).
    PENDING = 'PENDING'
    RUNNING = 'RUNNING'
    # The future was cancelled by the user...
    CANCELLED = 'CANCELLED'
    # ...and _Waiter.add_cancelled() was called by a worker.
    CANCELLED_AND_NOTIFIED = 'CANCELLED_AND_NOTIFIED'
    FINISHED = 'FINISHED'

    _STATE_TO_DESCRIPTION_MAP = {
        PENDING: "pending",
        RUNNING: "running",
        CANCELLED: "cancelled",
        CANCELLED_AND_NOTIFIED: "cancelled",
        FINISHED: "finished"
    }

    class Error(Exception):
        """Base class for all future-related exceptions."""

    class CancelledError(Error):
        """The Future was cancelled."""

    class InvalidStateError(Error):
        """The operation is not allowed in this state."""

    TimeoutError = TimeoutError

    class BrokenExecutor(RuntimeError):
        """Raised when a executor has become non-functional after a severe failure."""

    class BrokenThreadPool(BrokenExecutor):
        """Raised when a worker thread in a ThreadPoolExecutor failed initializing."""

    class Future(Generic[_T]):
        """Represents the result of an asynchronous computation."""

        def __init__(self) -> None:
            """Initializes the future. Should not be called by clients."""
            self._condition = _threading.Condition()
            self._state = PENDING
            self._result: list[_T] = []
            self._exception: BaseException | None = None
            self._waiters: list[_threading.Event] = []
            self._done_callbacks: list[Callable[[], None]] = []

        def _invoke_callbacks(self) -> None:
            for callback in self._done_callbacks:
                try:
                    callback()
                except Exception as e:
                    print("exception calling callback for", repr(self), "-", type(e).__name__ + ":", e, file=sys.stderr)

        def _wake(self) -> None:
            for w in self._waiters:
                w.set()

        def __repr__(self) -> str:
            with self._condition:
                head = '<%s at %#x state=%s' % (type(self).__name__, id(self), _STATE_TO_DESCRIPTION_MAP[self._state])
                if self._state == FINISHED:
                    e = self._exception
                    if e is not None:
                        return head + ' raised %s>' % type(e).__name__
                    return head + ' returned %s>' % type(self._result[0]).__name__
                return head + '>'

        def cancel(self) -> bool:
            """Cancel the future if possible.  True if the future was cancelled, False otherwise."""
            with self._condition:
                if self._state in [RUNNING, FINISHED]:
                    return False
                if self._state in [CANCELLED, CANCELLED_AND_NOTIFIED]:
                    return True
                self._state = CANCELLED
                self._condition.notify_all()
            self._wake()
            self._invoke_callbacks()
            return True

        def cancelled(self) -> bool:
            """Return True if the future was cancelled."""
            with self._condition:
                return self._state in [CANCELLED, CANCELLED_AND_NOTIFIED]

        def running(self) -> bool:
            """Return True if the future is currently executing."""
            with self._condition:
                return self._state == RUNNING

        def done(self) -> bool:
            """Return True if the future was cancelled or finished executing."""
            with self._condition:
                return self._state in [CANCELLED, CANCELLED_AND_NOTIFIED, FINISHED]

        def __get_result(self) -> _T:
            e = self._exception
            if e is not None:
                raise e
            return self._result[0]

        def add_done_callback(self, fn) -> None:
            """Attaches a callable that will be called when the future finishes (with the future)."""
            def call() -> None:
                fn(self)
            with self._condition:
                if self._state not in [CANCELLED, CANCELLED_AND_NOTIFIED, FINISHED]:
                    self._done_callbacks.append(call)
                    return
            try:
                call()
            except Exception as e:
                print("exception calling callback for", repr(self), "-", type(e).__name__ + ":", e, file=sys.stderr)

        def _wait_done(self, timeout: float | None) -> None:
            if self._state in [CANCELLED, CANCELLED_AND_NOTIFIED]:
                raise CancelledError()
            if self._state == FINISHED:
                return
            self._condition.wait(timeout)
            if self._state in [CANCELLED, CANCELLED_AND_NOTIFIED]:
                raise CancelledError()
            if self._state != FINISHED:
                raise TimeoutError()

        def result(self, timeout: float | None = None) -> _T:
            """Return the result of the call that the future represents (waiting at most timeout seconds)."""
            with self._condition:
                self._wait_done(timeout)
                return self.__get_result()

        def exception(self, timeout: float | None = None) -> BaseException | None:
            """Return the exception raised by the call that the future represents (None: it returned)."""
            with self._condition:
                self._wait_done(timeout)
                return self._exception

        def set_running_or_notify_cancel(self) -> bool:
            """Mark the future as running or process any cancel notifications (False: it was cancelled)."""
            with self._condition:
                if self._state == CANCELLED:
                    self._state = CANCELLED_AND_NOTIFIED
                    return False
                elif self._state == PENDING:
                    self._state = RUNNING
                    return True
                else:
                    raise RuntimeError('Future in unexpected state')

        def set_result(self, result: _T) -> None:
            """Sets the return value of work associated with the future."""
            with self._condition:
                if self._state in {CANCELLED, CANCELLED_AND_NOTIFIED, FINISHED}:
                    raise InvalidStateError('{}: {!r}'.format(self._state, self))
                self._result = [result]
                self._state = FINISHED
                self._condition.notify_all()
            self._wake()
            self._invoke_callbacks()

        def set_exception(self, exception: BaseException) -> None:
            """Sets the result of the future as being the given exception."""
            with self._condition:
                if self._state in {CANCELLED, CANCELLED_AND_NOTIFIED, FINISHED}:
                    raise InvalidStateError('{}: {!r}'.format(self._state, self))
                self._exception = exception
                self._state = FINISHED
                self._condition.notify_all()
            self._wake()
            self._invoke_callbacks()

    class DoneAndNotDoneFutures:
        """(done, not_done): sets of futures."""

        def __init__(self, done, not_done) -> None:
            self.done = done
            self.not_done = not_done

        def __iter__(self):
            yield self.done
            yield self.not_done

        def __getitem__(self, i: int):
            return self.done if i == 0 else self.not_done

        def __len__(self) -> int:
            return 2

        def __repr__(self) -> str:
            return "DoneAndNotDoneFutures(done=%r, not_done=%r)" % (self.done, self.not_done)

    def wait(fs, timeout: float | None = None, return_when: str = ALL_COMPLETED):
        """Wait for the futures in fs to complete: (done, not_done) - sets of them."""
        deadline = None if timeout is None else _time.monotonic() + timeout
        event = _threading.Event()
        for f in fs:
            f._waiters.append(event)
        try:
            while True:
                done = set([f for f in fs if f._state in [CANCELLED_AND_NOTIFIED, FINISHED, CANCELLED]])
                if return_when == FIRST_COMPLETED and done:
                    break
                if return_when == FIRST_EXCEPTION and any(f._exception is not None for f in done):
                    break
                if len(done) == len(set(fs)):
                    break
                left = None if deadline is None else deadline - _time.monotonic()
                if left is not None and left <= 0:
                    break
                event.wait(left)
                event.clear()
        finally:
            for f in fs:
                f._waiters.remove(event)
        not_done = set([f for f in fs if f not in done])
        return DoneAndNotDoneFutures(done, not_done)

    def as_completed(fs, timeout: float | None = None):
        """The futures of fs as they complete (finished or cancelled)."""
        deadline = None if timeout is None else _time.monotonic() + timeout
        pending = []
        for f in fs:
            if f not in pending:
                pending.append(f)
        total = len(pending)
        event = _threading.Event()
        for f in pending:
            f._waiters.append(event)
        try:
            while pending:
                finished = [f for f in pending if f._state in [CANCELLED_AND_NOTIFIED, FINISHED, CANCELLED]]
                for f in finished:
                    pending.remove(f)
                    yield f
                if not pending:
                    break
                left = None if deadline is None else deadline - _time.monotonic()
                if left is not None and left <= 0:
                    raise TimeoutError('%d (of %d) futures unfinished' % (len(pending), total))
                event.wait(left)
                event.clear()
        finally:
            for f in fs:
                if event in f._waiters:
                    f._waiters.remove(event)

    class Executor:
        """This is an abstract base class for concrete asynchronous executors."""

        def shutdown(self, wait: bool = True, *, cancel_futures: bool = False) -> None:
            pass

        def __enter__(self) -> "Executor":
            return self

        def __exit__(self, exc_type, exc_val, exc_tb) -> bool:
            self.shutdown(wait=True)
            return False

    _counter = 0

    class ThreadPoolExecutor(Executor):
        """An executor that runs calls in a pool of threads (cooperative threads in compiled programs)."""

        def __init__(self, max_workers: int | None = None, thread_name_prefix: str = '', initializer: _F = None,
                     initargs: _A = ()) -> None:
            global _counter
            if max_workers is None:
                max_workers = 5
            if max_workers <= 0:
                raise ValueError("max_workers must be greater than 0")
            self._max_workers = max_workers
            self._work: list[Callable[[], None]] = []
            self._cond = _threading.Condition()
            self._threads: list[_threading.Thread] = []
            self._idle = 0
            self._broken = ""
            self._shutdown = False
            _counter += 1
            self._thread_name_prefix = thread_name_prefix or ("ThreadPoolExecutor-%d" % (_counter - 1))
            self._initializer: Callable[[], None] | None = None
            if initializer is not None:
                def init() -> None:
                    initializer(*initargs)
                self._initializer = init

        def __enter__(self) -> "ThreadPoolExecutor":
            return self

        def submit(self, fn, *args: sys._PackArgs):
            """Submits fn(*args) to be executed: its Future."""
            with self._cond:
                if self._broken:
                    raise BrokenThreadPool(self._broken)
                if self._shutdown:
                    raise RuntimeError('cannot schedule new futures after shutdown')
                f = Future()

                def run() -> None:                  # f gets what fn(*args) returns (or raises)
                    if not f.set_running_or_notify_cancel():
                        return
                    try:
                        if sys._void(fn(*args)):
                            fn(*args)
                            f.set_result(None)
                        else:
                            f.set_result(fn(*args))
                    except BaseException as exc:
                        f.set_exception(exc)

                self._work.append(run)
                self._cond.notify()
                self._adjust_thread_count()
                return f

        def map(self, fn, *iterables: sys._PackArgs, timeout: float | None = None, chunksize: int = 1):
            """fn applied to the items of the iterables (in order), computed by the pool."""
            futures = [self.submit(fn, *args) for args in zip(*iterables)]
            deadline = None if timeout is None else _time.monotonic() + timeout
            for f in futures:
                yield f.result(None if deadline is None else deadline - _time.monotonic())

        def _adjust_thread_count(self) -> None:
            if self._idle > 0 or len(self._threads) >= self._max_workers:
                return
            name = '%s_%d' % (self._thread_name_prefix, len(self._threads))
            t = _threading.Thread(name=name, target=self._worker)
            t.daemon = True
            self._threads.append(t)
            t.start()

        def _worker(self) -> None:
            init = self._initializer
            if init is not None:
                try:
                    init()
                except BaseException as e:
                    print("Exception in initializer:", type(e).__name__ + ":", e, file=sys.stderr)
                    with self._cond:
                        self._broken = 'A thread initializer failed, the thread pool is not usable anymore'
                        for item in self._work:
                            pass
                        self._work = []
                    return
            while True:
                with self._cond:
                    while not self._work and not self._shutdown:
                        self._idle += 1
                        self._cond.wait()
                        self._idle -= 1
                    if not self._work:
                        return
                    item = self._work.pop(0)
                item()

        def shutdown(self, wait: bool = True, *, cancel_futures: bool = False) -> None:
            """Clean-up the resources (no new calls; waits for the running ones unless wait=False)."""
            with self._cond:
                self._shutdown = True
                if cancel_futures:
                    self._work = []
                self._cond.notify_all()
            if wait:
                for t in self._threads:
                    t.join()
