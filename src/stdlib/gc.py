"""gc in compiled programs (the interpreter's is native): objects are reference counted and
freed at once, so there is nothing to collect; these keep CPython's API."""

DEBUG_STATS = 1
DEBUG_COLLECTABLE = 2
DEBUG_UNCOLLECTABLE = 4
DEBUG_SAVEALL = 32
DEBUG_LEAK = 38

garbage: list[int] = []
_enabled = [True]


def enable() -> None:
    _enabled[0] = True


def disable() -> None:
    _enabled[0] = False


def isenabled() -> bool:
    return _enabled[0]


def collect(generation: int = 2) -> int:
    """The number of unreachable objects found (none: reference counting freed them)."""
    return 0


def get_count() -> tuple[int, int, int]:
    return 0, 0, 0


def get_threshold() -> tuple[int, int, int]:
    return 2000, 10, 0


def set_threshold(threshold0: int, threshold1: int = 10, threshold2: int = 0) -> None:
    pass


def get_debug() -> int:
    return 0


def set_debug(flags: int) -> None:
    pass


def freeze() -> None:
    pass


def unfreeze() -> None:
    pass


def get_freeze_count() -> int:
    return 0
