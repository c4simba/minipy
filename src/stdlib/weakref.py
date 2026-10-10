"""Weak references (CPython's weakref API). The interpreter has no weak references of its own:
these refer to their objects as ordinary references do (the objects stay alive while referred to)."""
import sys

if not sys._compiled:
    import sys

    __all__ = ["ref", "proxy", "getweakrefcount", "getweakrefs", "WeakKeyDictionary", "ReferenceType", "ProxyType",
               "CallableProxyType", "ProxyTypes", "WeakValueDictionary", "WeakSet", "WeakMethod", "finalize"]


    class ref:
        """ref(obj[, callback]): calling it gives obj."""

        __slots__ = ("_obj", "__callback__", "_hash")

        def __init__(self, obj, callback=None):
            if obj is None or isinstance(obj, (int, float, str, tuple, bool)):
                raise TypeError("cannot create weak reference to '%s' object" % type(obj).__name__)
            self._obj = obj
            self.__callback__ = callback
            self._hash = None

        def __call__(self):
            return self._obj

        def __hash__(self):
            if self._hash is None:
                self._hash = hash(self._obj)
            return self._hash

        def __eq__(self, other):
            if not isinstance(other, ref):
                return NotImplemented
            return self._obj == other._obj

        def __ne__(self, other):
            if not isinstance(other, ref):
                return NotImplemented
            return self._obj != other._obj

        def __repr__(self):
            o = self._obj
            return "<weakref at %#x; to '%s' at %#x>" % (id(self), type(o).__qualname__, id(o))


    ReferenceType = ref


    class WeakMethod(ref):
        """A weak reference to a bound method."""

        __slots__ = ("_meth_type", "_func_ref", "_alive")

        def __init__(self, meth, callback=None):
            try:
                obj = meth.__self__
                func = meth.__func__
            except AttributeError:
                raise TypeError("argument should be a bound method, not {}".format(type(meth))) from None
            ref.__init__(self, meth, callback)


    def proxy(obj, callback=None):
        """(The object itself.)"""
        return obj


    ProxyType = type(None)
    CallableProxyType = type(None)
    ProxyTypes = (ProxyType, CallableProxyType)


    def getweakrefcount(obj):
        return 0


    def getweakrefs(obj):
        return []


    class WeakValueDictionary(dict):
        """A dict whose values would be weakly referenced (here: as a dict)."""

        def __init__(self, other=(), /, **kw):
            super().__init__()
            self.update(other, **kw)

        def update(self, other=(), /, **kw):
            if hasattr(other, "items"):
                other = other.items()
            for k, v in other:
                self[k] = v
            for k, v in kw.items():
                self[k] = v

        def copy(self):
            return WeakValueDictionary(self)

        __copy__ = copy

        def itervaluerefs(self):
            return [ref(v) for v in self.values()]

        def valuerefs(self):
            return [ref(v) for v in self.values()]

        def __repr__(self):
            return "<%s at %#x>" % (self.__class__.__name__, id(self))


    class WeakKeyDictionary(dict):
        """A dict whose keys would be weakly referenced (here: as a dict)."""

        def __init__(self, dict=None):
            super().__init__()
            if dict is not None:
                self.update(dict)

        def copy(self):
            return WeakKeyDictionary(self)

        __copy__ = copy

        def keyrefs(self):
            return [ref(k) for k in self.keys()]

        def __repr__(self):
            return "<%s at %#x>" % (self.__class__.__name__, id(self))


    from _weakrefset import WeakSet


    class finalize:
        """finalize(obj, func, *args, **kwargs): func(*args, **kwargs) when called or at exit
    (objects are not collected under a weak reference here)."""

        _registry = {}
        _registered_with_atexit = False

        def __init__(self, obj, func, /, *args, **kwargs):
            if not self._registered_with_atexit:
                import atexit
                atexit.register(self._exitfunc)
                finalize._registered_with_atexit = True
            self._obj = obj
            self._info = (func, args, kwargs or {})
            self.atexit = True
            finalize._registry[self] = self._info

        def __call__(self, _=None):
            info = self._registry.pop(self, None)
            if info:
                func, args, kwargs = info
                return func(*args, **kwargs)

        def detach(self):
            info = self._registry.get(self)
            obj = self._obj
            if obj is not None and self._registry.pop(self, None):
                return (obj, info[0], info[1], info[2])

        def peek(self):
            info = self._registry.get(self)
            if info:
                return (self._obj, info[0], info[1], info[2])

        @property
        def alive(self):
            return self in self._registry

        def __repr__(self):
            return "<%s object at %#x; for %r at %#x>" % (type(self).__name__, id(self), type(self._obj).__name__, id(self._obj))

        @classmethod
        def _exitfunc(cls):
            pending = [f for f in list(cls._registry) if f.atexit]
            for f in reversed(pending):
                f()


