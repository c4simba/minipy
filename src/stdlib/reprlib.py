"""Redo the builtin repr() (representation) but with limits on most sizes."""
import sys

if not sys._compiled:

    __all__ = ["Repr", "repr", "recursive_repr"]

    import builtins
    from itertools import islice
    from _thread import get_ident

    def recursive_repr(fillvalue='...'):
        'Decorator to make a repr function return fillvalue for a recursive call'

        def decorating_function(user_function):
            repr_running = set()

            def wrapper(self):
                key = id(self), get_ident()
                if key in repr_running:
                    return fillvalue
                repr_running.add(key)
                try:
                    result = user_function(self)
                finally:
                    repr_running.discard(key)
                return result

            # Can't use functools.wraps() here because of bootstrap issues
            wrapper.__module__ = getattr(user_function, '__module__')
            wrapper.__doc__ = getattr(user_function, '__doc__')
            wrapper.__name__ = getattr(user_function, '__name__')
            wrapper.__qualname__ = getattr(user_function, '__qualname__')
            wrapper.__annotate__ = getattr(user_function, '__annotate__', None)
            wrapper.__type_params__ = getattr(user_function, '__type_params__', ())
            wrapper.__wrapped__ = user_function
            return wrapper

        return decorating_function

    class Repr:
        _lookup = {
            'tuple': 'builtins',
            'list': 'builtins',
            'array': 'array',
            'set': 'builtins',
            'frozenset': 'builtins',
            'deque': 'collections',
            'dict': 'builtins',
            'str': 'builtins',
            'int': 'builtins'
        }

        def __init__(
            self, *, maxlevel=6, maxtuple=6, maxlist=6, maxarray=5, maxdict=4,
            maxset=6, maxfrozenset=6, maxdeque=6, maxstring=30, maxlong=40,
            maxother=30, fillvalue='...', indent=None,
        ):
            self.maxlevel = maxlevel
            self.maxtuple = maxtuple
            self.maxlist = maxlist
            self.maxarray = maxarray
            self.maxdict = maxdict
            self.maxset = maxset
            self.maxfrozenset = maxfrozenset
            self.maxdeque = maxdeque
            self.maxstring = maxstring
            self.maxlong = maxlong
            self.maxother = maxother
            self.fillvalue = fillvalue
            self.indent = indent

        def repr(self, x):
            return self.repr1(x, self.maxlevel)

        def repr1(self, x, level):
            cls = type(x)
            typename = cls.__name__

            if ' ' in typename:
                parts = typename.split()
                typename = '_'.join(parts)

            method = getattr(self, 'repr_' + typename, None)
            if method:
                # not defined in this class
                if typename not in self._lookup:
                    return method(x, level)
                module = getattr(cls, '__module__', None)
                # defined in this class and is the module intended
                if module == self._lookup[typename]:
                    return method(x, level)

            return self.repr_instance(x, level)

        def _join(self, pieces, level):
            if self.indent is None:
                return ', '.join(pieces)
            if not pieces:
                return ''
            indent = self.indent
            if isinstance(indent, int):
                if indent < 0:
                    raise ValueError(
                        f'Repr.indent cannot be negative int (was {indent!r})'
                    )
                indent *= ' '
            try:
                sep = ',\n' + (self.maxlevel - level + 1) * indent
            except TypeError as error:
                raise TypeError(
                    f'Repr.indent must be a str, int or None, not {type(indent)}'
                ) from error
            return sep.join(('', *pieces, ''))[1:-len(indent) or None]

        def _repr_iterable(self, x, level, left, right, maxiter, trail=''):
            n = len(x)
            if level <= 0 and n:
                s = self.fillvalue
            else:
                newlevel = level - 1
                repr1 = self.repr1
                pieces = [repr1(elem, newlevel) for elem in islice(x, maxiter)]
                if n > maxiter:
                    pieces.append(self.fillvalue)
                s = self._join(pieces, level)
                if n == 1 and trail and self.indent is None:
                    right = trail + right
            return '%s%s%s' % (left, s, right)

        def repr_tuple(self, x, level):
            return self._repr_iterable(x, level, '(', ')', self.maxtuple, ',')

        def repr_list(self, x, level):
            return self._repr_iterable(x, level, '[', ']', self.maxlist)

        def repr_array(self, x, level):
            if not x:
                return "array('%s')" % x.typecode
            header = "array('%s', [" % x.typecode
            return self._repr_iterable(x, level, header, '])', self.maxarray)

        def repr_set(self, x, level):
            if not x:
                return 'set()'
            x = _possibly_sorted(x)
            return self._repr_iterable(x, level, '{', '}', self.maxset)

        def repr_frozenset(self, x, level):
            if not x:
                return 'frozenset()'
            x = _possibly_sorted(x)
            return self._repr_iterable(x, level, 'frozenset({', '})',
                                       self.maxfrozenset)

        def repr_deque(self, x, level):
            return self._repr_iterable(x, level, 'deque([', '])', self.maxdeque)

        def repr_dict(self, x, level):
            n = len(x)
            if n == 0:
                return '{}'
            if level <= 0:
                return '{' + self.fillvalue + '}'
            newlevel = level - 1
            repr1 = self.repr1
            pieces = []
            for key in islice(_possibly_sorted(x), self.maxdict):
                keyrepr = repr1(key, newlevel)
                valrepr = repr1(x[key], newlevel)
                pieces.append('%s: %s' % (keyrepr, valrepr))
            if n > self.maxdict:
                pieces.append(self.fillvalue)
            s = self._join(pieces, level)
            return '{%s}' % (s,)

        def repr_str(self, x, level):
            s = builtins.repr(x[:self.maxstring])
            if len(s) > self.maxstring:
                i = max(0, (self.maxstring-3)//2)
                j = max(0, self.maxstring-3-i)
                s = builtins.repr(x[:i] + x[len(x)-j:])
                s = s[:i] + self.fillvalue + s[len(s)-j:]
            return s

        def repr_int(self, x, level):
            try:
                s = builtins.repr(x)
            except ValueError as exc:
                assert 'sys.set_int_max_str_digits()' in str(exc)
                # Those imports must be deferred due to Python's build system
                # where the reprlib module is imported before the math module.
                import math, sys
                # Integers with more than sys.get_int_max_str_digits() digits
                # are rendered differently as their repr() raises a ValueError.
                # See https://github.com/python/cpython/issues/135487.
                k = 1 + int(math.log10(abs(x)))
                # Note: math.log10(abs(x)) may be overestimated or underestimated,
                # but for simplicity, we do not compute the exact number of digits.
                max_digits = sys.get_int_max_str_digits()
                return (f'<{x.__class__.__name__} instance with roughly {k} '
                        f'digits (limit at {max_digits}) at 0x{id(x):x}>')
            if len(s) > self.maxlong:
                i = max(0, (self.maxlong-3)//2)
                j = max(0, self.maxlong-3-i)
                s = s[:i] + self.fillvalue + s[len(s)-j:]
            return s

        def repr_instance(self, x, level):
            try:
                s = builtins.repr(x)
                # Bugs in x.__repr__() can cause arbitrary
                # exceptions -- then make up something
            except Exception:
                return '<%s instance at %#x>' % (x.__class__.__name__, id(x))
            if len(s) > self.maxother:
                i = max(0, (self.maxother-3)//2)
                j = max(0, self.maxother-3-i)
                s = s[:i] + self.fillvalue + s[len(s)-j:]
            return s


    def _possibly_sorted(x):
        # Since not all sequences of items can be sorted and comparison
        # functions may raise arbitrary exceptions, return an unsorted
        # sequence in that case.
        try:
            return sorted(x)
        except Exception:
            return list(x)

    aRepr = Repr()
    repr = aRepr.repr

if sys._compiled:
    # Compiled programs: the same limits for str, int, list, tuple, set, frozenset, dict and deque
    # (a typed copy of each step per kind of value); other objects: their repr() cut to maxother.
    # recursive_repr(fillvalue) keeps __repr__ as it is (typed values cannot contain themselves).
    from typing import TypeVar
    from collections import deque

    _T = TypeVar("_T")
    _S = TypeVar("_S")

    def recursive_repr(fillvalue: str = '...') -> None:
        """Decorator to make a repr function return fillvalue for a recursive call (@recursive_repr()
        is left out in compiled code)."""
        pass

    def _r(x: _T) -> str:
        return f"{x!r}"                 # (the built-in repr: this module's repr() is below)

    def _cut(s: str, limit: int, fill: str) -> str:
        if len(s) > limit:
            i = max(0, (limit - 3) // 2)
            j = max(0, limit - 3 - i)
            return s[:i] + fill + s[len(s) - j:]
        return s

    class Repr:
        def __init__(self, *, maxlevel: int = 6, maxtuple: int = 6, maxlist: int = 6, maxarray: int = 5,
                     maxdict: int = 4, maxset: int = 6, maxfrozenset: int = 6, maxdeque: int = 6,
                     maxstring: int = 30, maxlong: int = 40, maxother: int = 30, fillvalue: str = '...',
                     indent: str | None = None) -> None:
            self.maxlevel = maxlevel
            self.maxtuple = maxtuple
            self.maxlist = maxlist
            self.maxarray = maxarray
            self.maxdict = maxdict
            self.maxset = maxset
            self.maxfrozenset = maxfrozenset
            self.maxdeque = maxdeque
            self.maxstring = maxstring
            self.maxlong = maxlong
            self.maxother = maxother
            self.fillvalue = fillvalue
            self.indent = indent

        def repr(self, x: _T) -> str:
            return self.repr1(x, self.maxlevel)

        def repr1(self, x: _T, level: int) -> str:
            if isinstance(x, str):
                return self.repr_str(x, level)
            elif isinstance(x, bool):
                return self.repr_instance(x, level)
            elif isinstance(x, int):
                return self.repr_int(x, level)
            elif isinstance(x, list):
                return self._repr_iterable(x, level, '[', ']', self.maxlist)
            elif isinstance(x, tuple):
                return self.repr_tuple(x, level)
            elif isinstance(x, deque):
                return self._repr_iterable(list(x), level, 'deque([', '])', self.maxdeque)
            elif isinstance(x, frozenset):
                if not x:
                    return 'frozenset()'
                return self._repr_iterable(sorted(x), level, 'frozenset({', '})', self.maxfrozenset)
            elif isinstance(x, set):
                if not x:
                    return 'set()'
                return self._repr_iterable(sorted(x), level, '{', '}', self.maxset)
            elif isinstance(x, dict):
                return self.repr_dict(x, level)
            else:
                return self.repr_instance(x, level)

        def _join(self, pieces: list[str], level: int) -> str:
            indent = self.indent
            if indent is None:
                return ', '.join(pieces)
            if not pieces:
                return ''
            sep = ',\n' + (self.maxlevel - level + 1) * indent
            out = sep + sep.join(pieces) + sep
            return out[1:len(out) - len(indent)] if indent else out[1:]

        def _repr_iterable(self, x: list[_T], level: int, left: str, right: str, maxiter: int, trail: str = '') -> str:
            if level <= 0 and x:
                return self._pieces_iterable(["?"] * len(x), level, left, right, maxiter, trail)
            return self._pieces_iterable([self.repr1(elem, level - 1) for elem in x[:maxiter]] + ["?"] * max(0, len(x) - maxiter),
                                         level, left, right, maxiter, trail)

        def _pieces_iterable(self, reprs: list[str], level: int, left: str, right: str, maxiter: int, trail: str = '') -> str:
            """(reprs: those of all the items, as many as there are)"""
            n = len(reprs)
            if level <= 0 and n:
                s = self.fillvalue
            else:
                pieces = reprs[:maxiter]
                if n > maxiter:
                    pieces.append(self.fillvalue)
                s = self._join(pieces, level)
                if n == 1 and trail and self.indent is None:
                    right = trail + right
            return left + s + right

        def repr_tuple(self, x: _T, level: int) -> str:
            # (a tuple of several item types: one typed repr1() per item)
            return self._pieces_iterable([self.repr1(elem, level - 1) for elem in x], level, '(', ')', self.maxtuple, ',')

        def repr_list(self, x: list[_T], level: int) -> str:
            return self._repr_iterable(x, level, '[', ']', self.maxlist)

        def repr_set(self, x: set[_T], level: int) -> str:
            if not x:
                return 'set()'
            return self._repr_iterable(sorted(x), level, '{', '}', self.maxset)

        def repr_frozenset(self, x: frozenset[_T], level: int) -> str:
            if not x:
                return 'frozenset()'
            return self._repr_iterable(sorted(x), level, 'frozenset({', '})', self.maxfrozenset)

        def repr_dict(self, x: dict[_T, _S], level: int) -> str:
            n = len(x)
            if n == 0:
                return '{}'
            if level <= 0:
                return '{' + self.fillvalue + '}'
            pieces: list[str] = []
            for key in sorted(x)[:self.maxdict]:
                pieces.append(self.repr1(key, level - 1) + ': ' + self.repr1(x[key], level - 1))
            if n > self.maxdict:
                pieces.append(self.fillvalue)
            return '{' + self._join(pieces, level) + '}'

        def repr_str(self, x: str, level: int) -> str:
            s = _r(x[:self.maxstring])
            if len(s) > self.maxstring:
                i = max(0, (self.maxstring - 3) // 2)
                j = max(0, self.maxstring - 3 - i)
                s = _r(x[:i] + x[len(x) - j:])
                s = s[:i] + self.fillvalue + s[len(s) - j:]
            return s

        def repr_int(self, x: int, level: int) -> str:
            return _cut(_r(x), self.maxlong, self.fillvalue)

        def repr_instance(self, x: _T, level: int) -> str:
            return _cut(_r(x), self.maxother, self.fillvalue)

    aRepr = Repr()

    def repr(x: _T) -> str:
        return aRepr.repr(x)
