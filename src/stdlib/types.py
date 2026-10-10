"""Names for the interpreter's types (CPython's types): FunctionType, ModuleType, ...,
SimpleNamespace, MappingProxyType, DynamicClassAttribute, new_class and friends.

Compiled programs: SimpleNamespace (its fields: the keyword names the program's SimpleNamespace(...)
calls give - no attributes added later) and MappingProxyType; the other names are the interpreter's."""
import sys

if not sys._compiled:
    import sys


    def _f():
        pass


    FunctionType = type(_f)
    LambdaType = type(lambda: None)


    def _g():
        yield 1


    GeneratorType = type(_g())


    async def _c():
        pass


    _co = _c()
    CoroutineType = type(_co)
    _co.close()


    async def _ag():
        yield


    AsyncGeneratorType = type(_ag())


    class _C:
        def _m(self):
            pass


    MethodType = type(_C()._m)
    BuiltinFunctionType = type(len)
    BuiltinMethodType = type([].append)
    ModuleType = type(sys)
    NoneType = type(None)
    NotImplementedType = type(NotImplemented)
    EllipsisType = type(Ellipsis)
    CodeType = type(_f.__code__)
    CellType = type(None)
    GenericAlias = type(list[int])
    UnionType = type(int | str)
    TracebackType = type(None)
    FrameType = type(sys._getframe())
    WrapperDescriptorType = type(object.__init__)
    MethodWrapperType = type(object().__str__)
    MethodDescriptorType = type(str.join)
    ClassMethodDescriptorType = type(dict.fromkeys)
    GetSetDescriptorType = type(None)
    MemberDescriptorType = type(None)


    class SimpleNamespace:
        """An object whose attributes are the keyword arguments: namespace(a=1, b='x')."""

        def __init__(self, mapping_or_iterable=(), /, **kwargs):
            if isinstance(mapping_or_iterable, dict):
                items = list(mapping_or_iterable.items())
            else:
                items = list(mapping_or_iterable)
            for k, v in items:
                setattr(self, k, v)
            for k, v in kwargs.items():
                setattr(self, k, v)

        def __repr__(self):
            name = "namespace" if type(self) is SimpleNamespace else type(self).__name__
            return name + "(" + ", ".join(k + "=" + repr(v) for k, v in self.__dict__.items()) + ")"

        def __eq__(self, other):
            if isinstance(self, SimpleNamespace) and isinstance(other, SimpleNamespace):
                return self.__dict__ == other.__dict__
            return NotImplemented

        def __replace__(self, /, **changes):
            result = type(self)(**self.__dict__)
            result.__dict__.update(changes)
            return result


    class mappingproxy:
        """A read-only view of a mapping: mappingproxy({...})."""

        def __init__(self, mapping):
            if not hasattr(mapping, "keys") or not hasattr(mapping, "__getitem__"):
                raise TypeError("mappingproxy() argument must be a mapping, not " + type(mapping).__name__)
            self._mapping = mapping

        def __getitem__(self, key):
            return self._mapping[key]

        def __iter__(self):
            return iter(self._mapping)

        def __len__(self):
            return len(self._mapping)

        def __contains__(self, key):
            return key in self._mapping

        def __eq__(self, other):
            return self._mapping == other

        def __repr__(self):
            return "mappingproxy(" + repr(self._mapping) + ")"

        def __str__(self):
            return str(self._mapping)

        def __or__(self, other):
            return dict(self._mapping) | dict(other)

        def __ror__(self, other):
            return dict(other) | dict(self._mapping)

        def __reversed__(self):
            return reversed(list(self._mapping))

        def get(self, key, default=None):
            return self._mapping.get(key, default)

        def keys(self):
            return self._mapping.keys()

        def values(self):
            return self._mapping.values()

        def items(self):
            return self._mapping.items()

        def copy(self):
            return self._mapping.copy()


    MappingProxyType = mappingproxy


    class DynamicClassAttribute:
        """A property for instances; through the class, an AttributeError (so the class's
    __getattr__ answers): Enum's name and value use it."""

        def __init__(self, fget=None, fset=None, fdel=None, doc=None):
            self.fget = fget
            self.fset = fset
            self.fdel = fdel
            self.__doc__ = doc or getattr(fget, "__doc__", None)
            self.overwrite_doc = doc is None
            self.__isabstractmethod__ = bool(getattr(fget, "__isabstractmethod__", False))

        def __get__(self, instance, ownerclass=None):
            if instance is None:
                if self.__isabstractmethod__:
                    return self
                raise AttributeError()
            elif self.fget is None:
                raise AttributeError("unreadable attribute")
            return self.fget(instance)

        def __set__(self, instance, value):
            if self.fset is None:
                raise AttributeError("can't set attribute")
            self.fset(instance, value)

        def __delete__(self, instance):
            if self.fdel is None:
                raise AttributeError("can't delete attribute")
            self.fdel(instance)

        def getter(self, fget):
            fdoc = fget.__doc__ if self.overwrite_doc else None
            result = type(self)(fget, self.fset, self.fdel, fdoc or self.__doc__)
            result.overwrite_doc = self.overwrite_doc
            return result

        def setter(self, fset):
            result = type(self)(self.fget, fset, self.fdel, self.__doc__)
            result.overwrite_doc = self.overwrite_doc
            return result

        def deleter(self, fdel):
            result = type(self)(self.fget, self.fset, fdel, self.__doc__)
            result.overwrite_doc = self.overwrite_doc
            return result


    def resolve_bases(bases):
        """bases with __mro_entries__ replaced (PEP 560)."""
        new_bases = list(bases)
        updated = False
        shift = 0
        for i, base in enumerate(bases):
            if isinstance(base, type):
                continue
            if not hasattr(base, "__mro_entries__"):
                continue
            new_base = base.__mro_entries__(bases)
            updated = True
            if not isinstance(new_base, tuple):
                raise TypeError("__mro_entries__ must return a tuple")
            new_bases[i + shift:i + shift + 1] = new_base
            shift += len(new_base) - 1
        if not updated:
            return bases
        return tuple(new_bases)


    def _calculate_meta(meta, bases):
        winner = meta
        for base in bases:
            base_meta = type(base)
            if issubclass(winner, base_meta):
                continue
            if issubclass(base_meta, winner):
                winner = base_meta
                continue
            raise TypeError("metaclass conflict: the metaclass of a derived class must be a (non-strict) subclass "
                            "of the metaclasses of all its bases")
        return winner


    def prepare_class(name, bases=(), kwds=None):
        """(metaclass, namespace from its __prepare__, the other keywords) for a class statement."""
        kwds = {} if kwds is None else dict(kwds)
        if "metaclass" in kwds:
            meta = kwds.pop("metaclass")
        else:
            meta = type(bases[0]) if bases else type
        if isinstance(meta, type):
            meta = _calculate_meta(meta, bases)
        if hasattr(meta, "__prepare__"):
            ns = meta.__prepare__(name, bases, **kwds)
        else:
            ns = {}
        return meta, ns, kwds


    def new_class(name, bases=(), kwds=None, exec_body=None):
        """A class made as a class statement would make it (exec_body fills the namespace)."""
        resolved_bases = resolve_bases(bases)
        meta, ns, kwds = prepare_class(name, resolved_bases, kwds)
        if exec_body is not None:
            exec_body(ns)
        if resolved_bases is not bases:
            ns["__orig_bases__"] = bases
        return meta(name, resolved_bases, ns, **kwds)


    def get_original_bases(cls, /):
        try:
            return cls.__dict__.get("__orig_bases__", cls.__bases__)
        except AttributeError:
            raise TypeError("Expected an instance of type, not " + repr(type(cls).__name__)) from None


    def coroutine(func):
        """(A generator function as a coroutine: the interpreter's generators await as they are.)"""
        if not callable(func):
            raise TypeError("types.coroutine() expects a callable")
        return func


    del _f, _g, _c, _co, _ag, _C


if sys._compiled:
    from typing import Generic, TypeVar

    _K = TypeVar("_K")
    _V = TypeVar("_V")

    class mappingproxy(Generic[_K, _V]):
        """A read-only view of a mapping: mappingproxy({...})."""

        def __init__(self, mapping: dict[_K, _V]) -> None:
            self._mapping = mapping

        def __getitem__(self, key: _K) -> _V:
            return self._mapping[key]

        def __setitem__(self, key: _K, value: _V) -> None:
            raise TypeError("'mappingproxy' object does not support item assignment")

        def __iter__(self):
            for k in self._mapping:
                yield k

        def __len__(self) -> int:
            return len(self._mapping)

        def __contains__(self, key: _K) -> bool:
            return key in self._mapping

        def __repr__(self) -> str:
            return "mappingproxy(" + repr(self._mapping) + ")"

        def __str__(self) -> str:
            return str(self._mapping)

        def get(self, key: _K, default: _V | None = None) -> _V | None:
            return self._mapping.get(key, default)

        def keys(self):
            return self._mapping.keys()

        def values(self):
            return self._mapping.values()

        def items(self):
            return self._mapping.items()

        def copy(self) -> dict[_K, _V]:
            return self._mapping.copy()

    MappingProxyType = mappingproxy
