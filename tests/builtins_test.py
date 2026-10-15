# Group 5: builtin functions
# str / repr / int / float / bool
print(str(42))
print(str(3.5))
print(str([1, 2, 3]))
print(repr("hi"))
print(repr([1, "a"]))
print(int(3.9))
print(int("100"))
print(int(True))
print(float(2))
print(float("1.5"))
print(bool(0))
print(bool(7))
print(bool([]))

# abs / min / max / sum / round
print(abs(-9))
print(abs(-2.5))
print(min(5, 2, 8))
print(max([3, 9, 1]))
print(min("banana", "apple"))
print(sum([1, 2, 3, 4]))
print(sum([1, 2, 3], 100))
print(round(3.14159, 2))
print(round(2.5))
print(round(3.5))
print(round(1.23456, 3))

# sorted / reversed
print(sorted([3, 1, 2]))
print(sorted(["pear", "apple", "kiwi"]))
print(list(reversed([1, 2, 3])))

# enumerate / zip / map / filter
print(list(enumerate(["a", "b", "c"])))
print(list(enumerate(["x", "y"], 1)))
print(list(zip([1, 2, 3], [4, 5, 6])))
print(list(map(abs, [-1, 2, -3])))
print(list(filter(None, [0, 1, 0, 2, 0, 3])))

# list / tuple / set / dict constructors
print(list(range(4)))
print(tuple([1, 2, 3]))
print(sorted(set([3, 1, 2, 1, 3])))
print(dict([("a", 1), ("b", 2)]))

# isinstance
print(isinstance(5, int))
print(isinstance("x", str))
print(isinstance(5, str))
print(isinstance(5, (str, int)))
print(isinstance([1], list))

# ord / chr / any / all
print(ord("A"))
print(chr(66))
print(any([0, 0, 1]))
print(any([0, 0, 0]))
print(all([1, 1, 1]))
print(all([1, 0, 1]))

# composed
print(sum([x * x for x in range(5)]))
print(sorted([len(w) for w in ["hi", "hello", "hey"]]))
print(max(map(int, ["3", "7", "1"])))
