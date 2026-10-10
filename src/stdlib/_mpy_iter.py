"""Iterator objects (a class with __next__) in compiled programs: the compiler loops
over __mpy_iter(obj.__iter__()), a generator of the __next__() values until
StopIteration, and turns next(it, default) into __mpy_next(it, default)."""
from typing import TypeVar

T = TypeVar("T")
D = TypeVar("D")


def __mpy_iter(it: T):
    while True:
        try:
            x = it.__next__()
        except StopIteration:
            return
        yield x


def __mpy_next(it: T, default: D) -> D:
    try:
        return it.__next__()
    except StopIteration:
        return default
