"""A generally useful event scheduler class (CPython's sched).

    s = sched.scheduler(time.monotonic, time.sleep)
    s.enter(10, 1, action, argument=(...), kwargs={...})
    s.run()

Compiled programs: an event's action is kept with its arguments bound (event.action is that call;
event.argument / event.kwargs are what were given); kwargs values of one type."""
import sys
import heapq
import threading
from time import monotonic as _time
import time as _timemod
from typing import Callable, TypeVar

__all__ = ["scheduler"]

_A = TypeVar("_A")
_K = TypeVar("_K")
_F = TypeVar("_F")

_counter = [0]


class Event:
    """An event: time, priority, sequence, action, argument, kwargs (ordered by time, then priority,
    then sequence)."""

    def __init__(self, time: float, priority: int, sequence: int, action: Callable[[], None], argument: str,
                 kwargs: str) -> None:
        self.time = time
        self.priority = priority
        self.sequence = sequence
        self.action = action
        self.argument = argument
        self.kwargs = kwargs

    def _key(self) -> tuple[float, int, int]:
        return (self.time, self.priority, self.sequence)

    def __lt__(self, other: "Event") -> bool:
        return self._key() < other._key()

    def __le__(self, other: "Event") -> bool:
        return self._key() <= other._key()

    def __eq__(self, other: "Event") -> bool:
        return self._key() == other._key()

    def __repr__(self) -> str:
        return "Event(time=%r, priority=%r, sequence=%r, argument=%s, kwargs=%s)" % (
            self.time, self.priority, self.sequence, self.argument, self.kwargs)


def _bind(action: _F, argument: _A, kwargs: dict[str, _K] | None) -> Callable[[], None]:
    if kwargs is None:
        def call() -> None:
            action(*argument)
        return call

    def call_kw() -> None:
        action(*argument, **kwargs)
    return call_kw


class scheduler:
    def __init__(self, timefunc: Callable[[], float] = _time, delayfunc: Callable[[float], None] = _timemod.sleep) -> None:
        """Initialize a new instance, passing the time and delay functions"""
        self._queue: list[Event] = []
        self._lock = threading.RLock()
        self.timefunc = timefunc
        self.delayfunc = delayfunc

    def enterabs(self, time: float, priority: int, action: _F, argument: _A = (),
                 kwargs: dict[str, _K] | None = None) -> Event:
        """Enter a new event in the queue at an absolute time: the event (an ID for cancel())."""
        _counter[0] += 1
        event = Event(time, priority, _counter[0], _bind(action, argument, kwargs), repr(argument),
                      repr(kwargs if kwargs is not None else {}))
        with self._lock:
            heapq.heappush(self._queue, event)
        return event  # The ID

    def enter(self, delay: float, priority: int, action: _F, argument: _A = (),
              kwargs: dict[str, _K] | None = None) -> Event:
        """A new event in the queue, delay seconds from now."""
        time = self.timefunc() + delay
        return self.enterabs(time, priority, action, argument, kwargs)

    def cancel(self, event: Event) -> None:
        """Remove an event from the queue (ValueError: not there)."""
        with self._lock:
            for i in range(len(self._queue)):
                if self._queue[i] is event:
                    del self._queue[i]
                    heapq.heapify(self._queue)
                    return
            raise ValueError("list.remove(x): x not in list")

    def empty(self) -> bool:
        """Check whether the queue is empty."""
        with self._lock:
            return not self._queue

    def run(self, blocking: bool = True) -> float | None:
        """Execute events until the queue is empty (blocking=False: those due now, and the time until the
        next one is returned)."""
        lock = self._lock
        q = self._queue
        delayfunc = self.delayfunc
        timefunc = self.timefunc
        pop = heapq.heappop
        while True:
            with lock:
                if not q:
                    break
                ev = q[0]
                now = timefunc()
                if ev.time > now:
                    delay = True
                else:
                    delay = False
                    pop(q)
            if delay:
                if not blocking:
                    return ev.time - now
                delayfunc(ev.time - now)
            else:
                ev.action()
                delayfunc(0)   # Let other threads run
        return None

    @property
    def queue(self) -> list[Event]:
        """An ordered list of upcoming events."""
        with self._lock:
            events = list(self._queue)
        return sorted(events)
