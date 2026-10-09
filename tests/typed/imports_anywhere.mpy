# imports inside functions and blocks: the module's body runs when the import does
print("start")


def use() -> int:
    import lib.lazymod
    return lib.lazymod.twice(lib.lazymod.VALUE)


def use2() -> int:
    from lib.lazymod import twice
    return twice(5)


print("before the first call")
print(use(), use2(), use())
import math
print(math.sqrt(16.0))
if math.pi > 3:
    import json
    print(json.dumps({"a": [1, 2]}))
