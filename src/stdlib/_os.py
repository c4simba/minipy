"""The system layer under os, os.path and io: files, folders and the process.

Everywhere but KolibriOS these are the i386 Linux system calls (minipy emulates
them on other hosts, with Linux's numbers, flags and errno values). On
KolibriOS they are the file system functions 70/80 and the current folder
functions of 30. KolibriOS has no file descriptors: open() hands out numbers
for (path, position) pairs kept here, read() and write() go to that position
of the file, fds 0, 1 and 2 are the console.

The compiler keeps only the branch for its target of each
`if sys.platform == "kolibrios":`."""
import sys

O_RDONLY = 0
O_WRONLY = 1
O_RDWR = 2
O_ACCMODE = 3
O_CREAT = 0x40
O_EXCL = 0x80
O_NOCTTY = 0x100
O_TRUNC = 0x200
O_APPEND = 0x400
O_NONBLOCK = 0x800
O_DIRECTORY = 0x10000
O_CLOEXEC = 0x80000
SEEK_SET = 0
SEEK_CUR = 1
SEEK_END = 2
F_OK = 0
X_OK = 1
W_OK = 2
R_OK = 4

S_IFMT = 0o170000
S_IFDIR = 0o040000
S_IFREG = 0o100000
S_IFLNK = 0o120000
S_IFCHR = 0o020000
S_IFIFO = 0o010000
S_IFSOCK = 0o140000
S_IFBLK = 0o060000

# the texts of the error numbers, "number:text" lines (a dict only when first needed: a big
# dict literal is much code in a compiled program)
if sys.platform == "darwin":
    _STRERROR_TEXT = "1:Operation not permitted\n2:No such file or directory\n3:No such process\n4:Interrupted system call\n5:Input/output error\n6:Device not configured\n7:Argument list too long\n8:Exec format error\n9:Bad file descriptor\n10:No child processes\n11:Resource deadlock avoided\n12:Cannot allocate memory\n13:Permission denied\n14:Bad address\n15:Block device required\n16:Resource busy\n17:File exists\n18:Cross-device link\n19:Operation not supported by device\n20:Not a directory\n21:Is a directory\n22:Invalid argument\n23:Too many open files in system\n24:Too many open files\n25:Inappropriate ioctl for device\n26:Text file busy\n27:File too large\n28:No space left on device\n29:Illegal seek\n30:Read-only file system\n31:Too many links\n32:Broken pipe\n33:Numerical argument out of domain\n34:Result too large\n35:Resource temporarily unavailable\n36:Operation now in progress\n37:Operation already in progress\n38:Socket operation on non-socket\n39:Destination address required\n40:Message too long\n41:Protocol wrong type for socket\n42:Protocol not available\n43:Protocol not supported\n44:Socket type not supported\n45:Operation not supported\n46:Protocol family not supported\n47:Address family not supported by protocol family\n48:Address already in use\n49:Can't assign requested address\n50:Network is down\n51:Network is unreachable\n52:Network dropped connection on reset\n53:Software caused connection abort\n54:Connection reset by peer\n55:No buffer space available\n56:Socket is already connected\n57:Socket is not connected\n58:Can't send after socket shutdown\n59:Too many references: can't splice\n60:Operation timed out\n61:Connection refused\n62:Too many levels of symbolic links\n63:File name too long\n64:Host is down\n65:No route to host\n66:Directory not empty\n67:Too many processes\n68:Too many users\n69:Disc quota exceeded\n70:Stale NFS file handle\n71:Too many levels of remote in path\n72:RPC struct is bad\n73:RPC version wrong\n74:RPC prog. not avail\n75:Program version wrong\n76:Bad procedure for program\n77:No locks available\n78:Function not implemented\n79:Inappropriate file type or format\n80:Authentication error\n81:Need authenticator\n82:Device power is off\n83:Device error\n84:Value too large to be stored in data type\n85:Bad executable (or shared library)\n86:Bad CPU type in executable\n87:Shared library version mismatch\n88:Malformed Mach-o file\n89:Operation canceled\n90:Identifier removed\n91:No message of desired type\n92:Illegal byte sequence\n93:Attribute not found\n94:Bad message\n95:EMULTIHOP (Reserved)\n96:No message available on STREAM\n97:ENOLINK (Reserved)\n98:No STREAM resources\n99:Not a STREAM\n100:Protocol error\n101:STREAM ioctl timeout\n102:Operation not supported on socket\n103:Policy not found\n104:State not recoverable\n105:Previous owner died\n106:Interface output queue is full\n107:Capabilities insufficient"
