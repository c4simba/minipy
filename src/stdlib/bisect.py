"""Bisection algorithms: keep a sorted list sorted (CPython's bisect).

The compiled form has no key= argument (the interpreter's has, as CPython)."""
import sys
from typing import TypeVar

T = TypeVar("T")


if sys._compiled:
    def bisect_right(a: list[T], x: T, lo: int = 0, hi: int | None = None) -> int:
        """The index where to insert x in a, after any equal items."""
        if lo < 0:
            raise ValueError("lo must be non-negative")
        h = len(a)
        if hi is not None:
            h = hi
        while lo < h:
            mid = (lo + h) // 2
            if x < a[mid]:
                h = mid
            else:
                lo = mid + 1
        return lo

    def bisect_left(a: list[T], x: T, lo: int = 0, hi: int | None = None) -> int:
        """The index where to insert x in a, before any equal items."""
        if lo < 0:
            raise ValueError("lo must be non-negative")
        h = len(a)
        if hi is not None:
            h = hi
        while lo < h:
            mid = (lo + h) // 2
            if a[mid] < x:
                lo = mid + 1
            else:
                h = mid
        return lo

    def insort_right(a: list[T], x: T, lo: int = 0, hi: int | None = None) -> None:
        a.insert(bisect_right(a, x, lo, hi), x)

    def insort_left(a: list[T], x: T, lo: int = 0, hi: int | None = None) -> None:
        a.insert(bisect_left(a, x, lo, hi), x)
else:
    def bisect_right(a, x, lo=0, hi=None, *, key=None):
        """The index where to insert x in a, after any equal items."""
        if lo < 0:
            raise ValueError("lo must be non-negative")
        if hi is None:
            hi = len(a)
        while lo < hi:
            mid = (lo + hi) // 2
            if key is None:
                below = x < a[mid]
            else:
                below = x < key(a[mid])
            if below:
                hi = mid
            else:
                lo = mid + 1
        return lo

    def bisect_left(a, x, lo=0, hi=None, *, key=None):
        """The index where to insert x in a, before any equal items."""
        if lo < 0:
            raise ValueError("lo must be non-negative")
        if hi is None:
            hi = len(a)
        while lo < hi:
            mid = (lo + hi) // 2
            if key is None:
                above = a[mid] < x
            else:
                above = key(a[mid]) < x
            if above:
                lo = mid + 1
            else:
                hi = mid
        return lo

    def insort_right(a, x, lo=0, hi=None, *, key=None):
        if key is None:
            lo = bisect_right(a, x, lo, hi)
        else:
            lo = bisect_right(a, key(x), lo, hi, key=key)
        a.insert(lo, x)

    def insort_left(a, x, lo=0, hi=None, *, key=None):
        if key is None:
            lo = bisect_left(a, x, lo, hi)
        else:
            lo = bisect_left(a, key(x), lo, hi, key=key)
        a.insert(lo, x)

bisect = bisect_right
insort = insort_right
