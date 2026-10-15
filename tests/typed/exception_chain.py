# exception chaining (raise ... from, implicit __context__) and e.args
class AppError(Exception):
    def __init__(self, code: int) -> None:
        super().__init__("code " + str(code))
        self.code = code


def parse(s: str) -> int:
    try:
        return int(s)
    except ValueError as e:
        raise AppError(len(s)) from e


try:
    parse("abc")
except AppError as err:
    cause = err.__cause__
    print(err, err.code, err.args, type(cause).__name__, cause, err.__suppress_context__)
try:
    try:
        1 // 0
    except ZeroDivisionError:
        raise KeyError("k")
except KeyError as k:
    print(repr(k), k.args, type(k.__context__).__name__, k.__cause__ is None, k.__suppress_context__)
try:
    raise ValueError("plain")
except ValueError as v:
    print(v.args, v.__context__ is None, v.__cause__ is None)
try:
    try:
        raise IndexError("first")
    except IndexError:
        raise RuntimeError("second") from None
except RuntimeError as r:
    print(r, r.__cause__ is None, type(r.__context__).__name__, r.__suppress_context__)
print(ValueError().args, KeyError("").args)
