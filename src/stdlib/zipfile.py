"""Read and write ZIP files (CPython's zipfile): ZipFile, ZipInfo, is_zipfile, BadZipFile;
members stored (ZIP_STORED) or deflated (ZIP_DEFLATED, through zlib).

A ZipFile keeps the archive's bytes in memory: opened for reading, it reads the file once;
for writing ("w", "a", "x"), it writes the file when closed. No ZIP64, no encryption."""
import io
import os
import struct
import sys
import time
import zlib
import stat as _stat
from typing import Callable, Iterator, TypeVar

__all__ = ["BadZipFile", "BadZipfile", "error", "ZIP_STORED", "ZIP_DEFLATED", "ZIP_BZIP2", "ZIP_LZMA", "is_zipfile",
           "ZipInfo", "ZipFile", "LargeZipFile"]

_F = TypeVar("_F")
_D = TypeVar("_D")


class BadZipFile(Exception):
    pass


class LargeZipFile(Exception):
    """Raised when writing a zipfile, the zipfile requires ZIP64 extensions and those extensions are disabled."""


error = BadZipfile = BadZipFile

ZIP64_LIMIT = (1 << 31) - 1
ZIP_FILECOUNT_LIMIT = (1 << 16) - 1
ZIP_MAX_COMMENT = (1 << 16) - 1

ZIP_STORED = 0
ZIP_DEFLATED = 8
ZIP_BZIP2 = 12
ZIP_LZMA = 14
ZIP_ZSTANDARD = 93

compressor_names = {0: 'store', 1: 'shrink', 2: 'reduce', 3: 'reduce', 4: 'reduce', 5: 'reduce', 6: 'implode',
                    7: 'tokenize', 8: 'deflate', 9: 'deflate64', 10: 'implode', 12: 'bzip2', 14: 'lzma', 18: 'terse',
                    19: 'lz77', 93: 'zstd', 97: 'wavpack', 98: 'ppmd'}

DEFAULT_VERSION = 20
MAX_EXTRACT_VERSION = 63

_END_SIG = b"PK\005\006"
_CENTRAL_SIG = b"PK\001\002"
_LOCAL_SIG = b"PK\003\004"


def _u16(b: bytes, p: int) -> int:
    return b[p] | (b[p + 1] << 8)


def _u32(b: bytes, p: int) -> int:
    return b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24)


def _p16(n: int) -> bytes:
    return bytes([n & 0xFF, (n >> 8) & 0xFF])


def _p32(n: int) -> bytes:
    return bytes([n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, (n >> 24) & 0xFF])


def _find_end(data: bytes) -> int:
    """Where the end of central directory record starts (-1: none)."""
    start = max(0, len(data) - 22 - ZIP_MAX_COMMENT)
    i = data.rfind(_END_SIG, start)
    while i >= 0:
        if i + 22 <= len(data) and i + 22 + _u16(data, i + 20) <= len(data):
            return i
        i = data.rfind(_END_SIG, start, i)
    return -1


def is_zipfile(filename: _F) -> bool:
    """Whether filename (a path or a binary file object) is a ZIP file (it has the end record)."""
    try:
        if isinstance(filename, str):
            with io.open(filename, "rb") as f:
                data = f.read()
        else:
            data = filename.read()
    except OSError:
        return False
    return _find_end(data) >= 0


