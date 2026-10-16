# The interpreter runs every package (tests/run_pkg_tests.sh).
import okpkg
from okpkg.shapes import Rect
import mixpkg
import mixpkg.dyn
import unionpkg
import untypedpkg
print(okpkg.area(3, 4), okpkg.greet("plainpy"), Rect(2, 5).scaled(3).area())
print(mixpkg.total([Rect(1, 2), Rect(3, 4)]), mixpkg.square(7), mixpkg.dyn.pick(False))
print(unionpkg.size("abc"), unionpkg.size(5), untypedpkg.add(2, 3), untypedpkg.add("a", "b"))
