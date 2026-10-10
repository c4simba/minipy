import os
import sys
import warnings


class AppWarning(UserWarning):
    pass


def show(w: list[warnings.WarningMessage]) -> None:
    for m in w:
        print(m.category.__name__, str(m.message), os.path.basename(m.filename), m.lineno, repr(m.line))
    del w[:]


def old_api() -> None:
    warnings.warn("old_api is deprecated", DeprecationWarning)


with warnings.catch_warnings(record=True) as w:
    warnings.warn("plain")
    warnings.warn("plain")                       # (another line: shown again)
    for i in range(3):
        warnings.warn("in a loop")               # (the same line: once)
    warnings.warn("custom", AppWarning)
    warnings.warn(AppWarning("an instance"))
    warnings.warn("runtime", RuntimeWarning)
    old_api()                                    # (DeprecationWarning outside __main__'s own code: still __main__)
    warnings.warn("pending", PendingDeprecationWarning)
    show(w)

    warnings.simplefilter("always")
    for i in range(2):
        warnings.warn("always shown")
    warnings.simplefilter("ignore", AppWarning)
    warnings.warn("ignored", AppWarning)
    warnings.warn("not ignored", UserWarning)
    show(w)

    warnings.resetwarnings()
    warnings.filterwarnings("ignore", message="noisy")
    warnings.warn("noisy one")
    warnings.warn("Noisy two")
    warnings.warn("quiet one")
    warnings.filterwarnings("once", category=RuntimeWarning)
    warnings.warn("once", RuntimeWarning)
    warnings.warn("once", RuntimeWarning)
    show(w)

    warnings.simplefilter("error", AppWarning)
    try:
        warnings.warn("now an error", AppWarning)
    except AppWarning as e:
        print("raised", type(e).__name__, e)

with warnings.catch_warnings():
    warnings.simplefilter("error")
    try:
        warnings.warn("strict")
    except UserWarning as e:
        print("raised", repr(e))

for bad in ("bogus",):
    try:
        warnings.simplefilter(bad)
    except ValueError as e:
        print("ValueError:", e)
print(warnings.formatwarning("text", UserWarning, "f.py", 3, "  x = 1  "), end="")
print(warnings.formatwarning("text", DeprecationWarning, "f.py", 3, ""), end="")
print(len(warnings.filters) > 0)

sys.stdout.flush()
warnings.warn_explicit("to stderr", UserWarning, "fake.py", 7)