class ZipInfo:
    """Information about one member of the archive."""

    def __init__(self, filename: str = "NoName", date_time: tuple[int, int, int, int, int, int] = (1980, 1, 1, 0, 0, 0)) -> None:
        self.orig_filename = filename
        null = filename.find("\0")
        if null >= 0:
            filename = filename[:null]
        if os.sep != "/":
            filename = filename.replace(os.sep, "/")
        self.filename = filename
        if date_time[0] < 1980:
            raise ValueError('ZIP does not support timestamps before 1980')
        self.date_time = date_time
        self.compress_type = ZIP_STORED
        self.compress_level: int | None = None
        self.comment = b""
        self.extra = b""
        self.create_system = 3
        self.create_version = DEFAULT_VERSION
        self.extract_version = DEFAULT_VERSION
        self.reserved = 0
        self.flag_bits = 0
        self.volume = 0
        self.internal_attr = 0
        self.external_attr = 0
        self.header_offset = 0
        self.CRC = 0
        self.compress_size = 0
        self.file_size = 0
        self._data = b""                     # (the compressed data)

    def is_dir(self) -> bool:
        """Whether this archive member is a directory."""
        return self.filename.endswith('/')

    def _dos_time(self) -> tuple[int, int]:
        dt = self.date_time
        dosdate = (dt[0] - 1980) << 9 | dt[1] << 5 | dt[2]
        dostime = dt[3] << 11 | dt[4] << 5 | (dt[5] // 2)
        return dosdate, dostime

    def __repr__(self) -> str:
        result = ['<' + type(self).__name__ + ' filename=' + repr(self.filename)]
        if self.compress_type != ZIP_STORED:
            result.append(' compress_type=' + compressor_names.get(self.compress_type, str(self.compress_type)))
        hi = self.external_attr >> 16
        lo = self.external_attr & 0xFFFF
        if hi:
            result.append(' filemode=' + repr(_stat.filemode(hi)))
        if lo:
            result.append(' external_attr=' + hex(lo))
        isdir = self.is_dir()
        if not isdir or self.file_size:
            result.append(' file_size=' + repr(self.file_size))
        if (not isdir or self.compress_size) and (self.compress_type != ZIP_STORED or self.file_size != self.compress_size):
            result.append(' compress_size=' + repr(self.compress_size))
        result.append('>')
        return ''.join(result)

    @staticmethod
    def from_file(filename: str, arcname: str | None = None, *, strict_timestamps: bool = True) -> "ZipInfo":
        """A ZipInfo for a file on disk (its time, size and mode)."""
        st = os.stat(filename)
        isdir = _stat.S_ISDIR(st.st_mode)
        mtime = time.localtime(st.st_mtime)
        date_time = (mtime.tm_year, mtime.tm_mon, mtime.tm_mday, mtime.tm_hour, mtime.tm_min, mtime.tm_sec)
        if not strict_timestamps and date_time[0] < 1980:
            date_time = (1980, 1, 1, 0, 0, 0)
        name = arcname if arcname is not None else filename
        name = os.path.normpath(os.path.splitdrive(name)[1])
        while name[0:1] in ("/", os.sep):
            name = name[1:]
        if isdir:
            name += '/'
        zinfo = ZipInfo(name, date_time)
        zinfo.external_attr = (st.st_mode & 0xFFFF) << 16
        if isdir:
            zinfo.file_size = 0
            zinfo.external_attr |= 0x10
        else:
            zinfo.file_size = st.st_size
        return zinfo


class ZipExtFile:
    """A member opened for reading (its whole data)."""

    def __init__(self, name: str, data: bytes) -> None:
        self.name = name
        self._data = data
        self._pos = 0
        self._closed = False

    def read(self, n: int = -1) -> bytes:
        if n < 0:
            out = self._data[self._pos:]
        else:
            out = self._data[self._pos:self._pos + n]
        self._pos += len(out)
        return out

    def read1(self, n: int = -1) -> bytes:
        return self.read(n)

    def readline(self, limit: int = -1) -> bytes:
        nl = self._data.find(b"\n", self._pos)
        end = len(self._data) if nl < 0 else nl + 1
        if limit >= 0:
            end = min(end, self._pos + limit)
        out = self._data[self._pos:end]
        self._pos = end
        return out

    def readlines(self) -> list[bytes]:
        out: list[bytes] = []
        while True:
            line = self.readline()
            if not line:
                return out
            out.append(line)

    def __iter__(self) -> Iterator[bytes]:
        while True:
            line = self.readline()
            if not line:
                return
            yield line

    def seekable(self) -> bool:
        return True

    def seek(self, offset: int, whence: int = 0) -> int:
        if whence == 1:
            offset += self._pos
        elif whence == 2:
            offset += len(self._data)
        self._pos = max(0, min(offset, len(self._data)))
        return self._pos

    def tell(self) -> int:
        return self._pos

    def readable(self) -> bool:
        return True

    @property
    def closed(self) -> bool:
        return self._closed

    def close(self) -> None:
        self._closed = True

    def __enter__(self) -> "ZipExtFile":
        return self

    def __exit__(self, t, v, tb) -> None:
        self.close()


class _ZipWriteFile:
    """A member opened for writing: its data goes into the archive when closed."""

    def __init__(self, zf: "ZipFile", zinfo: ZipInfo) -> None:
        self._zf = zf
        self._zinfo = zinfo
        self._parts: list[bytes] = []
        self._closed = False

    def write(self, data: bytes) -> int:
        if self._closed:
            raise ValueError('I/O operation on closed file.')
        self._parts.append(bytes(data))
        return len(data)

    def writable(self) -> bool:
        return True

    @property
    def closed(self) -> bool:
        return self._closed

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._zf._add(self._zinfo, b"".join(self._parts))
        self._zf._writing = False

    def __enter__(self) -> "_ZipWriteFile":
        return self

    def __exit__(self, t, v, tb) -> None:
        self.close()


class ZipFile:
    """A ZIP archive: ZipFile(file, mode='r', compression=ZIP_STORED, allowZip64=True, compresslevel=None)."""

    def __init__(self, file: _F, mode: str = "r", compression: int = ZIP_STORED, allowZip64: bool = True,
                 compresslevel: int | None = None, *, strict_timestamps: bool = True,
                 metadata_encoding: str | None = None) -> None:
        if mode not in ('r', 'w', 'x', 'a'):
            raise ValueError("ZipFile requires mode 'r', 'w', 'x', or 'a'")
        if compression not in (ZIP_STORED, ZIP_DEFLATED):
            raise NotImplementedError("That compression method is not supported")
        self.mode = mode
        self.compression = compression
        self.compresslevel = compresslevel
        self.debug = 0
        self._comment = b""
        self.filelist: list[ZipInfo] = []
        self.NameToInfo: dict[str, ZipInfo] = {}
        self._closed = False
        self._writing = False
        self._strict_timestamps = strict_timestamps
        self.filename: str | None = _path_of(file)
        self._path = self.filename if self.filename is not None else ""
        data = _read_source(file, mode)
        self._sink: Callable[[bytes], None] = _sink_for(file, mode, self._path)
        if mode == 'r' or (mode == 'a' and data):
            self._read_directory(data)

    def _read_directory(self, data: bytes) -> None:
        end = _find_end(data)
        if end < 0:
            raise BadZipFile("File is not a zip file")
        count = _u16(data, end + 10)
        size = _u32(data, end + 12)
        offset = _u32(data, end + 16)
        clen = _u16(data, end + 20)
        self._comment = data[end + 22:end + 22 + clen]
        concat = end - size - offset                    # (bytes before the archive: a self-extracting stub)
        p = offset + concat
        for _ in range(count):
            if data[p:p + 4] != _CENTRAL_SIG:
                raise BadZipFile("Bad magic number for central directory")
            flags = _u16(data, p + 8)
            method = _u16(data, p + 10)
            t = _u16(data, p + 12)
            d = _u16(data, p + 14)
            crc = _u32(data, p + 16)
            csize = _u32(data, p + 20)
            usize = _u32(data, p + 24)
            nlen = _u16(data, p + 28)
            xlen = _u16(data, p + 30)
            klen = _u16(data, p + 32)
            raw = data[p + 46:p + 46 + nlen]
            name = raw.decode("utf-8") if flags & 0x800 or raw.isascii() else raw.decode("cp437")
            zi = ZipInfo(name, ((d >> 9) + 1980, (d >> 5) & 0xF, d & 0x1F, t >> 11, (t >> 5) & 0x3F, (t & 0x1F) * 2))
            zi.create_version = data[p + 4]
            zi.create_system = data[p + 5]
            zi.extract_version = data[p + 6]
            zi.reserved = data[p + 7]
            zi.flag_bits = flags
            zi.compress_type = method
            zi.CRC = crc
            zi.compress_size = csize
            zi.file_size = usize
            zi.volume = _u16(data, p + 34)
            zi.internal_attr = _u16(data, p + 36)
            zi.external_attr = _u32(data, p + 38)
            zi.header_offset = _u32(data, p + 42) + concat
            zi.extra = data[p + 46 + nlen:p + 46 + nlen + xlen]
            zi.comment = data[p + 46 + nlen + xlen:p + 46 + nlen + xlen + klen]
            h = zi.header_offset
            if data[h:h + 4] != _LOCAL_SIG:
                raise BadZipFile("Bad magic number for file header")
            start = h + 30 + _u16(data, h + 26) + _u16(data, h + 28)
            zi._data = data[start:start + csize]
            self.filelist.append(zi)
            self.NameToInfo[zi.filename] = zi
            p += 46 + nlen + xlen + klen

    @property
    def comment(self) -> bytes:
        return self._comment

    @comment.setter
    def comment(self, comment: bytes) -> None:
        self._comment = comment[:ZIP_MAX_COMMENT]

    def namelist(self) -> list[str]:
        """The names of the members."""
        return [zi.filename for zi in self.filelist]

    def infolist(self) -> list[ZipInfo]:
        """The ZipInfo of each member."""
        return list(self.filelist)

    def getinfo(self, name: str) -> ZipInfo:
        """The ZipInfo of the member called name."""
        info = self.NameToInfo.get(name)
        if info is None:
            raise KeyError("There is no item named " + repr(name) + " in the archive")
        return info

    def printdir(self, file: _D = None) -> None:
        """Prints a table of contents for the zip file."""
        print("%-46s %19s %12s" % ("File Name", "Modified    ", "Size"))
        for zinfo in self.filelist:
            dt = zinfo.date_time
            date = f"{dt[0]:d}-{dt[1]:02d}-{dt[2]:02d} {dt[3]:02d}:{dt[4]:02d}:{dt[5]:02d}"
            print(f"{zinfo.filename:<46s} {date:s} {zinfo.file_size:12d}")

    def _check_open(self) -> None:
        if self._closed:
            raise ValueError('Attempt to use ZIP archive that was already closed')

    def _member_data(self, zinfo: ZipInfo) -> bytes:
        if zinfo.flag_bits & 0x1:
            raise NotImplementedError("That compression method is not supported")
        if zinfo.compress_type == ZIP_STORED:
            out = zinfo._data
        elif zinfo.compress_type == ZIP_DEFLATED:
            out = zlib.decompress(zinfo._data, -15)
        else:
            raise NotImplementedError("That compression method is not supported")
        if zlib.crc32(out) != zinfo.CRC:
            raise BadZipFile("Bad CRC-32 for file " + repr(zinfo.filename))
        return out

    def read(self, name: _F, pwd: bytes | None = None) -> bytes:
        """The data of the member (a name or a ZipInfo)."""
        self._check_open()
        if isinstance(name, str):
            return self._member_data(self.getinfo(name))
        else:
            return self._member_data(name)

    def open(self, name: _F, mode: str = "r", pwd: bytes | None = None, *, force_zip64: bool = False) -> ZipExtFile:
        """A member opened for reading (mode "r") or writing ("w")."""
        if not sys._compiled:
            if mode == "w":
                return self._open_w(name)
        self._check_open()
        if mode != "r":
            raise ValueError('open() requires mode "r" or "w"')
        if isinstance(name, str):
            zi = self.getinfo(name)
        else:
            zi = name
        return ZipExtFile(zi.filename, self._member_data(zi))

    def _open_w(self, name: _F, mode: str = "w", pwd: bytes | None = None, *, force_zip64: bool = False) -> _ZipWriteFile:
        """A member opened for writing (zf.open(name, "w"))."""
        self._check_open()
        if self.mode == 'r':
            raise ValueError("write() requires mode 'w', 'x', or 'a'")
        if self._writing:
            raise ValueError("Can't write to the ZIP file while there is another write handle open on it. "
                             "Close the first handle before opening another.")
        if isinstance(name, str):
            zi = ZipInfo(name, _now())
            zi.compress_type = self.compression
            zi.external_attr = 0o600 << 16
        else:
            zi = name
        self._writing = True
        return _ZipWriteFile(self, zi)

    def extract(self, member: _F, path: str | None = None, pwd: bytes | None = None) -> str:
        """Extracts a member into the directory path (the current one): the path written."""
        if isinstance(member, str):
            zi = self.getinfo(member)
        else:
            zi = member
        return self._extract_member(zi, path if path is not None else os.getcwd())

    def extractall(self, path: str | None = None, members: list[str] | None = None, pwd: bytes | None = None) -> None:
        """Extracts all members (or those named) into the directory path (the current one)."""
        base = path if path is not None else os.getcwd()
        names = members if members is not None else self.namelist()
        for name in names:
            self._extract_member(self.getinfo(name), base)

    def _extract_member(self, member: ZipInfo, targetpath: str) -> str:
        arcname = member.filename.replace('/', os.sep)
        arcname = os.path.splitdrive(arcname)[1]
        parts = [x for x in arcname.split(os.sep) if x not in ('', os.curdir, os.pardir)]
        arcname = os.sep.join(parts)
        target = os.path.join(targetpath, arcname)
        target = os.path.normpath(target)
        upperdirs = os.path.dirname(target)
        if upperdirs and not os.path.exists(upperdirs):
            os.makedirs(upperdirs)
        if member.is_dir():
            if not os.path.isdir(target):
                os.mkdir(target)
            return target
        with io.open(target, "wb") as f:
            f.write(self._member_data(member))
        return target

    def testzip(self) -> str | None:
        """The name of the first bad member (None: all good)."""
        for zinfo in self.filelist:
            try:
                self._member_data(zinfo)
            except BadZipFile:
                return zinfo.filename
        return None

    def _add(self, zinfo: ZipInfo, data: bytes) -> None:
        if self.mode == 'r':
            raise ValueError("write() requires mode 'w', 'x', or 'a'")
        zinfo.file_size = len(data)
        zinfo.CRC = zlib.crc32(data)
        if zinfo.compress_type == ZIP_DEFLATED:
            lv = zinfo.compress_level
            if lv is None:
                lv = self.compresslevel if self.compresslevel is not None else 6
            zinfo._data = zlib.compress(data, lv, -15)
        else:
            zinfo._data = data
        zinfo.compress_size = len(zinfo._data)
        zinfo.flag_bits |= 0x800 if not zinfo.filename.isascii() else 0
        if zinfo.filename in self.NameToInfo:
            pass
        self.filelist.append(zinfo)
        self.NameToInfo[zinfo.filename] = zinfo

    def write(self, filename: str, arcname: str | None = None, compress_type: int | None = None,
              compresslevel: int | None = None) -> None:
        """Puts the file filename (or a directory) into the archive as arcname."""
        self._check_open()
        zinfo = ZipInfo.from_file(filename, arcname, strict_timestamps=self._strict_timestamps)
        if zinfo.is_dir():
            zinfo.compress_size = 0
            zinfo.CRC = 0
            self._add(zinfo, b"")
            return
        zinfo.compress_type = compress_type if compress_type is not None else self.compression
        zinfo.compress_level = compresslevel if compresslevel is not None else self.compresslevel
        with io.open(filename, "rb") as f:
            self._add(zinfo, f.read())

    def writestr(self, zinfo_or_arcname: _F, data: _D, compress_type: int | None = None,
                 compresslevel: int | None = None) -> None:
        """Puts data (bytes, or str: UTF-8) into the archive as a member (a name or a ZipInfo)."""
        self._check_open()
        if isinstance(data, str):
            b = data.encode("utf-8")
        else:
            b = bytes(data)
        if isinstance(zinfo_or_arcname, str):
            zinfo = ZipInfo(zinfo_or_arcname, _now())
            zinfo.compress_type = self.compression
            zinfo.compress_level = self.compresslevel
            if zinfo.filename.endswith('/'):
                zinfo.external_attr = 0o40775 << 16
                zinfo.external_attr |= 0x10
            else:
                zinfo.external_attr = 0o600 << 16
        else:
            zinfo = zinfo_or_arcname
        if compress_type is not None:
            zinfo.compress_type = compress_type
        if compresslevel is not None:
            zinfo.compress_level = compresslevel
        self._add(zinfo, b)

    def mkdir(self, zinfo_or_directory_name: str, mode: int = 511) -> None:
        """A directory member."""
        name = zinfo_or_directory_name
        if not name.endswith('/'):
            name += '/'
        zinfo = ZipInfo(name, _now())
        zinfo.external_attr = ((0o40000 | mode) & 0xFFFF) << 16
        zinfo.external_attr |= 0x10
        self._add(zinfo, b"")

    def _archive(self) -> bytes:
        out: list[bytes] = []
        pos = 0
        central: list[bytes] = []
        for zi in self.filelist:
            name = zi.filename.encode("utf-8") if zi.flag_bits & 0x800 else zi.filename.encode("ascii")
            dosdate, dostime = zi._dos_time()
            zi.header_offset = pos
            version = max(zi.extract_version, 20 if zi.compress_type == ZIP_DEFLATED else 10)
            local = (_LOCAL_SIG + _p16(version) + _p16(zi.flag_bits) + _p16(zi.compress_type) + _p16(dostime) +
                     _p16(dosdate) + _p32(zi.CRC) + _p32(zi.compress_size) + _p32(zi.file_size) + _p16(len(name)) +
                     _p16(len(zi.extra)) + name + zi.extra)
            out.append(local)
            out.append(zi._data)
            pos += len(local) + len(zi._data)
            central.append(_CENTRAL_SIG + bytes([zi.create_version, zi.create_system]) + _p16(version) +
                           _p16(zi.flag_bits) + _p16(zi.compress_type) + _p16(dostime) + _p16(dosdate) + _p32(zi.CRC) +
                           _p32(zi.compress_size) + _p32(zi.file_size) + _p16(len(name)) + _p16(len(zi.extra)) +
                           _p16(len(zi.comment)) + _p16(0) + _p16(zi.internal_attr) + _p32(zi.external_attr) +
                           _p32(zi.header_offset) + name + zi.extra + zi.comment)
        cd = b"".join(central)
        end = (_END_SIG + _p16(0) + _p16(0) + _p16(len(self.filelist)) + _p16(len(self.filelist)) + _p32(len(cd)) +
               _p32(pos) + _p16(len(self._comment)) + self._comment)
        return b"".join(out) + cd + end

    def close(self) -> None:
        """Closes the archive (and writes it, when made for writing)."""
        if self._closed:
            return
        self._closed = True
        if self.mode != 'r':
            self._sink(self._archive())

    def __enter__(self) -> "ZipFile":
        return self

    def __exit__(self, t, v, tb) -> None:
        self.close()

    def __repr__(self) -> str:
        r = ['<' + type(self).__name__]
        if self.filename is not None:
            r.append(' filename=' + repr(self.filename))
        r.append(' mode=' + repr(self.mode))
        if self._closed:
            r.append(' [closed]')
        r.append('>')
        return ''.join(r)


def _no_sink(b: bytes) -> None:
    pass


def _path_of(file: _F) -> str | None:
    if isinstance(file, str):
        p: str = file + ""
        return p
    else:
        return None


def _read_source(file: _F, mode: str) -> bytes:
    """The archive's bytes (none for a new one)."""
    if hasattr(file, "read"):
        if mode == 'r' or mode == 'a':
            return file.read()
        return b""
    else:
        name = str(file)
        if mode == 'r' or mode == 'a':
            try:
                with io.open(name, "rb") as f:
                    return f.read()
            except FileNotFoundError:
                if mode == 'r':
                    raise
        elif mode == 'x' and os.path.exists(name):
            raise FileExistsError(17, "File exists", name)
        return b""


def _sink_for(file: _F, mode: str, path: str) -> Callable[[bytes], None]:
    """Where close() writes the archive."""
    if hasattr(file, "write"):
        if mode == 'r':
            return _no_sink

        def to_obj(b: bytes) -> None:
            file.write(b)
        return to_obj
    else:
        def to_path(b: bytes) -> None:
            with io.open(path, "wb") as f:
                f.write(b)
        return to_path


def _now() -> tuple[int, int, int, int, int, int]:
    t = time.localtime(time.time())
    return t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec
