"""Thread-based parallelism (CPython's threading, on the interpreter's _thread): Thread, Timer,
Lock, RLock, Condition, Semaphore, BoundedSemaphore, Event, Barrier, local, current_thread ...
(The interpreter's threads take turns holding one interpreter lock, as CPython's do. In compiled
programs threads are cooperative tasks: they switch when one waits (sleep, a lock, I/O); there a
Thread's target with its args is bound when the Thread is made, and local() is not available.)"""
import sys

if not sys._compiled:
    import sys as _sys
    import _thread
    from time import monotonic as _time

    __all__ = ["get_ident", "active_count", "Condition", "current_thread", "enumerate", "main_thread", "TIMEOUT_MAX",
               "Event", "Lock", "RLock", "Semaphore", "BoundedSemaphore", "Thread", "Barrier", "BrokenBarrierError",
               "Timer", "ThreadError", "ExceptHookArgs", "setprofile", "settrace", "local", "stack_size", "excepthook",
               "get_native_id", "getprofile", "gettrace"]

    _start_new_thread = _thread.start_new_thread
    _allocate_lock = _thread.allocate_lock
    get_ident = _thread.get_ident
    get_native_id = _thread.get_native_id
    ThreadError = _thread.error
    TIMEOUT_MAX = _thread.TIMEOUT_MAX
    Lock = _allocate_lock

    _profile_hook = None
    _trace_hook = None


    def setprofile(func):
        global _profile_hook
        _profile_hook = func


    def getprofile():
        return _profile_hook


    def settrace(func):
        global _trace_hook
        _trace_hook = func


    def gettrace():
        return _trace_hook


    setprofile_all_threads = setprofile
    settrace_all_threads = settrace


    def RLock(*args, **kwargs):
        """A reentrant lock: the thread holding it may take it again (as often as it releases it)."""
        return _RLock(*args, **kwargs)


    class _RLock:
        def __init__(self):
            self._block = _allocate_lock()
            self._owner = None
            self._count = 0

        def __repr__(self):
            owner = self._owner
            return "<%s %s.%s object owner=%r count=%d at %s>" % (
                "locked" if self._block.locked() else "unlocked", self.__class__.__module__,
                self.__class__.__qualname__, owner, self._count, hex(id(self)))

        def acquire(self, blocking=True, timeout=-1):
            me = get_ident()
            if self._owner == me:
                self._count += 1
                return True
            rc = self._block.acquire(blocking, timeout)
            if rc:
                self._owner = me
                self._count = 1
            return rc

        __enter__ = acquire

        def release(self):
            if self._owner != get_ident():
                raise RuntimeError("cannot release un-acquired lock")
            self._count = count = self._count - 1
            if not count:
                self._owner = None
                self._block.release()

        def __exit__(self, t, v, tb):
            self.release()

        def locked(self):
            return self._block.locked()

        def _acquire_restore(self, state):
            self._block.acquire()
            self._count, self._owner = state

        def _release_save(self):
            if self._count == 0:
                raise RuntimeError("cannot release un-acquired lock")
            count = self._count
            self._count = 0
            owner = self._owner
            self._owner = None
            self._block.release()
            return (count, owner)

        def _is_owned(self):
            return self._owner == get_ident()

        def _recursion_count(self):
            if self._owner != get_ident():
                return 0
            return self._count


    _PyRLock = _RLock


    class Condition:
        """A condition variable: threads wait() until another notify()s them (holding the lock)."""

        def __init__(self, lock=None):
            if lock is None:
                lock = RLock()
            self._lock = lock
            self.acquire = lock.acquire
            self.release = lock.release
            if hasattr(lock, "_release_save"):
                self._release_save = lock._release_save
            if hasattr(lock, "_acquire_restore"):
                self._acquire_restore = lock._acquire_restore
            if hasattr(lock, "_is_owned"):
                self._is_owned = lock._is_owned
            self._waiters = []

        def __enter__(self):
            return self._lock.__enter__()

        def __exit__(self, *args):
            return self._lock.__exit__(*args)

        def __repr__(self):
            return "<Condition(%s, %d)>" % (self._lock, len(self._waiters))

        def _release_save(self):
            self._lock.release()

        def _acquire_restore(self, x):
            self._lock.acquire()

        def _is_owned(self):
            if self._lock.acquire(False):
                self._lock.release()
                return False
            return True

        def wait(self, timeout=None):
            """Releases the lock and waits for notify() (or timeout seconds); False on timeout."""
            if not self._is_owned():
                raise RuntimeError("cannot wait on un-acquired lock")
            waiter = _allocate_lock()
            waiter.acquire()
            self._waiters.append(waiter)
            saved_state = self._release_save()
            gotit = False
            try:
                if timeout is None:
                    waiter.acquire()
                    gotit = True
                else:
                    if timeout > 0:
                        gotit = waiter.acquire(True, timeout)
                    else:
                        gotit = waiter.acquire(False)
                return gotit
            finally:
                self._acquire_restore(saved_state)
                if not gotit:
                    try:
                        self._waiters.remove(waiter)
                    except ValueError:
                        pass

        def wait_for(self, predicate, timeout=None):
            """Waits until predicate() is true (or timeout seconds); the last predicate() value."""
            endtime = None
            waittime = timeout
            result = predicate()
            while not result:
                if waittime is not None:
                    if endtime is None:
                        endtime = _time() + waittime
                    else:
                        waittime = endtime - _time()
                        if waittime <= 0:
                            break
                self.wait(waittime)
                result = predicate()
            return result

        def notify(self, n=1):
            """Wakes up n of the waiting threads."""
            if not self._is_owned():
                raise RuntimeError("cannot notify on un-acquired lock")
            waiters = self._waiters
            while waiters and n > 0:
                waiter = waiters[0]
                try:
                    waiter.release()
                except RuntimeError:
                    pass
                else:
                    n -= 1
                try:
                    waiters.remove(waiter)
                except ValueError:
                    pass

        def notify_all(self):
            """Wakes up all the waiting threads."""
            self.notify(len(self._waiters))

        def notifyAll(self):
            self.notify_all()


    class Semaphore:
        """A counter: acquire() takes one (waiting at 0), release() gives back."""

        def __init__(self, value=1):
            if value < 0:
                raise ValueError("semaphore initial value must be >= 0")
            self._cond = Condition(Lock())
            self._value = value

        def __repr__(self):
            cls = self.__class__
            return f"<{cls.__module__}.{cls.__qualname__} at {id(self):#x}: value={self._value}>"

        def acquire(self, blocking=True, timeout=None):
            if not blocking and timeout is not None:
                raise ValueError("can't specify timeout for non-blocking acquire")
            rc = False
            endtime = None
            with self._cond:
                while self._value == 0:
                    if not blocking:
                        break
                    if timeout is not None:
                        if endtime is None:
                            endtime = _time() + timeout
                        else:
                            timeout = endtime - _time()
                            if timeout <= 0:
                                break
                    self._cond.wait(timeout)
                else:
                    self._value -= 1
                    rc = True
            return rc

        __enter__ = acquire

        def release(self, n=1):
            if n < 1:
                raise ValueError("n must be one or more")
            with self._cond:
                self._value += n
                self._cond.notify(n)

        def __exit__(self, t, v, tb):
            self.release()


    class BoundedSemaphore(Semaphore):
        """A Semaphore that may not be released above its initial value."""

        def __init__(self, value=1):
            super().__init__(value)
            self._initial_value = value

        def __repr__(self):
            cls = self.__class__
            return (f"<{cls.__module__}.{cls.__qualname__} at {id(self):#x}:"
                    f" value={self._value}/{self._initial_value}>")

        def release(self, n=1):
            if n < 1:
                raise ValueError("n must be one or more")
            with self._cond:
                if self._value + n > self._initial_value:
                    raise ValueError("Semaphore released too many times")
                self._value += n
                self._cond.notify(n)


    class Event:
        """A flag threads wait() for until another set()s it."""

        def __init__(self):
            self._cond = Condition(Lock())
            self._flag = False

        def __repr__(self):
            cls = self.__class__
            status = "set" if self._flag else "unset"
            return f"<{cls.__module__}.{cls.__qualname__} at {id(self):#x}: {status}>"

        def is_set(self):
            return self._flag

        def isSet(self):
            return self.is_set()

        def set(self):
            with self._cond:
                self._flag = True
                self._cond.notify_all()

        def clear(self):
            with self._cond:
                self._flag = False

        def wait(self, timeout=None):
            """Waits until the flag is set (or timeout seconds); the flag."""
            with self._cond:
                signaled = self._flag
                if not signaled:
                    signaled = self._cond.wait(timeout)
                return signaled


    class BrokenBarrierError(RuntimeError):
        pass


    class Barrier:
        """parties threads wait() until all of them have come."""

        def __init__(self, parties, action=None, timeout=None):
            if parties < 1:
                raise ValueError("parties must be >= 1")
            self._cond = Condition(Lock())
            self._action = action
            self._timeout = timeout
            self._parties = parties
            self._state = 0          # 0 filling, 1 draining, -1 resetting, -2 broken
            self._count = 0

        def __repr__(self):
            cls = self.__class__
            if self.broken:
                return f"<{cls.__module__}.{cls.__qualname__} at {id(self):#x}: broken>"
            return (f"<{cls.__module__}.{cls.__qualname__} at {id(self):#x}:"
                    f" waiters={self.n_waiting}/{self.parties}>")

        def wait(self, timeout=None):
            if timeout is None:
                timeout = self._timeout
            with self._cond:
                self._enter()
                index = self._count
                self._count += 1
                try:
                    if index + 1 == self._parties:
                        self._release()
                    else:
                        self._wait(timeout)
                    return index
                finally:
                    self._count -= 1
                    self._exit()

        def _enter(self):
            while self._state in (-1, 1):
                self._cond.wait()
            if self._state < 0:
                raise BrokenBarrierError
            assert self._state == 0

        def _release(self):
            try:
                if self._action:
                    self._action()
                self._state = 1
                self._cond.notify_all()
            except:
                self._break()
                raise

        def _wait(self, timeout):
            if not self._cond.wait_for(lambda: self._state != 0, timeout):
                self._break()
                raise BrokenBarrierError
            if self._state < 0:
                raise BrokenBarrierError
            assert self._state == 1

        def _exit(self):
            if self._count == 0:
                if self._state in (-1, 1):
                    self._state = 0
                    self._cond.notify_all()

        def reset(self):
            with self._cond:
                if self._count > 0:
                    if self._state == 0:
                        self._state = -1
                    elif self._state == -2:
                        self._state = -1
                else:
                    self._state = 0
                self._cond.notify_all()

        def abort(self):
            with self._cond:
                self._break()

        def _break(self):
            self._state = -2
            self._cond.notify_all()

        @property
        def parties(self):
            return self._parties

        @property
        def n_waiting(self):
            if self._state == 0:
                return self._count
            return 0

        @property
        def broken(self):
            return self._state == -2


    _counter = 0


    def _newname(name_template):
        global _counter
        _counter += 1
        return name_template % _counter


    _active_limbo_lock = RLock()
    _active = {}        # ident -> Thread
    _limbo = {}
    _shutdown_locks = set()


    class ExceptHookArgs:
        """excepthook()'s argument: exc_type, exc_value, exc_traceback, thread."""

        def __init__(self, exc_type, exc_value, exc_traceback, thread):
            self.exc_type = exc_type
            self.exc_value = exc_value
            self.exc_traceback = exc_traceback
            self.thread = thread

        def __iter__(self):
            return iter((self.exc_type, self.exc_value, self.exc_traceback, self.thread))

        def __getitem__(self, i):
            return (self.exc_type, self.exc_value, self.exc_traceback, self.thread)[i]

        def __len__(self):
            return 4


    def excepthook(args, /):
        """What a thread's uncaught exception does: its traceback on sys.stderr (SystemExit: nothing)."""
        if args.exc_type == SystemExit:
            return
        stderr = _sys.stderr
        if stderr is None:
            return
        if args.thread is not None:
            name = args.thread.name
        else:
            name = get_ident()
        print(f"Exception in thread {name}:", file=stderr, flush=True)
        stderr.write(_sys._format_exception(args.exc_value))
        stderr.flush()


    __excepthook__ = excepthook


    class Thread:
        """A thread of control: Thread(target=f, args=(...)).start(), or a subclass with run()."""

        _initialized = False

        def __init__(self, group=None, target=None, name=None, args=(), kwargs=None, *, daemon=None, context=None):
            assert group is None, "group argument must be None for now"
            if kwargs is None:
                kwargs = {}
            if name:
                name = str(name)
            else:
                name = _newname("Thread-%d")
                if target is not None:
                    try:
                        target_name = target.__name__
                        name += f" ({target_name})"
                    except AttributeError:
                        pass
            self._target = target
            self._name = name
            self._args = args
            self._kwargs = kwargs
            if daemon is not None:
                if daemon and not _daemon_threads_allowed():
                    raise RuntimeError("daemon threads are disabled in this (sub)interpreter")
                self._daemonic = daemon
            else:
                self._daemonic = current_thread().daemon
            self._ident = None
            self._native_id = None
            self._started = Event()
            self._is_stopped = False
            self._done = _allocate_lock()
            self._initialized = True
            self._stderr = _sys.stderr
            self._invoke_excepthook = _invoke_excepthook

        def __repr__(self):
            assert self._initialized, "Thread.__init__() was not called"
            status = "initial"
            if self._started.is_set():
                status = "started"
            if self._is_stopped:
                status = "stopped"
            if self._daemonic:
                status += " daemon"
            if self._ident is not None:
                status += " %s" % self._ident
            return "<%s(%s, %s)>" % (self.__class__.__name__, self._name, status)

        def start(self):
            """Runs run() in a new thread."""
            if not self._initialized:
                raise RuntimeError("thread.__init__() not called")
            if self._started.is_set():
                raise RuntimeError("threads can only be started once")
            with _active_limbo_lock:
                _limbo[self] = self
            self._done.acquire()
            try:
                _start_new_thread(self._bootstrap, ())
            except Exception:
                with _active_limbo_lock:
                    del _limbo[self]
                self._done.release()
                raise
            self._started.wait()

        def run(self):
            """What the thread does: target(*args, **kwargs) (subclasses: their own)."""
            try:
                if self._target is not None:
                    self._target(*self._args, **self._kwargs)
            finally:
                del self._target, self._args, self._kwargs

        def _bootstrap(self):
            try:
                self._ident = get_ident()
                self._native_id = get_native_id()
                self._started.set()
                with _active_limbo_lock:
                    _active[self._ident] = self
                    del _limbo[self]
                try:
                    self.run()
                except BaseException as e:
                    self._invoke_excepthook(self, e)
            finally:
                self._is_stopped = True
                with _active_limbo_lock:
                    _active.pop(self._ident, None)
                self._done.release()

        def join(self, timeout=None):
            """Waits for the thread to end (or timeout seconds: then is_alive() tells)."""
            if not self._initialized:
                raise RuntimeError("Thread.__init__() not called")
            if not self._started.is_set():
                raise RuntimeError("cannot join thread before it is started")
            if self is current_thread():
                raise RuntimeError("cannot join current thread")
            if timeout is None:
                got = self._done.acquire()
            else:
                got = self._done.acquire(True, max(timeout, 0))
            if got:
                self._done.release()

        @property
        def name(self):
            assert self._initialized, "Thread.__init__() not called"
            return self._name

        @name.setter
        def name(self, name):
            assert self._initialized, "Thread.__init__() not called"
            self._name = str(name)

        @property
        def ident(self):
            assert self._initialized, "Thread.__init__() not called"
            return self._ident

        @property
        def native_id(self):
            assert self._initialized, "Thread.__init__() not called"
            return self._native_id

        def is_alive(self):
            assert self._initialized, "Thread.__init__() not called"
            return self._started.is_set() and not self._is_stopped

        @property
        def daemon(self):
            assert self._initialized, "Thread.__init__() not called"
            return self._daemonic

        @daemon.setter
        def daemon(self, daemonic):
            if not self._initialized:
                raise RuntimeError("Thread.__init__() not called")
            if daemonic and not _daemon_threads_allowed():
                raise RuntimeError("daemon threads are disabled in this interpreter")
            if self._started.is_set():
                raise RuntimeError("cannot set daemon status of active thread")
            self._daemonic = daemonic

        def isDaemon(self):
            return self.daemon

        def setDaemon(self, daemonic):
            self.daemon = daemonic

        def getName(self):
            return self.name

        def setName(self, name):
            self.name = name


    def _daemon_threads_allowed():
        return True


    def _invoke_excepthook(thread, exc):
        hook = excepthook
        try:
            hook(ExceptHookArgs(type(exc), exc, exc.__traceback__, thread))
        except Exception as e:
            stderr = _sys.stderr
            print("Exception in threading.excepthook:", file=stderr, flush=True)
            stderr.write(_sys._format_exception(e))


    class Timer(Thread):
        """Calls function(*args, **kwargs) after interval seconds (unless cancel()led)."""

        def __init__(self, interval, function, args=None, kwargs=None):
            Thread.__init__(self)
            self.interval = interval
            self.function = function
            self.args = args if args is not None else []
            self.kwargs = kwargs if kwargs is not None else {}
            self.finished = Event()

        def cancel(self):
            self.finished.set()

        def run(self):
            self.finished.wait(self.interval)
            if not self.finished.is_set():
                self.function(*self.args, **self.kwargs)
            self.finished.set()


    class _MainThread(Thread):
        def __init__(self):
            Thread.__init__(self, name="MainThread", daemon=False)
            self._started.set()
            self._ident = get_ident()
            self._native_id = get_native_id()
            with _active_limbo_lock:
                _active[self._ident] = self


    class _DummyThread(Thread):
        def __init__(self):
            Thread.__init__(self, name=_newname("Dummy-%d"), daemon=_daemon_threads_allowed())
            self._started.set()
            self._ident = get_ident()
            self._native_id = get_native_id()
            with _active_limbo_lock:
                _active[self._ident] = self

        def is_alive(self):
            return not self._is_stopped and self._started.is_set()

        def join(self, timeout=None):
            raise RuntimeError("cannot join a dummy thread")


    def current_thread():
        """The Thread object of the running thread."""
        try:
            return _active[get_ident()]
        except KeyError:
            return _DummyThread()


    currentThread = current_thread


    def active_count():
        """The number of Thread objects alive."""
        with _active_limbo_lock:
            return len(_active) + len(_limbo)


    activeCount = active_count


    def _enumerate():
        return list(_active.values()) + list(_limbo.values())


    def enumerate():
        """The Thread objects alive."""
        with _active_limbo_lock:
            return list(_active.values()) + list(_limbo.values())


    _threading_atexits = []
    _SHUTTING_DOWN = False


    def _register_atexit(func, *arg, **kwargs):
        if _SHUTTING_DOWN:
            raise RuntimeError("can't register atexit after shutdown")
        _threading_atexits.append(lambda: func(*arg, **kwargs))


    def _shutdown():
        """(At exit: the threads that are not daemons are waited for.)"""
        global _SHUTTING_DOWN
        _SHUTTING_DOWN = True
        for atexit_call in reversed(_threading_atexits):
            atexit_call()
        while True:
            with _active_limbo_lock:
                threads = [t for t in _enumerate() if not t.daemon and t is not _main_thread and t.is_alive()]
            if not threads:
                break
            for t in threads:
                t.join()


    def main_thread():
        """The Thread object of the main thread."""
        return _main_thread


    def stack_size(size=0):
        return _thread.stack_size(size)


    class local:
        """Attributes of their own in each thread: data = local(); data.x = 1 (a subclass's __init__
        runs again in each thread that uses it)."""

        def __new__(cls, /, *args, **kw):
            if (args or kw) and (cls.__init__ is object.__init__):
                raise TypeError("Initialization arguments are not supported")
            self = object.__new__(cls)
            object.__setattr__(self, "_local__dicts", {})
            object.__setattr__(self, "_local__args", (args, kw))
            object.__setattr__(self, "_local__owner", get_ident())
            return self

        def _local__mine(self):
            dicts = object.__getattribute__(self, "_local__dicts")
            me = get_ident()
            d = dicts.get(me)
            if d is None:
                d = dicts[me] = {}
                if me != object.__getattribute__(self, "_local__owner"):
                    init = type(self).__init__
                    if init is not object.__init__:
                        args, kw = object.__getattribute__(self, "_local__args")
                        init(self, *args, **kw)
            return d

        def __getattribute__(self, name):
            if name.startswith("_local__") or name == "__class__":
                return object.__getattribute__(self, name)
            d = local._local__mine(self)
            if name == "__dict__":
                return d
            if name in d:
                return d[name]
            return object.__getattribute__(self, name)

        def __setattr__(self, name, value):
            if name == "__dict__":
                raise AttributeError("%r object attribute '__dict__' is read-only" % self.__class__.__name__)
            local._local__mine(self)[name] = value

        def __delattr__(self, name):
            if name == "__dict__":
                raise AttributeError("%r object attribute '__dict__' is read-only" % self.__class__.__name__)
            d = local._local__mine(self)
            if name not in d:
                raise AttributeError(name)
            del d[name]


    _main_thread = _MainThread()

