from typing import Callable
from minipy import Endpoint

routes: list[Endpoint] = []

def get(fn: Endpoint) -> Endpoint:
    routes.append(fn)
    return fn

@get
def total(values: list[int]) -> int:
    return sum(values)
