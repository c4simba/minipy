# Compiled: only modules that passed the check (okpkg, okpkg.shapes, mixpkg).
import okpkg
from okpkg.shapes import Rect
import mixpkg
print(okpkg.area(3, 4), okpkg.greet("minipy"), Rect(2, 5).scaled(3).area())
print(mixpkg.total([Rect(1, 2), Rect(3, 4)]), mixpkg.square(7))
