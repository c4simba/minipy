# operator functions, itemgetter/attrgetter/methodcaller keys
import operator
from operator import itemgetter, add, mul
from functools import reduce

print(operator.add(2, 3), operator.add("a", "b"), operator.sub(5.5, 2), operator.mul([1], 3), operator.truediv(7, 2), operator.floordiv(7, 2), operator.mod(-7, 3))
print(operator.neg(4), operator.pos(-4), operator.abs(-2.5), operator.inv(5), operator.pow(2, 10), operator.lshift(1, 4), operator.rshift(256, 3))
print(operator.and_(12, 10), operator.or_(12, 10), operator.xor(12, 10), operator.not_(0), operator.truth("x"), operator.is_(None, None), operator.is_not(1, None))
print(operator.lt(1, 2), operator.le(2, 2), operator.eq("a", "a"), operator.ne(1, 2), operator.ge(3, 4), operator.gt(5, 4))
print(operator.contains([1, 2, 3], 2), operator.countOf([1, 2, 1], 1), operator.indexOf("abc", "c"), operator.getitem([10, 20], 1), operator.concat([1], [2]))
d = {"a": 1}
operator.setitem(d, "b", 2)
operator.delitem(d, "a")
print(d, operator.length_hint([1, 2, 3]), operator.index(7))
xs = [1, 2]
print(operator.iadd(xs, [3]), xs, operator.imul(3, 4))
pairs = [("b", 2), ("a", 3), ("c", 1)]
print(sorted(pairs, key=itemgetter(1)), sorted(pairs, key=itemgetter(0)), list(map(itemgetter(0), pairs)))
print(reduce(add, [1, 2, 3, 4]), reduce(mul, [1, 2, 3, 4]))
try:
    operator.indexOf([1], 5)
except ValueError as e:
    print("ValueError", e)
from operator import attrgetter, methodcaller


class P:
    def __init__(self, name: str, age: int):
        self.name = name
        self.age = age


ps = [P("bob", 30), P("al", 25), P("cy", 35)]
print([p.name for p in sorted(ps, key=attrgetter("age"))], [p.name for p in sorted(ps, key=operator.attrgetter("name"))], max(ps, key=attrgetter("age")).name)
print(sorted(["B", "a", "C"], key=methodcaller("lower")), sorted(pairs, key=itemgetter(1, 0)), min(pairs, key=operator.itemgetter(1)))
