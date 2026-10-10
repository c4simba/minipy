"""Context variables (CPython's contextvars): ContextVar, Token, Context, copy_context().
Typed: a ContextVar holds values of one type; a context made by copy() sees the later changes of
the one it was copied from to the variables it has not set itself."""
from typing import Generic, TypeVar

__all__ = ("Context", "ContextVar", "Token", "copy_context")

_T = TypeVar("_T")
_U = TypeVar("_U")
_F = TypeVar("_F")
_A = TypeVar("_A")
_V = TypeVar("_V")
_D = TypeVar("_D")


class Context:
    """A mapping of context variables to their values; Context.run() runs code in it."""

    def __init__(self) -> None:
        self._parent: Context | None = None
        self._id = len(_ids)
        _ids.append(self._id)

    def run(self, callable: _F, *args: _A):
        """callable(*args) in this context (its changes to the variables stay in it)."""
        if self._entered:
            raise RuntimeError("cannot enter context: " + repr(self) + " is already entered")
        saved = _current[0]
        _current[0] = self
        self._entered = True
        try:
            return callable(*args)
        finally:
            self._entered = False
            _current[0] = saved

    @property
    def _entered(self) -> bool:
        return self._id in _entered

    @_entered.setter
    def _entered(self, v: bool) -> None:
        if v:
            _entered.add(self._id)
        else:
            _entered.discard(self._id)

    def copy(self) -> "Context":
        c = Context()
        c._parent = self
        return c

    def __getitem__(self, var: _V):
        return var._get_in(self)

    def get(self, var: _V, default: _D = None):
        if var._has_in(self):
            return var._get_in(self)
        return default

    def __contains__(self, var: _V) -> bool:
        return var._has_in(self)

    def __repr__(self) -> str:
        return "<Context object>"


_ids: list[int] = []
_entered: set[int] = set()
_current: list[Context] = [Context()]


class Token(Generic[_U]):
    """What ContextVar.set() gives: ContextVar.reset(token) puts back the value before it."""
    MISSING = None

    def __init__(self, var, ctx: Context, old_value: _U | None, has_old: bool) -> None:
        self._var = var
        self._ctx = ctx
        self.old_value = old_value
        self._has_old = has_old
        self._used = False

    @property
    def var(self):
        return self._var

    def __repr__(self) -> str:
        return "<Token var=" + repr(self._var) + ">"


class ContextVar(Generic[_T]):
    """A context variable: get(), set(value), reset(token)."""

    def __init__(self, name: str, *, default: _T | None = None) -> None:
        self._name = name
        self._default = default
        self._has_default = default is not None
        self._vals: dict[int, _T] = {}

    @property
    def name(self) -> str:
        return self._name

    def _has_in(self, ctx: Context) -> bool:
        c: Context | None = ctx
        while c is not None:
            if c._id in self._vals:
                return True
            c = c._parent
        return False

    def _get_in(self, ctx: Context) -> _T:
        c: Context | None = ctx
        while c is not None:
            if c._id in self._vals:
                return self._vals[c._id]
            c = c._parent
        raise KeyError(self)

    def get(self, default: _T | None = None) -> _T:
        """The value in the current context; else default, else the variable's default (else LookupError)."""
        ctx = _current[0]
        if self._has_in(ctx):
            return self._get_in(ctx)
        if default is not None:
            return default
        d = self._default
        if self._has_default and d is not None:
            return d
        raise LookupError(self)

    def set(self, value: _T):
        """Sets the value in the current context: a Token for reset()."""
        ctx = _current[0]
        has_old = ctx._id in self._vals
        old: _T | None = self._vals[ctx._id] if has_old else None
        if not has_old and self._has_in(ctx):
            old = self._get_in(ctx)
            has_old = True
        self._vals[ctx._id] = value
        return Token(self, ctx, old, has_old)

    def reset(self, token) -> None:
        """Puts back the value the variable had before token's set()."""
        if token._used:
            raise RuntimeError(repr(token) + " has already been used once")
        if token._var is not self:
            raise ValueError(repr(token) + " was created by a different ContextVar")
        ctx = _current[0]
        if token._ctx is not ctx:
            raise ValueError(repr(token) + " was created in a different Context")
        token._used = True
        old = token.old_value
        if token._has_old and old is not None:
            self._vals[ctx._id] = old
        elif ctx._id in self._vals:
            del self._vals[ctx._id]

    def __repr__(self) -> str:
        r = "<ContextVar name=" + repr(self._name)
        d = self._default
        if self._has_default and d is not None:
            r += " default=" + repr(d)
        return r + ">"


def copy_context() -> Context:
    """A copy of the current context."""
    return _current[0].copy()
