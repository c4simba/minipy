"""types.GenericAlias (list[int]) and typing.Union (int | str): the interpreter's,
as CPython 3.14's C ones (Objects/genericaliasobject.c, Objects/unionobject.c).

Small on purpose: the interpreter imports it for every `int | None` and
`list[int]`; typing.py is imported only when a Union[...] checks its arguments."""

_NoneType = type(None)
_FunctionType = type(lambda: 0)


def _type_repr(obj):
    """An argument as GenericAlias / Union reprs show it (_Py_typing_type_repr)."""
    if obj is ...:
        return "..."
    if obj is _NoneType:
        return "None"
    if isinstance(obj, GenericAlias):
        return repr(obj)
    if hasattr(obj, "__origin__") and hasattr(obj, "__args__"):
        return repr(obj)
    if isinstance(obj, type) or isinstance(obj, _FunctionType):
        qualname = obj.__qualname__
        module = obj.__module__
    else:
        try:
            qualname = obj.__qualname__
            module = obj.__module__
        except AttributeError:
            return repr(obj)
        if not isinstance(qualname, str):
            return repr(obj)
    if module is None:
        return repr(obj)
    if module == "builtins":
        return qualname
    return "%s.%s" % (module, qualname)


def _args_repr(args):
    if len(args) == 0:
        return "()"
    return ", ".join("[" + ", ".join(_type_repr(x) for x in a) + "]" if isinstance(a, list) else _type_repr(a)
                     for a in args)


def _collect_parameters(args):
    """The type variables in args, in order (_Py_make_parameters)."""
    params = []
    for t in args:
        if isinstance(t, type):
            continue
        if hasattr(t, "__typing_subst__"):
            if t not in params:
                params.append(t)
            continue
        sub = getattr(t, "__parameters__", None)
        if not sub and isinstance(t, (list, tuple)):
            sub = _collect_parameters(t)
        if isinstance(sub, tuple):
            for p in sub:
                if p not in params:
                    params.append(p)
    return tuple(params)


def _unpack_args(item):
    """Unpacked tuple types in item spread out: (int, *tuple[str, bytes]) -> (int, str, bytes)."""
    if not isinstance(item, tuple):
        return item
    out = []
    for a in item:
        if not isinstance(a, type):
            sub = getattr(a, "__typing_unpacked_tuple_args__", None)
            if isinstance(sub, tuple) and not (len(sub) and sub[-1] is ...):
                out.extend(sub)
                continue
        out.append(a)
    return tuple(out)


def _is_unpacked_typevartuple(a):
    return not isinstance(a, type) and getattr(a, "__typing_is_unpacked_typevartuple__", False) is True


def _subs_tvars(obj, params, argitems):
    sub = getattr(obj, "__parameters__", None)
    if isinstance(sub, tuple) and sub:
        subargs = []
        for p in sub:
            i = params.index(p)
            arg = argitems[i]
            if type(params[i]).__name__ == "TypeVarTuple" and isinstance(arg, tuple):
                subargs.extend(arg)
            else:
                subargs.append(arg)
        obj = obj[tuple(subargs) if len(subargs) != 1 else subargs[0]]
    return obj


def _subs_parameters(self, args, params, item):
    """args with the type variables (params) replaced by item (_Py_subs_parameters)."""
    if not params:
        raise TypeError("%r is not a generic class" % (self,))
    item = _unpack_args(item)
    for p in params:
        prepare = getattr(p, "__typing_prepare_subst__", None)
        if prepare is not None:
            item = prepare(self, item if isinstance(item, tuple) else (item,))
    argitems = item if isinstance(item, tuple) else (item,)
    if len(argitems) != len(params):
        raise TypeError("Too %s arguments for %r; actual %d, expected %d"
                        % ("many" if len(argitems) > len(params) else "few", self, len(argitems), len(params)))
    out = []
    for a in args:
        if isinstance(a, type):
            out.append(a)
            continue
        unpack = _is_unpacked_typevartuple(a)
        subst = getattr(a, "__typing_subst__", None)
        if subst is not None:
            a = subst(argitems[params.index(a)])
        elif isinstance(a, list):
            a = [_subs_tvars(x, params, argitems) if not hasattr(x, "__typing_subst__")
                 else x.__typing_subst__(argitems[params.index(x)]) for x in a]
        else:
            a = _subs_tvars(a, params, argitems)
        if unpack:
            out.extend(a)
        else:
            out.append(a)
    return tuple(out)


