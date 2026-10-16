"""Primitives of typing (CPython's _typing, written in C there): TypeVar,
ParamSpec, TypeVarTuple, TypeAliasType, Generic, Union, NoDefault.

As in CPython (Objects/typevarobject.c), the hard parts are done by typing.py:
substitution (_typevar_subst ...), Generic[...] (_generic_class_getitem) and
Generic subclasses (_generic_init_subclass)."""
import sys
from _ppy_generic import GenericAlias, Union

__all__ = ["_idfunc", "TypeVar", "ParamSpec", "TypeVarTuple", "ParamSpecArgs", "ParamSpecKwargs",
           "TypeAliasType", "Generic", "Union", "NoDefault"]


def _idfunc(_, x):
    return x


def _typing():
    t = sys.modules.get("typing")
    if t is None:
        import typing as t
    return t


def _type_check(arg, msg):
    if arg is None or isinstance(arg, type):
        return arg                                    # (what typing._type_check gives them: no need to import it)
    return _typing()._type_check(arg, msg)


def _caller(depth=2):
    try:
        return sys._getframemodulename(depth) or "__main__"
    except (AttributeError, ValueError):
        return None


def _no_subclass(name):
    def __init_subclass__(cls, *args, **kwargs):
        raise TypeError("type 'typing.%s' is not an acceptable base type" % name)
    return classmethod(__init_subclass__)


class _NoDefaultType:
    """The type of the NoDefault singleton."""
    __module__ = "typing"
    _instance = None

    def __new__(cls, *args, **kwargs):
        if args or kwargs:
            raise TypeError("NoDefaultType takes no arguments")
        return _NoDefaultType._instance

    def __repr__(self):
        return "typing.NoDefault"

    def __reduce__(self):
        return "NoDefault"


_NoDefaultType.__name__ = "NoDefaultType"
_NoDefaultType.__qualname__ = "NoDefaultType"
NoDefault = object.__new__(_NoDefaultType)
_NoDefaultType._instance = NoDefault
_NoDefaultType.__init_subclass__ = _no_subclass("NoDefaultType")


def _or(a, b):
    return Union[a, b]


class _Var:
    """What TypeVar, ParamSpec and TypeVarTuple share."""
    __module__ = "typing"

    def __or__(self, right):
        return Union[self, right]

    def __ror__(self, left):
        return Union[left, self]

    def __eq__(self, other):
        return self is other

    def __hash__(self):
        return object.__hash__(self)

    def __reduce__(self):
        return self.__name__

    def __copy__(self):
        return self

    def __deepcopy__(self, memo):
        return self

    def has_default(self):
        return self._default is not NoDefault

    @property
    def __default__(self):
        d = self._default
        if d is not NoDefault and self._lazy_default:
            d = self._default = d()
            self._lazy_default = False
        return d

    @property
    def evaluate_default(self):
        return None if self._default is NoDefault else _ConstEvaluator(self.__default__)

    def _mro_entries(self, kind):
        raise TypeError("Cannot subclass an instance of " + kind)

    def _variance(self):
        if self.__infer_variance__:
            return ""
        return "+" if self.__covariant__ else "-" if self.__contravariant__ else "~"


class _ConstEvaluator:
    """An evaluate_* function that only has a value (as CPython's constevaluator)."""

    def __init__(self, value):
        self.value = value

    def __call__(self, format):
        if format == 4:
            import annotationlib
            return annotationlib.type_repr(self.value)
        return self.value

    def __repr__(self):
        return "<constevaluator %r>" % (self.value,)


def _variance_check(covariant, contravariant, infer_variance):
    if covariant and contravariant:
        raise ValueError("Bivariant type variables are not supported.")
    if infer_variance and (covariant or contravariant):
        raise ValueError("Variance cannot be specified with infer_variance.")


