"""Helpers for introspecting and wrapping annotations (CPython 3.14's annotationlib).

plainpy keeps the source text of every annotation (as `from __future__ import
annotations` would) and evaluates it when asked: the FORWARDREF and STRING
formats come from those texts, not from running __annotate__ with fake globals."""
import builtins
import enum
import keyword
import sys

__all__ = [
    "Format",
    "ForwardRef",
    "call_annotate_function",
    "call_evaluate_function",
    "get_annotate_from_class_namespace",
    "get_annotations",
    "annotations_to_string",
    "type_repr",
]


class Format(enum.IntEnum):
    VALUE = 1
    VALUE_WITH_FAKE_GLOBALS = 2
    FORWARDREF = 3
    STRING = 4


_sentinel = object()
_NAME_ERROR_MSG = "name '{name:.200}' is not defined"
_ModuleType = type(sys)
_FunctionType = type(lambda: 0)
_BuiltinFunctionType = type(len)

# Slots of ForwardRef: the __forward__ names are kept for compatibility with the
# old typing.ForwardRef class (code looks for them in __slots__).
_SLOTS = (
    "__forward_is_argument__",
    "__forward_is_class__",
    "__forward_module__",
    "__weakref__",
    "__arg__",
    "__globals__",
    "__extra_names__",
    "__code__",
    "__ast_node__",
    "__cell__",
    "__owner__",
    "__stringifier_dict__",
    "__resolved_str_cache__",
)


