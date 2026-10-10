"""Constants and functions for the results of os.stat() (CPython's stat)."""

ST_MODE = 0
ST_INO = 1
ST_DEV = 2
ST_NLINK = 3
ST_UID = 4
ST_GID = 5
ST_SIZE = 6
ST_ATIME = 7
ST_MTIME = 8
ST_CTIME = 9

S_IFDIR = 0o040000
S_IFCHR = 0o020000
S_IFBLK = 0o060000
S_IFREG = 0o100000
S_IFIFO = 0o010000
S_IFLNK = 0o120000
S_IFSOCK = 0o140000
S_IFDOOR = 0
S_IFPORT = 0
S_IFWHT = 0

S_ISUID = 0o4000
S_ISGID = 0o2000
S_ENFMT = S_ISGID
S_ISVTX = 0o1000
S_IREAD = 0o0400
S_IWRITE = 0o0200
S_IEXEC = 0o0100
S_IRWXU = 0o0700
S_IRUSR = 0o0400
S_IWUSR = 0o0200
S_IXUSR = 0o0100
S_IRWXG = 0o0070
S_IRGRP = 0o0040
S_IWGRP = 0o0020
S_IXGRP = 0o0010
S_IRWXO = 0o0007
S_IROTH = 0o0004
S_IWOTH = 0o0002
S_IXOTH = 0o0001

UF_SETTABLE = 0x0000ffff
UF_NODUMP = 0x00000001
UF_IMMUTABLE = 0x00000002
UF_APPEND = 0x00000004
UF_OPAQUE = 0x00000008
UF_NOUNLINK = 0x00000010
UF_COMPRESSED = 0x00000020
UF_TRACKED = 0x00000040
UF_DATAVAULT = 0x00000080
UF_HIDDEN = 0x00008000
SF_SETTABLE = 0xffff0000
SF_ARCHIVED = 0x00010000
SF_IMMUTABLE = 0x00020000
SF_APPEND = 0x00040000
SF_RESTRICTED = 0x00080000
SF_NOUNLINK = 0x00100000
SF_SNAPSHOT = 0x00200000
SF_FIRMLINK = 0x00800000
SF_DATALESS = 0x40000000


def S_IMODE(mode: int) -> int:
    """The part of mode os.chmod() sets: permission bits, set-id and sticky bits."""
    return mode & 0o7777


def S_IFMT(mode: int) -> int:
    """The part of mode giving the kind of file."""
    return mode & 0o170000


def S_ISDIR(mode: int) -> bool:
    return S_IFMT(mode) == S_IFDIR


def S_ISCHR(mode: int) -> bool:
    return S_IFMT(mode) == S_IFCHR


def S_ISBLK(mode: int) -> bool:
    return S_IFMT(mode) == S_IFBLK


def S_ISREG(mode: int) -> bool:
    return S_IFMT(mode) == S_IFREG


def S_ISFIFO(mode: int) -> bool:
    return S_IFMT(mode) == S_IFIFO


def S_ISLNK(mode: int) -> bool:
    return S_IFMT(mode) == S_IFLNK


def S_ISSOCK(mode: int) -> bool:
    return S_IFMT(mode) == S_IFSOCK


def S_ISDOOR(mode: int) -> bool:
    return False


def S_ISPORT(mode: int) -> bool:
    return False


def S_ISWHT(mode: int) -> bool:
    return False


_KINDS = [(S_IFLNK, "l"), (S_IFSOCK, "s"), (S_IFREG, "-"), (S_IFBLK, "b"), (S_IFDIR, "d"), (S_IFCHR, "c"), (S_IFIFO, "p")]


def filemode(mode: int) -> str:
    """mode as ls shows it: '-rwxr-xr-x'."""
    out: list[str] = []
    kind = "?"
    for bits, c in _KINDS:
        if mode & 0o170000 == bits:
            kind = c
            break
    out.append(kind)
    out.append("r" if mode & S_IRUSR else "-")
    out.append("w" if mode & S_IWUSR else "-")
    if mode & S_ISUID:
        out.append("s" if mode & S_IXUSR else "S")
    else:
        out.append("x" if mode & S_IXUSR else "-")
    out.append("r" if mode & S_IRGRP else "-")
    out.append("w" if mode & S_IWGRP else "-")
    if mode & S_ISGID:
        out.append("s" if mode & S_IXGRP else "S")
    else:
        out.append("x" if mode & S_IXGRP else "-")
    out.append("r" if mode & S_IROTH else "-")
    out.append("w" if mode & S_IWOTH else "-")
    if mode & S_ISVTX:
        out.append("t" if mode & S_IXOTH else "T")
    else:
        out.append("x" if mode & S_IXOTH else "-")
    return "".join(out)
