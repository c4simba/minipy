# graphlib: TopologicalSorter (static_order, the get_ready / done protocol, cycles); colorsys conversions
import colorsys
from graphlib import CycleError, TopologicalSorter

graph = {"D": {"B", "C"}, "C": {"A"}, "B": {"A"}}
ts = TopologicalSorter(graph)
order = list(ts.static_order())
print(order[0], sorted(order[1:3]), order[3])

ts2: TopologicalSorter[int] = TopologicalSorter()
ts2.add(3, 2, 1)
ts2.add(2, 1)
ts2.add(5)
ts2.prepare()
rounds: list[list[int]] = []
while ts2.is_active():
    ready = ts2.get_ready()
    rounds.append(sorted(ready))
    ts2.done(*ready)
print(rounds, bool(ts2))
try:
    ts2.done(1)
except ValueError as e:
    print("ValueError:", e)

cyc: TopologicalSorter[str] = TopologicalSorter()
cyc.add("a", "b")
cyc.add("b", "c")
cyc.add("c", "a")
cyc.add("d")
try:
    cyc.prepare()
except CycleError as e:
    print("CycleError:", e.args[0], len(e.args[1]), e.args[1][0] == e.args[1][-1])
print(cyc.get_ready())
late: TopologicalSorter[str] = TopologicalSorter()
late.prepare()
try:
    late.add("x")
except ValueError as e:
    print("ValueError:", e)

for rgb in ((0.2, 0.4, 0.4), (1.0, 0.0, 0.0), (0.5, 0.5, 0.5)):
    h, l, s = colorsys.rgb_to_hls(*rgb)
    print([round(x, 4) for x in colorsys.rgb_to_hsv(*rgb)], [round(x, 4) for x in (h, l, s)],
          [round(x, 4) for x in colorsys.hls_to_rgb(h, l, s)], [round(x, 4) for x in colorsys.rgb_to_yiq(*rgb)])
print([round(x, 4) for x in colorsys.hsv_to_rgb(0.25, 0.5, 0.8)], [round(x, 4) for x in colorsys.yiq_to_rgb(0.5, 0.1, -0.1)])
print(colorsys.rgb_to_hsv(255, 0, 0), colorsys.hsv_to_rgb(0.0, 0.0, 0.3))