_OWN = frozenset(("__class__", "__bases__", "__origin__", "__args__", "__unpacked__", "__parameters__",
                  "__typing_unpacked_tuple_args__", "__mro_entries__", "__reduce_ex__", "__reduce__",
                  "__copy__", "__deepcopy__", "__doc__"))


class GenericAlias:
    """Represent a PEP 585 generic type

E.g. for t = list[int], t.__origin__ is list and t.__args__ is (int,)."""

    def __new__(cls, origin, args, /):
        self = object.__new__(cls)
        if not isinstance(args, tuple):
            args = (args,)
        d = object.__getattribute__(self, "__dict__")
        d["__origin__"] = origin
        d["__args__"] = args
        d["_params"] = None
        d["__unpacked__"] = False
        return self

    def __getattribute__(self, name):
        if name in _OWN or name.startswith("_") and not name.startswith("__") or name in GenericAlias.__dict__ and name not in ("__module__", "__name__", "__qualname__"):
            return object.__getattribute__(self, name)
        return getattr(object.__getattribute__(self, "__dict__")["__origin__"], name)

    @property
    def __parameters__(self):
        d = object.__getattribute__(self, "__dict__")
        if d["_params"] is None:
            d["_params"] = _collect_parameters(d["__args__"])
        return d["_params"]

    @property
    def __typing_unpacked_tuple_args__(self):
        if self.__unpacked__ and self.__origin__ is tuple:
            return self.__args__
        return None

    def __repr__(self):
        r = _type_repr(self.__origin__) + "[" + _args_repr(self.__args__) + "]"
        return "*" + r if self.__unpacked__ else r

    def __getitem__(self, item):
        if not self.__parameters__:
            raise TypeError("%r is not a generic class" % (self,))
        res = GenericAlias(self.__origin__, _subs_parameters(self, self.__args__, self.__parameters__, item))
        object.__getattribute__(res, "__dict__")["__unpacked__"] = self.__unpacked__
        return res

    def __call__(self, *args, **kwargs):
        result = self.__origin__(*args, **kwargs)
        try:
            result.__orig_class__ = self
        except (AttributeError, TypeError):
            pass
        return result

    def __mro_entries__(self, bases):
        return (self.__origin__,)

    def __instancecheck__(self, obj):
        raise TypeError("isinstance() argument 2 cannot be a parameterized generic")

    def __subclasscheck__(self, cls):
        raise TypeError("issubclass() argument 2 cannot be a parameterized generic")

    def __eq__(self, other):
        if not isinstance(other, GenericAlias):
            return NotImplemented
        return (self.__unpacked__ == other.__unpacked__ and self.__origin__ == other.__origin__
                and self.__args__ == other.__args__)

    def __ne__(self, other):
        r = self.__eq__(other)
        return r if r is NotImplemented else not r

    def __hash__(self):
        return hash(self.__origin__) ^ hash(self.__args__)

    def __or__(self, other):
        return _union2(self, other)

    def __ror__(self, other):
        return _union2(other, self)

    def __iter__(self):
        u = GenericAlias(self.__origin__, self.__args__)
        object.__getattribute__(u, "__dict__")["__unpacked__"] = True
        yield u

    def __setattr__(self, name, value):
        if name in _OWN or name in ("__module__", "__name__", "__qualname__"):
            raise AttributeError("readonly attribute")
        raise AttributeError("'types.GenericAlias' object has no attribute '%s'" % name)

    def __reduce__(self):
        if self.__unpacked__:
            return next, (iter(GenericAlias(self.__origin__, self.__args__)),)
        return GenericAlias, (self.__origin__, self.__args__)

    def __dir__(self):
        names = set(dir(self.__origin__))
        names.update(_OWN)
        return sorted(names)