else:
    _STRERROR_TEXT = "1:Operation not permitted\n2:No such file or directory\n3:No such process\n4:Interrupted system call\n5:Input/output error\n6:No such device or address\n7:Argument list too long\n8:Exec format error\n9:Bad file descriptor\n10:No child processes\n11:Resource temporarily unavailable\n12:Cannot allocate memory\n13:Permission denied\n14:Bad address\n15:Block device required\n16:Device or resource busy\n17:File exists\n18:Invalid cross-device link\n19:No such device\n20:Not a directory\n21:Is a directory\n22:Invalid argument\n23:Too many open files in system\n24:Too many open files\n25:Inappropriate ioctl for device\n26:Text file busy\n27:File too large\n28:No space left on device\n29:Illegal seek\n30:Read-only file system\n31:Too many links\n32:Broken pipe\n33:Numerical argument out of domain\n34:Numerical result out of range\n35:Resource deadlock avoided\n36:File name too long\n37:No locks available\n38:Function not implemented\n39:Directory not empty\n40:Too many levels of symbolic links\n42:No message of desired type\n43:Identifier removed\n60:Device not a stream\n61:No data available\n62:Timer expired\n63:Out of streams resources\n67:Link has been severed\n71:Protocol error\n72:Multihop attempted\n74:Bad message\n75:Value too large for defined data type\n84:Invalid or incomplete multibyte or wide character\n87:Too many users\n88:Socket operation on non-socket\n89:Destination address required\n90:Message too long\n91:Protocol wrong type for socket\n92:Protocol not available\n93:Protocol not supported\n94:Socket type not supported\n95:Operation not supported\n96:Protocol family not supported\n97:Address family not supported by protocol\n98:Address already in use\n99:Cannot assign requested address\n100:Network is down\n101:Network is unreachable\n102:Network dropped connection on reset\n103:Software caused connection abort\n104:Connection reset by peer\n105:No buffer space available\n106:Transport endpoint is already connected\n107:Transport endpoint is not connected\n108:Cannot send after transport endpoint shutdown\n109:Too many references: cannot splice\n110:Connection timed out\n111:Connection refused\n112:Host is down\n113:No route to host\n114:Operation already in progress\n115:Operation now in progress\n116:Stale file handle\n121:Remote I/O error\n122:Disk quota exceeded\n125:Operation canceled\n130:Owner died\n131:State not recoverable"
_STRERROR: dict[int, str] = {}


def strerror(code: int) -> str:
    """The text of error number code (the platform's numbers and texts, as CPython's)."""
    if not _STRERROR:
        for line in _STRERROR_TEXT.split("\n"):
            i = line.find(":")
            _STRERROR[int(line[:i])] = line[i + 1:]
    s = _STRERROR.get(code)
    if s is None:
        if sys.platform == "darwin":
            return "Unknown error: " + str(code)
        return "Unknown error " + str(code)
    return s


def _error(code: int, filename: str | None) -> OSError:
    return OSError(code, strerror(code), filename)


def _check(r: int, filename: str | None) -> int:
    if r < 0:
        raise _error(-r, filename)
    return r


def _path(p: str) -> str:
    if "\0" in p:
        raise ValueError("embedded null byte")
    return p


def _u32(v: int) -> int:
    return v & 0xFFFFFFFF


def _u64(b: buffer, off: int) -> int:
    return _u32(sys.peek(b, off, 4)) | ((sys.peek(b, off + 4, 4) & 0x7FFFFFFF) << 32)   # (ints are 64-bit)


def _cstr(b: buffer, off: int, maxlen: int) -> str:
    data = sys.peek_bytes(b, off, maxlen)
    end = data.find(b"\0")
    if end >= 0:
        data = data[:end]
    return data.decode("utf-8", "replace")


