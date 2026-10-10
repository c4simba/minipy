"""Keywords (from "Grammar/python.gram"): iskeyword(s), issoftkeyword(s)."""

__all__ = ["iskeyword", "issoftkeyword", "kwlist", "softkwlist"]

kwlist = [
    'False', 'None', 'True', 'and', 'as', 'assert', 'async', 'await', 'break', 'class', 'continue', 'def', 'del',
    'elif', 'else', 'except', 'finally', 'for', 'from', 'global', 'if', 'import', 'in', 'is', 'lambda', 'nonlocal',
    'not', 'or', 'pass', 'raise', 'return', 'try', 'while', 'with', 'yield',
]

softkwlist = ['_', 'case', 'match', 'type']

_kw = frozenset(kwlist)
_softkw = frozenset(softkwlist)


def iskeyword(s: str) -> bool:
    """Whether s is a Python keyword."""
    return s in _kw


def issoftkeyword(s: str) -> bool:
    """Whether s is a soft keyword (match, case, type, _)."""
    return s in _softkw