class ForwardRef:
    """Wrapper that holds a forward reference.

    Constructor arguments:
    * arg: a string representing the code to be evaluated.
    * module: the module where the forward reference was created.
      Must be a string, not a module object.
    * owner: The owning object (module, class, or function).
    * is_argument: Does nothing, retained for compatibility.
    * is_class: True if the forward reference was created in class scope.

    """

    __slots__ = _SLOTS

    def __init__(self, arg, *, module=None, owner=None, is_argument=True, is_class=False):
        if not isinstance(arg, str):
            raise TypeError(f"Forward reference must be a string -- got {arg!r}")
        self.__arg__ = arg
        self.__forward_is_argument__ = is_argument
        self.__forward_is_class__ = is_class
        self.__forward_module__ = module
        self.__owner__ = owner
        self.__globals__ = None
        self.__cell__ = None
        self.__extra_names__ = None
        self.__code__ = None
        self.__ast_node__ = None
        self.__resolved_str_cache__ = None

    def __init_subclass__(cls, /, *args, **kwds):
        raise TypeError("Cannot subclass ForwardRef")

    def evaluate(self, *, globals=None, locals=None, type_params=None, owner=None, format=Format.VALUE):
        """Evaluate the forward reference and return the value.

        If the forward reference cannot be evaluated, raise an exception.
        """
        if format == Format.STRING:
            return self.__resolved_str__
        if format == Format.VALUE:
            is_forwardref_format = False
        elif format == Format.FORWARDREF:
            is_forwardref_format = True
        else:
            raise NotImplementedError(format)
        if owner is None:
            owner = self.__owner__
        if globals is None and self.__forward_module__ is not None:
            globals = getattr(sys.modules.get(self.__forward_module__, None), "__dict__", None)
        if globals is None:
            globals = self.__globals__
        if globals is None:
            if isinstance(owner, type):
                module_name = getattr(owner, "__module__", None)
                if module_name:
                    module = sys.modules.get(module_name, None)
                    if module:
                        globals = getattr(module, "__dict__", None)
            elif isinstance(owner, _ModuleType):
                globals = getattr(owner, "__dict__", None)
            elif callable(owner):
                globals = getattr(owner, "__globals__", None)
        if globals is None:
            globals = {}
        if type_params is None and owner is not None:
            type_params = getattr(owner, "__type_params__", None)
        if locals is None:
            locals = {}
            if isinstance(owner, type):
                locals.update(vars(owner))
        elif type_params is not None or self.__extra_names__:
            locals = dict(locals)
        if type_params is not None:
            for param in type_params:
                locals.setdefault(param.__name__, param)
        if self.__extra_names__:
            locals.update(self.__extra_names__)

        arg = self.__forward_arg__
        if arg.isidentifier() and not keyword.iskeyword(arg):
            if arg in locals:
                return locals[arg]
            elif arg in globals:
                return globals[arg]
            elif hasattr(builtins, arg):
                return getattr(builtins, arg)
            elif is_forwardref_format:
                return self
            else:
                raise NameError(_NAME_ERROR_MSG.format(name=arg), name=arg)
        code = self.__forward_code__
        try:
            return eval(code, globals, locals)
        except Exception:
            if not is_forwardref_format:
                raise
        return self

    def _evaluate(self, globalns, localns, type_params=_sentinel, *, recursive_guard):
        import typing
        if type_params is _sentinel:
            type_params = ()
        return typing.evaluate_forward_ref(self, globals=globalns, locals=localns, type_params=type_params,
                                           _recursive_guard=recursive_guard)

    @property
    def __forward_arg__(self):
        if self.__arg__ is not None:
            return self.__arg__
        raise AssertionError("Attempted to access '__forward_arg__' on an uninitialized ForwardRef")

    @property
    def __resolved_str__(self):
        if self.__resolved_str_cache__ is None:
            self.__resolved_str_cache__ = self.__forward_arg__
        return self.__resolved_str_cache__

    @property
    def __forward_code__(self):
        if self.__code__ is not None:
            return self.__code__
        arg = self.__forward_arg__
        try:
            self.__code__ = compile(_rewrite_star_unpack(arg), "<string>", "eval")
        except SyntaxError:
            raise SyntaxError(f"Forward reference must be an expression -- got {arg!r}")
        return self.__code__

    def __eq__(self, other):
        if not isinstance(other, ForwardRef):
            return NotImplemented
        return (
            self.__forward_arg__ == other.__forward_arg__
            and self.__forward_module__ == other.__forward_module__
            and self.__globals__ is other.__globals__
            and self.__forward_is_class__ == other.__forward_is_class__
            and self.__cell__ is other.__cell__
            and self.__owner__ == other.__owner__
            and ((tuple(sorted(self.__extra_names__.items())) if self.__extra_names__ else None) ==
                 (tuple(sorted(other.__extra_names__.items())) if other.__extra_names__ else None))
        )

    def __hash__(self):
        return hash((
            self.__forward_arg__,
            self.__forward_module__,
            id(self.__globals__),
            self.__forward_is_class__,
            id(self.__cell__),
            self.__owner__,
            tuple(sorted(self.__extra_names__.items())) if self.__extra_names__ else None,
        ))

    def __or__(self, other):
        from _ppy_generic import Union
        return Union[self, other]

    def __ror__(self, other):
        from _ppy_generic import Union
        return Union[other, self]

    def __repr__(self):
        extra = []
        if self.__forward_module__ is not None:
            extra.append(f", module={self.__forward_module__!r}")
        if self.__forward_is_class__:
            extra.append(", is_class=True")
        if self.__owner__ is not None:
            extra.append(f", owner={self.__owner__!r}")
        return f"ForwardRef({self.__resolved_str__!r}{''.join(extra)})"


def _texts(obj):
    """name -> source text of obj's own annotations (strings: texts, other values: values), or None."""
    if isinstance(obj, type):
        d = obj.__dict__.get("__annotations__", None)
        return d if isinstance(d, dict) else None
    if isinstance(obj, _ModuleType):
        d = obj.__dict__.get("__annotations__", None)
        return d if isinstance(d, dict) else None
    try:
        return obj.__ppy_annotation_texts__
    except AttributeError:
        return None


def _scope(obj):
    """(globals, locals, module name, is_class) to evaluate obj's annotation texts in."""
    if isinstance(obj, type):
        module = getattr(obj, "__module__", None)
        g = getattr(sys.modules.get(module, None), "__dict__", None)
        return (g if g is not None else {}), dict(vars(obj)), module, True
    if isinstance(obj, _ModuleType):
        return obj.__dict__, None, obj.__name__, False
    g = getattr(obj, "__globals__", None)
    return (g if g is not None else {}), None, getattr(obj, "__module__", None), False


