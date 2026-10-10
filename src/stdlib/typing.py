"""Support for type hints (the interpreter's typing; compiled programs: the compiler's own)."""
class _Special:
    def __init__(self, name):
        self._name = name
    def __getitem__(self, params):
        if self._name == 'Optional':
            return _union(params, None)
        return _SpecialAlias(self, params)
    def __call__(self, *a, **k):
        raise TypeError('Cannot instantiate typing.' + self._name)
    def __repr__(self):
        return 'typing.' + self._name
    def __or__(self, other):
        return _union(self, other)
    def __ror__(self, other):
        return _union(other, self)
class ForwardRef:
    def __init__(self, arg):
        self.__forward_arg__ = arg
    def __repr__(self):
        return 'ForwardRef(' + repr(self.__forward_arg__) + ')'
    def __eq__(self, other):
        return isinstance(other, ForwardRef) and other.__forward_arg__ == self.__forward_arg__
    def __hash__(self):
        return hash(self.__forward_arg__)
    def __or__(self, other):
        return _union(self, other)
    def __ror__(self, other):
        return _union(other, self)
def _ref(a):
    if isinstance(a, str):
        return ForwardRef(a)
    if a is None:
        return type(None)
    return a
class Union:
    def __init__(self, args):
        self.__args__ = args
    def __class_getitem__(cls, params):
        if isinstance(params, tuple):
            return _union(*params)
        return _union(params)
    def __repr__(self):
        return ' | '.join([_type_repr(a) for a in self.__args__])
    def __eq__(self, other):
        return isinstance(other, Union) and set(self.__args__) == set(other.__args__)
    def __hash__(self):
        return hash(frozenset(self.__args__))
    def __or__(self, other):
        return _union(self, other)
    def __ror__(self, other):
        return _union(other, self)
    def __instancecheck__(self, obj):
        return isinstance(obj, self.__args__)
    def __subclasscheck__(self, cls):
        return issubclass(cls, self.__args__)
def _union(*params):
    args = []
    for p in params:
        p = _ref(p)
        for a in (p.__args__ if isinstance(p, Union) else (p,)):
            if a not in args:
                args.append(a)
    if len(args) == 1:
        return args[0]
    return Union(tuple(args))
def _union2(a, b):
    return _union(a, b)
class _SpecialAlias:
    def __init__(self, origin, params):
        self.__origin__ = origin
        ps = params if isinstance(params, tuple) else (params,)
        self.__args__ = ps if origin._name in ('Literal', 'Annotated') else tuple([_ref(p) if not isinstance(p, list) else [_ref(x) for x in p] for p in ps])
    def __repr__(self):
        return repr(self.__origin__) + '[' + ', '.join([_type_repr(a) for a in self.__args__]) + ']'
    def __getitem__(self, params):
        return self
    def __call__(self, *a, **k):
        raise TypeError('Cannot instantiate ' + repr(self))
    def __eq__(self, other):
        return isinstance(other, _SpecialAlias) and self.__origin__ is other.__origin__ and self.__args__ == other.__args__
    def __hash__(self):
        return hash((self.__origin__._name, self.__args__))
    def __or__(self, other):
        return _union(self, other)
    def __ror__(self, other):
        return _union(other, self)
for _n in ('Any', 'Optional', 'List', 'Dict', 'Set', 'FrozenSet', 'Tuple', 'Callable', 'Iterable', 'Iterator',
           'Generator', 'Sequence', 'MutableSequence', 'Mapping', 'MutableMapping', 'AbstractSet', 'MutableSet', 'Type',
           'ClassVar', 'Final', 'Literal', 'Awaitable', 'Coroutine', 'AsyncIterator', 'AsyncIterable', 'AsyncGenerator',
           'NoReturn', 'Never', 'Self', 'Annotated', 'TypeAlias', 'Deque', 'DefaultDict', 'Counter', 'OrderedDict',
           'Collection', 'Container', 'Hashable', 'Sized', 'Reversible', 'SupportsInt', 'SupportsFloat', 'IO', 'TextIO',
           'BinaryIO', 'Pattern', 'Match', 'Required', 'NotRequired', 'LiteralString', 'TypeGuard', 'Unpack', 'Concatenate'):
    globals()[_n] = _Special(_n)
TYPE_CHECKING = False
def cast(typ, val):
    return val
def overload(f):
    return f
def final(f):
    return f
def override(f):
    return f
def no_type_check(f):
    return f
def runtime_checkable(c):
    return c
