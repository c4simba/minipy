# A name a module only imports, taken from it by another module (as CPython
# allows): user.py's `from .config import Item`, where config.py has Item
# from item.py - whatever order the compiler meets the modules in.
from lib.reexport import config
from lib.reexport.user import describe, first
from lib.reexport.config import Item

print(describe(config.make("made")), describe(first()), isinstance(first(), Item))
