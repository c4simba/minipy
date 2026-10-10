"""Helpers for introspecting and wrapping annotations (CPython 3.14's annotationlib)."""
import enum
import sys

__all__ = ["Format", "ForwardRef", "call_annotate_function", "call_evaluate_function", "get_annotate_from_class_namespace",
           "get_annotations", "annotations_to_string", "type_repr"]


class Format(enum.IntEnum):
    VALUE = 1
    VALUE_WITH_FAKE_GLOBALS = 2
    FORWARDREF = 3
    STRING = 4


from typing import ForwardRef


def _texts(obj):
    """(name -> source text) of the annotations of a function, or None."""
    try:
        return obj.__mpy_annotation_texts__
    except AttributeError:
        return None


def call_annotate_function(annotate, format, *, owner=None):
    """annotate(format), FORWARDREF and STRING done here when annotate only knows VALUE."""
    format = Format(format)
    if format == Format.VALUE or format == Format.VALUE_WITH_FAKE_GLOBALS:
        return annotate(format)
    try:
        return annotate(format)
    except NotImplementedError:
        pass
    func = getattr(annotate, "__self__", None)
    texts = _texts(func) if func is not None else None
    if texts is None:
        if format == Format.FORWARDREF:
            return annotate(Format.VALUE)
        return annotations_to_string(annotate(Format.VALUE))
    if format == Format.STRING:
        return dict(texts)
    out = {}
    glob = getattr(func, "__globals__", {})
    for name, text in texts.items():
        try:
            out[name] = sys._eval_annotation(text, glob)
        except Exception:
            out[name] = ForwardRef(text)
    return out


def call_evaluate_function(evaluate, format, *, owner=None):
    return call_annotate_function(evaluate, format, owner=owner)


def get_annotate_from_class_namespace(obj):
    try:
        return obj["__annotate__"]
    except KeyError:
        return obj.get("__annotate_func__", None)


def get_annotations(obj, *, globals=None, locals=None, eval_str=False, format=Format.VALUE):
    """The annotations of a function, class or module, in the given format."""
    format = Format(format)
    if eval_str and format != Format.VALUE:
        raise ValueError("eval_str=True is only supported with format=Format.VALUE")
    annotate = getattr(obj, "__annotate__", None)
    if callable(annotate) and not isinstance(obj, type):
        ann = call_annotate_function(annotate, format, owner=obj)
    else:
        ann = getattr(obj, "__annotations__", None)
        if ann is None:
            if isinstance(obj, type) or callable(obj) or isinstance(obj, type(sys)):
                return {}
            raise TypeError(f"{obj!r} does not have annotations")
        ann = dict(ann)
        if format == Format.STRING:
            ann = {k: v if isinstance(v, str) else type_repr(v) for k, v in ann.items()}
    if not ann:
        return {}
    if eval_str:
        g = globals if globals is not None else getattr(obj, "__globals__", None)
        if g is None:
            mod = sys.modules.get(getattr(obj, "__module__", None))
            g = getattr(mod, "__dict__", {})
        ann = {k: (sys._eval_annotation(v, g, locals) if isinstance(v, str) else v) for k, v in ann.items()}
    return dict(ann)


def type_repr(value):
    """value as it appears in an annotation."""
    if isinstance(value, type):
        if value.__module__ == "builtins":
            return value.__qualname__
        return f"{value.__module__}.{value.__qualname__}"
    if value is ...:
        return "..."
    if isinstance(value, type(type_repr)):
        return value.__qualname__ if value.__module__ == "builtins" else f"{value.__module__}.{value.__qualname__}"
    return repr(value)


def annotations_to_string(annotations):
    return {n: t if isinstance(t, str) else type_repr(t) for n, t in annotations.items()}
