# CPython-like imports: dotted, alias, from-dotted, star, __name__, ModuleNotFoundError

import pkg.mod
print(pkg.mod.val)

import pkg.mod as pm
print(pm.val)

from pkg.mod import val
print(val)

# each module sees its own __name__
print(pkg.mod.name_here)

from utils import *
print(answer)
print(twice(5))

# entry script __name__
print(__name__)

try:
    import nonexistent_module
except ModuleNotFoundError as e:
    print("MNFE caught")

# ModuleNotFoundError is a subclass of ImportError
try:
    import another_missing
except ImportError:
    print("ImportError caught")