class TypeVar(_Var):
    """Type variable.

    The preferred way to construct a type variable is via the dedicated
    syntax for generic functions, classes, and type aliases::

        class Sequence[T]:  # T is a TypeVar
            ...

    This syntax can also be used to create bound and constrained type
    variables::

        # S is a TypeVar bound to str
        class StrSequence[S: str]:
            ...

        # A is a TypeVar constrained to str or bytes
        class StrOrBytesSequence[A: (str, bytes)]:
            ...

    Type variables can also have defaults:

        class IntDefault[T = int]:
            ...

    However, if desired, reusable type variables can also be constructed
    manually, like so::

       T = TypeVar('T')  # Can be anything
       S = TypeVar('S', bound=str)  # Can be any subtype of str
       A = TypeVar('A', str, bytes)  # Must be exactly str or bytes
       D = TypeVar('D', default=int)  # Defaults to int if not specified

    Type variables exist primarily for the benefit of static type
    checkers.  They serve as the parameters for generic types as well
    as for generic function and type alias definitions.

    The variance of type variables is inferred by type checkers when they
    are created through the type parameter syntax and when
    ``infer_variance=True`` is passed. Manually created type variables may
    be explicitly marked covariant or contravariant by passing
    ``covariant=True`` or ``contravariant=True``. By default, manually
    created type variables are invariant. See PEP 484 and PEP 695 for more
    details.
    """

    def __init__(self, name, *constraints, bound=None, covariant=False, contravariant=False,
                 infer_variance=False, default=NoDefault):
        if not isinstance(name, str):
            raise TypeError("TypeVar() argument 'name' must be str, not " + type(name).__name__)
        _variance_check(covariant, contravariant, infer_variance)
        if bound is not None:
            bound = _type_check(bound, "Bound must be a type.")
        if len(constraints) == 1:
            raise TypeError("A single constraint is not allowed")
        if constraints and bound is not None:
            raise TypeError("Constraints cannot be combined with bound=...")
        self.__name__ = name
        self._bound = bound
        self._constraints = tuple(constraints)
        self._lazy = False
        self._default = default
        self._lazy_default = False
        self.__covariant__ = bool(covariant)
        self.__contravariant__ = bool(contravariant)
        self.__infer_variance__ = bool(infer_variance)
        self.__module__ = _caller()

    __init_subclass__ = _no_subclass("TypeVar")

    @property
    def __bound__(self):
        if self._lazy and self._bound is not None:
            self._bound = self._bound()
            self._lazy = False
        return self._bound

    @property
    def __constraints__(self):
        if callable(self._constraints):
            self._constraints = tuple(self._constraints())
        return self._constraints

    @property
    def evaluate_bound(self):
        return None if self._bound is None else _ConstEvaluator(self.__bound__)

    @property
    def evaluate_constraints(self):
        return _ConstEvaluator(self.__constraints__) if self.__constraints__ else None

    def __repr__(self):
        return self._variance() + self.__name__

    def __typing_subst__(self, arg):
        return _typing()._typevar_subst(self, arg)

    def __typing_prepare_subst__(self, alias, args):
        params = alias.__parameters__
        i = params.index(self)
        if i < len(args):
            return args
        if i == len(args) and self.has_default():
            return tuple(args) + (self.__default__,)
        raise TypeError("Too few arguments for %r; actual %d, expected at least %d" % (alias, len(args), i + 1))

    def __mro_entries__(self, bases):
        self._mro_entries("TypeVar")


class ParamSpecArgs:
    """The args for a ParamSpec object.

    Given a ParamSpec object P, P.args is an instance of ParamSpecArgs.

    ParamSpecArgs objects have a reference back to their ParamSpec::

        >>> P = ParamSpec("P")
        >>> P.args.__origin__ is P
        True

    This type is meant for runtime introspection and has no special meaning
    to static type checkers.
    """
    __module__ = "typing"

    def __init__(self, origin):
        self.__origin__ = origin

    def __repr__(self):
        o = self.__origin__
        return "%s.args" % (o.__name__ if isinstance(o, ParamSpec) else repr(o),)

    def __eq__(self, other):
        if not isinstance(other, ParamSpecArgs):
            return NotImplemented
        return self.__origin__ == other.__origin__

    def __hash__(self):
        return object.__hash__(self)

    def __mro_entries__(self, bases):
        raise TypeError("Cannot subclass an instance of ParamSpecArgs")


class ParamSpecKwargs:
    """The kwargs for a ParamSpec object.

    Given a ParamSpec object P, P.kwargs is an instance of ParamSpecKwargs.

    ParamSpecKwargs objects have a reference back to their ParamSpec::

        >>> P = ParamSpec("P")
        >>> P.kwargs.__origin__ is P
        True

    This type is meant for runtime introspection and has no special meaning
    to static type checkers.
    """
    __module__ = "typing"

    def __init__(self, origin):
        self.__origin__ = origin

    def __repr__(self):
        o = self.__origin__
        return "%s.kwargs" % (o.__name__ if isinstance(o, ParamSpec) else repr(o),)

    def __eq__(self, other):
        if not isinstance(other, ParamSpecKwargs):
            return NotImplemented
        return self.__origin__ == other.__origin__

    def __hash__(self):
        return object.__hash__(self)

    def __mro_entries__(self, bases):
        raise TypeError("Cannot subclass an instance of ParamSpecKwargs")


