"""Arrays of numbers or characters, kept as their C values (CPython's array).

    array(typecode[, initializer])

Type codes: b/B (un)signed char, u/w unicode character (4 bytes), h/H short, i/I int,
l/L long, q/Q long long (l/L have 8 bytes, as on a 64-bit CPython), f float, d double.
Methods: append, buffer_info, byteswap, count, extend, frombytes, fromfile, fromlist,
fromunicode, index, insert, pop, remove, reverse, tobytes, tofile, tolist, tounicode;
indexing, slicing, +, *, in, comparisons, len(), iteration.

Compiled programs: the type code is a literal, which decides the type of the items
(int for the integer codes, float for f and d, str for u and w): array('d', [1, 2])
is an array[float]. Slice assignment takes an array of the same type code."""
import sys
import struct
from typing import Generic, TypeVar

__all__ = ["ArrayType", "array", "typecodes"]

_T = TypeVar("_T")
_X = TypeVar("_X")
_Y = TypeVar("_Y")
_A = TypeVar("_A")
_I = TypeVar("_I")

typecodes = "bBuwhHiIlLqQfd"

_SIZES = {"b": 1, "B": 1, "u": 4, "w": 4, "h": 2, "H": 2, "i": 4, "I": 4, "l": 8, "L": 8, "q": 8, "Q": 8,
          "f": 4, "d": 8}


def _short(v: int) -> None:
    if v < -32768:
        raise OverflowError("signed short integer is less than minimum")
    if v > 32767:
        raise OverflowError("signed short integer is greater than maximum")


def _int32(v: int) -> None:
    if v < -2147483648:
        raise OverflowError("signed integer is less than minimum")
    if v > 2147483647:
        raise OverflowError("signed integer is greater than maximum")


def _check_int(tc: str, v: int) -> int:
    """v, if it fits an item of type code tc (else OverflowError, with CPython's message)."""
    if tc == "b":
        _short(v)
        if v < -128:
            raise OverflowError("signed char is less than minimum")
        if v > 127:
            raise OverflowError("signed char is greater than maximum")
    elif tc == "B":
        if v < 0:
            raise OverflowError("unsigned byte integer is less than minimum")
        if v > 255:
            raise OverflowError("unsigned byte integer is greater than maximum")
    elif tc == "h":
        _short(v)
    elif tc == "H":
        _int32(v)
        if v < 0:
            raise OverflowError("unsigned short is less than minimum")
        if v > 65535:
            raise OverflowError("unsigned short is greater than maximum")
    elif tc == "i":
        _int32(v)
    elif tc == "I" or tc == "L":
        if v < 0:
            raise OverflowError("can't convert negative value to unsigned int")
        if tc == "I" and v > 4294967295:
            raise OverflowError("unsigned int is greater than maximum")
    elif tc == "Q":
        if v < 0:
            raise OverflowError("can't convert negative int to unsigned")
    return v


def _f32(x: float) -> float:
    """x rounded to a C float (inf when it is too large for one)."""
    try:
        b = struct._p_float(x, 4, True, "f")
    except OverflowError:
        return float("inf") if x > 0 else float("-inf")
    return struct._u_float(b, 0, 4, True)


def _char(v: str) -> str:
    if len(v) != 1:
        raise TypeError("array item must be a unicode character, not a string of length " + str(len(v)))
    return v


def _pack(tc: str, x: _X) -> bytes:
    """The bytes of item x (little-endian, as on the machines minipy runs on)."""
    if isinstance(x, str):
        return ord(x).to_bytes(4, "little")
    elif isinstance(x, float):
        return struct._p_float(x, 4 if tc == "f" else 8, True, tc)
    else:
        return x.to_bytes(_SIZES[tc], "little", signed=tc in "bhilq")