if sys._compiled:
    import thread as _th
    import time as _tm
    from typing import Callable, TypeVar

    _A = TypeVar("_A")
    _F = TypeVar("_F")
    _K = TypeVar("_K")

    TIMEOUT_MAX = 4294967.0
    ThreadError = RuntimeError

    def get_ident() -> int:
        """The running thread's identifier (0: the main program)."""
        return _th.current()

    def get_native_id() -> int:
        return _th.current()

    def _pause() -> None:
        _th.sleep(0.001)

    def _noop() -> None:
        pass

    def _bind(f: _F, args: _A) -> Callable[[], None]:
        return lambda: f(*args)

    def _bind_kw(f: _F, args: _A, kwargs: dict[str, _K]) -> Callable[[], None]:
        return lambda: f(*args, **kwargs)

    class _LockBase:
        def acquire(self, blocking: bool = True, timeout: float = -1) -> bool:
            return True

        def release(self) -> None:
            pass

        def locked(self) -> bool:
            return False

        def _release_save(self) -> int:
            self.release()
            return 0

        def _acquire_restore(self, state: int) -> None:
            self.acquire()

        def _is_owned(self) -> bool:
            if self.acquire(False):
                self.release()
                return False
            return True

        def __enter__(self) -> bool:
            return self.acquire()

        def __exit__(self, t, v, tb) -> None:
            self.release()

    def _wait_until(test: Callable[[], bool], timeout: float) -> bool:
        """Waits (letting the other threads run) until test() (timeout < 0: no end); test()'s last value."""
        if test():
            return True
        end = _tm.monotonic() + timeout
        while not test():
            if timeout >= 0 and _tm.monotonic() >= end:
                return False
            _pause()
        return True

    class Lock(_LockBase):
        """A lock: one holder at a time; any thread may release it."""

        def __init__(self) -> None:
            self._held = False

        def acquire(self, blocking: bool = True, timeout: float = -1) -> bool:
            if not self._held:
                self._held = True
                return True
            if not blocking:
                if timeout != -1:
                    raise ValueError("can't specify a timeout for a non-blocking call")
                return False
            if timeout < 0 and timeout != -1:
                raise ValueError("timeout value must be a non-negative number")
            if not _wait_until(lambda: not self._held, timeout):
                return False
            self._held = True
            return True

        def release(self) -> None:
            if not self._held:
                raise RuntimeError("release unlocked lock")
            self._held = False

        def locked(self) -> bool:
            return self._held

        def __repr__(self) -> str:
            return "<" + ("locked" if self._held else "unlocked") + " _thread.lock object>"

    class RLock(_LockBase):
        """A reentrant lock: its holder may take it again (and must release it as often)."""

        def __init__(self) -> None:
            self._owner = -1
            self._count = 0

        def acquire(self, blocking: bool = True, timeout: float = -1) -> bool:
            me = get_ident()
            if self._count and self._owner == me:
                self._count += 1
                return True
            if self._count:
                if not blocking:
                    return False
                if not _wait_until(lambda: self._count == 0, timeout):
                    return False
            self._owner = me
            self._count = 1
            return True

        def release(self) -> None:
            if not self._count or self._owner != get_ident():
                raise RuntimeError("cannot release un-acquired lock")
            self._count -= 1
            if not self._count:
                self._owner = -1

        def locked(self) -> bool:
            return self._count > 0

        def _release_save(self) -> int:
            if not self._count:
                raise RuntimeError("cannot release un-acquired lock")
            n = self._count
            self._count = 0
            self._owner = -1
            return n

        def _acquire_restore(self, state: int) -> None:
            self.acquire()
            self._count = state

        def _is_owned(self) -> bool:
            return self._count > 0 and self._owner == get_ident()

    class Condition:
        """Threads wait() until another notify()s them, holding the lock."""

        def __init__(self, lock: _LockBase | None = None) -> None:
            if lock is None:
                self._lock: _LockBase = RLock()
            else:
                self._lock = lock
            self._waiters: list[Lock] = []

        def acquire(self, blocking: bool = True, timeout: float = -1) -> bool:
            return self._lock.acquire(blocking, timeout)

        def release(self) -> None:
            self._lock.release()

        def __enter__(self) -> bool:
            return self._lock.acquire()

        def __exit__(self, t, v, tb) -> None:
            self._lock.release()

        def wait(self, timeout: float | None = None) -> bool:
            """Releases the lock and waits for notify() (or timeout seconds); False on timeout."""
            if not self._lock._is_owned():
                raise RuntimeError("cannot wait on un-acquired lock")
            waiter = Lock()
            waiter.acquire()
            self._waiters.append(waiter)
            saved = self._lock._release_save()
            got = waiter.acquire(True, -1 if timeout is None else max(timeout, 0.0))
            self._lock._acquire_restore(saved)
            if not got and waiter in self._waiters:
                self._waiters.remove(waiter)
            return got

        def wait_for(self, predicate: Callable[[], bool], timeout: float | None = None) -> bool:
            """Waits until predicate() is true (or timeout seconds); predicate()'s last value."""
            end = 0.0
            if timeout is not None:
                end = _tm.monotonic() + timeout
            result = predicate()
            while not result:
                if timeout is not None:
                    left = end - _tm.monotonic()
                    if left <= 0:
                        break
                    self.wait(left)
                else:
                    self.wait()
                result = predicate()
            return result

        def notify(self, n: int = 1) -> None:
            if not self._lock._is_owned():
                raise RuntimeError("cannot notify on un-acquired lock")
            while self._waiters and n > 0:
                w = self._waiters.pop(0)
                w.release()
                n -= 1

        def notify_all(self) -> None:
            self.notify(len(self._waiters))

    class Semaphore:
        """A counter: acquire() takes one (waiting at 0), release() gives back."""

        def __init__(self, value: int = 1) -> None:
            if value < 0:
                raise ValueError("semaphore initial value must be >= 0")
            self._value = value

        def acquire(self, blocking: bool = True, timeout: float | None = None) -> bool:
            if not blocking and timeout is not None:
                raise ValueError("can't specify timeout for non-blocking acquire")
            if self._value == 0:
                if not blocking:
                    return False
                if not _wait_until(lambda: self._value > 0, -1 if timeout is None else timeout):
                    return False
            self._value -= 1
            return True

        def release(self, n: int = 1) -> None:
            if n < 1:
                raise ValueError("n must be one or more")
            self._value += n

        def __enter__(self) -> bool:
            return self.acquire()

        def __exit__(self, t, v, tb) -> None:
            self.release()

    class BoundedSemaphore(Semaphore):
        """A Semaphore that may not be released above its initial value."""

        def __init__(self, value: int = 1) -> None:
            super().__init__(value)
            self._initial_value = value

        def release(self, n: int = 1) -> None:
            if n < 1:
                raise ValueError("n must be one or more")
            if self._value + n > self._initial_value:
                raise ValueError("Semaphore released too many times")
            self._value += n

    class Event:
        """A flag threads wait() for until another set()s it."""

        def __init__(self) -> None:
            self._flag = False

        def is_set(self) -> bool:
            return self._flag

        def set(self) -> None:
            self._flag = True

        def clear(self) -> None:
            self._flag = False

        def wait(self, timeout: float | None = None) -> bool:
            """Waits until the flag is set (or timeout seconds); the flag."""
            return _wait_until(lambda: self._flag, -1 if timeout is None else max(timeout, 0.0))

    class BrokenBarrierError(RuntimeError):
        pass

    class Barrier:
        """parties threads wait() until all of them have come."""

        def __init__(self, parties: int, timeout: float | None = None) -> None:
            if parties < 1:
                raise ValueError("parties must be >= 1")
            self._parties = parties
            self._timeout = timeout
            self._count = 0
            self._generation = 0
            self._broken = False

        def wait(self, timeout: float | None = None) -> int:
            if self._broken:
                raise BrokenBarrierError()
            gen = self._generation
            index = self._count
            self._count += 1
            if self._count == self._parties:
                self._count = 0
                self._generation += 1
                return index
            t = timeout if timeout is not None else self._timeout
            if not _wait_until(lambda: self._generation != gen or self._broken, -1 if t is None else t):
                self._broken = True
                raise BrokenBarrierError()
            if self._broken:
                raise BrokenBarrierError()
            return index

        def reset(self) -> None:
            self._count = 0
            self._broken = False
            self._generation += 1

        def abort(self) -> None:
            self._broken = True

        @property
        def parties(self) -> int:
            return self._parties

        @property
        def n_waiting(self) -> int:
            return self._count

        @property
        def broken(self) -> bool:
            return self._broken

    _counter = 0

    def _newname(prefix: str) -> str:
        global _counter
        _counter += 1
        return prefix + str(_counter)

    class Thread:
        """A thread: Thread(target=f, args=(...)).start(), or a subclass with run()."""

        def __init__(self, group: int | None = None, target: _F = None, name: str | None = None, args: _A = (),
                     kwargs: dict[str, _K] | None = None, *, daemon: bool | None = None) -> None:
            if target is None:
                self._call: Callable[[], None] = _noop
            elif kwargs is None:
                self._call = _bind(target, args)
            else:
                self._call = _bind_kw(target, args, kwargs)
            self._name = name if name else _newname("Thread-")
            self._daemonic = daemon if daemon is not None else current_thread().daemon
            self._ident = -1
            self._started = False
            self._stopped = False

        def __repr__(self) -> str:
            status = "initial"
            if self._started:
                status = "started"
            if self._stopped:
                status = "stopped"
            if self._daemonic:
                status += " daemon"
            if self._ident >= 0:
                status += " " + str(self._ident)
            return "<" + type(self).__name__ + "(" + self._name + ", " + status + ")>"

        def run(self) -> None:
            """What the thread does: its target (subclasses: their own)."""
            self._call()

        def _bootstrap(self) -> None:
            self._ident = get_ident()
            _active[self._ident] = self
            try:
                self.run()
            except BaseException as e:
                excepthook(ExceptHookArgs(e, self))
            self._stopped = True
            if self._ident in _active:
                del _active[self._ident]

        def start(self) -> None:
            """Runs run() in a new thread."""
            if self._started:
                raise RuntimeError("threads can only be started once")
            self._started = True
            _all.append(self)
            self._task = _th.start(self._bootstrap, ())

        def join(self, timeout: float | None = None) -> None:
            """Waits for the thread to end (or timeout seconds: then is_alive() tells)."""
            if not self._started:
                raise RuntimeError("cannot join thread before it is started")
            if self._ident >= 0 and self._ident == get_ident():
                raise RuntimeError("cannot join current thread")
            if timeout is None:
                _th.join(self._task)
            else:
                _wait_until(lambda: self._stopped, max(timeout, 0.0))

        def is_alive(self) -> bool:
            return self._started and not self._stopped

        @property
        def name(self) -> str:
            return self._name

        @name.setter
        def name(self, value: str) -> None:
            self._name = value

        @property
        def ident(self) -> int | None:
            return self._ident if self._ident >= 0 else None

        @property
        def native_id(self) -> int | None:
            return self._ident if self._ident >= 0 else None

        @property
        def daemon(self) -> bool:
            return self._daemonic

        @daemon.setter
        def daemon(self, value: bool) -> None:
            if self._started:
                raise RuntimeError("cannot set daemon status of active thread")
            self._daemonic = value

        def isDaemon(self) -> bool:
            return self._daemonic

        def setDaemon(self, value: bool) -> None:
            self.daemon = value

        def getName(self) -> str:
            return self._name

        def setName(self, value: str) -> None:
            self._name = value

    class ExceptHookArgs:
        """excepthook()'s argument: exc_type, exc_value, exc_traceback (None), thread."""

        def __init__(self, e: BaseException, t: Thread | None) -> None:
            self.exc_value = e
            self.exc_traceback = None
            self.thread = t

        @property
        def exc_type(self) -> type[BaseException]:
            return type(self.exc_value)

    def _default_excepthook(args: ExceptHookArgs) -> None:
        if isinstance(args.exc_value, SystemExit):
            return
        name = args.thread.name if args.thread is not None else str(get_ident())
        print("Exception in thread " + name + ":", file=sys.stderr)
        msg = str(args.exc_value)
        print(type(args.exc_value).__name__ + (": " + msg if msg else ""), file=sys.stderr)

    excepthook = _default_excepthook
    __excepthook__ = _default_excepthook

    class Timer(Thread):
        """Calls function(*args) after interval seconds (unless cancel()led)."""

        def __init__(self, interval: float, function: _F, args: _A = (), kwargs: dict[str, _K] | None = None) -> None:
            super().__init__()
            self.interval = interval
            if kwargs is None:
                self._fn: Callable[[], None] = _bind(function, args)
            else:
                self._fn = _bind_kw(function, args, kwargs)
            self.finished = Event()

        def cancel(self) -> None:
            self.finished.set()

        def run(self) -> None:
            self.finished.wait(self.interval)
            if not self.finished.is_set():
                self._fn()
            self.finished.set()

    class _MainThread(Thread):
        def __init__(self) -> None:
            super().__init__(name="MainThread", daemon=False)
            self._started = True
            self._ident = 0

    _active: dict[int, Thread] = {}
    _all: list[Thread] = []
    _main_thread = _MainThread()
    _active[0] = _main_thread

    def current_thread() -> Thread:
        """The Thread object of the running thread."""
        me = get_ident()
        if me in _active:
            return _active[me]
        return _main_thread

    currentThread = current_thread

    def main_thread() -> Thread:
        return _main_thread

    def active_count() -> int:
        """The number of threads alive (the main one too)."""
        return len(_active)

    def enumerate() -> list[Thread]:
        return list(_active.values())

    def _shutdown() -> None:
        """(At the end of the program: the threads that are not daemons are waited for.)"""
        while True:
            waiting = [t for t in _all if t.is_alive() and not t.daemon]
            if not waiting:
                break
            for t in waiting:
                t.join()

    def stack_size(size: int = 0) -> int:
        return 0

    def settrace(func: _F) -> None:
        pass

    def setprofile(func: _F) -> None:
        pass
