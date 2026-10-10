"""Heap queue algorithm (a.k.a. priority queue): CPython's heapq, in Python.

Heaps are lists for which a[k] <= a[2*k+1] and a[k] <= a[2*k+2] for all k;
the smallest element is a[0]."""
import sys
from typing import TypeVar

T = TypeVar("T")


def heappush(heap: list[T], item: T) -> None:
    """Push item onto heap, maintaining the heap invariant."""
    heap.append(item)
    _siftdown(heap, 0, len(heap) - 1)


def heappop(heap: list[T]) -> T:
    """Pop the smallest item off the heap, maintaining the heap invariant."""
    if not heap:
        raise IndexError("index out of range")
    lastelt = heap.pop()
    if heap:
        returnitem = heap[0]
        heap[0] = lastelt
        _siftup(heap, 0)
        return returnitem
    return lastelt


def heapreplace(heap: list[T], item: T) -> T:
    """Pop and return the current smallest value, and add the new item."""
    if not heap:
        raise IndexError("index out of range")
    returnitem = heap[0]
    heap[0] = item
    _siftup(heap, 0)
    return returnitem


def heappushpop(heap: list[T], item: T) -> T:
    """Fast version of a heappush followed by a heappop."""
    if heap and heap[0] < item:
        top = heap[0]
        heap[0] = item
        _siftup(heap, 0)
        return top
    return item


def heapify(x: list[T]) -> None:
    """Transform list into a heap, in-place, in O(len(x)) time."""
    n = len(x)
    for i in reversed(range(n // 2)):
        _siftup(x, i)


def _siftdown(heap: list[T], startpos: int, pos: int) -> None:
    newitem = heap[pos]
    while pos > startpos:
        parentpos = (pos - 1) >> 1
        parent = heap[parentpos]
        if newitem < parent:
            heap[pos] = parent
            pos = parentpos
            continue
        break
    heap[pos] = newitem


def _siftup(heap: list[T], pos: int) -> None:
    endpos = len(heap)
    startpos = pos
    newitem = heap[pos]
    childpos = 2 * pos + 1
    while childpos < endpos:
        rightpos = childpos + 1
        if rightpos < endpos and not heap[childpos] < heap[rightpos]:
            childpos = rightpos
        heap[pos] = heap[childpos]
        pos = childpos
        childpos = 2 * pos + 1
    heap[pos] = newitem
    _siftdown(heap, startpos, pos)


def heappush_max(heap: list[T], item: T) -> None:
    """Push item onto max-heap, maintaining the heap invariant."""
    heap.append(item)
    _siftdown_max(heap, 0, len(heap) - 1)


def heappop_max(heap: list[T]) -> T:
    """Pop the largest item off the max-heap, maintaining the heap invariant."""
    if not heap:
        raise IndexError("index out of range")
    lastelt = heap.pop()
    if heap:
        returnitem = heap[0]
        heap[0] = lastelt
        _siftup_max(heap, 0)
        return returnitem
    return lastelt


def heapreplace_max(heap: list[T], item: T) -> T:
    """Pop and return the current largest value, and add the new item."""
    if not heap:
        raise IndexError("index out of range")
    returnitem = heap[0]
    heap[0] = item
    _siftup_max(heap, 0)
    return returnitem


def heappushpop_max(heap: list[T], item: T) -> T:
    """Fast version of a heappush_max followed by a heappop_max."""
    if heap and item < heap[0]:
        top = heap[0]
        heap[0] = item
        _siftup_max(heap, 0)
        return top
    return item


def heapify_max(x: list[T]) -> None:
    """Transform list into a max-heap, in-place, in O(len(x)) time."""
    n = len(x)
    for i in reversed(range(n // 2)):
        _siftup_max(x, i)


def _siftdown_max(heap: list[T], startpos: int, pos: int) -> None:
    newitem = heap[pos]
    while pos > startpos:
        parentpos = (pos - 1) >> 1
        parent = heap[parentpos]
        if parent < newitem:
            heap[pos] = parent
            pos = parentpos
            continue
        break
    heap[pos] = newitem


def _siftup_max(heap: list[T], pos: int) -> None:
    endpos = len(heap)
    startpos = pos
    newitem = heap[pos]
    childpos = 2 * pos + 1
    while childpos < endpos:
        rightpos = childpos + 1
        if rightpos < endpos and not heap[rightpos] < heap[childpos]:
            childpos = rightpos
        heap[pos] = heap[childpos]
        pos = childpos
        childpos = 2 * pos + 1
    heap[pos] = newitem
    _siftdown_max(heap, startpos, pos)


def nlargest(n: int, iterable: list[T], key=None) -> list[T]:
    """The n largest elements, largest first (sorted(iterable, key=key, reverse=True)[:n])."""
    if key is None:
        result = sorted(iterable, reverse=True)
    else:
        result = sorted(iterable, key=key, reverse=True)
    return result[:n] if n > 0 else []


def nsmallest(n: int, iterable: list[T], key=None) -> list[T]:
    """The n smallest elements, smallest first (sorted(iterable, key=key)[:n])."""
    if key is None:
        result = sorted(iterable)
    else:
        result = sorted(iterable, key=key)
    return result[:n] if n > 0 else []


if not sys._compiled:
    def merge(*iterables, key=None, reverse=False):
        """The sorted iterables merged into one sorted stream (lazily)."""
        h = []
        if key is None:
            key = _identity
        for order, it in enumerate(map(iter, iterables)):
            for value in it:
                h.append([key(value), order * (-1 if reverse else 1), value, it])
                break
        if reverse:
            heapify_max(h)
            pop, replace = heappop_max, heapreplace_max
        else:
            heapify(h)
            pop, replace = heappop, heapreplace
        while h:
            s = h[0]
            yield s[2]
            for value in s[3]:
                s[0] = key(value)
                s[2] = value
                replace(h, s)
                break
            else:
                pop(h)

    def _identity(x):
        return x
