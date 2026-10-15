# modules: import, import as, from-import, packages; each module body runs once
import lib.geom
import lib.geom as g
from lib.util import origin, shout
from lib.shapes import square, KINDS
import lib.shapes.square as sq

print("main starts")
p = g.Point(3.0, 4.0)
print(p.dist2(origin()), lib.geom.circle_area(1.0), g.circle_area(2.0), g.calls)
print(shout("hi"), square.area(4), sq.area(5), KINDS)
print(g.PI, lib.geom.PI == g.PI)
print(__name__)
