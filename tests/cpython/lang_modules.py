# imports: packages, relative imports, star imports, module attributes
import pkg
from pkg import helper, VALUE
from pkg.sub import deep
import pkg.sub.deep as dd
from pkg.helper import *
import collections, collections.abc, os.path as osp, json
print(pkg.NAME, helper.twice(4), VALUE, deep.where(), dd is deep, public_one(), "hidden" in dir())
print(collections.OrderedDict(a=1), isinstance([], collections.abc.Sequence), osp.basename("/a/b.c"), json.dumps({"k": [1, 2]}))
print(__name__, pkg.__name__, deep.__name__, deep.__package__, pkg.helper.__file__.endswith("helper.py"))
import collections as C
print(C.Counter("abracadabra").most_common(2), collections.namedtuple("P", "x y")(1, 2), collections.namedtuple("P", "x")(1).__module__)