def _unpack(tc: str, b: bytes, off: int, z: _X) -> _X:
    """The item at b[off:] (z: an item of the array's type)."""
    n = _SIZES[tc]
    if isinstance(z, str):
        return chr(int.from_bytes(b[off:off + 4], "little"))
    elif isinstance(z, float):
        return struct._u_float(b, off, n, True)
    else:
        return int.from_bytes(b[off:off + n], "little", signed=tc in "bhilq")


def _tname(v: _Y) -> str:
    if isinstance(v, bool):
        return "bool"
    elif isinstance(v, int):
        return "int"
    elif isinstance(v, float):
        return "float"
    elif isinstance(v, str):
        return "str"
    elif isinstance(v, bytes):
        return "bytes"
    else:
        return type(v).__name__


def _as(z: _X, v: _Y) -> _X:
    """v as an item of z's type (an int for a float array: a float; TypeError: not one)."""
    if isinstance(z, float) and isinstance(v, int):
        return float(v)
    elif isinstance(z, float) and isinstance(v, float):
        return v
    elif isinstance(z, int) and isinstance(v, int):
        return v
    elif isinstance(z, str) and isinstance(v, str):
        return v
    elif isinstance(z, str):
        raise TypeError("array item must be a unicode character, not " + _tname(v))
    elif isinstance(z, float):
        raise TypeError("must be real number, not " + _tname(v))
    else:
        raise TypeError("'" + _tname(v) + "' object cannot be interpreted as an integer")


def _slice_bounds(n: int, start: int | None, stop: int | None, step: int | None) -> tuple[int, int, int]:
    """(start, stop, step) of xs[start:stop:step] for len(xs) == n, as for range()."""
    st = 1 if step is None else step
    if st == 0:
        raise ValueError("slice step cannot be zero")
    if st > 0:
        lo, hi = 0, n
    else:
        lo, hi = -1, n - 1
    if start is None:
        b = hi if st < 0 else lo
    else:
        b = start + n if start < 0 else start
        if b < lo:
            b = lo
        elif b > hi:
            b = hi
    if stop is None:
        e = lo if st < 0 else hi
    else:
        e = stop + n if stop < 0 else stop
        if e < lo:
            e = lo
        elif e > hi:
            e = hi
    return (b, e, st)


def _slice_range(n: int, start: int | None, stop: int | None, step: int | None) -> list[int]:
    """The indexes of xs[start:stop:step] for len(xs) == n."""
    b, e, st = _slice_bounds(n, start, stop, step)
    return list(range(b, e, st))


if not sys._compiled:
    import warnings

    def _conv(tc, v):
        """v as an item of type code tc (CPython's errors for a wrong type)."""
        if tc == "u" or tc == "w":
            if not isinstance(v, str):
                raise TypeError("array item must be a unicode character, not " + type(v).__name__)
            return _char(v)
        if tc == "f" or tc == "d":
            if isinstance(v, (str, bytes, bytearray)) or not (isinstance(v, (int, float)) or hasattr(v, "__float__")
                                                               or hasattr(v, "__index__")):
                raise TypeError("must be real number, not " + type(v).__name__)
            x = float(v)
            return _f32(x) if tc == "f" else x
        if isinstance(v, bool):
            v = int(v)
        elif not isinstance(v, int):
            if isinstance(v, float) or not hasattr(v, "__index__"):
                raise TypeError("'" + type(v).__name__ + "' object cannot be interpreted as an integer")
            v = v.__index__()
        return _check_int(tc, v)

    def _index(i):
        if isinstance(i, int):
            return i
        if hasattr(i, "__index__") and not isinstance(i, float):
            return i.__index__()
        raise TypeError("array indices must be integers")


def _extend_items(dst: list[_X], src: list[_Y], z: _X, w: _Y) -> None:
    """dst += src, for items of one kind (z, w: items of their types)."""
    if isinstance(z, int) and isinstance(w, int):
        dst.extend(src)
    elif isinstance(z, float) and isinstance(w, float):
        dst.extend(src)
    elif isinstance(z, str) and isinstance(w, str):
        dst.extend(src)