class ParamSpec(_Var):
    """Parameter specification variable.

    The preferred way to construct a parameter specification is via the
    dedicated syntax for generic functions, classes, and type aliases,
    where the use of '**' creates a parameter specification::

        type IntFunc[**P] = Callable[P, int]

    The following syntax creates a parameter specification that defaults
    to a callable accepting two positional-only arguments of types int
    and str:

        type IntFuncDefault[**P = (int, str)] = Callable[P, int]

    For compatibility with Python 3.11 and earlier, ParamSpec objects
    can also be created as follows::

        P = ParamSpec('P')
        DefaultP = ParamSpec('DefaultP', default=(int, str))

    Parameter specification variables exist primarily for the benefit of
    static type checkers.  They are used to forward the parameter types of
    one callable to another callable, a pattern commonly found in
    higher-order functions and decorators.  They are only valid when used
    in ``Concatenate``, or as the first argument to ``Callable``, or as
    parameters for user-defined Generics. See class Generic for more
    information on generic types.

    An example for annotating a decorator::

        def add_logging[**P, T](f: Callable[P, T]) -> Callable[P, T]:
            '''A type-safe decorator to add logging to a function.'''
            def inner(*args: P.args, **kwargs: P.kwargs) -> T:
                logging.info(f'{f.__name__} was called')
                return f(*args, **kwargs)
            return inner

        @add_logging
        def add_two(x: float, y: float) -> float:
            '''Add two numbers together.'''
            return x + y

    Parameter specification variables can be introspected. e.g.::

        >>> P = ParamSpec("P")
        >>> P.__name__
        'P'

    Note that only parameter specification variables defined in the global
    scope can be pickled.
    """

    def __init__(self, name, *, bound=None, covariant=False, contravariant=False, infer_variance=False,
                 default=NoDefault):
        if not isinstance(name, str):
            raise TypeError("ParamSpec() argument 'name' must be str, not " + type(name).__name__)
        _variance_check(covariant, contravariant, infer_variance)
        if bound is not None:
            bound = _type_check(bound, "Bound must be a type.")
        self.__name__ = name
        self.__bound__ = bound
        self._default = default
        self._lazy_default = False
        self.__covariant__ = bool(covariant)
        self.__contravariant__ = bool(contravariant)
        self.__infer_variance__ = bool(infer_variance)
        self.__module__ = _caller()

    __init_subclass__ = _no_subclass("ParamSpec")

    @property
    def args(self):
        return ParamSpecArgs(self)

    @property
    def kwargs(self):
        return ParamSpecKwargs(self)

    @property
    def evaluate_bound(self):
        return None if self.__bound__ is None else _ConstEvaluator(self.__bound__)

    def __repr__(self):
        return self._variance() + self.__name__

    def __typing_subst__(self, arg):
        return _typing()._paramspec_subst(self, arg)

    def __typing_prepare_subst__(self, alias, args):
        return _typing()._paramspec_prepare_subst(self, alias, args)

    def __mro_entries__(self, bases):
        self._mro_entries("ParamSpec")


class TypeVarTuple(_Var):
    """Type variable tuple. A specialized form of type variable that enables
    variadic generics.

    The preferred way to construct a type variable tuple is via the
    dedicated syntax for generic functions, classes, and type aliases,
    where a single '*' indicates a type variable tuple::

        def move_first_element_to_last[T, *Ts](tup: tuple[T, *Ts]) -> tuple[*Ts, T]:
            return (*tup[1:], tup[0])

    Type variables tuples can have default values:

        type AliasWithDefault[*Ts = (str, int)] = tuple[*Ts]

    For compatibility with Python 3.11 and earlier, TypeVarTuple objects
    can also be created as follows::

        Ts = TypeVarTuple('Ts')  # Can be given any name
        DefaultTs = TypeVarTuple('Ts', default=(str, int))

    Just as a TypeVar (type variable) is a placeholder for a single type,
    a TypeVarTuple is a placeholder for an *arbitrary* number of types. For
    example, if we define a generic class using a TypeVarTuple::

        class C[*Ts]: ...

    Then we can parameterize that class with an arbitrary number of type
    arguments::

        C[int]       # Fine
        C[int, str]  # Also fine
        C[()]        # Even this is fine

    For more details, see PEP 646.

    Note that only TypeVarTuples defined in the global scope can be
    pickled.
    """
    __infer_variance__ = False
    __covariant__ = False
    __contravariant__ = False

    def __init__(self, name, *, default=NoDefault):
        if not isinstance(name, str):
            raise TypeError("TypeVarTuple() argument 'name' must be str, not " + type(name).__name__)
        self.__name__ = name
        self._default = default
        self._lazy_default = False
        self.__module__ = _caller()

    __init_subclass__ = _no_subclass("TypeVarTuple")

    def __iter__(self):
        yield _typing().Unpack[self]

    def __repr__(self):
        return self.__name__

    def __typing_subst__(self, arg):
        raise TypeError("Substitution of bare TypeVarTuple is not supported")

    def __typing_prepare_subst__(self, alias, args):
        return _typing()._typevartuple_prepare_subst(self, alias, args)

    def __mro_entries__(self, bases):
        self._mro_entries("TypeVarTuple")


