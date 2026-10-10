"""types.GenericAlias (list[int]) and typing.Union (int | str): the interpreter's, as CPython's C ones."""

_NoneType = type(None)
_FunctionType = type(lambda: 0)


def _type_repr(obj):
    """An argument as GenericAlias / Union reprs show it."""
    if obj is ...:
        return "..."
    if obj is _NoneType:
        return "None"
    if isinstance(obj, list):
        return "[" + ", ".join(_type_repr(a) for a in obj) + "]"
    if isinstance(obj, GenericAlias):
        return repr(obj)
    if hasattr(obj, "__origin__") and hasattr(obj, "__args__"):
        return repr(obj)
    if isinstance(obj, type):
        if obj.__module__ == "builtins":
            return obj.__qualname__
        return obj.__module__ + "." + obj.__qualname__
    if isinstance(obj, _FunctionType):
        return obj.__qualname__ if obj.__module__ == "builtins" else obj.__module__ + "." + obj.__qualname__
    return repr(obj)


def _is_typevar_like(x):
    return not isinstance(x, type) and hasattr(x, "__typing_subst__")


def _collect_parameters(args):
    params = []
    for t in args:
        if isinstance(t, type):
            continue
        if _is_typevar_like(t):
            if t not in params:
                params.append(t)
        elif isinstance(t, (list, tuple)):
            for p in _collect_parameters(t):
                if p not in params:
                    params.append(p)
        else:
            for p in getattr(t, "__parameters__", ()) or ():
                if p not in params:
                    params.append(p)
    return tuple(params)


def _subst(params, args, item):
    """args with the parameters replaced by the items (typing's substitution protocol)."""
    if not isinstance(item, tuple):
        item = (item,)
    for p in params:
        prep = getattr(p, "__typing_prepare_subst__", None)
        if prep is not None:
            item = prep(_Holder(params), item)
    if len(item) != len(params):
        raise TypeError("Too " + ("many" if len(item) > len(params) else "few") + " arguments for " +
                        repr(_Holder(params)) + "; actual " + str(len(item)) + ", expected " + str(len(params)))
    subst = dict(zip(params, item))
    out = []
    for a in args:
        if isinstance(a, type):
            out.append(a)
            continue
        if _is_typevar_like(a):
            v = a.__typing_subst__(subst[a])
            if getattr(a, "__typing_is_unpacked_typevartuple__", False) and isinstance(v, tuple):
                out.extend(v)
            else:
                out.append(v)
            continue
        sub = getattr(a, "__parameters__", ()) or ()
        if sub:
            out.append(a[tuple(subst[p] for p in sub)])
        elif isinstance(a, list):
            out.append([subst.get(x, x) if _is_typevar_like(x) else x for x in a])
        else:
            out.append(a)
    return tuple(out)


class _Holder:
    def __init__(self, params):
        self.__parameters__ = params

    def __repr__(self):
        return "generic"


class GenericAlias:
    """A parameterized generic: list[int], dict[str, T]."""

    def __init__(self, origin, args, /):
        if not isinstance(args, tuple):
            args = (args,)
        object.__setattr__(self, "__origin__", origin)
        object.__setattr__(self, "__args__", args)
        object.__setattr__(self, "_params", None)
        object.__setattr__(self, "__unpacked__", False)

    @property
    def __parameters__(self):
        if self._params is None:
            object.__setattr__(self, "_params", _collect_parameters(self.__args__))
        return self._params

    @property
    def __typing_unpacked_tuple_args__(self):
        if self.__unpacked__ and self.__origin__ is tuple:
            return self.__args__
        return None

    def __repr__(self):
        if len(self.__args__) == 0:
            args = "()"
        else:
            args = ", ".join(_type_repr(a) for a in self.__args__)
        r = _type_repr(self.__origin__) + "[" + args + "]"
        return "*" + r if self.__unpacked__ else r

    def __getitem__(self, item):
        params = self.__parameters__
        if not params:
            raise TypeError(repr(self) + " is not a generic class")
        return GenericAlias(self.__origin__, _subst(params, self.__args__, item))

    def __call__(self, *args, **kwargs):
        result = self.__origin__(*args, **kwargs)
        try:
            result.__orig_class__ = self
        except Exception:
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
        return (self.__origin__ == other.__origin__ and self.__args__ == other.__args__
                and self.__unpacked__ == other.__unpacked__)

    def __hash__(self):
        return hash(self.__origin__) ^ hash(self.__args__)

    def __or__(self, other):
        return _union2(self, other)

    def __ror__(self, other):
        return _union2(other, self)

    def __iter__(self):
        u = GenericAlias(self.__origin__, self.__args__)
        object.__setattr__(u, "__unpacked__", True)
        yield u

    def __getattr__(self, name):
        if name.startswith("__") and name.endswith("__") and name not in ("__name__", "__qualname__", "__module__", "__doc__"):
            raise AttributeError(name)
        return getattr(self.__origin__, name)

    def __setattr__(self, name, value):
        raise AttributeError("readonly attribute")

    def __reduce__(self):
        return GenericAlias, (self.__origin__, self.__args__)

    def __dir__(self):
        names = set(dir(self.__origin__))
        names.update(("__origin__", "__args__", "__parameters__", "__unpacked__"))
        return sorted(names)


