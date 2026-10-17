# Libraries other than the C library's are loaded when one of their functions is first called
# (codegen/program.c, runtime rt_clazy): a program runs without a library it does not reach.
# x86run's fake C library has libplainpytest.so.1 (only under the emulator: the .requests file).
import ctypes

lib = ctypes.CDLL("libplainpytest.so.1")
lib.ppytest_add.argtypes = [ctypes.c_int, ctypes.c_int]
lib.ppytest_greeting.restype = ctypes.c_char_p
nope = ctypes.CDLL("libnope.so.9")


def never() -> None:
    nope.not_called()


print("started: no library loaded yet")
print(lib.ppytest_add(2, 40), lib.ppytest_add(1, 1))
print(lib.ppytest_greeting())
nope.nothing_here(1)
print("not reached")
