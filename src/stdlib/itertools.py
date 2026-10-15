"""Functions creating iterators for efficient looping (CPython's itertools, as generators).

In compiled programs the iterables of one call have one item type, and the
functions whose items are tuples of a length known only when running (product,
permutations, combinations, combinations_with_replacement, zip_longest, starmap,
tee, batched) and chain.from_iterable are not available; the interpreter has all."""
import sys


def count(start=0, step=1):
    """start, start + step, start + 2*step, ..."""
    n = start
    while True:
        yield n
        n += step


def cycle(iterable):
    """The items of iterable, then again, forever."""
    saved = []
    for x in iterable:
        yield x
        saved.append(x)
    if not saved:
        return
    while True:
        for x in saved:
            yield x


def repeat(obj, times: int = -1):
    """obj, times times (forever without times)."""
    if times < 0:
        while True:
            yield obj
    for i in range(times):
        yield obj


def accumulate(iterable, func=None, initial=None):
    """Running totals (or the results of func) of the items."""
    total = initial
    started = initial is not None
    if started:
        yield total
    for x in iterable:
        if not started:
            total = x
            started = True
        elif func is None:
            total = total + x
        else:
            total = func(total, x)
        yield total


def chain(*iterables):
    """The items of the first iterable, then of the next ..."""
    for it in iterables:
        for x in it:
            yield x


def islice(iterable, *args):
    """islice(it, stop) / islice(it, start, stop[, step]): the items of a slice."""
    start = 0
    stop: int | None = None
    step = 1
    if len(args) == 1:
        stop = args[0]
    elif len(args) >= 2:
        if args[0] is not None:
            start = args[0]
        stop = args[1]
        if len(args) >= 3 and args[2] is not None:
            step = args[2]
    if len(args) < 1 or len(args) > 3:
        raise TypeError("islice expected at most 4 arguments, got " + str(len(args) + 1))
    if start < 0 or (stop is not None and stop < 0):
        raise ValueError("Indices for islice() must be None or an integer: 0 <= x <= sys.maxsize.")
    if step < 1:
        raise ValueError("Step for islice() must be a positive integer or None.")
    nxt = start
    i = 0
    for x in iterable:
        if stop is not None and i >= stop:
            return
        if i == nxt:
            yield x
            nxt += step
        i += 1


def compress(data, selectors):
    """The items of data whose selector is true."""
    for d, s in zip(data, selectors):
        if s:
            yield d


def dropwhile(predicate, iterable):
    """The items from the first one predicate is false for."""
    dropping = True
    for x in iterable:
        if dropping and predicate(x):
            continue
        dropping = False
        yield x


def takewhile(predicate, iterable):
    """The items until the first one predicate is false for."""
    for x in iterable:
        if not predicate(x):
            return
        yield x


def filterfalse(predicate, iterable):
    """The items predicate is false for (None: the false items)."""
    for x in iterable:
        if predicate is None:
            if not x:
                yield x
        elif not predicate(x):
            yield x


def pairwise(iterable):
    """(a, b), (b, c), ... of consecutive items."""
    first = True
    prev = None
    for x in iterable:
        if not first:
            yield (prev, x)
        prev = x
        first = False


def groupby(iterable, key=None):
    """(key, the run of consecutive items with that key) for each run."""
    group = []
    gkey = None
    started = False
    for x in iterable:
        if key is None:
            k = x
        else:
            k = key(x)
        if started and k != gkey:
            yield (gkey, iter(group))
            group = []
        if not started or k != gkey:
            gkey = k
            started = True
        group.append(x)
    if started:
        yield (gkey, iter(group))


if not sys._compiled:
    def _from_iterable(iterables):
        for it in iterables:
            for x in it:
                yield x

    chain.from_iterable = _from_iterable

    def zip_longest(*iterables, fillvalue=None):
        its = [iter(it) for it in iterables]
        active = len(its)
        if not active:
            return
        while True:
            values = []
            for i, it in enumerate(its):
                try:
                    value = next(it)
                except StopIteration:
                    active -= 1
                    if not active:
                        return
                    its[i] = repeat(fillvalue)
                    value = fillvalue
                values.append(value)
            yield tuple(values)

    def starmap(function, iterable):
        for args in iterable:
            yield function(*args)

    def tee(iterable, n=2):
        if n < 0:
            raise ValueError("n must be >= 0")
        items = list(iterable)
        return tuple(iter(items) for i in range(n))

    def batched(iterable, n, *, strict=False):
        if n < 1:
            raise ValueError("n must be at least one")
        batch = []
        for x in iterable:
            batch.append(x)
            if len(batch) == n:
                yield tuple(batch)
                batch = []
        if batch:
            if strict:
                raise ValueError("batched(): incomplete batch")
            yield tuple(batch)

    def product(*iterables, repeat=1):
        if repeat < 0:
            raise ValueError("repeat argument cannot be negative")
        pools = [tuple(pool) for pool in iterables] * repeat
        result = [[]]
        for pool in pools:
            result = [x + [y] for x in result for y in pool]
        for prod in result:
            yield tuple(prod)

    def permutations(iterable, r=None):
        pool = tuple(iterable)
        n = len(pool)
        if r is None:
            r = n
        if r < 0:
            raise ValueError("r must be non-negative")
        if r > n:
            return
        indices = list(range(n))
        cycles = list(range(n, n - r, -1))
        yield tuple(pool[i] for i in indices[:r])
        while n:
            for i in reversed(range(r)):
                cycles[i] -= 1
                if cycles[i] == 0:
                    indices[i:] = indices[i + 1:] + indices[i:i + 1]
                    cycles[i] = n - i
                else:
                    j = cycles[i]
                    indices[i], indices[-j] = indices[-j], indices[i]
                    yield tuple(pool[i] for i in indices[:r])
                    break
            else:
                return

    def combinations(iterable, r):
        pool = tuple(iterable)
        n = len(pool)
        if r < 0:
            raise ValueError("r must be non-negative")
        if r > n:
            return
        indices = list(range(r))
        yield tuple(pool[i] for i in indices)
        while True:
            for i in reversed(range(r)):
                if indices[i] != i + n - r:
                    break
            else:
                return
            indices[i] += 1
            for j in range(i + 1, r):
                indices[j] = indices[j - 1] + 1
            yield tuple(pool[i] for i in indices)

    def combinations_with_replacement(iterable, r):
        pool = tuple(iterable)
        n = len(pool)
        if r < 0:
            raise ValueError("r must be non-negative")
        if not n and r:
            return
        indices = [0] * r
        yield tuple(pool[i] for i in indices)
        while True:
            for i in reversed(range(r)):
                if indices[i] != n - 1:
                    break
            else:
                return
            indices[i:] = [indices[i] + 1] * (r - i)
            yield tuple(pool[i] for i in indices)