def _union_args(params):
    args = []
    for p in params:
        if p is None:
            p = _NoneType
        for a in (p.__args__ if isinstance(p, Union) else (p,)):
            if a not in args:
                args.append(a)
    return args


def _unionable(x):
    return (x is None or isinstance(x, (type, GenericAlias, Union)) or _is_typevar_like(x)
            or type(x).__name__ in ("TypeAliasType", "_GenericAlias", "_SpecialGenericAlias", "_AnnotatedAlias",
                                     "_LiteralGenericAlias", "_CallableGenericAlias", "_UnpackGenericAlias",
                                     "_ConcatenateGenericAlias", "ForwardRef", "_SpecialForm", "_TypedCacheSpecialForm",
                                     "NewType", "_ProtocolMeta", "ParamSpecArgs", "ParamSpecKwargs")
            or isinstance(x, str))


class Union:
    """Union type: int | str, typing.Union[int, str]."""

    def __new__(cls, *args, **kwargs):
        raise TypeError("cannot create 'typing.Union' instances")

    @classmethod
    def _make(cls, args):
        args = _union_args(args)
        if len(args) == 1:
            return args[0]
        u = object.__new__(cls)
        object.__setattr__(u, "__args__", tuple(args))
        object.__setattr__(u, "_params", None)
        return u

    def __class_getitem__(cls, params):
        if not isinstance(params, tuple):
            params = (params,)
        if not params:
            raise TypeError("Cannot take a Union of no types.")
        return cls._make(params)

    @property
    def __parameters__(self):
        if self._params is None:
            object.__setattr__(self, "_params", _collect_parameters(self.__args__))
        return self._params

    @property
    def __origin__(self):
        return Union

    __name__ = "Union"
    __qualname__ = "Union"

    def __repr__(self):
        return " | ".join(_type_repr(a) for a in self.__args__)

    def __getitem__(self, item):
        params = self.__parameters__
        if not params:
            raise TypeError(repr(self) + " is not a generic class")
        return Union._make(_subst(params, self.__args__, item))

    def __eq__(self, other):
        if not isinstance(other, Union):
            return NotImplemented
        try:
            return set(self.__args__) == set(other.__args__)
        except TypeError:
            return self.__args__ == other.__args__

    def __hash__(self):
        try:
            return hash(frozenset(self.__args__))
        except TypeError:
            return hash(self.__args__)

    def __or__(self, other):
        return _union2(self, other)

    def __ror__(self, other):
        return _union2(other, self)

    def __instancecheck__(self, obj):
        for a in self.__args__:
            if isinstance(a, GenericAlias) or hasattr(a, "__origin__") and not isinstance(a, type):
                raise TypeError("isinstance() argument 2 cannot be a parameterized generic")
        return isinstance(obj, tuple(self.__args__))

    def __subclasscheck__(self, cls):
        for a in self.__args__:
            if isinstance(a, GenericAlias):
                raise TypeError("issubclass() argument 2 cannot be a parameterized generic")
        return issubclass(cls, tuple(self.__args__))

    def __mro_entries__(self, bases):
        raise TypeError("Cannot subclass typing.Union")

    def __setattr__(self, name, value):
        raise AttributeError("readonly attribute")

    def __reduce__(self):
        import functools
        import operator
        return functools.reduce, (operator.or_, self.__args__)


Union.__module__ = "typing"


def _union2(a, b):
    """a | b of types (the interpreter's | on classes, None and aliases)."""
    if not (_unionable(a) and _unionable(b)) or isinstance(a, str) and isinstance(b, str):
        return NotImplemented
    return Union._make((a, b))