GenericAlias.__module__ = "types"


class _Builder:
    """Union arguments: flattened, None as NoneType, each once (CPython's unionbuilder)."""

    def __init__(self, checked):
        self.checked = checked
        self.args = []
        self.hashable = set()
        self.unhashable = []

    def add(self, arg):
        if arg is None:
            arg = _NoneType
        elif isinstance(arg, Union):
            for a in arg.__args__:
                self.add(a)
            return
        if self.checked:
            arg = _type_check(arg, "Union[arg, ...]: each arg must be a type.")
            if isinstance(arg, Union):
                for a in arg.__args__:
                    self.add(a)
                return
        try:
            hash(arg)
        except TypeError:
            for u in self.unhashable:
                if u == arg:
                    return
            self.unhashable.append(arg)
            self.args.append(arg)
            return
        if arg in self.hashable:
            return
        self.hashable.add(arg)
        self.args.append(arg)

    def make(self):
        if len(self.args) == 1:
            return self.args[0]
        u = object.__new__(Union)
        d = object.__getattribute__(u, "__dict__")
        d["__args__"] = tuple(self.args)
        d["_hashable"] = frozenset(self.hashable)
        d["_unhashable"] = tuple(self.unhashable)
        d["_params"] = None
        return u


def _type_check(arg, msg):
    if _unionable(arg):
        return arg
    import typing
    return typing._type_check(arg, msg)


def _unionable(x):
    return (x is None or isinstance(x, (type, GenericAlias, Union))
            or type(x).__name__ == "TypeAliasType" and type(x).__module__ == "typing")


def _union_of(args, checked):
    b = _Builder(checked)
    for a in args:
        b.add(a)
    return b.make()


class Union:
    """Represent a union type

E.g. for int | str"""

    def __new__(cls, *args, **kwargs):
        raise TypeError("cannot create 'typing.Union' instances")

    def __class_getitem__(cls, params):
        if not isinstance(params, tuple):
            params = (params,)
        if not params:
            raise TypeError("Cannot take a Union of no types.")
        return _union_of(params, True)

    @property
    def __parameters__(self):
        d = object.__getattribute__(self, "__dict__")
        if d["_params"] is None:
            d["_params"] = _collect_parameters(d["__args__"])
        return d["_params"]

    @property
    def __origin__(self):
        return Union

    __name__ = "Union"

    def __repr__(self):
        return " | ".join(_type_repr(a) for a in self.__args__)

    def __getitem__(self, item):
        args = _subs_parameters(self, self.__args__, self.__parameters__, item)
        return _union_of(args, False)

    def __eq__(self, other):
        if not isinstance(other, Union):
            return NotImplemented
        if self._hashable != other._hashable or len(self._unhashable) != len(other._unhashable):
            return False
        return all(any(a == b for b in other._unhashable) for a in self._unhashable)

    def __ne__(self, other):
        r = self.__eq__(other)
        return r if r is NotImplemented else not r

    def __hash__(self):
        n = len(self._unhashable)
        if n:
            raise TypeError("union contains %d unhashable element%s" % (n, "s" if n > 1 else ""))
        return hash(self._hashable)

    def __or__(self, other):
        return _union2(self, other)

    def __ror__(self, other):
        return _union2(other, self)

    def __instancecheck__(self, obj):
        return isinstance(obj, self.__args__)

    def __subclasscheck__(self, cls):
        return issubclass(cls, self.__args__)

    def __mro_entries__(self, bases):
        raise TypeError("Cannot subclass %r" % (self,))

    def __setattr__(self, name, value):
        raise AttributeError("readonly attribute")

    def __reduce__(self):
        import functools
        import operator
        return functools.reduce, (operator.or_, self.__args__)


Union.__module__ = "typing"


def _union2(a, b):
    """a | b of types (the interpreter's | on classes, None and aliases)."""
    if not (_unionable(a) and _unionable(b)):
        return NotImplemented
    return _union_of((a, b), False)