def _check_type_params(type_params):
    if not isinstance(type_params, tuple):
        raise TypeError("type_params must be a tuple")
    default_seen = False
    for p in type_params:
        if not isinstance(p, (TypeVar, ParamSpec, TypeVarTuple)):
            raise TypeError("Expected a type param, got %r" % (p,))
        if p.has_default():
            default_seen = True
        elif default_seen:
            raise TypeError("non-default type parameter '%r' follows default type parameter" % (p,))


class TypeAliasType:
    """Type alias.

    Type aliases are created through the type statement::

        type Alias = int

    In this example, Alias and int are not equivalent. They have the same
    behavior when used in annotations, but Alias is not a class.

    The value of a type alias is evaluated lazily: `Alias.__value__` is
    computed from the alias's definition the first time it is asked for.
    """
    __module__ = "typing"

    def __init__(self, name, value, *, type_params=()):
        if not isinstance(name, str):
            raise TypeError("TypeAliasType() argument 'name' must be str, not " + type(name).__name__)
        _check_type_params(type_params)
        self.__name__ = name
        self.__qualname__ = name
        self.__type_params__ = type_params
        self._value = value
        self._lazy = False
        self.__module__ = _caller()

    __init_subclass__ = _no_subclass("TypeAliasType")

    @property
    def __value__(self):
        if self._lazy:
            self._value = self._value(*self.__type_params__)
            self._lazy = False
        return self._value

    @property
    def evaluate_value(self):
        return _ConstEvaluator(self.__value__)

    @property
    def __parameters__(self):
        out = []
        for p in self.__type_params__:
            if isinstance(p, TypeVarTuple):
                out.extend(p)
            else:
                out.append(p)
        return tuple(out)

    def __getitem__(self, params):
        if not self.__type_params__:
            raise TypeError("Only generic type aliases are subscriptable")
        if not isinstance(params, tuple):
            params = (params,)
        return GenericAlias(self, params)

    def __repr__(self):
        return self.__name__

    def __reduce__(self):
        return self.__name__

    def __or__(self, right):
        return Union[self, right]

    def __ror__(self, left):
        return Union[left, self]

    def __mro_entries__(self, bases):
        raise TypeError("Cannot subclass an instance of TypeAliasType")

    def __call__(self, *args, **kwargs):
        raise TypeError("'typing.TypeAliasType' object is not callable")


def _type_alias(name, value, names):
    """type X[T, ...] = value (the interpreter's statement): value is a function
    of the type parameters, called the first time __value__ is asked for."""
    params = tuple(TypeVar(p, infer_variance=True) for p in names)
    a = TypeAliasType(name, value, type_params=params)
    a._lazy = True
    a.__module__ = _caller()
    for p in params:
        p.__module__ = a.__module__
    return a


class Generic:
    """Abstract base class for generic types.

    On Python 3.12 and newer, generic classes implicitly inherit from
    Generic when they declare a parameter list after the class's name::

        class Mapping[KT, VT]:
            def __getitem__(self, key: KT) -> VT:
                ...
            # Etc.

    On older versions of Python, however, generic classes have to
    explicitly inherit from Generic.

    After a class has been declared to be generic, it can then be used as
    follows::

        def lookup_name[KT, VT](mapping: Mapping[KT, VT], key: KT, default: VT) -> VT:
            try:
                return mapping[key]
            except KeyError:
                return default
    """
    __module__ = "typing"

    def __class_getitem__(cls, params):
        """Parameterizes a generic class.

        At least, parameterizing a generic class is the *main* thing this
        method does. For example, for some generic class `Foo`, this is called
        when we do `Foo[int]` - there, with `cls=Foo` and `params=int`.

        However, note that this method is also called when defining generic
        classes in the first place with `class Foo[T]: ...`.
        """
        return _typing()._generic_class_getitem(cls, params)

    def __init_subclass__(cls, *args, **kwargs):
        """Function to initialize subclasses."""
        return _typing()._generic_init_subclass(cls, *args, **kwargs)


for _c in (TypeVar, ParamSpec, TypeVarTuple, ParamSpecArgs, ParamSpecKwargs, TypeAliasType, Generic):
    _c.__module__ = "typing"
del _c
