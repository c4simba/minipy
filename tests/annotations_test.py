# __annotations__ (evaluated when read, as CPython 3.14 does) and typing's reprs
from typing import ClassVar, Optional, Union, List, Dict, Callable, Any, Literal, Tuple
class N: pass
class Node:
    val: int
    nxt: Optional["Node"] = None
    kids: list[Node]
    k: ClassVar[int] = 3
def f(a: int, b: "Node", *c: str, d: float = 1.0, **e: bool) -> list[int]:
    return []
print(Node.__annotations__)
print(f.__annotations__)
class B(Node): pass
print(B.__annotations__)
print(repr(int | None), repr(int | str), repr(N | None), repr(list[int] | None))
print(Optional[int], Optional["N"], Union[int, str], Union[int, None], Union[int])
print(List[int], Dict[str, int], ClassVar[int], Callable[[int], str], Any, Literal[1, "a"], Tuple[int, ...])
print(Optional[List[int]], list[Optional[int]], dict[str, list[N]])
print((int | None) == Optional[int], type(int | None).__name__, isinstance(3, int | str), isinstance(None, int | None))
