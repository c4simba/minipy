# types: SimpleNamespace (fields from the keyword arguments), MappingProxyType (a read-only view)
import types
from types import SimpleNamespace

cfg = SimpleNamespace(host="localhost", port=8080)
print(cfg, cfg.host, cfg.port)
cfg.port = 9090
print(cfg.port, repr(cfg))
other = types.SimpleNamespace(debug=True)
print(other, other.debug)
mp = types.MappingProxyType({"a": 1, "b": 2})
print(mp["a"], len(mp), "b" in mp, list(mp), mp.get("z"), sorted(mp.items()), mp)
try:
    mp["c"] = 3
except TypeError as e:
    print("TypeError")
