"""The typing names plainpy's own standard library uses (`from _ppy_typing import ...`).

The compiler reads this module name as typing. The interpreter gets light
stand-ins that do not import typing.py: typing imports collections, functools,
operator ... and those modules annotate with these names. As in CPython, the
library's classes are not Generic subclasses here: deque[int] is a
types.GenericAlias."""
from _typing import TypeVar

__all__ = ["TypeVar", "Generic", "Callable", "Iterator", "Sequence"]


class _NoBase:
    """Generic[T] as a base class: no base at all."""

    def __mro_entries__(self, bases):
        return ()


class _Form:
    """A name only annotations use: Callable[[int], str] is Callable again."""

    def __init__(self, name):
        self._name = name

    def __getitem__(self, params):
        return self

    def __or__(self, other):
        return self

    def __ror__(self, other):
        return self

    def __repr__(self):
        return "typing." + self._name


class _Generic(_Form):
    def __getitem__(self, params):
        return _NoBase()

    def __mro_entries__(self, bases):
        return ()


Generic = _Generic("Generic")
Callable = _Form("Callable")
Iterator = _Form("Iterator")
Sequence = _Form("Sequence")
