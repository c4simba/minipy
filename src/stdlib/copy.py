"""Shallow and deep copies (CPython's copy).

copy(x): a new list, dict or set with the same items (or what x.__copy__()
gives); deepcopy(x): the same for what they hold too. Immutable values are
themselves. In compiled programs an object is copied by its __copy__ /
__deepcopy__ methods, and deepcopy does not keep shared parts shared."""
import sys
from typing import TypeVar

T = TypeVar("T")


class Error(Exception):
    pass


error = Error


def copy(x: T) -> T:
    """A shallow copy of x."""
    if isinstance(x, (int, float, str, bytes, bool, tuple, frozenset)):
        return x
    elif isinstance(x, list):
        return x.copy()
    elif isinstance(x, dict):
        return x.copy()
    elif isinstance(x, set):
        return x.copy()
    else:
        if sys._compiled:
            return x.__copy__()
        return _copy_object(x)


def deepcopy(x: T, memo=None) -> T:
    """A copy of x and, deeply, of everything it holds."""
    if sys._compiled:
        return _deep(x)
    if memo is None:
        memo = {}
    return _deepcopy_dyn(x, memo)


def _deep(x: T) -> T:
    """deepcopy of a compiled program (one type per container: a copy of this function per type)."""
    if isinstance(x, (int, float, str, bytes, bool)):
        return x
    elif isinstance(x, list):
        return [_deep(e) for e in x]
    elif isinstance(x, dict):
        return {_deep(k): _deep(v) for k, v in x.items()}
    elif isinstance(x, set):
        return {_deep(e) for e in x}
    elif isinstance(x, tuple):
        return x
    else:
        return x.__deepcopy__({})


if not sys._compiled:
    _ATOMIC = (type(None), int, float, bool, complex, str, bytes, type, range, type(Ellipsis), type(NotImplemented))

    def _copy_object(x):
        cls = type(x)
        if isinstance(x, _ATOMIC) or callable(x) and not hasattr(x, "__dict__"):
            return x
        f = getattr(cls, "__copy__", None)
        if f is not None:
            return f(x)
        rv = _reduce(x, cls, "shallow")
        if isinstance(rv, str):
            return x
        return _reconstruct(x, None, *rv)

    def _reduce(x, cls, how):
        import copyreg
        reductor = copyreg.dispatch_table.get(cls)
        if reductor is not None:
            return reductor(x)
        reductor = getattr(x, "__reduce_ex__", None)
        if reductor is not None:
            return reductor(4)
        reductor = getattr(x, "__reduce__", None)
        if reductor:
            return reductor()
        raise Error("un(" + how + ")copyable object of type " + str(cls))

    def _reconstruct(x, memo, func, args, state=None, listiter=None, dictiter=None):
        """The copy a __reduce_ex__ result describes (memo: deep)."""
        deep = memo is not None
        if deep and args:
            args = _deepcopy_dyn(args, memo)
        y = func(*args)
        if deep:
            memo[id(x)] = y
        if state is not None:
            if deep:
                state = _deepcopy_dyn(state, memo)
            if hasattr(y, '__setstate__'):
                y.__setstate__(state)
            else:
                if isinstance(state, tuple) and len(state) == 2:
                    state, slotstate = state
                else:
                    slotstate = None
                if state is not None:
                    y.__dict__.update(state)
                if slotstate is not None:
                    for key, value in slotstate.items():
                        setattr(y, key, value)
        if listiter is not None:
            if deep:
                for item in listiter:
                    y.append(_deepcopy_dyn(item, memo))
            else:
                for item in listiter:
                    y.append(item)
        if dictiter is not None:
            if deep:
                for key, value in dictiter:
                    y[_deepcopy_dyn(key, memo)] = _deepcopy_dyn(value, memo)
            else:
                for key, value in dictiter:
                    y[key] = value
        return y

    def _deepcopy_dyn(x, memo):
        i = id(x)
        if i in memo:
            return memo[i]
        cls = type(x)
        if isinstance(x, _ATOMIC) or (callable(x) and not hasattr(x, "__dict__")):
            return x
        if cls is list:
            y = []
            memo[i] = y
            _keep_alive(x, memo)
            for e in x:
                y.append(_deepcopy_dyn(e, memo))
            return y
        if cls is dict:
            y = {}
            memo[i] = y
            _keep_alive(x, memo)
            for k, v in x.items():
                y[_deepcopy_dyn(k, memo)] = _deepcopy_dyn(v, memo)
            return y
        if cls is set:
            y = set(_deepcopy_dyn(e, memo) for e in x)
            memo[i] = y
            _keep_alive(x, memo)
            return y
        if cls is frozenset:
            y = frozenset(_deepcopy_dyn(e, memo) for e in x)
            memo[i] = y
            return y
        if cls is tuple:
            items = [_deepcopy_dyn(e, memo) for e in x]
            if i in memo:
                return memo[i]
            same = all(a is b for a, b in zip(items, x))
            y = x if same else tuple(items)
            memo[i] = y
            _keep_alive(x, memo)
            return y
        f = getattr(cls, "__deepcopy__", None)
        if f is not None:
            y = f(x, memo)
            memo[i] = y
            _keep_alive(x, memo)
            return y
        rv = _reduce(x, cls, "deep")
        if isinstance(rv, str):
            return x
        y = _reconstruct(x, memo, *rv)
        if y is not x:
            memo[i] = y
            _keep_alive(x, memo)
        return y

    def _keep_alive(x, memo):
        """x stays alive while memo does (its id is not given to another object)."""
        try:
            memo[id(memo)].append(x)
        except KeyError:
            memo[id(memo)] = [x]

    def replace(obj, /, **changes):
        """A copy of obj with some fields changed (obj.__replace__)."""
        cls = obj.__class__
        func = getattr(cls, "__replace__", None)
        if func is None:
            raise TypeError("replace() does not support " + cls.__name__ + " objects")
        return func(obj, **changes)