class stat_result:
    """os.stat_result: st_mode, st_size, st_mtime ... (as a tuple: the first ten, times as ints)."""

    def __init__(self, mode: int, ino: int, dev: int, nlink: int, uid: int, gid: int, size: int,
                 atime_ns: int, mtime_ns: int, ctime_ns: int, blocks: int, blksize: int):
        self.st_mode = mode
        self.st_ino = ino
        self.st_dev = dev
        self.st_nlink = nlink
        self.st_uid = uid
        self.st_gid = gid
        self.st_size = size
        self.st_atime_ns = atime_ns
        self.st_mtime_ns = mtime_ns
        self.st_ctime_ns = ctime_ns
        self.st_atime = atime_ns // 1000000000 + (atime_ns % 1000000000) * 1e-9     # (as CPython computes them)
        self.st_mtime = mtime_ns // 1000000000 + (mtime_ns % 1000000000) * 1e-9
        self.st_ctime = ctime_ns // 1000000000 + (ctime_ns % 1000000000) * 1e-9
        self.st_blocks = blocks
        self.st_blksize = blksize

    def _fields(self) -> list[int]:
        return [self.st_mode, self.st_ino, self.st_dev, self.st_nlink, self.st_uid, self.st_gid, self.st_size,
                self.st_atime_ns // 1000000000, self.st_mtime_ns // 1000000000, self.st_ctime_ns // 1000000000]

    def __getitem__(self, i: int) -> int:
        return self._fields()[i]

    def __len__(self) -> int:
        return 10

    def __repr__(self) -> str:
        f = self._fields()
        names = ["st_mode", "st_ino", "st_dev", "st_nlink", "st_uid", "st_gid", "st_size", "st_atime", "st_mtime", "st_ctime"]
        return "os.stat_result(" + ", ".join([names[i] + "=" + str(f[i]) for i in range(10)]) + ")"


def S_ISDIR(mode: int) -> bool:
    return (mode & S_IFMT) == S_IFDIR


def S_ISREG(mode: int) -> bool:
    return (mode & S_IFMT) == S_IFREG


def S_ISLNK(mode: int) -> bool:
    return (mode & S_IFMT) == S_IFLNK


# ---------------------------------------------------------------- KolibriOS: functions 70 / 80 and 30

_KERR = {2: 95, 3: 19, 5: 2, 6: 5, 7: 14, 8: 28, 9: 5, 10: 13, 11: 5, 12: 12}   # file system status -> errno


def _kerrno(status: int) -> int:
    e = _KERR.get(status)
    if e is None:
        return 5
    return e


def _ascii(s: str) -> bool:
    for ch in s:
        if ord(ch) > 127:
            return False
    return True


def _kfs(sub: int, a4: int, a8: int, size: int, ptr: int, p: str) -> list[int]:
    """The information block of fn 70 (an ASCII path inside it) or fn 80 (a UTF-8 path) -> the registers."""
    if _ascii(p):
        info = sys.buffer(21 + len(p) + 1)
        sys.poke(info, 0, sub, 4)
        sys.poke(info, 4, a4, 4)
        sys.poke(info, 8, a8, 4)
        sys.poke(info, 12, size, 4)
        sys.poke(info, 16, ptr, 4)
        sys.poke_str(info, 20, p)
        return sys.syscall(70, info)
    name = sys.buffer(p.encode() + b"\0")
    info = sys.buffer(28)
    sys.poke(info, 0, sub, 4)
    sys.poke(info, 4, a4, 4)
    sys.poke(info, 8, a8, 4)
    sys.poke(info, 12, size, 4)
    sys.poke(info, 16, ptr, 4)
    sys.poke(info, 20, 3, 4)
    sys.poke(info, 24, sys.addr(name), 4)
    r = sys.syscall(80, info)
    sys.peek(name, 0, 1)                         # (the path stays alive until here)
    return r


def _kstatus(r: list[int]) -> int:
    return r[0] & 0xFFFFFFFF


def _kinfo(p: str) -> buffer:
    """fn 70.5: the BDVK of p (attributes at +0, times at +8/+16/+24, size at +32), or an error."""
    b = sys.buffer(40)
    st = _kstatus(_kfs(5, 0, 0, 0, sys.addr(b), p))
    if st != 0:
        raise _error(_kerrno(st), p)
    return b


def _days(y: int, m: int, d: int) -> int:
    """Days from 1970-01-01 to a date (proleptic Gregorian)."""
    if m <= 2:
        y -= 1
        m += 12
    era = y // 400
    yoe = y - era * 400
    doy = (153 * (m - 3) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def _kstamp(b: buffer, off: int) -> int:
    """A BDVK time (sec, min, hour, -, day, month, year) as nanoseconds since 1970 (KolibriOS keeps local time)."""
    sec = sys.peek(b, off, 1)
    mi = sys.peek(b, off + 1, 1)
    hour = sys.peek(b, off + 2, 1)
    day = sys.peek(b, off + 4, 1)
    month = sys.peek(b, off + 5, 1)
    year = sys.peek(b, off + 6, 2)
    if month < 1 or month > 12 or day < 1:
        return 0
    return ((_days(year, month, day) * 24 + hour) * 3600 + mi * 60 + sec) * 1000000000


def _kstat_result(b: buffer) -> stat_result:
    attr = sys.peek(b, 0, 4)
    if attr & 0x10:
        mode = S_IFDIR | 0o755
    elif attr & 1:
        mode = S_IFREG | 0o444
    else:
        mode = S_IFREG | 0o644
    size = _u64(b, 32)
    if attr & 0x10:
        size = 0
    return stat_result(mode, 0, 0, 1, 0, 0, size, _kstamp(b, 16), _kstamp(b, 24), _kstamp(b, 8), (size + 511) // 512, 512)


class _KFile:
    def __init__(self, p: str, flags: int):
        self.path = p
        self.flags = flags
        self.pos = 0


_kfiles: dict[int, _KFile] = {}
_knext = 3


def _kfile(fd: int) -> _KFile:
    f = _kfiles.get(fd)
    if f is None:
        raise _error(9, None)
    return f


def _kexists(p: str) -> int:
    """-1: no such path, 0: a file, 1: a folder."""
    b = sys.buffer(40)
    if _kstatus(_kfs(5, 0, 0, 0, sys.addr(b), p)) != 0:
        return -1
    if sys.peek(b, 0, 4) & 0x10:
        return 1
    return 0


def _kcreate(p: str) -> None:
    st = _kstatus(_kfs(2, 0, 0, 0, 0, p))           # fn 70.2: create / truncate to 0 bytes
    if st != 0:
        raise _error(_kerrno(st), p)


# ---------------------------------------------------------------- files

def open(file: str, flags: int, mode: int = 0o777) -> int:
    """A file descriptor for file (os.O_RDONLY / O_WRONLY / O_RDWR, O_CREAT, O_EXCL, O_TRUNC, O_APPEND)."""
    p = _path(file)
    if sys.platform == "kolibrios":
        global _knext
        kind = _kexists(p)
        if kind < 0:
            if not (flags & O_CREAT):
                raise _error(2, p)
            _kcreate(p)
        else:
            if (flags & O_CREAT) and (flags & O_EXCL):
                raise _error(17, p)
            if kind == 1 and (flags & O_ACCMODE) != O_RDONLY:
                raise _error(21, p)
            if kind == 0 and (flags & O_TRUNC) and (flags & O_ACCMODE) != O_RDONLY:
                _kcreate(p)
        fd = _knext
        _knext += 1
        _kfiles[fd] = _KFile(p, flags)
        return fd
    return _check(sys.syscall(5, p, flags | O_CLOEXEC, mode)[0], p)


def close(fd: int) -> None:
    if sys.platform == "kolibrios":
        _kfile(fd)
        del _kfiles[fd]
        return
    _check(sys.syscall(6, fd)[0], None)


def read(fd: int, n: int) -> bytes:
    """Up to n bytes from fd (b"" at the end)."""
    if n < 0:
        raise ValueError("negative count")
    if n == 0:
        return b""
    if sys.platform == "kolibrios":
        if fd == 0:
            line = input() + "\n"
            return line.encode()[:n]
        f = _kfile(fd)
        if (f.flags & O_ACCMODE) == O_WRONLY:
            raise _error(9, None)
        buf = sys.buffer(n)
        r = _kfs(0, f.pos, 0, n, sys.addr(buf), f.path)
        st = _kstatus(r)
        if st != 0 and st != 6:
            raise _error(_kerrno(st), None)
        got = r[1] & 0xFFFFFFFF
        f.pos += got
        return sys.peek_bytes(buf, 0, got)
    buf = sys.buffer(n)
    got = _check(sys.syscall(3, fd, buf, n)[0], None)
    return sys.peek_bytes(buf, 0, got)


def write(fd: int, data: bytes) -> int:
    """Write data to fd: the number of bytes written."""
    if sys.platform == "kolibrios":
        if fd == 1 or fd == 2:
            print(data.decode("utf-8", "replace"), end="")
            return len(data)
        f = _kfile(fd)
        if (f.flags & O_ACCMODE) == O_RDONLY:
            raise _error(9, None)
        if f.flags & O_APPEND:
            f.pos = _u64(_kinfo(f.path), 32)
        if len(data) == 0:
            return 0
        buf = sys.buffer(data)
        r = _kfs(3, f.pos, 0, len(data), sys.addr(buf), f.path)
        st = _kstatus(r)
        if st != 0:
            raise _error(_kerrno(st), None)
        done = r[1] & 0xFFFFFFFF
        f.pos += done
        return done
    return _check(sys.syscall(4, fd, data, len(data))[0], None)


def lseek(fd: int, pos: int, how: int) -> int:
    """Move fd's position (SEEK_SET, SEEK_CUR, SEEK_END): the new one."""
    if sys.platform == "kolibrios":
        f = _kfile(fd)
        if how == SEEK_SET:
            n = pos
        elif how == SEEK_CUR:
            n = f.pos + pos
        elif how == SEEK_END:
            n = _u64(_kinfo(f.path), 32) + pos
        else:
            raise _error(22, None)
        if n < 0:
            raise _error(22, None)
        f.pos = n
        return n
    out = sys.buffer(8)
    _check(sys.syscall(140, fd, (pos >> 32) & 0xFFFFFFFF, pos & 0xFFFFFFFF, out, how)[0], None)
    return _u64(out, 0)


def ftruncate(fd: int, length: int) -> None:
    if sys.platform == "kolibrios":
        f = _kfile(fd)
        st = _kstatus(_kfs(4, length, 0, 0, 0, f.path))     # fn 70.4: set the size
        if st != 0:
            raise _error(_kerrno(st), None)
        return
    _check(sys.syscall(194, fd, length & 0xFFFFFFFF, (length >> 32) & 0xFFFFFFFF)[0], None)


def truncate(p: str, length: int) -> None:
    if sys.platform == "kolibrios":
        st = _kstatus(_kfs(4, length, 0, 0, 0, _path(p)))
        if st != 0:
            raise _error(_kerrno(st), p)
        return
    _check(sys.syscall(193, _path(p), length & 0xFFFFFFFF, (length >> 32) & 0xFFFFFFFF)[0], p)


def fsync(fd: int) -> None:
    if sys.platform == "kolibrios":
        _kfile(fd)
        return
    _check(sys.syscall(118, fd)[0], None)


def isatty(fd: int) -> bool:
    if sys.platform == "kolibrios":
        return fd >= 0 and fd <= 2
    return sys.syscall(54, fd, 0x5401, sys.buffer(64))[0] == 0      # TCGETS


def _stat_of(b: buffer) -> stat_result:
    """struct stat64 of i386 Linux."""
    return stat_result(sys.peek(b, 16, 4), _u64(b, 88), _u64(b, 0), sys.peek(b, 20, 4), sys.peek(b, 24, 4),
                       sys.peek(b, 28, 4), _u64(b, 44),
                       sys.peek(b, 64, 4) * 1000000000 + sys.peek(b, 68, 4),
                       sys.peek(b, 72, 4) * 1000000000 + sys.peek(b, 76, 4),
                       sys.peek(b, 80, 4) * 1000000000 + sys.peek(b, 84, 4),
                       _u64(b, 56), sys.peek(b, 52, 4))


def stat(p: str) -> stat_result:
    if sys.platform == "kolibrios":
        return _kstat_result(_kinfo(_path(p)))
    b = sys.buffer(96)
    _check(sys.syscall(195, _path(p), b)[0], p)
    return _stat_of(b)


def lstat(p: str) -> stat_result:
    if sys.platform == "kolibrios":
        return _kstat_result(_kinfo(_path(p)))
    b = sys.buffer(96)
    _check(sys.syscall(196, _path(p), b)[0], p)
    return _stat_of(b)


def fstat(fd: int) -> stat_result:
    if sys.platform == "kolibrios":
        return _kstat_result(_kinfo(_kfile(fd).path))
    b = sys.buffer(96)
    _check(sys.syscall(197, fd, b)[0], None)
    return _stat_of(b)


# ---------------------------------------------------------------- folders

def listdir_types(p: str) -> list[tuple[str, int]]:
    """The entries of folder p (not . and ..) with their kind: 1 a folder, 0 a file, 2 a link, 3 something else, -1 unknown."""
    p = _path(p)
    out: list[tuple[str, int]] = []
    if sys.platform == "kolibrios":
        if _kexists(p) == 0:
            raise _error(20, p)
        batch = 16
        buf = sys.buffer(32 + batch * 560)
        start = 0
        while True:
            r = _kfs(1, start, 3, batch, sys.addr(buf), p)    # fn 70.1, names in UTF-8
            st = _kstatus(r)
            if st != 0 and st != 6:
                raise _error(_kerrno(st), p)
            n = sys.peek(buf, 4, 4)
            for i in range(n):
                off = 32 + i * 560
                name = _cstr(buf, off + 40, 520)
                if name != "." and name != "..":
                    kind = 0
                    if sys.peek(buf, off, 4) & 0x10:
                        kind = 1
                    out.append((name, kind))
            start += n
            if st == 6 or n < batch:
                break
        return out
    fd = _check(sys.syscall(5, p, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0)[0], p)
    buf = sys.buffer(8192)
    try:
        while True:
            n = _check(sys.syscall(220, fd, buf, 8192)[0], p)   # getdents64
            if n == 0:
                break
            off = 0
            while off < n:
                reclen = sys.peek(buf, off + 16, 2)
                dtype = sys.peek(buf, off + 18, 1)
                name = _cstr(buf, off + 19, reclen - 19)
                if name != "." and name != "..":
                    kind = -1                          # d_type: 4 folder, 8 file, 10 link, 0 unknown
                    if dtype == 4:
                        kind = 1
                    elif dtype == 8:
                        kind = 0
                    elif dtype == 10:
                        kind = 2
                    elif dtype != 0:
                        kind = 3
                    out.append((name, kind))
                off += reclen
    finally:
        sys.syscall(6, fd)
    return out


def listdir(p: str = ".") -> list[str]:
    return [name for name, kind in listdir_types(p)]


def mkdir(p: str, mode: int = 0o777) -> None:
    p = _path(p)
    if sys.platform == "kolibrios":
        if _kexists(p) >= 0:
            raise _error(17, p)
        st = _kstatus(_kfs(9, 0, 0, 0, 0, p))           # fn 70.9
        if st != 0:
            raise _error(_kerrno(st), p)
        return
    _check(sys.syscall(39, p, mode)[0], p)


def rmdir(p: str) -> None:
    p = _path(p)
    if sys.platform == "kolibrios":
        kind = _kexists(p)
        if kind < 0:
            raise _error(2, p)
        if kind == 0:
            raise _error(20, p)
        if len(listdir_types(p)) > 0:
            raise _error(39, p)
        st = _kstatus(_kfs(8, 0, 0, 0, 0, p))           # fn 70.8
        if st != 0:
            raise _error(_kerrno(st), p)
        return
    _check(sys.syscall(40, p)[0], p)


def unlink(p: str) -> None:
    p = _path(p)
    if sys.platform == "kolibrios":
        kind = _kexists(p)
        if kind < 0:
            raise _error(2, p)
        if kind == 1:
            raise _error(21, p)
        st = _kstatus(_kfs(8, 0, 0, 0, 0, p))
        if st != 0:
            raise _error(_kerrno(st), p)
        return
    _check(sys.syscall(10, p)[0], p)


def rename(src: str, dst: str) -> None:
    src = _path(src)
    dst = _path(dst)
    if sys.platform == "kolibrios":
        kind = _kexists(src)
        if kind < 0:
            raise OSError(2, strerror(2), src, None, dst)
        target = _kexists(dst)
        if target == 1 and kind == 0:
            raise OSError(21, strerror(21), src, None, dst)
        if target == 0 and kind == 1:
            raise OSError(20, strerror(20), src, None, dst)
        if target == 1 and len(listdir_types(dst)) > 0:
            raise OSError(39, strerror(39), src, None, dst)
        if target >= 0:
            _kfs(8, 0, 0, 0, 0, dst)
        full = dst
        if not dst.startswith("/"):
            full = getcwd() + "/" + dst
        name = sys.buffer(full.encode() + b"\0")
        st = _kstatus(_kfs(10, 0, 0, 0, sys.addr(name), src))   # fn 70.10: a full new path moves it
        sys.peek(name, 0, 1)
        if st != 0:
            raise OSError(_kerrno(st), strerror(_kerrno(st)), src, None, dst)
        return
    r = sys.syscall(38, src, dst)[0]
    if r < 0:
        raise OSError(-r, strerror(-r), src, None, dst)


def getcwd() -> str:
    if sys.platform == "kolibrios":
        b = sys.buffer(1024)
        sys.syscall(30, 5, b, 1024, 3)                 # fn 30.5: the current folder, UTF-8
        return _cstr(b, 0, 1024)
    b = sys.buffer(4096)
    n = _check(sys.syscall(183, b, 4096)[0], None)
    return _cstr(b, 0, n)


def chdir(p: str) -> None:
    p = _path(p)
    if sys.platform == "kolibrios":
        kind = _kexists(p)
        if kind < 0:
            raise _error(2, p)
        if kind == 0:
            raise _error(20, p)
        full = p
        if not p.startswith("/"):
            full = getcwd() + "/" + p
        sys.syscall(30, 4, full, 3)                      # fn 30.4 (UTF-8)
        return
    _check(sys.syscall(12, p)[0], p)


def access(p: str, mode: int) -> bool:
    if sys.platform == "kolibrios":
        kind = _kexists(_path(p))
        return kind >= 0
    return sys.syscall(33, _path(p), mode)[0] == 0


def getpid() -> int:
    if sys.platform == "kolibrios":
        b = sys.buffer(1024)
        sys.syscall(9, b, -1)                          # fn 9: this thread's information
        return sys.peek(b, 30, 4)
    return sys.syscall(20)[0]


def urandom(n: int) -> bytes:
    """n random bytes (Linux: getrandom; KolibriOS has no source of its own: a generator seeded from its clocks)."""
    if n < 0:
        raise ValueError("negative argument not allowed")
    if sys.platform == "kolibrios":
        global _kseed
        if _kseed == 0:
            t = sys.syscall(26, 10)                    # fn 26.10: nanoseconds since boot
            _kseed = ((t[0] ^ (t[1] << 7) ^ (sys.syscall(3)[0] << 13)) & 0xFFFFFFFF) | 1
        out: list[int] = []
        for i in range(n):
            x = _kseed                                 # xorshift32
            x ^= (x << 13) & 0xFFFFFFFF
            x ^= x >> 17
            x ^= (x << 5) & 0xFFFFFFFF
            _kseed = x
            out.append((x >> 11) & 255)
        return bytes(out)
    buf = sys.buffer(n)
    got = 0
    while got < n:
        r = _check(sys.syscall(355, sys.addr(buf) + got, n - got, 0)[0], None)
        got += r
    return sys.peek_bytes(buf, 0, n)


_kseed = 0


def readlink(p: str) -> str:
    p = _path(p)
    if sys.platform == "kolibrios":
        b = sys.buffer(1024)
        st = _kstatus(_kfs(12, 0, 0, 1024, sys.addr(b), p))   # fn 70.12
        if st != 0:
            raise _error(_kerrno(st), p)
        return _cstr(b, 0, 1024)
    b = sys.buffer(4096)
    n = _check(sys.syscall(85, p, b, 4096)[0], p)
    return sys.peek_bytes(b, 0, n).decode("utf-8", "replace")


def symlink(src: str, dst: str) -> None:
    src = _path(src)
    dst = _path(dst)
    if sys.platform == "kolibrios":
        t = sys.buffer(src.encode() + b"\0")
        st = _kstatus(_kfs(11, 0, 0, 0, sys.addr(t), dst))   # fn 70.11
        sys.peek(t, 0, 1)
        if st != 0:
            raise OSError(_kerrno(st), strerror(_kerrno(st)), src, None, dst)
        return
    r = sys.syscall(83, src, dst)[0]
    if r < 0:
        raise OSError(-r, strerror(-r), src, None, dst)


def chmod(p: str, mode: int) -> None:
    p = _path(p)
    if sys.platform == "kolibrios":
        b = _kinfo(p)                                  # fn 70.6: the read-only attribute
        attr = sys.peek(b, 0, 4)
        if mode & 0o222:
            attr &= ~1
        else:
            attr |= 1
        sys.poke(b, 0, attr, 4)
        st = _kstatus(_kfs(6, 0, 0, 0, sys.addr(b), p))
        if st != 0:
            raise _error(_kerrno(st), p)
        return
    _check(sys.syscall(15, p, mode)[0], p)


def _civil(days: int) -> tuple[int, int, int]:
    """(year, month, day) of a day counted from 1970-01-01."""
    z = days + 719468
    era = z // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    m = mp + 3 if mp < 10 else mp - 9
    return (yoe + era * 400 + (1 if m <= 2 else 0), m, doy - (153 * mp + 2) // 5 + 1)


def _kput_stamp(b: buffer, off: int, ns: int) -> None:
    """Time ns (nanoseconds since 1970) as a BDVK time (sec, min, hour, -, day, month, year)."""
    t = ns // 1000000000
    days = t // 86400
    rem = t - days * 86400
    y, m, d = _civil(days)
    sys.poke(b, off, rem % 60, 1)
    sys.poke(b, off + 1, rem // 60 % 60, 1)
    sys.poke(b, off + 2, rem // 3600, 1)
    sys.poke(b, off + 3, 0, 1)
    sys.poke(b, off + 4, d, 1)
    sys.poke(b, off + 5, m, 1)
    sys.poke(b, off + 6, y, 2)


def _know_ns() -> int:
    """KolibriOS's clock (fn 3, fn 29) in nanoseconds since 1970."""
    d = sys.syscall(29)[0]
    t = sys.syscall(3)[0]
    bcd = lambda x: (x >> 4) * 10 + (x & 15)
    days = _days(2000 + bcd(d & 255), bcd((d >> 8) & 255), bcd((d >> 16) & 255))
    return (((days * 24 + bcd(t & 255)) * 60 + bcd((t >> 8) & 255)) * 60 + bcd((t >> 16) & 255)) * 1000000000


def _ns_of(x) -> int:
    """A timestamp (seconds, int or float) in nanoseconds (rounded down)."""
    if isinstance(x, int):
        return x * 1000000000
    else:
        whole = x // 1
        return int(whole) * 1000000000 + int(((x - whole) * 1e9) // 1)


def _utime(p: str, at: int, mt: int, now: bool, follow: bool) -> None:
    """Set the access and modification times of p (nanoseconds; now: the current time)."""
    if sys.platform == "kolibrios":
        if now:
            at = _know_ns()
            mt = at
        b = _kinfo(p)                                  # fn 70.6 with the times changed
        _kput_stamp(b, 16, at)
        _kput_stamp(b, 24, mt)
        st = _kstatus(_kfs(6, 0, 0, 0, sys.addr(b), p))
        if st != 0:
            raise _error(_kerrno(st), p)
        return
    flags = 0 if follow else 0x100
    if now:
        _check(sys.syscall(320, -100, p, 0, flags)[0], p)
        return
    b = sys.buffer(16)
    sys.poke(b, 0, at // 1000000000, 4)
    sys.poke(b, 4, at % 1000000000, 4)
    sys.poke(b, 8, mt // 1000000000, 4)
    sys.poke(b, 12, mt % 1000000000, 4)
    _check(sys.syscall(320, -100, p, b, flags)[0], p)


def utime(p: str, times=None, *, ns=None, follow_symlinks: bool = True) -> None:
    """Set the access and modification times of p: times (seconds) or ns (nanoseconds), now without them."""
    p = _path(p)
    if times is not None and ns is not None:
        raise ValueError("utime: you may specify either 'times' or 'ns' but not both")
    if times is not None:
        _utime(p, _ns_of(times[0]), _ns_of(times[1]), False, follow_symlinks)
    elif ns is not None:
        _utime(p, ns[0], ns[1], False, follow_symlinks)
    else:
        _utime(p, 0, 0, True, follow_symlinks)


_kumask = [0o022]


def umask(mask: int) -> int:
    """Set the mask of permission bits new files leave out; the old one."""
    if sys.platform == "kolibrios":
        old = _kumask[0]
        _kumask[0] = mask & 0o777
        return old
    return sys.syscall(60, mask & 0o777)[0]


def link(src: str, dst: str) -> None:
    """A hard link dst to file src."""
    src = _path(src)
    dst = _path(dst)
    if sys.platform == "kolibrios":
        raise OSError(38, strerror(38), src, None, dst)
    r = sys.syscall(9, src, dst)[0]
    if r < 0:
        raise OSError(-r, strerror(-r), src, None, dst)


def _environ() -> dict[str, str]:
    env: dict[str, str] = {}
    for item in sys._rawargs(1):
        i = item.find("=")
        if i > 0:
            env[item[:i]] = item[i + 1:]
    return env


environ = _environ()


def uname() -> tuple[str, str, str, str, str]:
    """(sysname, nodename, release, version, machine)."""
    if sys.platform == "kolibrios":
        b = sys.buffer(16)
        sys.syscall(18, 13, b)                         # fn 18.13: the kernel's version
        ver = str(sys.peek(b, 0, 1)) + "." + str(sys.peek(b, 1, 1)) + "." + str(sys.peek(b, 2, 1)) + "." + str(sys.peek(b, 3, 1))
        return ("KolibriOS", "", ver, "", "i686")
    b = sys.buffer(6 * 65)
    _check(sys.syscall(122, b)[0], None)
    return (_cstr(b, 0, 65), _cstr(b, 65, 65), _cstr(b, 130, 65), _cstr(b, 195, 65), _cstr(b, 260, 65))


def terminal_size_of(fd: int) -> tuple[int, int]:
    """(columns, lines) of the terminal fd is, or an error."""
    if sys.platform == "kolibrios":
        return (80, 25)
    b = sys.buffer(8)
    _check(sys.syscall(54, fd, 0x5413, b)[0], None)  # TIOCGWINSZ
    return (sys.peek(b, 2, 2), sys.peek(b, 0, 2))


def getppid() -> int:
    if sys.platform == "kolibrios":
        return 0
    return sys.syscall(64)[0]


def getuid() -> int:
    if sys.platform == "kolibrios":
        return 0
    return sys.syscall(199)[0]


def getgid() -> int:
    if sys.platform == "kolibrios":
        return 0
    return sys.syscall(200)[0]


def kill(pid: int, sig: int) -> None:
    if sys.platform == "kolibrios":
        if sys.syscall(18, 18, pid)[0] != 0:            # fn 18.18: end the process with that PID
            raise _error(3, None)
        return
    _check(sys.syscall(37, pid, sig)[0], None)