def _from_texts(obj, texts, format):
    """The annotations of obj from their texts: FORWARDREF (a ForwardRef for what cannot
    be evaluated) or STRING."""
    if format == Format.STRING:
        return {n: t if isinstance(t, str) else type_repr(t) for n, t in texts.items()}
    g, l, module, is_class = _scope(obj)
    out = {}
    for name, text in texts.items():
        if not isinstance(text, str):
            out[name] = text
            continue
        try:
            out[name] = eval(compile(_rewrite_star_unpack(text), "<annotation>", "eval"), g, l)
        except Exception:
            out[name] = ForwardRef(text, module=module, owner=obj, is_class=is_class)
    return out


def call_annotate_function(annotate, format, *, owner=None, _is_evaluate=False):
    """Call an __annotate__ function. __annotate__ functions are normally
    generated by the compiler to defer the evaluation of annotations. They
    can be called with any of the format arguments in the Format enum, but
    compiler-generated __annotate__ functions only support the VALUE format.
    This function provides additional functionality to call __annotate__
    functions with the FORWARDREF and STRING formats.

    *annotate* must be an __annotate__ function, which takes a single argument
    and returns a dict of annotations.

    *format* must be a member of the Format enum or one of the corresponding
    integer values.

    *owner* can be the object that owns the annotations (i.e., the module,
    class, or function that the __annotate__ function derives from). With the
    FORWARDREF format, it is used to provide better evaluation capabilities
    on the generated ForwardRef objects.

    """
    if format == Format.VALUE_WITH_FAKE_GLOBALS:
        raise ValueError("The VALUE_WITH_FAKE_GLOBALS format is for internal use only")
    try:
        return annotate(format)
    except NotImplementedError:
        pass
    func = getattr(annotate, "__self__", None)          # (a function's own __annotate__: its texts)
    texts = _texts(func) if func is not None else None
    if texts is not None:
        return _from_texts(func, texts, format)
    if format == Format.STRING:
        return annotations_to_string(annotate(Format.VALUE))
    if format == Format.FORWARDREF:
        return annotate(Format.VALUE)
    raise ValueError(f"Invalid format: {format!r}")


def call_evaluate_function(evaluate, format, *, owner=None):
    """Call an evaluate function. Evaluate functions are normally generated for
    the value of type aliases and the bounds, constraints, and defaults of
    type parameter objects.
    """
    return call_annotate_function(evaluate, format, owner=owner, _is_evaluate=True)


def get_annotate_from_class_namespace(obj):
    """Retrieve the annotate function from a class namespace dictionary.

    Return None if the namespace does not contain an annotate function.
    This is useful in metaclass ``__new__`` methods to retrieve the annotate function.
    """
    try:
        return obj["__annotate__"]
    except KeyError:
        return obj.get("__annotate_func__", None)


