"""Selectors module (CPython's selectors): high-level I/O multiplexing over select.select().

DefaultSelector (= SelectSelector; PollSelector is the same here) watches file objects (sockets, or
anything with fileno(), or descriptors) for reading / writing: register(fileobj, events, data),
select(timeout) -> [(key, events)]. Compiled programs: a selector's file objects are of one type, and
so are their data."""
import sys
import select
from typing import Generic, TypeVar

__all__ = ["EVENT_READ", "EVENT_WRITE", "SelectorKey", "BaseSelector", "SelectSelector", "PollSelector",
           "DefaultSelector"]

_F = TypeVar("_F")
_D = TypeVar("_D")

# generic events, that must be mapped to implementation-specific ones
EVENT_READ = (1 << 0)
EVENT_WRITE = (1 << 1)


def _fileobj_to_fd(fileobj: _F) -> int:
    """Return a file descriptor from a file object (an int: itself)."""
    if isinstance(fileobj, int):
        fd = fileobj
    else:
        try:
            fd = int(fileobj.fileno())
        except (AttributeError, TypeError, ValueError):
            raise ValueError("Invalid file object: {!r}".format(fileobj)) from None
    if fd < 0:
        raise ValueError("Invalid file descriptor: {}".format(fd))
    return fd


class SelectorKey(Generic[_F, _D]):
    """SelectorKey(fileobj, fd, events, data): what a file object is registered for."""

    def __init__(self, fileobj: _F, fd: int, events: int, data: _D) -> None:
        self.fileobj = fileobj
        self.fd = fd
        self.events = events
        self.data = data

    def __repr__(self) -> str:
        return "SelectorKey(fileobj=%r, fd=%r, events=%r, data=%r)" % (self.fileobj, self.fd, self.events, self.data)

    def __eq__(self, other: "SelectorKey[_F, _D]") -> bool:
        return self.fd == other.fd and self.events == other.events


class _Fd:
    """(a descriptor, for select.select())"""

    def __init__(self, fd: int) -> None:
        self.fd = fd

    def fileno(self) -> int:
        return self.fd


class _SelectorMapping(Generic[_F, _D]):
    """Mapping of file objects to selector keys (get_map())."""

    def __init__(self, selector) -> None:
        self._selector = selector

    def __len__(self) -> int:
        return len(self._selector._fd_to_key)

    def get(self, fileobj: _F, default: SelectorKey[_F, _D] | None = None) -> SelectorKey[_F, _D] | None:
        fd = self._selector._fileobj_lookup(fileobj)
        return self._selector._fd_to_key.get(fd, default)

    def __getitem__(self, fileobj: _F) -> SelectorKey[_F, _D]:
        fd = self._selector._fileobj_lookup(fileobj)
        key = self._selector._fd_to_key.get(fd)
        if key is None:
            raise KeyError("{!r} is not registered".format(fileobj))
        return key

    def __contains__(self, fileobj: _F) -> bool:
        return self.get(fileobj) is not None

    def __iter__(self):
        for key in self._selector._fd_to_key.values():
            yield key.fileobj


class SelectSelector(Generic[_F, _D]):
    """Select-based selector."""

    def __init__(self) -> None:
        self._fd_to_key: dict[int, SelectorKey[_F, _D]] = {}
        self._map: _SelectorMapping[_F, _D] | None = _SelectorMapping(self)
        self._readers: set[int] = set()
        self._writers: set[int] = set()

    def _fileobj_lookup(self, fileobj: _F) -> int:
        """Return a file descriptor from a file object (also when it was closed since: the key of it)."""
        try:
            return _fileobj_to_fd(fileobj)
        except ValueError:
            # Do an exhaustive search.
            for key in self._fd_to_key.values():
                if key.fileobj is fileobj:
                    return key.fd
            # Raise ValueError after all.
            raise

    def register(self, fileobj: _F, events: int, data: _D = None) -> SelectorKey[_F, _D]:
        """Register a file object for events (EVENT_READ | EVENT_WRITE) with data: its key."""
        if (not events) or (events & ~(EVENT_READ | EVENT_WRITE)):
            raise ValueError("Invalid events: {!r}".format(events))
        key = SelectorKey(fileobj, self._fileobj_lookup(fileobj), events, data)
        if key.fd in self._fd_to_key:
            raise KeyError("{!r} (FD {}) is already registered".format(fileobj, key.fd))
        self._fd_to_key[key.fd] = key
        if events & EVENT_READ:
            self._readers.add(key.fd)
        if events & EVENT_WRITE:
            self._writers.add(key.fd)
        return key

    def unregister(self, fileobj: _F) -> SelectorKey[_F, _D]:
        """Unregister a file object: its key."""
        fd = self._fileobj_lookup(fileobj)
        if fd not in self._fd_to_key:
            raise KeyError("{!r} is not registered".format(fileobj))
        key = self._fd_to_key.pop(fd)
        self._readers.discard(key.fd)
        self._writers.discard(key.fd)
        return key

    def modify(self, fileobj: _F, events: int, data: _D = None) -> SelectorKey[_F, _D]:
        """Change a registered file object's events or data."""
        fd = self._fileobj_lookup(fileobj)
        key = self._fd_to_key.get(fd)
        if key is None:
            raise KeyError("{!r} is not registered".format(fileobj))
        if events != key.events:
            self.unregister(fileobj)
            key = self.register(fileobj, events, data)
        elif data != key.data:
            key = SelectorKey(key.fileobj, key.fd, key.events, data)
            self._fd_to_key[key.fd] = key
        return key

    def select(self, timeout: float | None = None) -> list[tuple[SelectorKey[_F, _D], int]]:
        """Wait (timeout: at most that many seconds, <= 0: not at all) until some registered file objects
        are ready: [(key, events)]."""
        if timeout is not None:
            timeout = max(timeout, 0.0)
        readers = [_Fd(fd) for fd in self._readers]
        writers = [_Fd(fd) for fd in self._writers]
        ready: list[tuple[SelectorKey[_F, _D], int]] = []
        if not readers and not writers:
            if timeout is not None:
                import time
                time.sleep(timeout)
            return ready
        try:
            r, w, _ = select.select(readers, writers, [], timeout)
        except InterruptedError:
            return ready
        rset = set([x.fd for x in r])
        wset = set([x.fd for x in w])
        for fd in sorted(rset | wset):
            events = 0
            if fd in rset:
                events |= EVENT_READ
            if fd in wset:
                events |= EVENT_WRITE
            key = self._fd_to_key.get(fd)
            if key is not None:
                ready.append((key, events & key.events))
        return ready

    def close(self) -> None:
        """Close the selector."""
        self._fd_to_key.clear()
        self._readers.clear()
        self._writers.clear()
        self._map = None

    def get_key(self, fileobj: _F) -> SelectorKey[_F, _D]:
        """Return the key associated to a registered file object."""
        mapping = self.get_map()
        if mapping is None:
            raise RuntimeError('Selector is closed')
        return mapping[fileobj]

    def get_map(self) -> _SelectorMapping[_F, _D] | None:
        """Return a mapping of file objects to selector keys."""
        return self._map

    def __enter__(self) -> "SelectSelector[_F, _D]":
        return self

    def __exit__(self, et, ev, tb) -> None:
        self.close()


BaseSelector = SelectSelector
PollSelector = SelectSelector
DefaultSelector = SelectSelector