if sys._compiled:
    # Compiled programs: the same API, typed; the references are ordinary ones (as the
    # interpreter's): an object lives while something refers to it.
    import atexit as _atexit
    from typing import Generic, Iterator, TypeVar

    _T = TypeVar("_T")
    _K = TypeVar("_K")
    _V = TypeVar("_V")
    _F = TypeVar("_F")
    _A = TypeVar("_A")

    class ref(Generic[_T]):
        """ref(obj[, callback]): calling it gives obj."""

        def __init__(self, obj: _T, callback: _F = None) -> None:
            self._obj = obj

        def __call__(self) -> _T:
            return self._obj

        def __repr__(self) -> str:
            return "<weakref; to '" + type(self._obj).__qualname__ + "'>"

    def proxy(obj: _T, callback: _F = None) -> _T:
        """(The object itself.)"""
        return obj

    def getweakrefcount(obj: _T) -> int:
        return 0

    class WeakValueDictionary(Generic[_K, _V]):
        """A dict (whose values here are ordinary references)."""

        def __init__(self) -> None:
            self.data: dict[_K, _V] = {}

        def __getitem__(self, key: _K) -> _V:
            return self.data[key]

        def __setitem__(self, key: _K, value: _V) -> None:
            self.data[key] = value

        def __delitem__(self, key: _K) -> None:
            del self.data[key]

        def __contains__(self, key: _K) -> bool:
            return key in self.data

        def __len__(self) -> int:
            return len(self.data)

        def __iter__(self) -> Iterator[_K]:
            for k in list(self.data):
                yield k

        def get(self, key: _K, default: _V | None = None) -> _V | None:
            if key in self.data:
                return self.data[key]
            return default

        def setdefault(self, key: _K, default: _V) -> _V:
            if key not in self.data:
                self.data[key] = default
            return self.data[key]

        def pop(self, key: _K) -> _V:
            return self.data.pop(key)

        def keys(self) -> list[_K]:
            return list(self.data.keys())

        def values(self) -> list[_V]:
            return list(self.data.values())

        def items(self) -> list[tuple[_K, _V]]:
            return list(self.data.items())

        def clear(self) -> None:
            self.data.clear()

        def __repr__(self) -> str:
            return "<" + type(self).__name__ + ">"

    class WeakKeyDictionary(Generic[_K, _V]):
        """A dict (whose keys here are ordinary references)."""

        def __init__(self) -> None:
            self.data: dict[_K, _V] = {}

        def __getitem__(self, key: _K) -> _V:
            return self.data[key]

        def __setitem__(self, key: _K, value: _V) -> None:
            self.data[key] = value

        def __delitem__(self, key: _K) -> None:
            del self.data[key]

        def __contains__(self, key: _K) -> bool:
            return key in self.data

        def __len__(self) -> int:
            return len(self.data)

        def __iter__(self) -> Iterator[_K]:
            for k in list(self.data):
                yield k

        def get(self, key: _K, default: _V | None = None) -> _V | None:
            if key in self.data:
                return self.data[key]
            return default

        def setdefault(self, key: _K, default: _V) -> _V:
            if key not in self.data:
                self.data[key] = default
            return self.data[key]

        def pop(self, key: _K) -> _V:
            return self.data.pop(key)

        def keys(self) -> list[_K]:
            return list(self.data.keys())

        def values(self) -> list[_V]:
            return list(self.data.values())

        def items(self) -> list[tuple[_K, _V]]:
            return list(self.data.items())

        def clear(self) -> None:
            self.data.clear()

        def __repr__(self) -> str:
            return "<" + type(self).__name__ + ">"

    class WeakSet(Generic[_T]):
        """A set (of ordinary references here)."""

        def __init__(self) -> None:
            self.data: set[_T] = set()

        def add(self, item: _T) -> None:
            self.data.add(item)

        def discard(self, item: _T) -> None:
            self.data.discard(item)

        def remove(self, item: _T) -> None:
            self.data.remove(item)

        def pop(self) -> _T:
            return self.data.pop()

        def clear(self) -> None:
            self.data.clear()

        def __contains__(self, item: _T) -> bool:
            return item in self.data

        def __len__(self) -> int:
            return len(self.data)

        def __iter__(self) -> Iterator[_T]:
            for x in list(self.data):
                yield x

        def __repr__(self) -> str:
            return "<WeakSet>"

    class finalize:
        """finalize(obj, func, *args): func(*args) when called, or at exit."""

        def __init__(self, obj: _T, func: _F, *args: _A) -> None:
            def call() -> None:
                func(*args)

            self._call = call
            self._alive = True
            self.atexit = True
            _registry.append(self)

        def __call__(self) -> None:
            if self._alive:
                self._alive = False
                self._call()

        def detach(self) -> bool:
            was = self._alive
            self._alive = False
            return was

        @property
        def alive(self) -> bool:
            return self._alive

    _registry: list[finalize] = []

    def _exitfunc() -> None:
        for fin in _registry[::-1]:
            if fin.atexit:
                fin.__call__()

    _atexit.register(_exitfunc)
