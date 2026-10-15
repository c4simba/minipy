# try / except / else / finally, exception classes, runtime errors as exceptions
class AppError(Exception):
    pass

class NotFound(AppError):
    def __init__(self, what: str):
        super().__init__("not found: " + what)
        self.what = what

def lookup(d: dict[str, int], key: str) -> int:
    if key not in d:
        raise NotFound(key)
    return d[key]

def safe_div(a: int, b: int) -> int:
    try:
        return a // b
    except ZeroDivisionError:
        print("division by zero, using 0")
        return 0
    finally:
        print("safe_div done")

data = {"a": 1}
try:
    print(lookup(data, "a"))
    print(lookup(data, "zz"))
    print("not reached")
except NotFound as e:
    print("caught", e, e.what, isinstance(e, AppError))

print(safe_div(7, 2), safe_div(1, 0))

xs = [1, 2, 3]
try:
    print(xs[10])
except IndexError as e:
    print("IndexError:", e)

try:
    print(data["missing"])
except KeyError:
    print("KeyError caught")
except Exception:
    print("not this one")

try:
    v = int("abc")
except ValueError as e:
    print("ValueError:", e)
else:
    print("no error", v)

try:
    v = int("42")
except ValueError:
    print("bad")
else:
    print("parsed", v)
finally:
    print("finally runs")

def risky(n: int) -> str:
    try:
        if n == 1:
            raise ValueError("one")
        if n == 2:
            raise RuntimeError("two")
        return "ok"
    except (ValueError, RuntimeError) as e:
        return "handled " + str(e)

print(risky(0), risky(1), risky(2))

def reraise():
    try:
        raise KeyError("inner")
    except KeyError:
        print("cleaning up")
        raise

try:
    reraise()
except KeyError as e:
    print("outer got", e)

count = 0
for i in range(5):
    try:
        if i == 3:
            break
        if i % 2:
            continue
        count += 10
    finally:
        count += 1
print("count", count)

class Node:
    def __init__(self):
        self.next: Node = None

try:
    n = Node()
    print(n.next.next)
except AttributeError:
    print("None has no attribute")

try:
    assert 1 > 2, "math is broken"
except AssertionError as e:
    print("assert:", e)

def nested() -> int:
    total = 0
    for i in range(3):
        try:
            try:
                if i == 1:
                    raise ValueError("v")
                total += 1
            finally:
                total += 100
        except ValueError:
            total += 1000
    return total

print(nested())
try:
    raise AppError
except Exception as e:
    print("bare class", repr(e), str(e) == "")
print("end")
raise NotFound("final")
