# Group 8: typed except, multiple clauses, else, finally, raise, standard types
# typed match by specific type
try:
    x = [1, 2][10]
except IndexError:
    print("index error")

# multiple except clauses, first non-matching
try:
    y = 1 / 0
except IndexError:
    print("wrong")
except ZeroDivisionError:
    print("zero division")

# except as, custom message
try:
    raise ValueError("bad value")
except ValueError as e:
    print(e)

# Exception catches everything
try:
    d = {}
    v = d["missing"]
except Exception:
    print("caught via Exception")

# KeyError / NameError / TypeError
try:
    {}["k"]
except KeyError:
    print("key error")
try:
    undefined_name
except NameError:
    print("name error")
try:
    z = 1 + "s"
except TypeError:
    print("type error")

# else runs only without exception
try:
    ok = 10
except ValueError:
    print("no")
else:
    print("else after success")

# else skipped on exception
try:
    raise RuntimeError("r")
except RuntimeError:
    print("handled")
else:
    print("no else")

# finally always runs (normal + handled)
def f1():
    try:
        return "a"
    finally:
        print("finally A")
print(f1())

try:
    raise KeyError("k")
except KeyError:
    print("caught k")
finally:
    print("finally B")

# nested try: inner does not match, outer catches
try:
    try:
        raise ValueError("deep")
    except KeyError:
        print("no")
except ValueError:
    print("outer caught")

# raise ... from ... (cause is accepted, ignored)
try:
    raise ValueError("v") from KeyError("cause")
except ValueError as e:
    print(e)

# custom raise re-raise via bare except
try:
    try:
        raise IndexError("x")
    except:
        print("bare except")
        raise
except IndexError:
    print("reraised to outer")

# exception object is usable
try:
    raise ValueError("payload here")
except ValueError as e:
    print(str(e))
    print(len(str(e)))