def get_type_hints(obj, globalns=None, localns=None, include_extras=False, *, format=None):
    """The annotations of obj (a function, class or module), string ones evaluated."""
    import sys
    import annotationlib
    fmt = annotationlib.Format.VALUE if format is None else format
    if isinstance(obj, type):
        hints = {}
        for base in reversed(obj.__mro__):
            ann = base.__dict__.get('__annotations__', {})
            if not ann:
                continue
            g = globalns if globalns is not None else getattr(sys.modules.get(base.__module__, None), '__dict__', {})
            for name, value in ann.items():
                if value is None:
                    value = type(None)
                if isinstance(value, str):
                    try:
                        value = sys._eval_annotation(value, g, dict(base.__dict__) if localns is None else localns)
                    except NameError:
                        if fmt != annotationlib.Format.FORWARDREF:
                            raise
                        value = ForwardRef(value)
                hints[name] = value
        return hints if include_extras else {k: _strip_annotations(v) for k, v in hints.items()}
    hints = annotationlib.get_annotations(obj, format=fmt)
    g = globalns if globalns is not None else getattr(obj, '__globals__', None)
    if g is None:
        g = getattr(sys.modules.get(getattr(obj, '__module__', None), None), '__dict__', {})
    out = {}
    for name, value in hints.items():
        if value is None:
            value = type(None)
        if isinstance(value, str):
            try:
                value = sys._eval_annotation(value, g, localns)
            except NameError:
                if fmt != annotationlib.Format.FORWARDREF:
                    raise
                value = ForwardRef(value)
        out[name] = value
    return out if include_extras else {k: _strip_annotations(v) for k, v in out.items()}
def _strip_annotations(t):
    if isinstance(t, _SpecialAlias) and t.__origin__._name == 'Annotated':
        return t.__args__[0]
    return t
def get_origin(tp):
    if isinstance(tp, (GenericAlias, _SpecialAlias)):
        return tp.__origin__
    if isinstance(tp, Union):
        return Union
    return None
def get_args(tp):
    if isinstance(tp, (GenericAlias, _SpecialAlias, Union)):
        return tp.__args__
    return ()
class TypeVar(_Special):
    def __init__(self, name, *constraints, bound=None, covariant=False, contravariant=False, infer_variance=False, default=None):
        _Special.__init__(self, name)
        self.__name__ = name
        self._prefix = '' if infer_variance else '+' if covariant else '-' if contravariant else '~'
    def __repr__(self):
        return self._prefix + self._name
class ParamSpec(TypeVar):
    pass
class TypeVarTuple(TypeVar):
    pass
def NewType(name, tp):
    def new_type(x):
        return x
    new_type.__name__ = name
    return new_type
def _type_repr(t):
    if isinstance(t, list):
        return '[' + ', '.join([_type_repr(a) for a in t]) + ']'
    if t is type(None) or t is None:
        return 'None'
    if isinstance(t, type):
        if t.__module__ == 'builtins':
            return t.__qualname__
        return t.__module__ + '.' + t.__qualname__
    if t is ...:
        return '...'
    return repr(t)
class GenericAlias:
    def __init__(self, origin, args):
        self.__origin__ = origin
        self.__args__ = args if isinstance(args, tuple) else (args,)
    def __repr__(self):
        return _type_repr(self.__origin__) + '[' + ', '.join([_type_repr(a) for a in self.__args__]) + ']'
    def __call__(self, *a, **k):
        return self.__origin__(*a, **k)
    def __getattr__(self, name):
        return getattr(self.__origin__, name)
    def __eq__(self, other):
        return isinstance(other, GenericAlias) and self.__origin__ is other.__origin__ and self.__args__ == other.__args__
    def __hash__(self):
        return hash((self.__origin__, self.__args__))
    def __getitem__(self, params):
        return self
    def __or__(self, other):
        return _union(self, other)
    def __ror__(self, other):
        return _union(other, self)
class TypeAliasType:
    def __init__(self, name, value, type_params=()):
        self.__name__ = name
        self.__qualname__ = name
        self._value = value
        self.__type_params__ = tuple(TypeVar(p, infer_variance=True) for p in type_params)
    @property
    def __value__(self):
        return self._value(*self.__type_params__)
    def __getitem__(self, params):
        return self
    def __repr__(self):
        return self.__name__
    def __or__(self, other):
        return _union(self, other)
    def __ror__(self, other):
        return _union(other, self)
class Generic:
    def __class_getitem__(cls, item):
        return cls
class Protocol(Generic):
    pass
AnyStr = TypeVar('AnyStr')
