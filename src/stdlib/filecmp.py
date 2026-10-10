"""Utilities for comparing files and directories (CPython's filecmp).

Classes:
    dircmp

Functions:
    cmp(f1, f2, shallow=True) -> int
    cmpfiles(a, b, common) -> ([], [], [])
    clear_cache()

(dircmp works out its lists when it is made; its subdirs when first asked for.)"""
import os
import stat

__all__ = ['clear_cache', 'cmp', 'dircmp', 'cmpfiles', 'DEFAULT_IGNORES']

_cache: dict[tuple[str, str, tuple[int, int, float], tuple[int, int, float]], bool] = {}
BUFSIZE = 8 * 1024

DEFAULT_IGNORES = ['RCS', 'CVS', 'tags', '.git', '.hg', '.bzr', '_darcs', '__pycache__']


def clear_cache() -> None:
    """Clear the filecmp cache."""
    _cache.clear()


def cmp(f1: str, f2: str, shallow: bool = True) -> bool:
    """Compare two files: True if they are the same (shallow: their stat signatures - type, size, mtime -
    equal is enough; else their contents)."""
    s1 = _sig(os.stat(f1))
    s2 = _sig(os.stat(f2))
    if s1[0] != stat.S_IFREG or s2[0] != stat.S_IFREG:
        return False
    if shallow and s1 == s2:
        return True
    if s1[1] != s2[1]:
        return False

    key = (f1, f2, s1, s2)
    outcome = _cache.get(key)
    if outcome is None:
        outcome = _do_cmp(f1, f2)
        if len(_cache) > 100:      # limit the maximum size of the cache
            clear_cache()
        _cache[key] = outcome
    return outcome


def _sig(st: os.stat_result) -> tuple[int, int, float]:
    return (stat.S_IFMT(st.st_mode), st.st_size, st.st_mtime)


def _do_cmp(f1: str, f2: str) -> bool:
    bufsize = BUFSIZE
    with open(f1, 'rb') as fp1:
        with open(f2, 'rb') as fp2:
            while True:
                b1 = fp1.read(bufsize)
                b2 = fp2.read(bufsize)
                if b1 != b2:
                    return False
                if not b1:
                    return True


def _filter(flist: list[str], skip: list[str]) -> list[str]:
    return [x for x in flist if x not in skip]


class dircmp:
    """A class that manages the comparison of 2 directories: dircmp(a, b, ignore=None, hide=None, *,
    shallow=True); report(), report_partial_closure(), report_full_closure(); left_list, right_list,
    common, left_only, right_only, common_dirs, common_files, common_funny, same_files, diff_files,
    funny_files, subdirs."""

    def __init__(self, a: str, b: str, ignore: list[str] | None = None, hide: list[str] | None = None, *,
                 shallow: bool = True) -> None:
        self.left = a
        self.right = b
        self.hide = hide if hide is not None else [os.curdir, os.pardir]  # Names never to be shown
        self.ignore = ignore if ignore is not None else DEFAULT_IGNORES
        self.shallow = shallow
        self._subdirs: dict[str, dircmp] | None = None
        self.phase0()
        self.phase1()
        self.phase2()
        self.phase3()

    def phase0(self) -> None:  # Compare everything except common subdirectories
        self.left_list = _filter(os.listdir(self.left), self.hide + self.ignore)
        self.right_list = _filter(os.listdir(self.right), self.hide + self.ignore)
        self.left_list.sort()
        self.right_list.sort()

    def phase1(self) -> None:  # Compute common names
        a = dict(zip([os.path.normcase(x) for x in self.left_list], self.left_list))
        b = dict(zip([os.path.normcase(x) for x in self.right_list], self.right_list))
        self.common = [a[k] for k in a if k in b]
        self.left_only = [a[k] for k in a if k not in b]
        self.right_only = [b[k] for k in b if k not in a]

    def phase2(self) -> None:  # Distinguish files, directories, funnies
        self.common_dirs: list[str] = []
        self.common_files: list[str] = []
        self.common_funny: list[str] = []

        for x in self.common:
            a_path = os.path.join(self.left, x)
            b_path = os.path.join(self.right, x)

            try:
                a_type = stat.S_IFMT(os.stat(a_path).st_mode)
                b_type = stat.S_IFMT(os.stat(b_path).st_mode)
            except (OSError, ValueError):
                self.common_funny.append(x)
                continue
            if a_type != b_type:
                self.common_funny.append(x)
            elif stat.S_ISDIR(a_type):
                self.common_dirs.append(x)
            elif stat.S_ISREG(a_type):
                self.common_files.append(x)
            else:
                self.common_funny.append(x)

    def phase3(self) -> None:  # Find out differences between common files
        xx = cmpfiles(self.left, self.right, self.common_files, self.shallow)
        self.same_files, self.diff_files, self.funny_files = xx

    def phase4(self) -> None:  # Find out differences between common subdirectories
        subdirs: dict[str, dircmp] = {}
        for x in self.common_dirs:
            a_x = os.path.join(self.left, x)
            b_x = os.path.join(self.right, x)
            subdirs[x] = dircmp(a_x, b_x, self.ignore, self.hide, shallow=self.shallow)
        self._subdirs = subdirs

    def phase4_closure(self) -> None:  # Recursively call phase4() on subdirectories
        self.phase4()
        for sd in self.subdirs.values():
            sd.phase4_closure()

    @property
    def subdirs(self) -> dict[str, "dircmp"]:
        s = self._subdirs
        if s is None:
            self.phase4()
            s = self._subdirs
        return s if s is not None else {}

    def report(self) -> None:  # Print a report on the differences between a and b
        # Output format is purposely lousy
        print('diff', self.left, self.right)
        if self.left_only:
            self.left_only.sort()
            print('Only in', self.left, ':', self.left_only)
        if self.right_only:
            self.right_only.sort()
            print('Only in', self.right, ':', self.right_only)
        if self.same_files:
            self.same_files.sort()
            print('Identical files :', self.same_files)
        if self.diff_files:
            self.diff_files.sort()
            print('Differing files :', self.diff_files)
        if self.funny_files:
            self.funny_files.sort()
            print('Trouble with common files :', self.funny_files)
        if self.common_dirs:
            self.common_dirs.sort()
            print('Common subdirectories :', self.common_dirs)
        if self.common_funny:
            self.common_funny.sort()
            print('Common funny cases :', self.common_funny)

    def report_partial_closure(self) -> None:  # Print reports on self and on subdirs
        self.report()
        for sd in self.subdirs.values():
            print()
            sd.report()

    def report_full_closure(self) -> None:  # Report on self and subdirs recursively
        self.report()
        for sd in self.subdirs.values():
            print()
            sd.report_full_closure()


def cmpfiles(a: str, b: str, common: list[str], shallow: bool = True) -> tuple[list[str], list[str], list[str]]:
    """Compare common files in two directories: (equal files, different files, not regular files)."""
    res: tuple[list[str], list[str], list[str]] = ([], [], [])
    for x in common:
        ax = os.path.join(a, x)
        bx = os.path.join(b, x)
        res[_cmp(ax, bx, shallow)].append(x)
    return res


def _cmp(a: str, b: str, sh: bool) -> int:
    """0 for equal, 1 for different, 2 for funny cases (can't stat, etc.)"""
    try:
        return 0 if cmp(a, b, sh) else 1
    except (OSError, ValueError):
        return 2