def _cmp_items(xs: list[_X], ys: list[_Y]) -> int:
    """-1, 0 or 1: xs against ys, item by item."""
    n = min(len(xs), len(ys))
    for k in range(n):
        if xs[k] != ys[k]:
            return -1 if xs[k] < ys[k] else 1
    if len(xs) == len(ys):
        return 0
    return -1 if len(xs) < len(ys) else 1


class array(Generic[_T]):
    """array(typecode[, initializer]): an array of items of one C type."""

    if sys._compiled:
        def __init__(self, typecode: str, items: list[_T], zero: _T) -> None:
            if typecode not in _SIZES:
                raise ValueError("bad typecode (must be b, B, u, w, h, H, i, I, l, L, q, Q, f or d)")
            self.typecode = typecode
            self.itemsize = _SIZES[typecode]
            self._items = items
            self._zero = zero
    else:
        __hash__ = None

        def __init__(self, typecode, initializer=None):
            if not isinstance(typecode, str):
                raise TypeError("array() argument 1 must be a unicode character, not " + type(typecode).__name__)
            if len(typecode) != 1:
                raise TypeError("array() argument 1 must be a unicode character, not a string of length "
                                + str(len(typecode)))
            if typecode not in _SIZES:
                raise ValueError("bad typecode (must be b, B, u, w, h, H, i, I, l, L, q, Q, f or d)")
            if typecode == "u":
                warnings.warn("The 'u' type code is deprecated and will be removed in Python 3.16",
                              DeprecationWarning, stacklevel=2)
            self.typecode = typecode
            self.itemsize = _SIZES[typecode]
            self._items = []
            self._zero = "\0" if typecode in "uw" else 0.0 if typecode in "fd" else 0
            if initializer is None:
                return
            if isinstance(initializer, str):
                if typecode not in "uw":
                    raise TypeError("cannot use a str to initialize an array with typecode '" + typecode + "'")
                self.fromunicode(initializer)
            elif isinstance(initializer, (bytes, bytearray)):
                self.frombytes(bytes(initializer))
            elif isinstance(initializer, array):
                if (initializer.typecode in "uw") != (typecode in "uw"):
                    if initializer.typecode in "uw":
                        raise TypeError("cannot use a unicode array to initialize an array with typecode '"
                                        + typecode + "'")
                self._items = [_conv(typecode, v) for v in initializer._items]
            else:
                for v in initializer:
                    self._items.append(_conv(typecode, v))

    def _conv(self, v: _T) -> _T:
        if not sys._compiled:
            return _conv(self.typecode, v)
        if isinstance(v, str):
            return _char(v)
        elif isinstance(v, float):
            return _f32(v) if self.typecode == "f" else v
        else:
            return _check_int(self.typecode, v)

    def _make(self, items: list[_T]):
        """A new array of this type code holding items."""
        if sys._compiled:
            return array(self.typecode, items, self._zero)
        r = array(self.typecode)
        r._items = items
        return r

    # ------------------------------------------------------------ the sequence

    def __len__(self) -> int:
        return len(self._items)

    def __bool__(self) -> bool:
        return len(self._items) > 0

    def __iter__(self):
        for x in self._items:
            yield x

    def __reversed__(self):
        for i in range(len(self._items) - 1, -1, -1):
            yield self._items[i]

    def __contains__(self, v: _T) -> bool:
        return v in self._items

    def __getitem__(self, i: int) -> _T:
        if not sys._compiled:
            if isinstance(i, slice):
                return self._make([self._items[k] for k in _slice_range(len(self._items), i.start, i.stop, i.step)])
            i = _index(i)
        n = len(self._items)
        if i < -n or i >= n:
            raise IndexError("array index out of range")
        return self._items[i]

    def __setitem__(self, i: int, v: _T) -> None:
        if not sys._compiled:
            if isinstance(i, slice):
                self.__mpy_setslice__(i.start, i.stop, i.step, v)
                return
            i = _index(i)
        n = len(self._items)
        if i < -n or i >= n:
            raise IndexError("array assignment index out of range")
        self._items[i] = self._conv(v)

    def __delitem__(self, i: int) -> None:
        if not sys._compiled:
            if isinstance(i, slice):
                self.__mpy_delslice__(i.start, i.stop, i.step)
                return
            i = _index(i)
        n = len(self._items)
        if i < -n or i >= n:
            raise IndexError("array assignment index out of range")
        del self._items[i]

    def __mpy_getslice__(self, start: int | None, stop: int | None, step: int | None):
        return self._make([self._items[k] for k in _slice_range(len(self._items), start, stop, step)])

    def __mpy_setslice__(self, start: int | None, stop: int | None, step: int | None, v: _A) -> None:
        if not isinstance(v, array):
            raise TypeError('can only assign array (not "' + type(v).__name__ + '") to array slice')
        if v.typecode != self.typecode:
            raise TypeError("bad argument type for built-in operation")
        new: list[_T] = []
        _extend_items(new, v._items, self._zero, v._zero)
        if step is None or step == 1:
            lo, hi, st = _slice_bounds(len(self._items), start, stop, step)
            if hi < lo:
                hi = lo
            self._items[lo:hi] = new
            return
        idx = _slice_range(len(self._items), start, stop, step)
        if len(idx) != len(new):
            raise ValueError("attempt to assign array of size " + str(len(new)) + " to extended slice of size "
                             + str(len(idx)))
        for j in range(len(new)):
            self._items[idx[j]] = new[j]

    def __mpy_delslice__(self, start: int | None, stop: int | None, step: int | None) -> None:
        gone = set(_slice_range(len(self._items), start, stop, step))
        self._items = [self._items[k] for k in range(len(self._items)) if k not in gone]

    # ------------------------------------------------------------ operators

    def __add__(self, other: _A):
        if not isinstance(other, array):
            raise TypeError('can only append array (not "' + type(other).__name__ + '") to array')
        if other.typecode != self.typecode:
            raise TypeError("bad argument type for built-in operation")
        items = list(self._items)
        _extend_items(items, other._items, self._zero, other._zero)
        return self._make(items)

    def __iadd__(self, other: _A):
        if not isinstance(other, array):
            raise TypeError('can only extend array with array (not "' + type(other).__name__ + '")')
        if other.typecode != self.typecode:
            raise TypeError("can only extend with array of same kind")
        _extend_items(self._items, list(other._items), self._zero, other._zero)
        return self

    def __mul__(self, n: int):
        return self._make(self._items * n)

    def __rmul__(self, n: int):
        return self._make(self._items * n)

    def __imul__(self, n: int):
        self._items = self._items * n
        return self

    def __eq__(self, other: _A) -> bool:
        if not isinstance(other, array):
            return False
        if len(self._items) != len(other._items):
            return False
        return _cmp_items(self._items, other._items) == 0

    def __ne__(self, other: _A) -> bool:
        return not self == other

    def __lt__(self, other: _A) -> bool:
        return _cmp_items(self._items, other._items) < 0

    def __le__(self, other: _A) -> bool:
        return _cmp_items(self._items, other._items) <= 0

    def __gt__(self, other: _A) -> bool:
        return _cmp_items(self._items, other._items) > 0

    def __ge__(self, other: _A) -> bool:
        return _cmp_items(self._items, other._items) >= 0

    def __repr__(self) -> str:
        if not self._items:
            return "array('" + self.typecode + "')"
        if self.typecode in "uw":
            return "array('" + self.typecode + "', " + repr(self.tounicode()) + ")"
        return "array('" + self.typecode + "', " + repr(self._items) + ")"

    def __bytes__(self) -> bytes:
        return self.tobytes()

    def __copy__(self):
        return self._make(list(self._items))

    def __deepcopy__(self, memo):
        return self._make(list(self._items))

    # ------------------------------------------------------------ methods

    def append(self, v: _T) -> None:
        """Append new value v to the end of the array."""
        self._items.append(self._conv(v))

    def extend(self, it: _I) -> None:
        """Append items to the end of the array (it: an array of the same kind, or an iterable)."""
        if isinstance(it, array):
            if it.typecode != self.typecode:
                raise TypeError("can only extend with array of same kind")
            _extend_items(self._items, list(it._items), self._zero, it._zero)
        else:
            for v in it:
                self._items.append(self._conv(_as(self._zero, v)))

    def insert(self, i: int, v: _T) -> None:
        """Insert a new item v into the array before position i."""
        self._items.insert(i, self._conv(v))

    def pop(self, i: int = -1) -> _T:
        """Return the i-th element and delete it from the array (i=-1: the last one)."""
        n = len(self._items)
        if n == 0:
            raise IndexError("pop from empty array")
        if i < -n or i >= n:
            raise IndexError("pop index out of range")
        return self._items.pop(i)

    def remove(self, v: _T) -> None:
        """Remove the first occurrence of v in the array."""
        for k in range(len(self._items)):
            if self._items[k] == v:
                del self._items[k]
                return
        raise ValueError("array.remove(x): x not in array")

    def index(self, v: _T, start: int = 0, stop: int = sys.maxsize) -> int:
        """Return index of first occurrence of v in the array (between start and stop)."""
        n = len(self._items)
        if start < 0:
            start = max(start + n, 0)
        if stop < 0:
            stop = max(stop + n, 0)
        for k in range(start, min(stop, n)):
            if self._items[k] == v:
                return k
        raise ValueError("array.index(x): x not in array")

    def count(self, v: _T) -> int:
        """Return number of occurrences of v in the array."""
        k = 0
        for x in self._items:
            if x == v:
                k += 1
        return k

    def reverse(self) -> None:
        """Reverse the order of the items in the array."""
        self._items.reverse()

    def tolist(self) -> list[_T]:
        """Convert array to an ordinary list with the same items."""
        return list(self._items)

    def fromlist(self, xs: list[_T]) -> None:
        """Append items to array from list (all of them, or none: an item of a wrong type)."""
        if not sys._compiled:
            if not isinstance(xs, list):
                raise TypeError("arg must be list")
        new = [self._conv(v) for v in xs]
        self._items.extend(new)

    def tobytes(self) -> bytes:
        """The array's machine values as bytes."""
        out: list[bytes] = []
        for x in self._items:
            out.append(_pack(self.typecode, x))
        return b"".join(out)

    def frombytes(self, b: bytes) -> None:
        """Append items from the bytes b (machine values)."""
        if not sys._compiled:
            b = bytes(b)
        n = self.itemsize
        if len(b) % n:
            raise ValueError("bytes length not a multiple of item size")
        for off in range(0, len(b), n):
            self._items.append(_unpack(self.typecode, b, off, self._zero))

    def tofile(self, f: _I) -> None:
        """Write all items (as machine values) to the file object f."""
        f.write(self.tobytes())

    def fromfile(self, f: _I, n: int) -> None:
        """Read n objects from the file object f and append them to the end of the array."""
        if n < 0:
            raise ValueError("negative count")
        want = n * self.itemsize
        b = f.read(want)
        if not sys._compiled:
            if not isinstance(b, bytes):
                raise TypeError("read() didn't return bytes")
        whole = len(b) // self.itemsize * self.itemsize
        self.frombytes(b[:whole])
        if len(b) != want:
            raise EOFError("read() didn't return enough bytes")

    def tounicode(self) -> str:
        """The array (of type code 'u' or 'w') as a str."""
        if self.typecode not in "uw":
            raise ValueError("tounicode() may only be called on unicode type arrays ('u' or 'w')")
        out: list[str] = []
        for x in self._items:
            out.append(str(x))
        return "".join(out)

    def fromunicode(self, s: str) -> None:
        """Extends this array (of type code 'u' or 'w') with the characters of s."""
        if not sys._compiled:
            if not isinstance(s, str):
                raise TypeError("fromunicode() argument must be str, not " + type(s).__name__)
        if self.typecode not in "uw":
            raise ValueError("fromunicode() may only be called on unicode type arrays ('u' or 'w')")
        for ch in s:
            self._items.append(_as(self._zero, ch))

    def buffer_info(self) -> tuple[int, int]:
        """(address, length): the address is an identifier of the item storage here."""
        return (id(self._items), len(self._items))

    def byteswap(self) -> None:
        """Byteswap all items of the array."""
        n = self.itemsize
        b = self.tobytes()
        out: list[bytes] = []
        for off in range(0, len(b), n):
            out.append(b[off:off + n][::-1])
        self._items = []
        self.frombytes(b"".join(out))