def get_annotations(obj, *, globals=None, locals=None, eval_str=False, format=Format.VALUE):
    """Compute the annotations dict for an object.

    obj may be a callable, class, module, or other object with
    __annotate__ or __annotations__ attributes.
    Passing any other object raises TypeError.

    The *format* parameter controls the format in which annotations are returned,
    and must be a member of the Format enum or its integer equivalent.
    For the VALUE format, the __annotations__ is tried first; if it
    does not exist, the __annotate__ function is called. The
    FORWARDREF format uses __annotations__ if it exists and can be
    evaluated, and otherwise falls back to calling the __annotate__ function.
    The STRING format tries __annotate__ first, and falls back to
    using __annotations__, stringified using annotations_to_string().

    If eval_str is true, values of type str are replaced with the result of
    calling eval() on them (in obj's globals and locals, or the ones given).
    """
    if eval_str and format != Format.VALUE:
        raise ValueError("eval_str=True is only supported with format=Format.VALUE")
    if format == Format.VALUE:
        ann = _get_dunder_annotations(obj)
        if ann is None:
            ann = _get_and_call_annotate(obj, format)
    elif format == Format.FORWARDREF:
        try:
            ann = _get_dunder_annotations(obj)
        except Exception:
            pass
        else:
            if ann is not None:
                return dict(ann)
        texts = _texts(obj)
        if texts is not None:
            return _from_texts(obj, texts, format)
        ann = _get_and_call_annotate(obj, format)
        if ann is None:
            ann = _get_dunder_annotations(obj)
    elif format == Format.STRING:
        texts = _texts(obj)
        if texts is not None:
            return _from_texts(obj, texts, format)
        ann = _get_and_call_annotate(obj, format)
        if ann is not None:
            return dict(ann)
        ann = _get_dunder_annotations(obj)
        if ann is not None:
            return annotations_to_string(ann)
    elif format == Format.VALUE_WITH_FAKE_GLOBALS:
        raise ValueError("The VALUE_WITH_FAKE_GLOBALS format is for internal use only")
    else:
        raise ValueError(f"Unsupported format {format!r}")

    if ann is None:
        if isinstance(obj, type) or callable(obj):
            return {}
        raise TypeError(f"{obj!r} does not have annotations")
    if not ann:
        return {}
    if not eval_str:
        return dict(ann)

    if globals is None or locals is None:
        if isinstance(obj, type):
            obj_globals = None
            module_name = getattr(obj, "__module__", None)
            if module_name:
                module = sys.modules.get(module_name, None)
                if module:
                    obj_globals = getattr(module, "__dict__", None)
            obj_locals = dict(vars(obj))
            unwrap = obj
        elif isinstance(obj, _ModuleType):
            obj_globals = getattr(obj, "__dict__")
            obj_locals = None
            unwrap = None
        elif callable(obj):
            obj_globals = getattr(obj, "__globals__", None)
            obj_locals = None
            unwrap = obj
        else:
            obj_globals = obj_locals = unwrap = None
        if unwrap is not None:
            seen = {id(unwrap)}
            while True:
                if hasattr(unwrap, "__wrapped__"):
                    candidate = unwrap.__wrapped__
                    if id(candidate) in seen:
                        break
                    seen.add(id(candidate))
                    unwrap = candidate
                    continue
                functools = sys.modules.get("functools")
                if functools and isinstance(unwrap, functools.partial):
                    candidate = unwrap.func
                    if id(candidate) in seen:
                        break
                    seen.add(id(candidate))
                    unwrap = candidate
                    continue
                break
            if hasattr(unwrap, "__globals__"):
                obj_globals = unwrap.__globals__
        if globals is None:
            globals = obj_globals
        if locals is None:
            locals = obj_locals
    type_params = getattr(obj, "__type_params__", ())
    if type_params:
        locals = {param.__name__: param for param in type_params} | (locals or {})
    return {key: value if not isinstance(value, str) else eval(_rewrite_star_unpack(value), globals, locals)
            for key, value in ann.items()}


def type_repr(value):
    """Convert a Python value to a format suitable for use with the STRING format.

    This is intended as a helper for tools that support the STRING format but do
    not have access to the code that originally produced the annotations. It uses
    repr() for most objects.

    """
    if isinstance(value, (type, _FunctionType, _BuiltinFunctionType)):
        if value.__module__ == "builtins":
            return value.__qualname__
        return f"{value.__module__}.{value.__qualname__}"
    if value is ...:
        return "..."
    return repr(value)


def annotations_to_string(annotations):
    """Convert an annotation dict containing values to approximately the STRING format.

    Always returns a fresh a dictionary.
    """
    return {n: t if isinstance(t, str) else type_repr(t) for n, t in annotations.items()}


def _rewrite_star_unpack(arg):
    """If the given argument annotation expression is a star unpack e.g. `'*Ts'`
       rewrite it to a valid expression.
       """
    if arg.lstrip().startswith("*"):
        return f"({arg},)[0]"
    return arg


def _get_and_call_annotate(obj, format):
    """Get the __annotate__ function and call it."""
    annotate = getattr(obj, "__annotate__", None)
    if annotate is not None:
        ann = call_annotate_function(annotate, format, owner=obj)
        if not isinstance(ann, dict):
            raise ValueError(f"{obj!r}.__annotate__ returned a non-dict")
        return ann
    return None


def _get_dunder_annotations(obj):
    """Return the annotations for an object, checking that it is a dictionary."""
    ann = getattr(obj, "__annotations__", None)
    if ann is None:
        return None
    if not isinstance(ann, dict):
        raise ValueError(f"{obj!r}.__annotations__ is neither a dict nor None")
    return ann