ArrayType = array


if sys._compiled:
    def _ints() -> list[int]:
        return []

    def _floats() -> list[float]:
        return []

    def _strs() -> list[str]:
        return []

    def _fill(a: _A, init: _I) -> _A:
        """a with the items of the initializer init (array(typecode, init))."""
        if init is not None:
            if isinstance(init, bytes):
                a.frombytes(init)
            elif isinstance(init, str):
                if a.typecode not in "uw":
                    raise TypeError("cannot use a str to initialize an array with typecode '" + a.typecode + "'")
                a.fromunicode(init)
            elif isinstance(init, array):
                if init.typecode in "uw" and a.typecode not in "uw":
                    raise TypeError("cannot use a unicode array to initialize an array with typecode '"
                                    + a.typecode + "'")
                for v in init._items:
                    a.append(_as(a._zero, v))
            else:
                for v in init:
                    a.append(_as(a._zero, v))
        return a

    # array(typecode, initializer) with a literal type code: the one of these for it
    def _array_b(typecode: str, initializer: _I = None) -> array[int]:
        return _fill(array(typecode, _ints(), 0), initializer)

    def _array_B(typecode: str, initializer: _I = None) -> array[int]:
        return _fill(array(typecode, _ints(), 0), initializer)

    def _array_h(typecode: str, initializer: _I = None) -> array[int]:
        return _fill(array(typecode, _ints(), 0), initializer)

    def _array_H(typecode: str, initializer: _I = None) -> array[int]:
        return _fill(array(typecode, _ints(), 0), initializer)

    def _array_i(typecode: str, initializer: _I = None) -> array[int]:
        return _fill(array(typecode, _ints(), 0), initializer)

    def _array_I(typecode: str, initializer: _I = None) -> array[int]:
        return _fill(array(typecode, _ints(), 0), initializer)

    def _array_l(typecode: str, initializer: _I = None) -> array[int]:
        return _fill(array(typecode, _ints(), 0), initializer)

    def _array_L(typecode: str, initializer: _I = None) -> array[int]:
        return _fill(array(typecode, _ints(), 0), initializer)

    def _array_q(typecode: str, initializer: _I = None) -> array[int]:
        return _fill(array(typecode, _ints(), 0), initializer)

    def _array_Q(typecode: str, initializer: _I = None) -> array[int]:
        return _fill(array(typecode, _ints(), 0), initializer)

    def _array_f(typecode: str, initializer: _I = None) -> array[float]:
        return _fill(array(typecode, _floats(), 0.0), initializer)

    def _array_d(typecode: str, initializer: _I = None) -> array[float]:
        return _fill(array(typecode, _floats(), 0.0), initializer)

    def _array_u(typecode: str, initializer: _I = None) -> array[str]:
        return _fill(array(typecode, _strs(), "\0"), initializer)

    def _array_w(typecode: str, initializer: _I = None) -> array[str]:
        return _fill(array(typecode, _strs(), "\0"), initializer)
