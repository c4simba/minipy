"""Read from and write to tar format archives (CPython's tarfile).

    with tarfile.open("x.tar.gz", "w:gz") as t:
        t.add("src")
    with tarfile.open("x.tar.gz") as t:
        t.extractall("out")

Formats: ustar, GNU and pax (the default for writing); gzip compression (bz2, lzma and zstd
are not available here).

Compiled programs: the same formats and messages, with archives kept in memory (one opened
for reading is read whole; one opened for writing is written when closed). TarInfo.mtime
is a float; extraction filters are the named ones ('data', the default, 'tar',
'fully_trusted'); add(filter=f) takes a function. Not there: sparse files, the stream
modes' streaming (r|... and w|... work like r:... and w:...), the command line."""
import sys

if not sys._compiled:

    version     = "0.9.0"
    __author__  = "Lars Gust\u00e4bel (lars@gustaebel.de)"
    __credits__ = "Gustavo Niemeyer, Niels Gust\u00e4bel, Richard Townsend."

    #---------
    # Imports
    #---------
    from builtins import open as bltn_open
    import sys
    import os
    import io
    import shutil
    import stat
    import time
    import struct
    import copy
    import re

    try:
        import pwd
    except ImportError:
        pwd = None
    try:
        import grp
    except ImportError:
        grp = None

    # os.symlink on Windows prior to 6.0 raises NotImplementedError
    # OSError (winerror=1314) will be raised if the caller does not hold the
    # SeCreateSymbolicLinkPrivilege privilege
    symlink_exception = (AttributeError, NotImplementedError, OSError)

    # from tarfile import *
    __all__ = ["TarFile", "TarInfo", "is_tarfile", "TarError", "ReadError",
               "CompressionError", "StreamError", "ExtractError", "HeaderError",
               "ENCODING", "USTAR_FORMAT", "GNU_FORMAT", "PAX_FORMAT",
               "DEFAULT_FORMAT", "open","fully_trusted_filter", "data_filter",
               "tar_filter", "FilterError", "AbsoluteLinkError",
               "OutsideDestinationError", "SpecialFileError", "AbsolutePathError",
               "LinkOutsideDestinationError", "LinkFallbackError"]


    #---------------------------------------------------------
    # tar constants
    #---------------------------------------------------------
    NUL = b"\0"                     # the null character
    BLOCKSIZE = 512                 # length of processing blocks
    RECORDSIZE = BLOCKSIZE * 20     # length of records
    GNU_MAGIC = b"ustar  \0"        # magic gnu tar string
    POSIX_MAGIC = b"ustar\x0000"    # magic posix tar string

    LENGTH_NAME = 100               # maximum length of a filename
    LENGTH_LINK = 100               # maximum length of a linkname
    LENGTH_PREFIX = 155             # maximum length of the prefix field

    REGTYPE = b"0"                  # regular file
    AREGTYPE = b"\0"                # regular file
    LNKTYPE = b"1"                  # link (inside tarfile)
    SYMTYPE = b"2"                  # symbolic link
    CHRTYPE = b"3"                  # character special device
    BLKTYPE = b"4"                  # block special device
    DIRTYPE = b"5"                  # directory
    FIFOTYPE = b"6"                 # fifo special device
    CONTTYPE = b"7"                 # contiguous file

    GNUTYPE_LONGNAME = b"L"         # GNU tar longname
    GNUTYPE_LONGLINK = b"K"         # GNU tar longlink
    GNUTYPE_SPARSE = b"S"           # GNU tar sparse file

    XHDTYPE = b"x"                  # POSIX.1-2001 extended header
    XGLTYPE = b"g"                  # POSIX.1-2001 global header
    SOLARIS_XHDTYPE = b"X"          # Solaris extended header

    USTAR_FORMAT = 0                # POSIX.1-1988 (ustar) format
    GNU_FORMAT = 1                  # GNU tar format
    PAX_FORMAT = 2                  # POSIX.1-2001 (pax) format
    DEFAULT_FORMAT = PAX_FORMAT

    #---------------------------------------------------------
    # tarfile constants
    #---------------------------------------------------------
    # File types that tarfile supports:
    SUPPORTED_TYPES = (REGTYPE, AREGTYPE, LNKTYPE,
                       SYMTYPE, DIRTYPE, FIFOTYPE,
                       CONTTYPE, CHRTYPE, BLKTYPE,
                       GNUTYPE_LONGNAME, GNUTYPE_LONGLINK,
                       GNUTYPE_SPARSE)

    # File types that will be treated as a regular file.
    REGULAR_TYPES = (REGTYPE, AREGTYPE,
                     CONTTYPE, GNUTYPE_SPARSE)

    # File types that are part of the GNU tar format.
    GNU_TYPES = (GNUTYPE_LONGNAME, GNUTYPE_LONGLINK,
                 GNUTYPE_SPARSE)

    # Fields from a pax header that override a TarInfo attribute.
    PAX_FIELDS = ("path", "linkpath", "size", "mtime",
                  "uid", "gid", "uname", "gname")

    # Fields from a pax header that are affected by hdrcharset.
    PAX_NAME_FIELDS = {"path", "linkpath", "uname", "gname"}

    # Fields in a pax header that are numbers, all other fields
    # are treated as strings.
    PAX_NUMBER_FIELDS = {
        "atime": float,
        "ctime": float,
        "mtime": float,
        "uid": int,
        "gid": int,
        "size": int
    }

    #---------------------------------------------------------
    # initialization
    #---------------------------------------------------------
    if os.name == "nt":
        ENCODING = "utf-8"
    else:
        ENCODING = sys.getfilesystemencoding()

    #---------------------------------------------------------
    # Some useful functions
    #---------------------------------------------------------

    def stn(s, length, encoding, errors):
        """Convert a string to a null-terminated bytes object.
        """
        if s is None:
            raise ValueError("metadata cannot contain None")
        s = s.encode(encoding, errors)
        return s[:length] + (length - len(s)) * NUL

    def nts(s, encoding, errors):
        """Convert a null-terminated bytes object to a string.
        """
        p = s.find(b"\0")
        if p != -1:
            s = s[:p]
        return s.decode(encoding, errors)

    def nti(s):
        """Convert a number field to a python number.
        """
        # There are two possible encodings for a number field, see
        # itn() below.
        if s[0] in (0o200, 0o377):
            n = 0
            for i in range(len(s) - 1):
                n <<= 8
                n += s[i + 1]
            if s[0] == 0o377:
                n = -(256 ** (len(s) - 1) - n)
        else:
            try:
                s = nts(s, "ascii", "strict")
                n = int(s.strip() or "0", 8)
            except ValueError:
                raise InvalidHeaderError("invalid header")
        return n

    def itn(n, digits=8, format=DEFAULT_FORMAT):
        """Convert a python number to a number field.
        """
        # POSIX 1003.1-1988 requires numbers to be encoded as a string of
        # octal digits followed by a null-byte, this allows values up to
        # (8**(digits-1))-1. GNU tar allows storing numbers greater than
        # that if necessary. A leading 0o200 or 0o377 byte indicate this
        # particular encoding, the following digits-1 bytes are a big-endian
        # base-256 representation. This allows values up to (256**(digits-1))-1.
        # A 0o200 byte indicates a positive number, a 0o377 byte a negative
        # number.
        original_n = n
        n = int(n)
        if 0 <= n < 8 ** (digits - 1):
            s = bytes("%0*o" % (digits - 1, n), "ascii") + NUL
        elif format == GNU_FORMAT and -256 ** (digits - 1) <= n < 256 ** (digits - 1):
            if n >= 0:
                s = bytearray([0o200])
            else:
                s = bytearray([0o377])
                n = 256 ** digits + n

            for i in range(digits - 1):
                s.insert(1, n & 0o377)
                n >>= 8
        else:
            raise ValueError("overflow in number field")

        return s

    def calc_chksums(buf):
        """Calculate the checksum for a member's header by summing up all
           characters except for the chksum field which is treated as if
           it was filled with spaces. According to the GNU tar sources,
           some tars (Sun and NeXT) calculate chksum with signed char,
           which will be different if there are chars in the buffer with
           the high bit set. So we calculate two checksums, unsigned and
           signed.
        """
        unsigned_chksum = 256 + sum(struct.unpack_from("148B8x356B", buf))
        signed_chksum = 256 + sum(struct.unpack_from("148b8x356b", buf))
        return unsigned_chksum, signed_chksum

    def copyfileobj(src, dst, length=None, exception=OSError, bufsize=None):
        """Copy length bytes from fileobj src to fileobj dst.
           If length is None, copy the entire content.
        """
        bufsize = bufsize or 16 * 1024
        if length == 0:
            return
        if length is None:
            shutil.copyfileobj(src, dst, bufsize)
            return

        blocks, remainder = divmod(length, bufsize)
        for b in range(blocks):
            buf = src.read(bufsize)
            if len(buf) < bufsize:
                raise exception("unexpected end of data")
            dst.write(buf)

        if remainder != 0:
            buf = src.read(remainder)
            if len(buf) < remainder:
                raise exception("unexpected end of data")
            dst.write(buf)
        return

    def _safe_print(s):
        encoding = getattr(sys.stdout, 'encoding', None)
        if encoding is not None:
            s = s.encode(encoding, 'backslashreplace').decode(encoding)
        print(s, end=' ')


    class TarError(Exception):
        """Base exception."""
        pass
    class ExtractError(TarError):
        """General exception for extract errors."""
        pass
    class ReadError(TarError):
        """Exception for unreadable tar archives."""
        pass
    class CompressionError(TarError):
        """Exception for unavailable compression methods."""
        pass
    class StreamError(TarError):
        """Exception for unsupported operations on stream-like TarFiles."""
        pass
    class HeaderError(TarError):
        """Base exception for header errors."""
        pass
    class EmptyHeaderError(HeaderError):
        """Exception for empty headers."""
        pass
    class TruncatedHeaderError(HeaderError):
        """Exception for truncated headers."""
        pass
    class EOFHeaderError(HeaderError):
        """Exception for end of file headers."""
        pass
    class InvalidHeaderError(HeaderError):
        """Exception for invalid headers."""
        pass
    class SubsequentHeaderError(HeaderError):
        """Exception for missing and invalid extended headers."""
        pass

    #---------------------------
    # internal stream interface
    #---------------------------
    class _LowLevelFile:
        """Low-level file object. Supports reading and writing.
           It is used instead of a regular file object for streaming
           access.
        """

        def __init__(self, name, mode):
            mode = {
                "r": os.O_RDONLY,
                "w": os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
            }[mode]
            if hasattr(os, "O_BINARY"):
                mode |= os.O_BINARY
            self.fd = os.open(name, mode, 0o666)

        def close(self):
            os.close(self.fd)

        def read(self, size):
            return os.read(self.fd, size)

        def write(self, s):
            os.write(self.fd, s)

    class _Stream:
        """Class that serves as an adapter between TarFile and
           a stream-like object.  The stream-like object only
           needs to have a read() or write() method that works with bytes,
           and the method is accessed blockwise.
           Use of gzip or bzip2 compression is possible.
           A stream-like object could be for example: sys.stdin.buffer,
           sys.stdout.buffer, a socket, a tape device etc.

           _Stream is intended to be used only internally.
        """

        def __init__(self, name, mode, comptype, fileobj, bufsize,
                     compresslevel, preset):
            """Construct a _Stream object.
            """
            self._extfileobj = True
            if fileobj is None:
                fileobj = _LowLevelFile(name, mode)
                self._extfileobj = False

            if comptype == '*':
                # Enable transparent compression detection for the
                # stream interface
                fileobj = _StreamProxy(fileobj)
                comptype = fileobj.getcomptype()

            self.name     = os.fspath(name) if name is not None else ""
            self.mode     = mode
            self.comptype = comptype
            self.fileobj  = fileobj
            self.bufsize  = bufsize
            self.buf      = b""
            self.pos      = 0
            self.closed   = False

            try:
                if comptype == "gz":
                    try:
                        import zlib
                    except ImportError:
                        raise CompressionError("zlib module is not available") from None
                    self.zlib = zlib
                    self.crc = zlib.crc32(b"")
                    if mode == "r":
                        self.exception = zlib.error
                        self._init_read_gz()
                    else:
                        self._init_write_gz(compresslevel)

                elif comptype == "bz2":
                    try:
                        import bz2
                    except ImportError:
                        raise CompressionError("bz2 module is not available") from None
                    if mode == "r":
                        self.dbuf = b""
                        self.cmp = bz2.BZ2Decompressor()
                        self.exception = OSError
                    else:
                        self.cmp = bz2.BZ2Compressor(compresslevel)

                elif comptype == "xz":
                    try:
                        import lzma
                    except ImportError:
                        raise CompressionError("lzma module is not available") from None
                    if mode == "r":
                        self.dbuf = b""
                        self.cmp = lzma.LZMADecompressor()
                        self.exception = lzma.LZMAError
                    else:
                        self.cmp = lzma.LZMACompressor(preset=preset)
                elif comptype == "zst":
                    try:
                        from compression import zstd
                    except ImportError:
                        raise CompressionError("compression.zstd module is not available") from None
                    if mode == "r":
                        self.dbuf = b""
                        self.cmp = zstd.ZstdDecompressor()
                        self.exception = zstd.ZstdError
                    else:
                        self.cmp = zstd.ZstdCompressor()
                elif comptype != "tar":
                    raise CompressionError("unknown compression type %r" % comptype)

            except:
                if not self._extfileobj:
                    self.fileobj.close()
                self.closed = True
                raise

        def __del__(self):
            if hasattr(self, "closed") and not self.closed:
                self.close()

        def _init_write_gz(self, compresslevel):
            """Initialize for writing with gzip compression.
            """
            self.cmp = self.zlib.compressobj(compresslevel,
                                             self.zlib.DEFLATED,
                                             -self.zlib.MAX_WBITS,
                                             self.zlib.DEF_MEM_LEVEL,
                                             0)
            timestamp = struct.pack("<L", int(time.time()))
            self.__write(b"\037\213\010\010" + timestamp + b"\002\377")
            if self.name.endswith(".gz"):
                self.name = self.name[:-3]
            # Honor "directory components removed" from RFC1952
            self.name = os.path.basename(self.name)
            # RFC1952 says we must use ISO-8859-1 for the FNAME field.
            self.__write(self.name.encode("iso-8859-1", "replace") + NUL)

        def write(self, s):
            """Write string s to the stream.
            """
            if self.comptype == "gz":
                self.crc = self.zlib.crc32(s, self.crc)
            self.pos += len(s)
            if self.comptype != "tar":
                s = self.cmp.compress(s)
            self.__write(s)

        def __write(self, s):
            """Write string s to the stream if a whole new block
               is ready to be written.
            """
            self.buf += s
            while len(self.buf) > self.bufsize:
                self.fileobj.write(self.buf[:self.bufsize])
                self.buf = self.buf[self.bufsize:]

        def close(self):
            """Close the _Stream object. No operation should be
               done on it afterwards.
            """
            if self.closed:
                return

            self.closed = True
            try:
                if self.mode == "w" and self.comptype != "tar":
                    self.buf += self.cmp.flush()

                if self.mode == "w" and self.buf:
                    self.fileobj.write(self.buf)
                    self.buf = b""
                    if self.comptype == "gz":
                        self.fileobj.write(struct.pack("<L", self.crc))
                        self.fileobj.write(struct.pack("<L", self.pos & 0xffffFFFF))
            finally:
                if not self._extfileobj:
                    self.fileobj.close()

        def _init_read_gz(self):
            """Initialize for reading a gzip compressed fileobj.
            """
            self.cmp = self.zlib.decompressobj(-self.zlib.MAX_WBITS)
            self.dbuf = b""

            # taken from gzip.GzipFile with some alterations
            if self.__read(2) != b"\037\213":
                raise ReadError("not a gzip file")
            if self.__read(1) != b"\010":
                raise CompressionError("unsupported compression method")

            flag = ord(self.__read(1))
            self.__read(6)

            if flag & 4:
                xlen = ord(self.__read(1)) + 256 * ord(self.__read(1))
                self.__read(xlen)
            if flag & 8:
                while True:
                    s = self.__read(1)
                    if not s or s == NUL:
                        break
            if flag & 16:
                while True:
                    s = self.__read(1)
                    if not s or s == NUL:
                        break
            if flag & 2:
                self.__read(2)

        def tell(self):
            """Return the stream's file pointer position.
            """
            return self.pos

        def seek(self, pos=0):
            """Set the stream's file pointer to pos. Negative seeking
               is forbidden.
            """
            if pos - self.pos >= 0:
                blocks, remainder = divmod(pos - self.pos, self.bufsize)
                for i in range(blocks):
                    self.read(self.bufsize)
                self.read(remainder)
            else:
                raise StreamError("seeking backwards is not allowed")
            return self.pos

        def read(self, size):
            """Return the next size number of bytes from the stream."""
            assert size is not None
            buf = self._read(size)
            self.pos += len(buf)
            return buf

        def _read(self, size):
            """Return size bytes from the stream.
            """
            if self.comptype == "tar":
                return self.__read(size)

            c = len(self.dbuf)
            t = [self.dbuf]
            while c < size:
                # Skip underlying buffer to avoid unaligned double buffering.
                if self.buf:
                    buf = self.buf
                    self.buf = b""
                else:
                    buf = self.fileobj.read(self.bufsize)
                    if not buf:
                        break
                try:
                    buf = self.cmp.decompress(buf)
                except self.exception as e:
                    raise ReadError("invalid compressed data") from e
                t.append(buf)
                c += len(buf)
            t = b"".join(t)
            self.dbuf = t[size:]
            return t[:size]

        def __read(self, size):
            """Return size bytes from stream. If internal buffer is empty,
               read another block from the stream.
            """
            c = len(self.buf)
            t = [self.buf]
            while c < size:
                buf = self.fileobj.read(self.bufsize)
                if not buf:
                    break
                t.append(buf)
                c += len(buf)
            t = b"".join(t)
            self.buf = t[size:]
            return t[:size]
    # class _Stream

    class _StreamProxy(object):
        """Small proxy class that enables transparent compression
           detection for the Stream interface (mode 'r|*').
        """

        def __init__(self, fileobj):
            self.fileobj = fileobj
            self.buf = self.fileobj.read(BLOCKSIZE)

        def read(self, size):
            self.read = self.fileobj.read
            return self.buf

        def getcomptype(self):
            if self.buf.startswith(b"\x1f\x8b\x08"):
                return "gz"
            elif self.buf[0:3] == b"BZh" and self.buf[4:10] == b"1AY&SY":
                return "bz2"
            elif self.buf.startswith((b"\x5d\x00\x00\x80", b"\xfd7zXZ")):
                return "xz"
            elif self.buf.startswith(b"\x28\xb5\x2f\xfd"):
                return "zst"
            else:
                return "tar"

        def close(self):
            self.fileobj.close()
    # class StreamProxy

    #------------------------
    # Extraction file object
    #------------------------
    class _FileInFile(object):
        """A thin wrapper around an existing file object that
           provides a part of its data as an individual file
           object.
        """

        def __init__(self, fileobj, offset, size, name, blockinfo=None):
            self.fileobj = fileobj
            self.offset = offset
            self.size = size
            self.position = 0
            self.name = name
            self.closed = False

            if blockinfo is None:
                blockinfo = [(0, size)]

            # Construct a map with data and zero blocks.
            self.map_index = 0
            self.map = []
            lastpos = 0
            realpos = self.offset
            for offset, size in blockinfo:
                if offset > lastpos:
                    self.map.append((False, lastpos, offset, None))
                self.map.append((True, offset, offset + size, realpos))
                realpos += size
                lastpos = offset + size
            if lastpos < self.size:
                self.map.append((False, lastpos, self.size, None))

        def flush(self):
            pass

        @property
        def mode(self):
            return 'rb'

        def readable(self):
            return True

        def writable(self):
            return False

        def seekable(self):
            return self.fileobj.seekable()

        def tell(self):
            """Return the current file position.
            """
            return self.position

        def seek(self, position, whence=io.SEEK_SET):
            """Seek to a position in the file.
            """
            if whence == io.SEEK_SET:
                self.position = min(max(position, 0), self.size)
            elif whence == io.SEEK_CUR:
                if position < 0:
                    self.position = max(self.position + position, 0)
                else:
                    self.position = min(self.position + position, self.size)
            elif whence == io.SEEK_END:
                self.position = max(min(self.size + position, self.size), 0)
            else:
                raise ValueError("Invalid argument")
            return self.position

        def read(self, size=None):
            """Read data from the file.
            """
            if size is None:
                size = self.size - self.position
            else:
                size = min(size, self.size - self.position)

            buf = b""
            while size > 0:
                while True:
                    data, start, stop, offset = self.map[self.map_index]
                    if start <= self.position < stop:
                        break
                    else:
                        self.map_index += 1
                        if self.map_index == len(self.map):
                            self.map_index = 0
                length = min(size, stop - self.position)
                if data:
                    self.fileobj.seek(offset + (self.position - start))
                    b = self.fileobj.read(length)
                    if len(b) != length:
                        raise ReadError("unexpected end of data")
                    buf += b
                else:
                    buf += NUL * length
                size -= length
                self.position += length
            return buf

        def readinto(self, b):
            buf = self.read(len(b))
            b[:len(buf)] = buf
            return len(buf)

        def close(self):
            self.closed = True
    #class _FileInFile

    class ExFileObject:
        """The file object extractfile() gives: a member's data (read-only, seekable)."""

        def __init__(self, tarfile, tarinfo):
            self.raw = _FileInFile(tarfile.fileobj, tarinfo.offset_data,
                    tarinfo.size, tarinfo.name, tarinfo.sparse)
            self.name = tarinfo.name
            self.mode = 'rb'

        @property
        def closed(self):
            return self.raw.closed

        def _check(self):
            if self.raw.closed:
                raise ValueError("read of closed file")

        def read(self, size=-1):
            self._check()
            if size is None or size < 0:
                return self.raw.read()
            return self.raw.read(size)

        read1 = read

        def readinto(self, b):
            self._check()
            return self.raw.readinto(b)

        def peek(self, size=0):
            self._check()
            pos = self.raw.tell()
            data = self.raw.read(max(size, 1))
            self.raw.seek(pos)
            return data

        def readline(self, size=-1):
            self._check()
            out = b""
            while size is None or size < 0 or len(out) < size:
                pos = self.raw.tell()
                chunk = self.raw.read(1024)
                if not chunk:
                    break
                nl = chunk.find(b"\n")
                if nl >= 0:
                    chunk = chunk[:nl + 1]
                if size is not None and size >= 0 and len(out) + len(chunk) > size:
                    chunk = chunk[:size - len(out)]
                out += chunk
                self.raw.seek(pos + len(chunk))
                if chunk.endswith(b"\n"):
                    break
            return out

        def readlines(self, hint=-1):
            lines = []
            total = 0
            for line in self:
                lines.append(line)
                total += len(line)
                if hint is not None and 0 < hint <= total:
                    break
            return lines

        def __iter__(self):
            return self

        def __next__(self):
            line = self.readline()
            if not line:
                raise StopIteration
            return line

        def seek(self, pos, whence=io.SEEK_SET):
            self._check()
            return self.raw.seek(pos, whence)

        def tell(self):
            self._check()
            return self.raw.tell()

        def readable(self):
            return True

        def writable(self):
            return False

        def seekable(self):
            return self.raw.seekable()

        def flush(self):
            pass

        def close(self):
            self.raw.close()

        def __enter__(self):
            return self

        def __exit__(self, *args):
            self.close()

        def __repr__(self):
            return "<ExFileObject name=%r>" % (self.name,)
    #class ExFileObject


    #-----------------------------
    # extraction filters (PEP 706)
    #-----------------------------

    class FilterError(TarError):
        pass

    class AbsolutePathError(FilterError):
        def __init__(self, tarinfo):
            self.tarinfo = tarinfo
            super().__init__(f'member {tarinfo.name!r} has an absolute path')

    class OutsideDestinationError(FilterError):
        def __init__(self, tarinfo, path):
            self.tarinfo = tarinfo
            self._path = path
            super().__init__(f'{tarinfo.name!r} would be extracted to {path!r}, '
                             + 'which is outside the destination')

    class SpecialFileError(FilterError):
        def __init__(self, tarinfo):
            self.tarinfo = tarinfo
            super().__init__(f'{tarinfo.name!r} is a special file')

    class AbsoluteLinkError(FilterError):
        def __init__(self, tarinfo):
            self.tarinfo = tarinfo
            super().__init__(f'{tarinfo.name!r} is a link to an absolute path')

    class LinkOutsideDestinationError(FilterError):
        def __init__(self, tarinfo, path):
            self.tarinfo = tarinfo
            self._path = path
            super().__init__(f'{tarinfo.name!r} would link to {path!r}, '
                             + 'which is outside the destination')

    class LinkFallbackError(FilterError):
        def __init__(self, tarinfo, path):
            self.tarinfo = tarinfo
            self._path = path
            super().__init__(f'link {tarinfo.name!r} would be extracted as a '
                             + f'copy of {path!r}, which was rejected')

    # Errors caused by filters -- both "fatal" and "non-fatal" -- that
    # we consider to be issues with the argument, rather than a bug in the
    # filter function
    _FILTER_ERRORS = (FilterError, OSError, ExtractError)

    def _get_filtered_attrs(member, dest_path, for_data=True):
        new_attrs = {}
        name = member.name
        dest_path = os.path.realpath(dest_path, strict=os.path.ALLOW_MISSING)
        # Strip leading / (tar's directory separator) from filenames.
        # Include os.sep (target OS directory separator) as well.
        if name.startswith(('/', os.sep)):
            name = new_attrs['name'] = member.path.lstrip('/' + os.sep)
        if os.path.isabs(name):
            # Path is absolute even after stripping.
            # For example, 'C:/foo' on Windows.
            raise AbsolutePathError(member)
        # Ensure we stay in the destination
        target_path = os.path.realpath(os.path.join(dest_path, name),
                                       strict=os.path.ALLOW_MISSING)
        if os.path.commonpath([target_path, dest_path]) != dest_path:
            raise OutsideDestinationError(member, target_path)
        # Limit permissions (no high bits, and go-w)
        mode = member.mode
        if mode is not None:
            # Strip high bits & group/other write bits
            mode = mode & 0o755
            if for_data:
                # For data, handle permissions & file types
                if member.isreg() or member.islnk():
                    if not mode & 0o100:
                        # Clear executable bits if not executable by user
                        mode &= ~0o111
                    # Ensure owner can read & write
                    mode |= 0o600
                elif member.isdir() or member.issym():
                    # Ignore mode for directories & symlinks
                    mode = None
                else:
                    # Reject special files
                    raise SpecialFileError(member)
            if mode != member.mode:
                new_attrs['mode'] = mode
        if for_data:
            # Ignore ownership for 'data'
            if member.uid is not None:
                new_attrs['uid'] = None
            if member.gid is not None:
                new_attrs['gid'] = None
            if member.uname is not None:
                new_attrs['uname'] = None
            if member.gname is not None:
                new_attrs['gname'] = None
            # Check link destination for 'data'
            if member.islnk() or member.issym():
                if os.path.isabs(member.linkname):
                    raise AbsoluteLinkError(member)
                # A link member that resolves to the destination directory itself
                # would replace it with a (sym)link, redirecting the destination
                # for all subsequent members.
                if target_path == dest_path:
                    raise OutsideDestinationError(member, target_path)
                normalized = os.path.normpath(member.linkname)
                if normalized != member.linkname:
                    new_attrs['linkname'] = normalized
                if member.issym():
                    # The symlink is created at `name` with trailing separators
                    # stripped, so its target is relative to the directory
                    # containing that path.
                    link_dir = os.path.dirname(name.rstrip('/' + os.sep))
                    target_path = os.path.join(dest_path, link_dir, normalized)
                else:
                    target_path = os.path.join(dest_path, normalized)
                target_path = os.path.realpath(target_path,
                                               strict=os.path.ALLOW_MISSING)
                if os.path.commonpath([target_path, dest_path]) != dest_path:
                    raise LinkOutsideDestinationError(member, target_path)
        return new_attrs

    def fully_trusted_filter(member, dest_path):
        return member

    def tar_filter(member, dest_path):
        new_attrs = _get_filtered_attrs(member, dest_path, False)
        if new_attrs:
            return member.replace(**new_attrs, deep=False)
        return member

    def data_filter(member, dest_path):
        new_attrs = _get_filtered_attrs(member, dest_path, True)
        if new_attrs:
            return member.replace(**new_attrs, deep=False)
        return member

    _NAMED_FILTERS = {
        "fully_trusted": fully_trusted_filter,
        "tar": tar_filter,
        "data": data_filter,
    }

    #------------------
    # Exported Classes
    #------------------

    # Sentinel for replace() defaults, meaning "don't change the attribute"
    _KEEP = object()

    # Header length is digits followed by a space.
    _header_length_prefix_re = re.compile(br"([0-9]{1,20}) ")

    class TarInfo(object):
        """Informational class which holds the details about an
           archive member given by a tar header block.
           TarInfo objects are returned by TarFile.getmember(),
           TarFile.getmembers() and TarFile.gettarinfo() and are
           usually created internally.
        """

        __slots__ = dict(
            name = 'Name of the archive member.',
            mode = 'Permission bits.',
            uid = 'User ID of the user who originally stored this member.',
            gid = 'Group ID of the user who originally stored this member.',
            size = 'Size in bytes.',
            mtime = 'Time of last modification.',
            chksum = 'Header checksum.',
            type = ('File type.  type is usually one of these constants: '
                    'REGTYPE,\n'
                    'AREGTYPE, LNKTYPE, SYMTYPE, DIRTYPE, FIFOTYPE, '
                    'CONTTYPE, CHRTYPE,\n'
                    'BLKTYPE, GNUTYPE_SPARSE.'),
            linkname = ('Name of the target file name, which is only present '
                        'in TarInfo\n'
                        'objects of type LNKTYPE and SYMTYPE.'),
            uname = 'User name.',
            gname = 'Group name.',
            devmajor = 'Device major number.',
            devminor = 'Device minor number.',
            offset = 'The tar header starts here.',
            offset_data = "The file's data starts here.",
            pax_headers = ('A dictionary containing key-value pairs of an '
                           'associated pax\n'
                           'extended header.'),
            sparse = 'Sparse member information.',
            _tarfile = None,
            _sparse_structs = None,
            _link_target = None,
            )

        def __init__(self, name=""):
            """Construct a TarInfo object. name is the optional name
               of the member.
            """
            self.name = name        # member name
            self.mode = 0o644       # file permissions
            self.uid = 0            # user id
            self.gid = 0            # group id
            self.size = 0           # file size
            self.mtime = 0          # modification time
            self.chksum = 0         # header checksum
            self.type = REGTYPE     # member type
            self.linkname = ""      # link name
            self.uname = ""         # user name
            self.gname = ""         # group name
            self.devmajor = 0       # device major number
            self.devminor = 0       # device minor number

            self.offset = 0         # the tar header starts here
            self.offset_data = 0    # the file's data starts here

            self.sparse = None      # sparse member information
            self.pax_headers = {}   # pax header information

        @property
        def tarfile(self):
            import warnings
            warnings.warn(
                'The undocumented "tarfile" attribute of TarInfo objects '
                + 'is deprecated and will be removed in Python 3.16',
                DeprecationWarning, stacklevel=2)
            return self._tarfile

        @tarfile.setter
        def tarfile(self, tarfile):
            import warnings
            warnings.warn(
                'The undocumented "tarfile" attribute of TarInfo objects '
                + 'is deprecated and will be removed in Python 3.16',
                DeprecationWarning, stacklevel=2)
            self._tarfile = tarfile

        @property
        def path(self):
            'In pax headers, "name" is called "path".'
            return self.name

        @path.setter
        def path(self, name):
            self.name = name

        @property
        def linkpath(self):
            'In pax headers, "linkname" is called "linkpath".'
            return self.linkname

        @linkpath.setter
        def linkpath(self, linkname):
            self.linkname = linkname

        def __repr__(self):
            return "<%s %r at %#x>" % (self.__class__.__name__,self.name,id(self))

        def replace(self, *,
                    name=_KEEP, mtime=_KEEP, mode=_KEEP, linkname=_KEEP,
                    uid=_KEEP, gid=_KEEP, uname=_KEEP, gname=_KEEP,
                    deep=True, _KEEP=_KEEP):
            """Return a deep copy of self with the given attributes replaced.
            """
            if deep:
                result = copy.deepcopy(self)
            else:
                result = copy.copy(self)
            if name is not _KEEP:
                result.name = name
            if mtime is not _KEEP:
                result.mtime = mtime
            if mode is not _KEEP:
                result.mode = mode
            if linkname is not _KEEP:
                result.linkname = linkname
            if uid is not _KEEP:
                result.uid = uid
            if gid is not _KEEP:
                result.gid = gid
            if uname is not _KEEP:
                result.uname = uname
            if gname is not _KEEP:
                result.gname = gname
            return result

        def get_info(self):
            """Return the TarInfo's attributes as a dictionary.
            """
            if self.mode is None:
                mode = None
            else:
                mode = self.mode & 0o7777
            info = {
                "name":     self.name,
                "mode":     mode,
                "uid":      self.uid,
                "gid":      self.gid,
                "size":     self.size,
                "mtime":    self.mtime,
                "chksum":   self.chksum,
                "type":     self.type,
                "linkname": self.linkname,
                "uname":    self.uname,
                "gname":    self.gname,
                "devmajor": self.devmajor,
                "devminor": self.devminor
            }

            if info["type"] == DIRTYPE and not info["name"].endswith("/"):
                info["name"] += "/"

            return info

        def tobuf(self, format=DEFAULT_FORMAT, encoding=ENCODING, errors="surrogateescape"):
            """Return a tar header as a string of 512 byte blocks.
            """
            info = self.get_info()
            for name, value in info.items():
                if value is None:
                    raise ValueError("%s may not be None" % name)

            if format == USTAR_FORMAT:
                return self.create_ustar_header(info, encoding, errors)
            elif format == GNU_FORMAT:
                return self.create_gnu_header(info, encoding, errors)
            elif format == PAX_FORMAT:
                return self.create_pax_header(info, encoding)
            else:
                raise ValueError("invalid format")

        def create_ustar_header(self, info, encoding, errors):
            """Return the object as a ustar header block.
            """
            info["magic"] = POSIX_MAGIC

            if len(info["linkname"].encode(encoding, errors)) > LENGTH_LINK:
                raise ValueError("linkname is too long")

            if len(info["name"].encode(encoding, errors)) > LENGTH_NAME:
                info["prefix"], info["name"] = self._posix_split_name(info["name"], encoding, errors)

            return self._create_header(info, USTAR_FORMAT, encoding, errors)

        def create_gnu_header(self, info, encoding, errors):
            """Return the object as a GNU header block sequence.
            """
            info["magic"] = GNU_MAGIC

            buf = b""
            if len(info["linkname"].encode(encoding, errors)) > LENGTH_LINK:
                buf += self._create_gnu_long_header(info["linkname"], GNUTYPE_LONGLINK, encoding, errors)

            if len(info["name"].encode(encoding, errors)) > LENGTH_NAME:
                buf += self._create_gnu_long_header(info["name"], GNUTYPE_LONGNAME, encoding, errors)

            return buf + self._create_header(info, GNU_FORMAT, encoding, errors)

        def create_pax_header(self, info, encoding):
            """Return the object as a ustar header block. If it cannot be
               represented this way, prepend a pax extended header sequence
               with supplement information.
            """
            info["magic"] = POSIX_MAGIC
            pax_headers = self.pax_headers.copy()

            # Test string fields for values that exceed the field length or cannot
            # be represented in ASCII encoding.
            for name, hname, length in (
                    ("name", "path", LENGTH_NAME), ("linkname", "linkpath", LENGTH_LINK),
                    ("uname", "uname", 32), ("gname", "gname", 32)):

                if hname in pax_headers:
                    # The pax header has priority.
                    continue

                # Try to encode the string as ASCII.
                try:
                    info[name].encode("ascii", "strict")
                except UnicodeEncodeError:
                    pax_headers[hname] = info[name]
                    continue

                if len(info[name]) > length:
                    pax_headers[hname] = info[name]

            # Test number fields for values that exceed the field limit or values
            # that like to be stored as float.
            for name, digits in (("uid", 8), ("gid", 8), ("size", 12), ("mtime", 12)):
                needs_pax = False

                val = info[name]
                val_is_float = isinstance(val, float)
                val_int = round(val) if val_is_float else val
                if not 0 <= val_int < 8 ** (digits - 1):
                    # Avoid overflow.
                    info[name] = 0
                    needs_pax = True
                elif val_is_float:
                    # Put rounded value in ustar header, and full
                    # precision value in pax header.
                    info[name] = val_int
                    needs_pax = True

                # The existing pax header has priority.
                if needs_pax and name not in pax_headers:
                    pax_headers[name] = str(val)

            # Create a pax extended header if necessary.
            if pax_headers:
                buf = self._create_pax_generic_header(pax_headers, XHDTYPE, encoding)
            else:
                buf = b""

            return buf + self._create_header(info, USTAR_FORMAT, "ascii", "replace")

        @classmethod
        def create_pax_global_header(cls, pax_headers):
            """Return the object as a pax global header block sequence.
            """
            return cls._create_pax_generic_header(pax_headers, XGLTYPE, "utf-8")

        def _posix_split_name(self, name, encoding, errors):
            """Split a name longer than 100 chars into a prefix
               and a name part.
            """
            components = name.split("/")
            for i in range(1, len(components)):
                prefix = "/".join(components[:i])
                name = "/".join(components[i:])
                if len(prefix.encode(encoding, errors)) <= LENGTH_PREFIX and \
                        len(name.encode(encoding, errors)) <= LENGTH_NAME:
                    break
            else:
                raise ValueError("name is too long")

            return prefix, name

        @staticmethod
        def _create_header(info, format, encoding, errors):
            """Return a header block. info is a dictionary with file
               information, format must be one of the *_FORMAT constants.
            """
            has_device_fields = info.get("type") in (CHRTYPE, BLKTYPE)
            if has_device_fields:
                devmajor = itn(info.get("devmajor", 0), 8, format)
                devminor = itn(info.get("devminor", 0), 8, format)
            else:
                devmajor = stn("", 8, encoding, errors)
                devminor = stn("", 8, encoding, errors)

            # None values in metadata should cause ValueError.
            # itn()/stn() do this for all fields except type.
            filetype = info.get("type", REGTYPE)
            if filetype is None:
                raise ValueError("TarInfo.type must not be None")

            parts = [
                stn(info.get("name", ""), 100, encoding, errors),
                itn(info.get("mode", 0) & 0o7777, 8, format),
                itn(info.get("uid", 0), 8, format),
                itn(info.get("gid", 0), 8, format),
                itn(info.get("size", 0), 12, format),
                itn(info.get("mtime", 0), 12, format),
                b"        ", # checksum field
                filetype,
                stn(info.get("linkname", ""), 100, encoding, errors),
                info.get("magic", POSIX_MAGIC),
                stn(info.get("uname", ""), 32, encoding, errors),
                stn(info.get("gname", ""), 32, encoding, errors),
                devmajor,
                devminor,
                stn(info.get("prefix", ""), 155, encoding, errors)
            ]

            buf = struct.pack("%ds" % BLOCKSIZE, b"".join(parts))
            chksum = calc_chksums(buf[-BLOCKSIZE:])[0]
            buf = buf[:-364] + bytes("%06o\0" % chksum, "ascii") + buf[-357:]
            return buf

        @staticmethod
        def _create_payload(payload):
            """Return the string payload filled with zero bytes
               up to the next 512 byte border.
            """
            blocks, remainder = divmod(len(payload), BLOCKSIZE)
            if remainder > 0:
                payload += (BLOCKSIZE - remainder) * NUL
            return payload

        @classmethod
        def _create_gnu_long_header(cls, name, type, encoding, errors):
            """Return a GNUTYPE_LONGNAME or GNUTYPE_LONGLINK sequence
               for name.
            """
            name = name.encode(encoding, errors) + NUL

            info = {}
            info["name"] = "././@LongLink"
            info["type"] = type
            info["size"] = len(name)
            info["magic"] = GNU_MAGIC

            # create extended header + name blocks.
            return cls._create_header(info, USTAR_FORMAT, encoding, errors) + \
                    cls._create_payload(name)

        @classmethod
        def _create_pax_generic_header(cls, pax_headers, type, encoding):
            """Return a POSIX.1-2008 extended or global header sequence
               that contains a list of keyword, value pairs. The values
               must be strings.
            """
            # Check if one of the fields contains surrogate characters and thereby
            # forces hdrcharset=BINARY, see _proc_pax() for more information.
            binary = False
            for keyword, value in pax_headers.items():
                try:
                    value.encode("utf-8", "strict")
                except UnicodeEncodeError:
                    binary = True
                    break

            records = b""
            if binary:
                # Put the hdrcharset field at the beginning of the header.
                records += b"21 hdrcharset=BINARY\n"

            for keyword, value in pax_headers.items():
                keyword = keyword.encode("utf-8")
                if binary:
                    # Try to restore the original byte representation of 'value'.
                    # Needless to say, that the encoding must match the string.
                    value = value.encode(encoding, "surrogateescape")
                else:
                    value = value.encode("utf-8")

                l = len(keyword) + len(value) + 3   # ' ' + '=' + '\n'
                n = p = 0
                while True:
                    n = l + len(str(p))
                    if n == p:
                        break
                    p = n
                records += bytes(str(p), "ascii") + b" " + keyword + b"=" + value + b"\n"

            # We use a hardcoded "././@PaxHeader" name like star does
            # instead of the one that POSIX recommends.
            info = {}
            info["name"] = "././@PaxHeader"
            info["type"] = type
            info["size"] = len(records)
            info["magic"] = POSIX_MAGIC

            # Create pax header + record blocks.
            return cls._create_header(info, USTAR_FORMAT, "ascii", "replace") + \
                    cls._create_payload(records)

        @classmethod
        def frombuf(cls, buf, encoding, errors):
            """Construct a TarInfo object from a 512 byte bytes object.

            To support the old v7 tar format AREGTYPE headers are
            transformed to DIRTYPE headers if their name ends in '/'.
            """
            return cls._frombuf(buf, encoding, errors)

        @classmethod
        def _frombuf(cls, buf, encoding, errors, *, dircheck=True):
            """Construct a TarInfo object from a 512 byte bytes object.

            If ``dircheck`` is set to ``True`` then ``AREGTYPE`` headers will
            be normalized to ``DIRTYPE`` if the name ends in a trailing slash.
            ``dircheck`` must be set to ``False`` if this function is called
            on a follow-up header such as ``GNUTYPE_LONGNAME``.
            """
            if len(buf) == 0:
                raise EmptyHeaderError("empty header")
            if len(buf) != BLOCKSIZE:
                raise TruncatedHeaderError("truncated header")
            if buf.count(NUL) == BLOCKSIZE:
                raise EOFHeaderError("end of file header")

            chksum = nti(buf[148:156])
            if chksum not in calc_chksums(buf):
                raise InvalidHeaderError("bad checksum")

            obj = cls()
            obj.name = nts(buf[0:100], encoding, errors)
            obj.mode = nti(buf[100:108])
            obj.uid = nti(buf[108:116])
            obj.gid = nti(buf[116:124])
            obj.size = nti(buf[124:136])
            obj.mtime = nti(buf[136:148])
            obj.chksum = chksum
            obj.type = buf[156:157]
            obj.linkname = nts(buf[157:257], encoding, errors)
            obj.uname = nts(buf[265:297], encoding, errors)
            obj.gname = nts(buf[297:329], encoding, errors)
            obj.devmajor = nti(buf[329:337])
            obj.devminor = nti(buf[337:345])
            prefix = nts(buf[345:500], encoding, errors)

            # Old V7 tar format represents a directory as a regular
            # file with a trailing slash.
            if dircheck and obj.type == AREGTYPE and obj.name.endswith("/"):
                obj.type = DIRTYPE

            # The old GNU sparse format occupies some of the unused
            # space in the buffer for up to 4 sparse structures.
            # Save them for later processing in _proc_sparse().
            if obj.type == GNUTYPE_SPARSE:
                pos = 386
                structs = []
                for i in range(4):
                    try:
                        offset = nti(buf[pos:pos + 12])
                        numbytes = nti(buf[pos + 12:pos + 24])
                    except ValueError:
                        break
                    structs.append((offset, numbytes))
                    pos += 24
                isextended = bool(buf[482])
                origsize = nti(buf[483:495])
                obj._sparse_structs = (structs, isextended, origsize)

            # Remove redundant slashes from directories.
            if obj.isdir():
                obj.name = obj.name.rstrip("/")

            # Reconstruct a ustar longname.
            if prefix and obj.type not in GNU_TYPES:
                obj.name = prefix + "/" + obj.name
            return obj

        @classmethod
        def fromtarfile(cls, tarfile):
            """Return the next TarInfo object from TarFile object
               tarfile.
            """
            return cls._fromtarfile(tarfile)

        @classmethod
        def _fromtarfile(cls, tarfile, *, dircheck=True):
            """
            See dircheck documentation in _frombuf().
            """
            buf = tarfile.fileobj.read(BLOCKSIZE)
            obj = cls._frombuf(buf, tarfile.encoding, tarfile.errors, dircheck=dircheck)
            obj.offset = tarfile.fileobj.tell() - BLOCKSIZE
            return obj._proc_member(tarfile)

        #--------------------------------------------------------------------------
        # The following are methods that are called depending on the type of a
        # member. The entry point is _proc_member() which can be overridden in a
        # subclass to add custom _proc_*() methods. A _proc_*() method MUST
        # implement the following
        # operations:
        # 1. Set self.offset_data to the position where the data blocks begin,
        #    if there is data that follows.
        # 2. Set tarfile.offset to the position where the next member's header will
        #    begin.
        # 3. Return self or another valid TarInfo object.
        def _proc_member(self, tarfile):
            """Choose the right processing method depending on
               the type and call it.
            """
            if self.type in (GNUTYPE_LONGNAME, GNUTYPE_LONGLINK):
                return self._proc_gnulong(tarfile)
            elif self.type == GNUTYPE_SPARSE:
                return self._proc_sparse(tarfile)
            elif self.type in (XHDTYPE, XGLTYPE, SOLARIS_XHDTYPE):
                return self._proc_pax(tarfile)
            else:
                return self._proc_builtin(tarfile)

        def _proc_builtin(self, tarfile):
            """Process a builtin type or an unknown type which
               will be treated as a regular file.
            """
            self.offset_data = tarfile.fileobj.tell()
            offset = self.offset_data
            if self.isreg() or self.type not in SUPPORTED_TYPES:
                # Skip the following data blocks.
                offset += self._block(self.size)
            tarfile.offset = offset

            # Patch the TarInfo object with saved global
            # header information.
            self._apply_pax_info(tarfile.pax_headers, tarfile.encoding, tarfile.errors)

            # Remove redundant slashes from directories. This is to be consistent
            # with frombuf().
            if self.isdir():
                self.name = self.name.rstrip("/")

            return self

        def _proc_gnulong(self, tarfile):
            """Process the blocks that hold a GNU longname
               or longlink member.
            """
            buf = tarfile.fileobj.read(self._block(self.size))

            # Fetch the next header and process it.
            try:
                next = self._fromtarfile(tarfile, dircheck=False)
            except HeaderError as e:
                raise SubsequentHeaderError(str(e)) from None

            # Patch the TarInfo object from the next header with
            # the longname information.
            next.offset = self.offset
            if self.type == GNUTYPE_LONGNAME:
                next.name = nts(buf, tarfile.encoding, tarfile.errors)
            elif self.type == GNUTYPE_LONGLINK:
                next.linkname = nts(buf, tarfile.encoding, tarfile.errors)

            # Remove redundant slashes from directories. This is to be consistent
            # with frombuf().
            if next.isdir():
                next.name = next.name.removesuffix("/")

            return next

        def _proc_sparse(self, tarfile):
            """Process a GNU sparse header plus extra headers.
            """
            # We already collected some sparse structures in frombuf().
            structs, isextended, origsize = self._sparse_structs
            del self._sparse_structs

            # Collect sparse structures from extended header blocks.
            while isextended:
                buf = tarfile.fileobj.read(BLOCKSIZE)
                pos = 0
                for i in range(21):
                    try:
                        offset = nti(buf[pos:pos + 12])
                        numbytes = nti(buf[pos + 12:pos + 24])
                    except ValueError:
                        break
                    if offset and numbytes:
                        structs.append((offset, numbytes))
                    pos += 24
                isextended = bool(buf[504])
            self.sparse = structs

            self.offset_data = tarfile.fileobj.tell()
            tarfile.offset = self.offset_data + self._block(self.size)
            self.size = origsize
            return self

        def _proc_pax(self, tarfile):
            """Process an extended or global header as described in
               POSIX.1-2008.
            """
            # Read the header information.
            buf = tarfile.fileobj.read(self._block(self.size))

            # A pax header stores supplemental information for either
            # the following file (extended) or all following files
            # (global).
            if self.type == XGLTYPE:
                pax_headers = tarfile.pax_headers
            else:
                pax_headers = tarfile.pax_headers.copy()

            # Parse pax header information. A record looks like that:
            # "%d %s=%s\n" % (length, keyword, value). length is the size
            # of the complete record including the length field itself and
            # the newline.
            pos = 0
            encoding = None
            raw_headers = []
            while len(buf) > pos and buf[pos] != 0x00:
                if not (match := _header_length_prefix_re.match(buf, pos)):
                    raise InvalidHeaderError("invalid header")
                try:
                    length = int(match.group(1))
                except ValueError:
                    raise InvalidHeaderError("invalid header")
                # Headers must be at least 5 bytes, shortest being '5 x=\n'.
                # Value is allowed to be empty.
                if length < 5:
                    raise InvalidHeaderError("invalid header")
                if pos + length > len(buf):
                    raise InvalidHeaderError("invalid header")

                header_value_end_offset = match.start(1) + length - 1  # Last byte of the header
                keyword_and_value = buf[match.end(1) + 1:header_value_end_offset]
                raw_keyword, equals, raw_value = keyword_and_value.partition(b"=")

                # Check the framing of the header. The last character must be '\n' (0x0A)
                if not raw_keyword or equals != b"=" or buf[header_value_end_offset] != 0x0A:
                    raise InvalidHeaderError("invalid header")
                raw_headers.append((length, raw_keyword, raw_value))

                # Check if the pax header contains a hdrcharset field. This tells us
                # the encoding of the path, linkpath, uname and gname fields. Normally,
                # these fields are UTF-8 encoded but since POSIX.1-2008 tar
                # implementations are allowed to store them as raw binary strings if
                # the translation to UTF-8 fails. For the time being, we don't care about
                # anything other than "BINARY". The only other value that is currently
                # allowed by the standard is "ISO-IR 10646 2000 UTF-8" in other words UTF-8.
                # Note that we only follow the initial 'hdrcharset' setting to preserve
                # the initial behavior of the 'tarfile' module.
                if raw_keyword == b"hdrcharset" and encoding is None:
                    if raw_value == b"BINARY":
                        encoding = tarfile.encoding
                    else:  # This branch ensures only the first 'hdrcharset' header is used.
                        encoding = "utf-8"

                pos += length

            # If no explicit hdrcharset is set, we use UTF-8 as a default.
            if encoding is None:
                encoding = "utf-8"

            # After parsing the raw headers we can decode them to text.
            for length, raw_keyword, raw_value in raw_headers:
                # Normally, we could just use "utf-8" as the encoding and "strict"
                # as the error handler, but we better not take the risk. For
                # example, GNU tar <= 1.23 is known to store filenames it cannot
                # translate to UTF-8 as raw strings (unfortunately without a
                # hdrcharset=BINARY header).
                # We first try the strict standard encoding, and if that fails we
                # fall back on the user's encoding and error handler.
                keyword = self._decode_pax_field(raw_keyword, "utf-8", "utf-8",
                        tarfile.errors)
                if keyword in PAX_NAME_FIELDS:
                    value = self._decode_pax_field(raw_value, encoding, tarfile.encoding,
                            tarfile.errors)
                else:
                    value = self._decode_pax_field(raw_value, "utf-8", "utf-8",
                            tarfile.errors)

                pax_headers[keyword] = value

            # Fetch the next header.
            try:
                next = self._fromtarfile(tarfile, dircheck=False)
            except HeaderError as e:
                raise SubsequentHeaderError(str(e)) from None

            # Process GNU sparse information.
            if "GNU.sparse.map" in pax_headers:
                # GNU extended sparse format version 0.1.
                self._proc_gnusparse_01(next, pax_headers)

            elif "GNU.sparse.size" in pax_headers:
                # GNU extended sparse format version 0.0.
                self._proc_gnusparse_00(next, raw_headers)

            elif pax_headers.get("GNU.sparse.major") == "1" and pax_headers.get("GNU.sparse.minor") == "0":
                # GNU extended sparse format version 1.0.
                self._proc_gnusparse_10(next, pax_headers, tarfile)

            if self.type in (XHDTYPE, SOLARIS_XHDTYPE):
                # Patch the TarInfo object with the extended header info.
                next._apply_pax_info(pax_headers, tarfile.encoding, tarfile.errors)
                next.offset = self.offset

                if "size" in pax_headers:
                    # If the extended header replaces the size field,
                    # we need to recalculate the offset where the next
                    # header starts.
                    offset = next.offset_data
                    if next.isreg() or next.type not in SUPPORTED_TYPES:
                        offset += next._block(next.size)
                    tarfile.offset = offset

            return next

        def _proc_gnusparse_00(self, next, raw_headers):
            """Process a GNU tar extended sparse header, version 0.0.
            """
            offsets = []
            numbytes = []
            for _, keyword, value in raw_headers:
                if keyword == b"GNU.sparse.offset":
                    try:
                        offsets.append(int(value.decode()))
                    except ValueError:
                        raise InvalidHeaderError("invalid header")

                elif keyword == b"GNU.sparse.numbytes":
                    try:
                        numbytes.append(int(value.decode()))
                    except ValueError:
                        raise InvalidHeaderError("invalid header")

            next.sparse = list(zip(offsets, numbytes))

        def _proc_gnusparse_01(self, next, pax_headers):
            """Process a GNU tar extended sparse header, version 0.1.
            """
            sparse = [int(x) for x in pax_headers["GNU.sparse.map"].split(",")]
            next.sparse = list(zip(sparse[::2], sparse[1::2]))

        def _proc_gnusparse_10(self, next, pax_headers, tarfile):
            """Process a GNU tar extended sparse header, version 1.0.
            """
            fields = None
            sparse = []
            buf = tarfile.fileobj.read(BLOCKSIZE)
            fields, buf = buf.split(b"\n", 1)
            fields = int(fields)
            while len(sparse) < fields * 2:
                if b"\n" not in buf:
                    buf += tarfile.fileobj.read(BLOCKSIZE)
                number, buf = buf.split(b"\n", 1)
                sparse.append(int(number))
            next.offset_data = tarfile.fileobj.tell()
            next.sparse = list(zip(sparse[::2], sparse[1::2]))

        def _apply_pax_info(self, pax_headers, encoding, errors):
            """Replace fields with supplemental information from a previous
               pax extended or global header.
            """
            for keyword, value in pax_headers.items():
                if keyword == "GNU.sparse.name":
                    setattr(self, "path", value)
                elif keyword == "GNU.sparse.size":
                    setattr(self, "size", int(value))
                elif keyword == "GNU.sparse.realsize":
                    setattr(self, "size", int(value))
                elif keyword in PAX_FIELDS:
                    if keyword in PAX_NUMBER_FIELDS:
                        try:
                            value = PAX_NUMBER_FIELDS[keyword](value)
                        except ValueError:
                            value = 0
                    if keyword == "path":
                        value = value.rstrip("/")
                    setattr(self, keyword, value)

            self.pax_headers = pax_headers.copy()

        def _decode_pax_field(self, value, encoding, fallback_encoding, fallback_errors):
            """Decode a single field from a pax record.
            """
            try:
                return value.decode(encoding, "strict")
            except UnicodeDecodeError:
                return value.decode(fallback_encoding, fallback_errors)

        def _block(self, count):
            """Round up a byte count by BLOCKSIZE and return it,
               e.g. _block(834) => 1024.
            """
            # Only non-negative offsets are allowed
            if count < 0:
                raise InvalidHeaderError("invalid offset")
            blocks, remainder = divmod(count, BLOCKSIZE)
            if remainder:
                blocks += 1
            return blocks * BLOCKSIZE

        def isreg(self):
            'Return True if the Tarinfo object is a regular file.'
            return self.type in REGULAR_TYPES

        def isfile(self):
            'Return True if the Tarinfo object is a regular file.'
            return self.isreg()

        def isdir(self):
            'Return True if it is a directory.'
            return self.type == DIRTYPE

        def issym(self):
            'Return True if it is a symbolic link.'
            return self.type == SYMTYPE

        def islnk(self):
            'Return True if it is a hard link.'
            return self.type == LNKTYPE

        def ischr(self):
            'Return True if it is a character device.'
            return self.type == CHRTYPE

        def isblk(self):
            'Return True if it is a block device.'
            return self.type == BLKTYPE

        def isfifo(self):
            'Return True if it is a FIFO.'
            return self.type == FIFOTYPE

        def issparse(self):
            return self.sparse is not None

        def isdev(self):
            'Return True if it is one of character device, block device or FIFO.'
            return self.type in (CHRTYPE, BLKTYPE, FIFOTYPE)
    # class TarInfo

    class TarFile(object):
        """The TarFile Class provides an interface to tar archives.
        """

        debug = 0                   # May be set from 0 (no msgs) to 3 (all msgs)

        dereference = False         # If true, add content of linked file to the
                                    # tar file, else the link.

        ignore_zeros = False        # If true, skips empty or invalid blocks and
                                    # continues processing.

        errorlevel = 1              # If 0, fatal errors only appear in debug
                                    # messages (if debug >= 0). If > 0, errors
                                    # are passed to the caller as exceptions.

        format = DEFAULT_FORMAT     # The format to use when creating an archive.

        encoding = ENCODING         # Encoding for 8-bit character strings.

        errors = None               # Error handler for unicode conversion.

        tarinfo = TarInfo           # The default TarInfo class to use.

        fileobject = ExFileObject   # The file-object for extractfile().

        extraction_filter = None    # The default filter for extraction.

        def __init__(self, name=None, mode="r", fileobj=None, format=None,
                tarinfo=None, dereference=None, ignore_zeros=None, encoding=None,
                errors="surrogateescape", pax_headers=None, debug=None,
                errorlevel=None, copybufsize=None, stream=False):
            """Open an (uncompressed) tar archive 'name'. 'mode' is either 'r' to
               read from an existing archive, 'a' to append data to an existing
               file or 'w' to create a new file overwriting an existing one. 'mode'
               defaults to 'r'.
               If 'fileobj' is given, it is used for reading or writing data. If it
               can be determined, 'mode' is overridden by 'fileobj's mode.
               'fileobj' is not closed, when TarFile is closed.
            """
            modes = {"r": "rb", "a": "r+b", "w": "wb", "x": "xb"}
            if mode not in modes:
                raise ValueError("mode must be 'r', 'a', 'w' or 'x'")
            self.mode = mode
            self._mode = modes[mode]

            if not fileobj:
                if self.mode == "a" and not os.path.exists(name):
                    # Create nonexistent files in append mode.
                    self.mode = "w"
                    self._mode = "wb"
                fileobj = bltn_open(name, self._mode)
                self._extfileobj = False
            else:
                if (name is None and hasattr(fileobj, "name") and
                    isinstance(fileobj.name, (str, bytes))):
                    name = fileobj.name
                if hasattr(fileobj, "mode"):
                    self._mode = fileobj.mode
                self._extfileobj = True
            self.name = os.path.abspath(name) if name else None
            self.fileobj = fileobj

            self.stream = stream

            # Init attributes.
            if format is not None:
                self.format = format
            if tarinfo is not None:
                self.tarinfo = tarinfo
            if dereference is not None:
                self.dereference = dereference
            if ignore_zeros is not None:
                self.ignore_zeros = ignore_zeros
            if encoding is not None:
                self.encoding = encoding
            self.errors = errors

            if pax_headers is not None and self.format == PAX_FORMAT:
                self.pax_headers = pax_headers
            else:
                self.pax_headers = {}

            if debug is not None:
                self.debug = debug
            if errorlevel is not None:
                self.errorlevel = errorlevel

            # Init datastructures.
            self.copybufsize = copybufsize
            self.closed = False
            self.members = []       # list of members as TarInfo objects
            self._loaded = False    # flag if all members have been read
            self.offset = self.fileobj.tell()
                                    # current position in the archive file
            self.inodes = {}        # dictionary caching the inodes of
                                    # archive members already added
            self._unames = {}       # Cached mappings of uid -> uname
            self._gnames = {}       # Cached mappings of gid -> gname

            try:
                if self.mode == "r":
                    self.firstmember = None
                    self.firstmember = self.next()

                if self.mode == "a":
                    # Move to the end of the archive,
                    # before the first empty block.
                    while True:
                        self.fileobj.seek(self.offset)
                        try:
                            tarinfo = self.tarinfo.fromtarfile(self)
                            self.members.append(tarinfo)
                        except EOFHeaderError:
                            self.fileobj.seek(self.offset)
                            break
                        except HeaderError as e:
                            raise ReadError(str(e)) from None

                if self.mode in ("a", "w", "x"):
                    self._loaded = True

                    if self.pax_headers:
                        buf = self.tarinfo.create_pax_global_header(self.pax_headers.copy())
                        self.fileobj.write(buf)
                        self.offset += len(buf)
            except:
                if not self._extfileobj:
                    self.fileobj.close()
                self.closed = True
                raise

        #--------------------------------------------------------------------------
        # Below are the classmethods which act as alternate constructors to the
        # TarFile class. The open() method is the only one that is needed for
        # public use; it is the "super"-constructor and is able to select an
        # adequate "sub"-constructor for a particular compression using the mapping
        # from OPEN_METH.
        #
        # This concept allows one to subclass TarFile without losing the comfort of
        # the super-constructor. A sub-constructor is registered and made available
        # by adding it to the mapping in OPEN_METH.

        @classmethod
        def open(cls, name=None, mode="r", fileobj=None, bufsize=RECORDSIZE, **kwargs):
            """Open a tar archive for reading, writing or appending. Return
               an appropriate TarFile class.

               mode:
               'r' or 'r:*' open for reading with transparent compression
               'r:'         open for reading exclusively uncompressed
               'r:gz'       open for reading with gzip compression
               'r:bz2'      open for reading with bzip2 compression
               'r:xz'       open for reading with lzma compression
               'r:zst'      open for reading with zstd compression
               'a' or 'a:'  open for appending, creating the file if necessary
               'w' or 'w:'  open for writing without compression
               'w:gz'       open for writing with gzip compression
               'w:bz2'      open for writing with bzip2 compression
               'w:xz'       open for writing with lzma compression
               'w:zst'      open for writing with zstd compression

               'x' or 'x:'  create a tarfile exclusively without compression, raise
                            an exception if the file is already created
               'x:gz'       create a gzip compressed tarfile, raise an exception
                            if the file is already created
               'x:bz2'      create a bzip2 compressed tarfile, raise an exception
                            if the file is already created
               'x:xz'       create an lzma compressed tarfile, raise an exception
                            if the file is already created
               'x:zst'      create a zstd compressed tarfile, raise an exception
                            if the file is already created

               'r|*'        open a stream of tar blocks with transparent compression
               'r|'         open an uncompressed stream of tar blocks for reading
               'r|gz'       open a gzip compressed stream of tar blocks
               'r|bz2'      open a bzip2 compressed stream of tar blocks
               'r|xz'       open an lzma compressed stream of tar blocks
               'r|zst'      open a zstd compressed stream of tar blocks
               'w|'         open an uncompressed stream for writing
               'w|gz'       open a gzip compressed stream for writing
               'w|bz2'      open a bzip2 compressed stream for writing
               'w|xz'       open an lzma compressed stream for writing
               'w|zst'      open a zstd compressed stream for writing
            """

            if not name and not fileobj:
                raise ValueError("nothing to open")

            if mode in ("r", "r:*"):
                # Find out which *open() is appropriate for opening the file.
                def not_compressed(comptype):
                    return cls.OPEN_METH[comptype] == 'taropen'
                error_msgs = []
                for comptype in sorted(cls.OPEN_METH, key=not_compressed):
                    func = getattr(cls, cls.OPEN_METH[comptype])
                    if fileobj is not None:
                        saved_pos = fileobj.tell()
                    try:
                        return func(name, "r", fileobj, **kwargs)
                    except (ReadError, CompressionError) as e:
                        error_msgs.append(f'- method {comptype}: {e!r}')
                        if fileobj is not None:
                            fileobj.seek(saved_pos)
                        continue
                error_msgs_summary = '\n'.join(error_msgs)
                raise ReadError(f"file could not be opened successfully:\n{error_msgs_summary}")

            elif ":" in mode:
                filemode, comptype = mode.split(":", 1)
                filemode = filemode or "r"
                comptype = comptype or "tar"

                # Select the *open() function according to
                # given compression.
                if comptype in cls.OPEN_METH:
                    func = getattr(cls, cls.OPEN_METH[comptype])
                else:
                    raise CompressionError("unknown compression type %r" % comptype)
                return func(name, filemode, fileobj, **kwargs)

            elif "|" in mode:
                filemode, comptype = mode.split("|", 1)
                filemode = filemode or "r"
                comptype = comptype or "tar"

                if filemode not in ("r", "w"):
                    raise ValueError("mode must be 'r' or 'w'")
                if "compresslevel" in kwargs and comptype not in ("gz", "bz2"):
                    raise ValueError(
                        "compresslevel is only valid for w|gz and w|bz2 modes"
                    )
                if "preset" in kwargs and comptype not in ("xz",):
                    raise ValueError("preset is only valid for w|xz mode")

                compresslevel = kwargs.pop("compresslevel", 9)
                preset = kwargs.pop("preset", None)
                stream = _Stream(name, filemode, comptype, fileobj, bufsize,
                                 compresslevel, preset)
                try:
                    t = cls(name, filemode, stream, **kwargs)
                except:
                    stream.close()
                    raise
                t._extfileobj = False
                return t

            elif mode in ("a", "w", "x"):
                return cls.taropen(name, mode, fileobj, **kwargs)

            raise ValueError("undiscernible mode")

        @classmethod
        def taropen(cls, name, mode="r", fileobj=None, **kwargs):
            """Open uncompressed tar archive name for reading or writing.
            """
            if mode not in ("r", "a", "w", "x"):
                raise ValueError("mode must be 'r', 'a', 'w' or 'x'")
            return cls(name, mode, fileobj, **kwargs)

        @classmethod
        def gzopen(cls, name, mode="r", fileobj=None, compresslevel=9, **kwargs):
            """Open gzip compressed tar archive name for reading or writing.
               Appending is not allowed.
            """
            if mode not in ("r", "w", "x"):
                raise ValueError("mode must be 'r', 'w' or 'x'")

            try:
                from gzip import GzipFile
            except ImportError:
                raise CompressionError("gzip module is not available") from None

            try:
                fileobj = GzipFile(name, mode + "b", compresslevel, fileobj)
            except OSError as e:
                if fileobj is not None and mode == 'r':
                    raise ReadError("not a gzip file") from e
                raise

            try:
                t = cls.taropen(name, mode, fileobj, **kwargs)
            except OSError as e:
                fileobj.close()
                if mode == 'r':
                    raise ReadError("not a gzip file") from e
                raise
            except:
                fileobj.close()
                raise
            t._extfileobj = False
            return t

        @classmethod
        def bz2open(cls, name, mode="r", fileobj=None, compresslevel=9, **kwargs):
            """Open bzip2 compressed tar archive name for reading or writing.
               Appending is not allowed.
            """
            if mode not in ("r", "w", "x"):
                raise ValueError("mode must be 'r', 'w' or 'x'")

            try:
                from bz2 import BZ2File
            except ImportError:
                raise CompressionError("bz2 module is not available") from None

            fileobj = BZ2File(fileobj or name, mode, compresslevel=compresslevel)

            try:
                t = cls.taropen(name, mode, fileobj, **kwargs)
            except (OSError, EOFError) as e:
                fileobj.close()
                if mode == 'r':
                    raise ReadError("not a bzip2 file") from e
                raise
            except:
                fileobj.close()
                raise
            t._extfileobj = False
            return t

        @classmethod
        def xzopen(cls, name, mode="r", fileobj=None, preset=None, **kwargs):
            """Open lzma compressed tar archive name for reading or writing.
               Appending is not allowed.
            """
            if mode not in ("r", "w", "x"):
                raise ValueError("mode must be 'r', 'w' or 'x'")

            try:
                from lzma import LZMAFile, LZMAError
            except ImportError:
                raise CompressionError("lzma module is not available") from None

            fileobj = LZMAFile(fileobj or name, mode, preset=preset)

            try:
                t = cls.taropen(name, mode, fileobj, **kwargs)
            except (LZMAError, EOFError) as e:
                fileobj.close()
                if mode == 'r':
                    raise ReadError("not an lzma file") from e
                raise
            except:
                fileobj.close()
                raise
            t._extfileobj = False
            return t

        @classmethod
        def zstopen(cls, name, mode="r", fileobj=None, level=None, options=None,
                    zstd_dict=None, **kwargs):
            """Open zstd compressed tar archive name for reading or writing.
               Appending is not allowed.
            """
            if mode not in ("r", "w", "x"):
                raise ValueError("mode must be 'r', 'w' or 'x'")

            try:
                from compression.zstd import ZstdFile, ZstdError
            except ImportError:
                raise CompressionError("compression.zstd module is not available") from None

            fileobj = ZstdFile(
                fileobj or name,
                mode,
                level=level,
                options=options,
                zstd_dict=zstd_dict
            )

            try:
                t = cls.taropen(name, mode, fileobj, **kwargs)
            except (ZstdError, EOFError) as e:
                fileobj.close()
                if mode == 'r':
                    raise ReadError("not a zstd file") from e
                raise
            except Exception:
                fileobj.close()
                raise
            t._extfileobj = False
            return t

        # All *open() methods are registered here.
        OPEN_METH = {
            "tar": "taropen",   # uncompressed tar
            "gz":  "gzopen",    # gzip compressed tar
            "bz2": "bz2open",   # bzip2 compressed tar
            "xz":  "xzopen",    # lzma compressed tar
            "zst": "zstopen",   # zstd compressed tar
        }

        #--------------------------------------------------------------------------
        # The public methods which TarFile provides:

        def close(self):
            """Close the TarFile. In write-mode, two finishing zero blocks are
               appended to the archive.
            """
            if self.closed:
                return

            self.closed = True
            try:
                if self.mode in ("a", "w", "x"):
                    self.fileobj.write(NUL * (BLOCKSIZE * 2))
                    self.offset += (BLOCKSIZE * 2)
                    # fill up the end with zero-blocks
                    # (like option -b20 for tar does)
                    blocks, remainder = divmod(self.offset, RECORDSIZE)
                    if remainder > 0:
                        self.fileobj.write(NUL * (RECORDSIZE - remainder))
            finally:
                if not self._extfileobj:
                    self.fileobj.close()

        def getmember(self, name):
            """Return a TarInfo object for member 'name'. If 'name' can not be
               found in the archive, KeyError is raised. If a member occurs more
               than once in the archive, its last occurrence is assumed to be the
               most up-to-date version.
            """
            tarinfo = self._getmember(name.rstrip('/'))
            if tarinfo is None:
                raise KeyError("filename %r not found" % name)
            return tarinfo

        def getmembers(self):
            """Return the members of the archive as a list of TarInfo objects. The
               list has the same order as the members in the archive.
            """
            self._check()
            if not self._loaded:    # if we want to obtain a list of
                self._load()        # all members, we first have to
                                    # scan the whole archive.
            return self.members

        def getnames(self):
            """Return the members of the archive as a list of their names. It has
               the same order as the list returned by getmembers().
            """
            return [tarinfo.name for tarinfo in self.getmembers()]

        def gettarinfo(self, name=None, arcname=None, fileobj=None):
            """Create a TarInfo object from the result of os.stat or equivalent
               on an existing file. The file is either named by 'name', or
               specified as a file object 'fileobj' with a file descriptor. If
               given, 'arcname' specifies an alternative name for the file in the
               archive, otherwise, the name is taken from the 'name' attribute of
               'fileobj', or the 'name' argument. The name should be a text
               string.
            """
            self._check("awx")

            # When fileobj is given, replace name by
            # fileobj's real name.
            if fileobj is not None:
                name = fileobj.name

            # Building the name of the member in the archive.
            # Backward slashes are converted to forward slashes,
            # Absolute paths are turned to relative paths.
            if arcname is None:
                arcname = name
            drv, arcname = os.path.splitdrive(arcname)
            arcname = arcname.replace(os.sep, "/")
            arcname = arcname.lstrip("/")

            # Now, fill the TarInfo object with
            # information specific for the file.
            tarinfo = self.tarinfo()
            tarinfo._tarfile = self  # To be removed in 3.16.

            # Use os.stat or os.lstat, depending on if symlinks shall be resolved.
            if fileobj is None:
                if not self.dereference:
                    statres = os.lstat(name)
                else:
                    statres = os.stat(name)
            else:
                statres = os.fstat(fileobj.fileno())
            linkname = ""

            stmd = statres.st_mode
            if stat.S_ISREG(stmd):
                inode = (statres.st_ino, statres.st_dev)
                if not self.dereference and statres.st_nlink > 1 and \
                        inode in self.inodes and arcname != self.inodes[inode]:
                    # Is it a hardlink to an already
                    # archived file?
                    type = LNKTYPE
                    linkname = self.inodes[inode]
                else:
                    # The inode is added only if its valid.
                    # For win32 it is always 0.
                    type = REGTYPE
                    if inode[0]:
                        self.inodes[inode] = arcname
            elif stat.S_ISDIR(stmd):
                type = DIRTYPE
            elif stat.S_ISFIFO(stmd):
                type = FIFOTYPE
            elif stat.S_ISLNK(stmd):
                type = SYMTYPE
                linkname = os.readlink(name)
            elif stat.S_ISCHR(stmd):
                type = CHRTYPE
            elif stat.S_ISBLK(stmd):
                type = BLKTYPE
            else:
                return None

            # Fill the TarInfo object with all
            # information we can get.
            tarinfo.name = arcname
            tarinfo.mode = stmd
            tarinfo.uid = statres.st_uid
            tarinfo.gid = statres.st_gid
            if type == REGTYPE:
                tarinfo.size = statres.st_size
            else:
                tarinfo.size = 0
            tarinfo.mtime = statres.st_mtime
            tarinfo.type = type
            tarinfo.linkname = linkname

            # Calls to pwd.getpwuid() and grp.getgrgid() tend to be expensive. To
            # speed things up, cache the resolved usernames and group names.
            if pwd:
                if tarinfo.uid not in self._unames:
                    try:
                        self._unames[tarinfo.uid] = pwd.getpwuid(tarinfo.uid)[0]
                    except KeyError:
                        self._unames[tarinfo.uid] = ''
                tarinfo.uname = self._unames[tarinfo.uid]
            if grp:
                if tarinfo.gid not in self._gnames:
                    try:
                        self._gnames[tarinfo.gid] = grp.getgrgid(tarinfo.gid)[0]
                    except KeyError:
                        self._gnames[tarinfo.gid] = ''
                tarinfo.gname = self._gnames[tarinfo.gid]

            if type in (CHRTYPE, BLKTYPE):
                if hasattr(os, "major") and hasattr(os, "minor"):
                    tarinfo.devmajor = os.major(statres.st_rdev)
                    tarinfo.devminor = os.minor(statres.st_rdev)
            return tarinfo

        def list(self, verbose=True, *, members=None):
            """Print a table of contents to sys.stdout.

            If 'verbose' is False, only the names of the members are printed.
            If it is True, an 'ls -l'-like output is produced.  'members' is
            optional and must be a subset of the list returned by getmembers().
            """
            # Convert tarinfo type to stat type.
            type2mode = {REGTYPE: stat.S_IFREG, SYMTYPE: stat.S_IFLNK,
                         FIFOTYPE: stat.S_IFIFO, CHRTYPE: stat.S_IFCHR,
                         DIRTYPE: stat.S_IFDIR, BLKTYPE: stat.S_IFBLK}
            self._check()

            if members is None:
                members = self
            for tarinfo in members:
                if verbose:
                    if tarinfo.mode is None:
                        _safe_print("??????????")
                    else:
                        modetype = type2mode.get(tarinfo.type, 0)
                        _safe_print(stat.filemode(modetype | tarinfo.mode))
                    _safe_print("%s/%s" % (tarinfo.uname or tarinfo.uid,
                                           tarinfo.gname or tarinfo.gid))
                    if tarinfo.ischr() or tarinfo.isblk():
                        _safe_print("%10s" %
                                ("%d,%d" % (tarinfo.devmajor, tarinfo.devminor)))
                    else:
                        _safe_print("%10d" % tarinfo.size)
                    if tarinfo.mtime is None:
                        _safe_print("????-??-?? ??:??:??")
                    else:
                        _safe_print("%d-%02d-%02d %02d:%02d:%02d" \
                                    % time.localtime(tarinfo.mtime)[:6])

                _safe_print(tarinfo.name + ("/" if tarinfo.isdir() else ""))

                if verbose:
                    if tarinfo.issym():
                        _safe_print("-> " + tarinfo.linkname)
                    if tarinfo.islnk():
                        _safe_print("link to " + tarinfo.linkname)
                print()

        def add(self, name, arcname=None, recursive=True, *, filter=None):
            """Add the file 'name' to the archive. 'name' may be any type of file
               (directory, fifo, symbolic link, etc.). If given, 'arcname'
               specifies an alternative name for the file in the archive.
               Directories are added recursively by default. This can be avoided by
               setting 'recursive' to False. 'filter' is a function
               that expects a TarInfo object argument and returns the changed
               TarInfo object, if it returns None the TarInfo object will be
               excluded from the archive.
            """
            self._check("awx")

            if arcname is None:
                arcname = name

            # Skip if somebody tries to archive the archive...
            if self.name is not None and os.path.abspath(name) == self.name:
                self._dbg(2, "tarfile: Skipped %r" % name)
                return

            self._dbg(1, name)

            # Create a TarInfo object from the file.
            tarinfo = self.gettarinfo(name, arcname)

            if tarinfo is None:
                self._dbg(1, "tarfile: Unsupported type %r" % name)
                return

            # Change or exclude the TarInfo object.
            if filter is not None:
                tarinfo = filter(tarinfo)
                if tarinfo is None:
                    self._dbg(2, "tarfile: Excluded %r" % name)
                    return

            # Append the tar header and data to the archive.
            if tarinfo.isreg():
                with bltn_open(name, "rb") as f:
                    self.addfile(tarinfo, f)

            elif tarinfo.isdir():
                self.addfile(tarinfo)
                if recursive:
                    for f in sorted(os.listdir(name)):
                        self.add(os.path.join(name, f), os.path.join(arcname, f),
                                recursive, filter=filter)

            else:
                self.addfile(tarinfo)

        def addfile(self, tarinfo, fileobj=None):
            """Add the TarInfo object 'tarinfo' to the archive.

            If 'tarinfo' represents a non zero-size regular file, the 'fileobj'
            argument should be a binary file, and tarinfo.size bytes are read
            from it and added to the archive. You can create TarInfo objects
            directly, or by using gettarinfo().
            """
            self._check("awx")

            if fileobj is None and tarinfo.isreg() and tarinfo.size != 0:
                raise ValueError("fileobj not provided for non zero-size regular file")

            tarinfo = copy.copy(tarinfo)

            buf = tarinfo.tobuf(self.format, self.encoding, self.errors)
            self.fileobj.write(buf)
            self.offset += len(buf)
            bufsize=self.copybufsize
            # If there's data to follow, append it.
            if fileobj is not None:
                copyfileobj(fileobj, self.fileobj, tarinfo.size, bufsize=bufsize)
                blocks, remainder = divmod(tarinfo.size, BLOCKSIZE)
                if remainder > 0:
                    self.fileobj.write(NUL * (BLOCKSIZE - remainder))
                    blocks += 1
                self.offset += blocks * BLOCKSIZE

            self.members.append(tarinfo)

        def _get_filter_function(self, filter):
            if filter is None:
                filter = self.extraction_filter
                if filter is None:
                    return data_filter
                if isinstance(filter, str):
                    raise TypeError(
                        'String names are not supported for '
                        + 'TarFile.extraction_filter. Use a function such as '
                        + 'tarfile.data_filter directly.')
                return filter
            if callable(filter):
                return filter
            try:
                return _NAMED_FILTERS[filter]
            except KeyError:
                raise ValueError(f"filter {filter!r} not found") from None

        def extractall(self, path=".", members=None, *, numeric_owner=False,
                       filter=None):
            """Extract all members from the archive to the current working
               directory and set owner, modification time and permissions on
               directories afterwards. 'path' specifies a different directory
               to extract to. 'members' is optional and must be a subset of the
               list returned by getmembers(). If 'numeric_owner' is True, only
               the numbers for user/group names are used and not the names.

               The 'filter' function will be called on each member just
               before extraction.
               It can return a changed TarInfo or None to skip the member.
               String names of common filters are accepted.
            """
            directories = []

            filter_function = self._get_filter_function(filter)
            if members is None:
                members = self

            for member in members:
                tarinfo, unfiltered = self._get_extract_tarinfo(
                    member, filter_function, path)
                if tarinfo is None:
                    continue
                if tarinfo.isdir():
                    # For directories, delay setting attributes until later,
                    # since permissions can interfere with extraction and
                    # extracting contents can reset mtime.
                    directories.append(unfiltered)
                self._extract_one(tarinfo, path, set_attrs=not tarinfo.isdir(),
                                  numeric_owner=numeric_owner,
                                  filter_function=filter_function)

            # Reverse sort directories.
            directories.sort(key=lambda a: a.name, reverse=True)


            # Set correct owner, mtime and filemode on directories.
            for unfiltered in directories:
                try:
                    # Need to re-apply any filter, to take the *current* filesystem
                    # state into account.
                    try:
                        tarinfo = filter_function(unfiltered, path)
                    except _FILTER_ERRORS as exc:
                        self._log_no_directory_fixup(unfiltered, repr(exc))
                        continue
                    if tarinfo is None:
                        self._log_no_directory_fixup(unfiltered,
                                                     'excluded by filter')
                        continue
                    dirpath = os.path.join(path, tarinfo.name)
                    try:
                        lstat = os.lstat(dirpath)
                    except FileNotFoundError:
                        self._log_no_directory_fixup(tarinfo, 'missing')
                        continue
                    if not stat.S_ISDIR(lstat.st_mode):
                        # This is no longer a directory; presumably a later
                        # member overwrote the entry.
                        self._log_no_directory_fixup(tarinfo, 'not a directory')
                        continue
                    self.chown(tarinfo, dirpath, numeric_owner=numeric_owner)
                    self.utime(tarinfo, dirpath)
                    self.chmod(tarinfo, dirpath)
                except ExtractError as e:
                    self._handle_nonfatal_error(e)

        def _log_no_directory_fixup(self, member, reason):
            self._dbg(2, "tarfile: Not fixing up directory %r (%s)" %
                      (member.name, reason))

        def extract(self, member, path="", set_attrs=True, *, numeric_owner=False,
                    filter=None):
            """Extract a member from the archive to the current working directory,
               using its full name. Its file information is extracted as accurately
               as possible. 'member' may be a filename or a TarInfo object. You can
               specify a different directory using 'path'. File attributes (owner,
               mtime, mode) are set unless 'set_attrs' is False. If 'numeric_owner'
               is True, only the numbers for user/group names are used and not
               the names.

               The 'filter' function will be called before extraction.
               It can return a changed TarInfo or None to skip the member.
               String names of common filters are accepted.
            """
            filter_function = self._get_filter_function(filter)
            tarinfo, unfiltered = self._get_extract_tarinfo(
                member, filter_function, path)
            if tarinfo is not None:
                self._extract_one(tarinfo, path, set_attrs, numeric_owner)

        def _get_extract_tarinfo(self, member, filter_function, path):
            """Get (filtered, unfiltered) TarInfos from *member*

            *member* might be a string.

            Return (None, None) if not found.
            """

            if isinstance(member, str):
                unfiltered = self.getmember(member)
            else:
                unfiltered = member

            filtered = None
            try:
                filtered = filter_function(unfiltered, path)
            except (OSError, UnicodeEncodeError, FilterError) as e:
                self._handle_fatal_error(e)
            except ExtractError as e:
                self._handle_nonfatal_error(e)
            if filtered is None:
                self._dbg(2, "tarfile: Excluded %r" % unfiltered.name)
                return None, None

            # Prepare the link target for makelink().
            if filtered.islnk():
                filtered = copy.copy(filtered)
                filtered._link_target = os.path.join(path, filtered.linkname)
            return filtered, unfiltered

        def _extract_one(self, tarinfo, path, set_attrs, numeric_owner,
                         filter_function=None):
            """Extract from filtered tarinfo to disk.

               filter_function is only used when extracting a *different*
               member (e.g. as fallback to creating a symlink)
            """
            self._check("r")

            try:
                self._extract_member(tarinfo, os.path.join(path, tarinfo.name),
                                     set_attrs=set_attrs,
                                     numeric_owner=numeric_owner,
                                     filter_function=filter_function,
                                     extraction_root=path)
            except (OSError, UnicodeEncodeError) as e:
                self._handle_fatal_error(e)
            except ExtractError as e:
                self._handle_nonfatal_error(e)

        def _handle_nonfatal_error(self, e):
            """Handle non-fatal error (ExtractError) according to errorlevel"""
            if self.errorlevel > 1:
                raise
            else:
                self._dbg(1, "tarfile: %s" % e)

        def _handle_fatal_error(self, e):
            """Handle "fatal" error according to self.errorlevel"""
            if self.errorlevel > 0:
                raise
            elif isinstance(e, OSError):
                if e.filename is None:
                    self._dbg(1, "tarfile: %s" % e.strerror)
                else:
                    self._dbg(1, "tarfile: %s %r" % (e.strerror, e.filename))
            else:
                self._dbg(1, "tarfile: %s %s" % (type(e).__name__, e))

        def extractfile(self, member):
            """Extract a member from the archive as a file object. 'member' may be
               a filename or a TarInfo object. If 'member' is a regular file or
               a link, an io.BufferedReader object is returned. For all other
               existing members, None is returned. If 'member' does not appear
               in the archive, KeyError is raised.
            """
            self._check("r")

            if isinstance(member, str):
                tarinfo = self.getmember(member)
            else:
                tarinfo = member

            if tarinfo.isreg() or tarinfo.type not in SUPPORTED_TYPES:
                # Members with unknown types are treated as regular files.
                return self.fileobject(self, tarinfo)

            elif tarinfo.islnk() or tarinfo.issym():
                if isinstance(self.fileobj, _Stream):
                    # A small but ugly workaround for the case that someone tries
                    # to extract a (sym)link as a file-object from a non-seekable
                    # stream of tar blocks.
                    raise StreamError("cannot extract (sym)link as file object")
                else:
                    # A (sym)link's file object is its target's file object.
                    return self.extractfile(self._find_link_target(tarinfo))
            else:
                # If there's no data associated with the member (directory, chrdev,
                # blkdev, etc.), return None instead of a file object.
                return None

        def _extract_member(self, tarinfo, targetpath, set_attrs=True,
                            numeric_owner=False, *, filter_function=None,
                            extraction_root=None):
            """Extract the filtered TarInfo object tarinfo to a physical
               file called targetpath.

               filter_function is only used when extracting a *different*
               member (e.g. as fallback to creating a symlink)
            """
            # Fetch the TarInfo object for the given name
            # and build the destination pathname, replacing
            # forward slashes to platform specific separators.
            targetpath = targetpath.rstrip("/")
            targetpath = targetpath.replace("/", os.sep)

            # Create all upper directories.
            upperdirs = os.path.dirname(targetpath)
            if upperdirs and not os.path.exists(upperdirs):
                # Create directories that are not part of the archive with
                # default permissions.
                os.makedirs(upperdirs, exist_ok=True)

            if tarinfo.islnk() or tarinfo.issym():
                self._dbg(1, "%s -> %s" % (tarinfo.name, tarinfo.linkname))
            else:
                self._dbg(1, tarinfo.name)

            if tarinfo.isreg():
                self.makefile(tarinfo, targetpath)
            elif tarinfo.isdir():
                self.makedir(tarinfo, targetpath)
            elif tarinfo.isfifo():
                self.makefifo(tarinfo, targetpath)
            elif tarinfo.ischr() or tarinfo.isblk():
                self.makedev(tarinfo, targetpath)
            elif tarinfo.islnk() or tarinfo.issym():
                self.makelink_with_filter(
                    tarinfo, targetpath,
                    filter_function=filter_function,
                    extraction_root=extraction_root)
            elif tarinfo.type not in SUPPORTED_TYPES:
                self.makeunknown(tarinfo, targetpath)
            else:
                self.makefile(tarinfo, targetpath)

            if set_attrs:
                self.chown(tarinfo, targetpath, numeric_owner)
                if not tarinfo.issym():
                    self.chmod(tarinfo, targetpath)
                    self.utime(tarinfo, targetpath)

        #--------------------------------------------------------------------------
        # Below are the different file methods. They are called via
        # _extract_member() when extract() is called. They can be replaced in a
        # subclass to implement other functionality.

        def makedir(self, tarinfo, targetpath):
            """Make a directory called targetpath.
            """
            try:
                if tarinfo.mode is None:
                    # Use the system's default mode
                    os.mkdir(targetpath)
                else:
                    # Use a safe mode for the directory, the real mode is set
                    # later in _extract_member().
                    os.mkdir(targetpath, 0o700)
            except FileExistsError:
                if not os.path.isdir(targetpath):
                    raise

        def makefile(self, tarinfo, targetpath):
            """Make a file called targetpath.
            """
            source = self.fileobj
            source.seek(tarinfo.offset_data)
            bufsize = self.copybufsize
            with bltn_open(targetpath, "wb") as target:
                if tarinfo.sparse is not None:
                    for offset, size in tarinfo.sparse:
                        target.seek(offset)
                        copyfileobj(source, target, size, ReadError, bufsize)
                    target.seek(tarinfo.size)
                    target.truncate()
                else:
                    copyfileobj(source, target, tarinfo.size, ReadError, bufsize)

        def makeunknown(self, tarinfo, targetpath):
            """Make a file from a TarInfo object with an unknown type
               at targetpath.
            """
            self.makefile(tarinfo, targetpath)
            self._dbg(1, "tarfile: Unknown file type %r, " \
                         "extracted as regular file." % tarinfo.type)

        def makefifo(self, tarinfo, targetpath):
            """Make a fifo called targetpath.
            """
            if hasattr(os, "mkfifo"):
                os.mkfifo(targetpath)
            else:
                raise ExtractError("fifo not supported by system")

        def makedev(self, tarinfo, targetpath):
            """Make a character or block device called targetpath.
            """
            if not hasattr(os, "mknod") or not hasattr(os, "makedev"):
                raise ExtractError("special devices not supported by system")

            mode = tarinfo.mode
            if mode is None:
                # Use mknod's default
                mode = 0o600
            if tarinfo.isblk():
                mode |= stat.S_IFBLK
            else:
                mode |= stat.S_IFCHR

            os.mknod(targetpath, mode,
                     os.makedev(tarinfo.devmajor, tarinfo.devminor))

        def makelink(self, tarinfo, targetpath):
            return self.makelink_with_filter(tarinfo, targetpath, None, None)

        def makelink_with_filter(self, tarinfo, targetpath,
                                 filter_function, extraction_root):
            """Make a (symbolic) link called targetpath. If it cannot be created
              (platform limitation), we try to make a copy of the referenced file
              instead of a link.

              filter_function is only used when extracting a *different*
              member (e.g. as fallback to creating a link).
            """
            keyerror_to_extracterror = False
            try:
                # For systems that support symbolic and hard links.
                if tarinfo.issym():
                    if os.path.lexists(targetpath):
                        # Avoid FileExistsError on following os.symlink.
                        os.unlink(targetpath)
                    os.symlink(tarinfo.linkname, targetpath)
                    return
                else:
                    if os.path.exists(tarinfo._link_target):
                        if os.path.lexists(targetpath):
                            # Avoid FileExistsError on following os.link.
                            os.unlink(targetpath)
                        os.link(tarinfo._link_target, targetpath)
                        return
            except symlink_exception:
                keyerror_to_extracterror = True

            try:
                unfiltered = self._find_link_target(tarinfo)
            except KeyError:
                if keyerror_to_extracterror:
                    raise ExtractError(
                        "unable to resolve link inside archive") from None
                else:
                    raise

            if filter_function is None:
                filtered = unfiltered
            else:
                if extraction_root is None:
                    raise ExtractError(
                        "makelink_with_filter: if filter_function is not None, "
                        + "extraction_root must also not be None")
                try:
                    filtered = filter_function(unfiltered, extraction_root)
                except _FILTER_ERRORS as cause:
                    raise LinkFallbackError(tarinfo, unfiltered.name) from cause
            if filtered is not None:
                self._extract_member(filtered, targetpath,
                                     filter_function=filter_function,
                                     extraction_root=extraction_root)

        def chown(self, tarinfo, targetpath, numeric_owner):
            """Set owner of targetpath according to tarinfo. If numeric_owner
               is True, use .gid/.uid instead of .gname/.uname. If numeric_owner
               is False, fall back to .gid/.uid when the search based on name
               fails.
            """
            if hasattr(os, "geteuid") and os.geteuid() == 0:
                # We have to be root to do so.
                g = tarinfo.gid
                u = tarinfo.uid
                if not numeric_owner:
                    try:
                        if grp and tarinfo.gname:
                            g = grp.getgrnam(tarinfo.gname)[2]
                    except KeyError:
                        pass
                    try:
                        if pwd and tarinfo.uname:
                            u = pwd.getpwnam(tarinfo.uname)[2]
                    except KeyError:
                        pass
                if g is None:
                    g = -1
                if u is None:
                    u = -1
                try:
                    if tarinfo.issym() and hasattr(os, "lchown"):
                        os.lchown(targetpath, u, g)
                    else:
                        os.chown(targetpath, u, g)
                except (OSError, OverflowError) as e:
                    # OverflowError can be raised if an ID doesn't fit in 'id_t'
                    raise ExtractError("could not change owner") from e

        def chmod(self, tarinfo, targetpath):
            """Set file permissions of targetpath according to tarinfo.
            """
            if tarinfo.mode is None:
                return
            try:
                os.chmod(targetpath, tarinfo.mode)
            except OSError as e:
                raise ExtractError("could not change mode") from e

        def utime(self, tarinfo, targetpath):
            """Set modification time of targetpath according to tarinfo.
            """
            mtime = tarinfo.mtime
            if mtime is None:
                return
            if not hasattr(os, 'utime'):
                return
            try:
                os.utime(targetpath, (mtime, mtime))
            except OSError as e:
                raise ExtractError("could not change modification time") from e

        #--------------------------------------------------------------------------
        def next(self):
            """Return the next member of the archive as a TarInfo object, when
               TarFile is opened for reading. Return None if there is no more
               available.
            """
            self._check("ra")
            if self.firstmember is not None:
                m = self.firstmember
                self.firstmember = None
                return m

            # Advance the file pointer.
            if self.offset != self.fileobj.tell():
                if self.offset == 0:
                    return None
                self.fileobj.seek(self.offset - 1)
                if not self.fileobj.read(1):
                    raise ReadError("unexpected end of data")

            # Read the next block.
            tarinfo = None
            while True:
                try:
                    tarinfo = self.tarinfo.fromtarfile(self)
                except EOFHeaderError as e:
                    if self.ignore_zeros:
                        self._dbg(2, "0x%X: %s" % (self.offset, e))
                        self.offset += BLOCKSIZE
                        continue
                except InvalidHeaderError as e:
                    if self.ignore_zeros:
                        self._dbg(2, "0x%X: %s" % (self.offset, e))
                        self.offset += BLOCKSIZE
                        continue
                    elif self.offset == 0:
                        raise ReadError(str(e)) from None
                except EmptyHeaderError:
                    if self.offset == 0:
                        raise ReadError("empty file") from None
                except TruncatedHeaderError as e:
                    if self.offset == 0:
                        raise ReadError(str(e)) from None
                except SubsequentHeaderError as e:
                    raise ReadError(str(e)) from None
                except Exception as e:
                    try:
                        import zlib
                        if isinstance(e, zlib.error):
                            raise ReadError(f'zlib error: {e}') from None
                        else:
                            raise e
                    except ImportError:
                        raise e
                break

            if tarinfo is not None:
                # if streaming the file we do not want to cache the tarinfo
                if not self.stream:
                    self.members.append(tarinfo)
            else:
                self._loaded = True

            return tarinfo

        #--------------------------------------------------------------------------
        # Little helper methods:

        def _getmember(self, name, tarinfo=None, normalize=False):
            """Find an archive member by name from bottom to top.
               If tarinfo is given, it is used as the starting point.
            """
            # Ensure that all members have been loaded.
            members = self.getmembers()

            # Limit the member search list up to tarinfo.
            skipping = False
            if tarinfo is not None:
                try:
                    index = members.index(tarinfo)
                except ValueError:
                    # The given starting point might be a (modified) copy.
                    # We'll later skip members until we find an equivalent.
                    skipping = True
                else:
                    # Happy fast path
                    members = members[:index]

            if normalize:
                name = os.path.normpath(name)

            for member in reversed(members):
                if skipping:
                    if tarinfo.offset == member.offset:
                        skipping = False
                    continue
                if normalize:
                    member_name = os.path.normpath(member.name)
                else:
                    member_name = member.name

                if name == member_name:
                    return member

            if skipping:
                # Starting point was not found
                raise ValueError(tarinfo)

        def _load(self):
            """Read through the entire archive file and look for readable
               members. This should not run if the file is set to stream.
            """
            if not self.stream:
                while self.next() is not None:
                    pass
                self._loaded = True

        def _check(self, mode=None):
            """Check if TarFile is still open, and if the operation's mode
               corresponds to TarFile's mode.
            """
            if self.closed:
                raise OSError("%s is closed" % self.__class__.__name__)
            if mode is not None and self.mode not in mode:
                raise OSError("bad operation for mode %r" % self.mode)

        def _find_link_target(self, tarinfo):
            """Find the target member of a symlink or hardlink member in the
               archive.
            """
            if tarinfo.issym():
                # Always search the entire archive.
                linkname = "/".join(filter(None, (os.path.dirname(tarinfo.name), tarinfo.linkname)))
                limit = None
            else:
                # Search the archive before the link, because a hard link is
                # just a reference to an already archived file.
                linkname = tarinfo.linkname
                limit = tarinfo

            member = self._getmember(linkname, tarinfo=limit, normalize=True)
            if member is None:
                raise KeyError("linkname %r not found" % linkname)
            return member

        def __iter__(self):
            """Provide an iterator object.
            """
            if self._loaded:
                yield from self.members
                return

            # Yield items using TarFile's next() method.
            # When all members have been read, set TarFile as _loaded.
            index = 0
            # Fix for SF #1100429: Under rare circumstances it can
            # happen that getmembers() is called during iteration,
            # which will have already exhausted the next() method.
            if self.firstmember is not None:
                tarinfo = self.next()
                index += 1
                yield tarinfo

            while True:
                if index < len(self.members):
                    tarinfo = self.members[index]
                elif not self._loaded:
                    tarinfo = self.next()
                    if not tarinfo:
                        self._loaded = True
                        return
                else:
                    return
                index += 1
                yield tarinfo

        def _dbg(self, level, msg):
            """Write debugging output to sys.stderr.
            """
            if level <= self.debug:
                print(msg, file=sys.stderr)

        def __enter__(self):
            self._check()
            return self

        def __exit__(self, type, value, traceback):
            if type is None:
                self.close()
            else:
                # An exception occurred. We must not call close() because
                # it would try to write end-of-archive blocks and padding.
                if not self._extfileobj:
                    self.fileobj.close()
                self.closed = True

    #--------------------
    # exported functions
    #--------------------

    def is_tarfile(name):
        """Return True if name points to a tar archive that we
           are able to handle, else return False.

           'name' should be a string, file, or file-like object.
        """
        try:
            if hasattr(name, "read"):
                pos = name.tell()
                t = open(fileobj=name)
                name.seek(pos)
            else:
                t = open(name)
            t.close()
            return True
        except TarError:
            return False

    open = TarFile.open


    def main():
        import argparse

        description = 'A simple command-line interface for tarfile module.'
        parser = argparse.ArgumentParser(description=description, color=True)
        parser.add_argument('-v', '--verbose', action='store_true', default=False,
                            help='Verbose output')
        parser.add_argument('--filter', metavar='<filtername>',
                            choices=_NAMED_FILTERS,
                            help='Filter for extraction')

        group = parser.add_mutually_exclusive_group(required=True)
        group.add_argument('-l', '--list', metavar='<tarfile>',
                           help='Show listing of a tarfile')
        group.add_argument('-e', '--extract', nargs='+',
                           metavar=('<tarfile>', '<output_dir>'),
                           help='Extract tarfile into target dir')
        group.add_argument('-c', '--create', nargs='+',
                           metavar=('<name>', '<file>'),
                           help='Create tarfile from sources')
        group.add_argument('-t', '--test', metavar='<tarfile>',
                           help='Test if a tarfile is valid')

        args = parser.parse_args()

        if args.filter and args.extract is None:
            parser.exit(1, '--filter is only valid for extraction\n')

        if args.test is not None:
            src = args.test
            if is_tarfile(src):
                with open(src, 'r') as tar:
                    tar.getmembers()
                    print(tar.getmembers(), file=sys.stderr)
                if args.verbose:
                    print('{!r} is a tar archive.'.format(src))
            else:
                parser.exit(1, '{!r} is not a tar archive.\n'.format(src))

        elif args.list is not None:
            src = args.list
            if is_tarfile(src):
                with TarFile.open(src, 'r:*') as tf:
                    tf.list(verbose=args.verbose)
            else:
                parser.exit(1, '{!r} is not a tar archive.\n'.format(src))

        elif args.extract is not None:
            if len(args.extract) == 1:
                src = args.extract[0]
                curdir = os.curdir
            elif len(args.extract) == 2:
                src, curdir = args.extract
            else:
                parser.exit(1, parser.format_help())

            if is_tarfile(src):
                with TarFile.open(src, 'r:*') as tf:
                    tf.extractall(path=curdir, filter=args.filter)
                if args.verbose:
                    if curdir == '.':
                        msg = '{!r} file is extracted.'.format(src)
                    else:
                        msg = ('{!r} file is extracted '
                               'into {!r} directory.').format(src, curdir)
                    print(msg)
            else:
                parser.exit(1, '{!r} is not a tar archive.\n'.format(src))

        elif args.create is not None:
            tar_name = args.create.pop(0)
            _, ext = os.path.splitext(tar_name)
            compressions = {
                # gz
                '.gz': 'gz',
                '.tgz': 'gz',
                # xz
                '.xz': 'xz',
                '.txz': 'xz',
                # bz2
                '.bz2': 'bz2',
                '.tbz': 'bz2',
                '.tbz2': 'bz2',
                '.tb2': 'bz2',
                # zstd
                '.zst': 'zst',
                '.tzst': 'zst',
            }
            tar_mode = 'w:' + compressions[ext] if ext in compressions else 'w'
            tar_files = args.create

            with TarFile.open(tar_name, tar_mode) as tf:
                for file_name in tar_files:
                    tf.add(file_name)

            if args.verbose:
                print('{!r} file created.'.format(tar_name))

    if __name__ == '__main__':
        main()


if sys._compiled:
    # Compiled programs: the same formats (ustar, GNU, pax; gzip compression), kept in memory: an
    # archive opened for reading is read whole, one opened for writing is written when closed.
    # TarInfo.mtime is a float (a pax mtime record is written when it has a fraction); filters set no
    # None attributes (the data filter's mode / owner changes are applied when extracting).
    import os
    import io
    import stat
    import time
    import gzip
    from typing import Callable, TypeVar

    _F = TypeVar("_F")
    _M = TypeVar("_M")
    _P = TypeVar("_P")

    NUL = b"\0"
    BLOCKSIZE = 512
    RECORDSIZE = BLOCKSIZE * 20
    GNU_MAGIC = b"ustar  \0"
    POSIX_MAGIC = b"ustar\x0000"
    LENGTH_NAME = 100
    LENGTH_LINK = 100
    LENGTH_PREFIX = 155
    REGTYPE = b"0"
    AREGTYPE = b"\0"
    LNKTYPE = b"1"
    SYMTYPE = b"2"
    CHRTYPE = b"3"
    BLKTYPE = b"4"
    DIRTYPE = b"5"
    FIFOTYPE = b"6"
    CONTTYPE = b"7"
    GNUTYPE_LONGNAME = b"L"
    GNUTYPE_LONGLINK = b"K"
    GNUTYPE_SPARSE = b"S"
    XHDTYPE = b"x"
    XGLTYPE = b"g"
    SOLARIS_XHDTYPE = b"X"
    USTAR_FORMAT = 0
    GNU_FORMAT = 1
    PAX_FORMAT = 2
    DEFAULT_FORMAT = PAX_FORMAT
    SUPPORTED_TYPES = (REGTYPE, AREGTYPE, LNKTYPE, SYMTYPE, DIRTYPE, FIFOTYPE, CONTTYPE, CHRTYPE, BLKTYPE,
                       GNUTYPE_LONGNAME, GNUTYPE_LONGLINK, GNUTYPE_SPARSE)
    REGULAR_TYPES = (REGTYPE, AREGTYPE, CONTTYPE, GNUTYPE_SPARSE)
    GNU_TYPES = (GNUTYPE_LONGNAME, GNUTYPE_LONGLINK, GNUTYPE_SPARSE)
    PAX_FIELDS = ("path", "linkpath", "size", "mtime", "uid", "gid", "uname", "gname")
    ENCODING = "utf-8"

    class TarError(Exception):
        """Base exception."""

    class ExtractError(TarError):
        """General exception for extract errors."""

    class ReadError(TarError):
        """Exception for unreadable tar archives."""

    class CompressionError(TarError):
        """Exception for unavailable compression methods."""

    class StreamError(TarError):
        """Exception for unsupported operations on stream-like TarFiles."""

    class HeaderError(TarError):
        """Base exception for header errors."""

    class EmptyHeaderError(HeaderError):
        """Exception for empty headers."""

    class TruncatedHeaderError(HeaderError):
        """Exception for truncated headers."""

    class EOFHeaderError(HeaderError):
        """Exception for end of file headers."""

    class InvalidHeaderError(HeaderError):
        """Exception for invalid headers."""

    class SubsequentHeaderError(HeaderError):
        """Exception for missing and invalid extended headers."""

    class FilterError(TarError):
        pass

    class AbsolutePathError(FilterError):
        def __init__(self, tarinfo: "TarInfo") -> None:
            super().__init__("member %r has an absolute path" % tarinfo.name)
            self.tarinfo = tarinfo

    class OutsideDestinationError(FilterError):
        def __init__(self, tarinfo: "TarInfo", path: str) -> None:
            super().__init__("%r would be extracted to %r, which is outside the destination" % (tarinfo.name, path))
            self.tarinfo = tarinfo
            self._path = path

    class SpecialFileError(FilterError):
        def __init__(self, tarinfo: "TarInfo") -> None:
            super().__init__("%r is a special file" % tarinfo.name)
            self.tarinfo = tarinfo

    class AbsoluteLinkError(FilterError):
        def __init__(self, tarinfo: "TarInfo") -> None:
            super().__init__("%r is a link to an absolute path" % tarinfo.name)
            self.tarinfo = tarinfo

    class LinkOutsideDestinationError(FilterError):
        def __init__(self, tarinfo: "TarInfo", path: str) -> None:
            super().__init__("%r would link to %r, which is outside the destination" % (tarinfo.name, path))
            self.tarinfo = tarinfo
            self._path = path

    # ------------------------------------------------------------ fields

    def stn(s: str, length: int, encoding: str, errors: str) -> bytes:
        """A string as a NUL-padded bytes field."""
        b = s.encode(encoding, errors)
        return b[:length] + (length - len(b)) * NUL

    def nts(s: bytes, encoding: str, errors: str) -> str:
        """A NUL-terminated bytes field as a string."""
        p = s.find(b"\0")
        if p != -1:
            s = s[:p]
        return s.decode(encoding, errors)

    def nti(s: bytes) -> int:
        """A number field as an int."""
        if s[0] == 0o200 or s[0] == 0o377:
            n = 0
            for i in range(len(s) - 1):
                n <<= 8
                n += s[i + 1]
            if s[0] == 0o377:
                n = -((1 << (8 * (len(s) - 1))) - n)
            return n
        t = ""
        try:
            t = nts(s, "ascii", "strict").strip()
            return int(t or "0", 8)
        except ValueError:
            raise InvalidHeaderError("invalid header")
        except UnicodeDecodeError:
            raise InvalidHeaderError("invalid header")

    def itn(n: int, digits: int = 8, format: int = DEFAULT_FORMAT) -> bytes:
        """An int as a number field (digits octal digits, or GNU's base-256)."""
        if 0 <= n < (1 << (3 * (digits - 1))):
            return format_octal(n, digits - 1).encode("ascii") + NUL
        if format == GNU_FORMAT and n < (1 << (8 * (digits - 1))) and n >= -(1 << (8 * (digits - 1))):
            out: list[int] = []
            v = n
            if n < 0:
                v = (1 << (8 * digits)) + n
            for i in range(digits - 1):
                out.append(v & 0o377)
                v >>= 8
            out.append(0o200 if n >= 0 else 0o377)
            out.reverse()
            return bytes(out)
        raise ValueError("overflow in number field")

    def format_octal(n: int, width: int) -> str:
        s = format(n, "o")
        return "0" * (width - len(s)) + s

    def calc_chksums(buf: bytes) -> tuple[int, int]:
        """The unsigned and the signed checksum of a header block (its checksum field taken as spaces)."""
        u = 256
        sg = 256
        for i in range(512):
            if 148 <= i < 156:
                continue
            b = buf[i]
            u += b
            sg += b - 256 if b >= 128 else b
        return (u, sg)

    def _block(count: int) -> int:
        if count < 0:
            raise InvalidHeaderError("invalid offset")
        blocks, remainder = divmod(count, BLOCKSIZE)
        if remainder:
            blocks += 1
        return blocks * BLOCKSIZE

    def _create_payload(payload: bytes) -> bytes:
        r = len(payload) % BLOCKSIZE
        if r > 0:
            payload += (BLOCKSIZE - r) * NUL
        return payload

    def _num_text(v: float) -> str:
        """A number as the text of a pax record (an integral value without a fraction)."""
        if v == int(v):
            return str(int(v))
        return repr(v)

    # ------------------------------------------------------------ TarInfo

    class TarInfo:
        """Informational class which holds the details about an archive member given by a tar header
        block."""

        def __init__(self, name: str = "") -> None:
            self.name: str = name
            self.mode: int = 0o644
            self.uid: int = 0
            self.gid: int = 0
            self.size: int = 0
            self.mtime: float = 0.0
            self.chksum: int = 0
            self.type: bytes = REGTYPE
            self.linkname: str = ""
            self.uname: str = ""
            self.gname: str = ""
            self.devmajor: int = 0
            self.devminor: int = 0
            self.offset: int = 0
            self.offset_data: int = 0
            self.pax_headers: dict[str, str] = {}
            self.sparse: list[tuple[int, int]] | None = None
            self._link_target = ""

        @property
        def path(self) -> str:
            """In pax headers, "name" is called "path"."""
            return self.name

        @path.setter
        def path(self, name: str) -> None:
            self.name = name

        @property
        def linkpath(self) -> str:
            """In pax headers, "linkname" is called "linkpath"."""
            return self.linkname

        @linkpath.setter
        def linkpath(self, linkname: str) -> None:
            self.linkname = linkname

        def __repr__(self) -> str:
            return "<%s %r at %#x>" % ("TarInfo", self.name, id(self))

        def _copy(self) -> "TarInfo":
            t = TarInfo(self.name)
            t.mode = self.mode
            t.uid = self.uid
            t.gid = self.gid
            t.size = self.size
            t.mtime = self.mtime
            t.chksum = self.chksum
            t.type = self.type
            t.linkname = self.linkname
            t.uname = self.uname
            t.gname = self.gname
            t.devmajor = self.devmajor
            t.devminor = self.devminor
            t.offset = self.offset
            t.offset_data = self.offset_data
            t.pax_headers = dict(self.pax_headers)
            t.sparse = self.sparse
            t._link_target = self._link_target
            return t

        def replace(self, *, name: str | None = None, mtime: float | None = None, mode: int | None = None,
                    linkname: str | None = None, uid: int | None = None, gid: int | None = None,
                    uname: str | None = None, gname: str | None = None, deep: bool = True) -> "TarInfo":
            """A copy with the given attributes changed."""
            t = self._copy()
            if name is not None:
                t.name = name
            if mtime is not None:
                t.mtime = mtime
            if mode is not None:
                t.mode = mode
            if linkname is not None:
                t.linkname = linkname
            if uid is not None:
                t.uid = uid
            if gid is not None:
                t.gid = gid
            if uname is not None:
                t.uname = uname
            if gname is not None:
                t.gname = gname
            return t

        def _info_name(self) -> str:
            if self.type == DIRTYPE and not self.name.endswith("/"):
                return self.name + "/"
            return self.name

        def tobuf(self, format: int = DEFAULT_FORMAT, encoding: str = ENCODING,
                  errors: str = "surrogateescape") -> bytes:
            """The tar header blocks of this member."""
            if format == USTAR_FORMAT:
                return self.create_ustar_header(encoding, errors)
            elif format == GNU_FORMAT:
                return self.create_gnu_header(encoding, errors)
            elif format == PAX_FORMAT:
                return self.create_pax_header(encoding)
            raise ValueError("invalid format")

        def create_ustar_header(self, encoding: str, errors: str) -> bytes:
            name = self._info_name()
            prefix = ""
            if len(self.linkname.encode(encoding, errors)) > LENGTH_LINK:
                raise ValueError("linkname is too long")
            if len(name.encode(encoding, errors)) > LENGTH_NAME:
                prefix, name = self._posix_split_name(name, encoding, errors)
            return _create_header(name, (self.mode & 0o7777, self.uid, self.gid, self.size, int(self.mtime)),
                                  self.type, self.linkname, POSIX_MAGIC, self.uname, self.gname, self.devmajor,
                                  self.devminor, prefix, USTAR_FORMAT, encoding, errors)

        def create_gnu_header(self, encoding: str, errors: str) -> bytes:
            name = self._info_name()
            buf = b""
            if len(self.linkname.encode(encoding, errors)) > LENGTH_LINK:
                buf += _create_gnu_long_header(self.linkname, GNUTYPE_LONGLINK, encoding, errors)
            if len(name.encode(encoding, errors)) > LENGTH_NAME:
                buf += _create_gnu_long_header(name, GNUTYPE_LONGNAME, encoding, errors)
            return buf + _create_header(name, (self.mode & 0o7777, self.uid, self.gid, self.size, int(self.mtime)),
                                        self.type, self.linkname, GNU_MAGIC, self.uname, self.gname, self.devmajor,
                                        self.devminor, "", GNU_FORMAT, encoding, errors)

        def create_pax_header(self, encoding: str) -> bytes:
            pax_headers = dict(self.pax_headers)
            name = self._info_name()
            texts = [("path", name, LENGTH_NAME), ("linkpath", self.linkname, LENGTH_LINK),
                     ("uname", self.uname, 32), ("gname", self.gname, 32)]
            for hname, value, length in texts:
                if hname in pax_headers:
                    continue
                if not value.isascii():
                    pax_headers[hname] = value
                    continue
                if len(value) > length:
                    pax_headers[hname] = value
            nums = [("uid", float(self.uid), 8), ("gid", float(self.gid), 8), ("size", float(self.size), 12),
                    ("mtime", self.mtime, 12)]
            out: dict[str, int] = {}
            for hname, val, digits in nums:
                is_float = hname == "mtime" and val != int(val)
                val_int = round(val) if is_float else int(val)
                needs_pax = False
                if not 0 <= val_int < (1 << (3 * (digits - 1))):
                    out[hname] = 0
                    needs_pax = True
                else:
                    out[hname] = val_int
                    if is_float:
                        needs_pax = True
                if needs_pax and hname not in pax_headers:
                    pax_headers[hname] = repr(val) if is_float else _num_text(val)
            buf = b""
            if pax_headers:
                buf = _create_pax_generic_header(pax_headers, XHDTYPE, encoding)
            return buf + _create_header(name, (self.mode & 0o7777, out["uid"], out["gid"], out["size"], out["mtime"]),
                                        self.type, self.linkname, POSIX_MAGIC, self.uname, self.gname,
                                        self.devmajor, self.devminor, "", USTAR_FORMAT, "ascii", "replace")

        def _posix_split_name(self, name: str, encoding: str, errors: str) -> tuple[str, str]:
            components = name.split("/")
            for i in range(1, len(components)):
                prefix = "/".join(components[:i])
                rest = "/".join(components[i:])
                if len(prefix.encode(encoding, errors)) <= LENGTH_PREFIX and \
                        len(rest.encode(encoding, errors)) <= LENGTH_NAME:
                    return (prefix, rest)
            raise ValueError("name is too long")

        def isreg(self) -> bool:
            """Return True if the Tarinfo object is a regular file."""
            return self.type in REGULAR_TYPES

        def isfile(self) -> bool:
            """Return True if the Tarinfo object is a regular file."""
            return self.isreg()

        def isdir(self) -> bool:
            """Return True if it is a directory."""
            return self.type == DIRTYPE

        def issym(self) -> bool:
            """Return True if it is a symbolic link."""
            return self.type == SYMTYPE

        def islnk(self) -> bool:
            """Return True if it is a hard link."""
            return self.type == LNKTYPE

        def ischr(self) -> bool:
            return self.type == CHRTYPE

        def isblk(self) -> bool:
            return self.type == BLKTYPE

        def isfifo(self) -> bool:
            return self.type == FIFOTYPE

        def issparse(self) -> bool:
            return self.sparse is not None

        def isdev(self) -> bool:
            return self.type == CHRTYPE or self.type == BLKTYPE or self.type == FIFOTYPE

        def _apply_pax_info(self, pax_headers: dict[str, str]) -> None:
            for keyword in pax_headers:
                value = pax_headers[keyword]
                if keyword == "GNU.sparse.name" or keyword == "path":
                    self.name = value.rstrip("/") if keyword == "path" else value
                elif keyword == "GNU.sparse.size" or keyword == "GNU.sparse.realsize" or keyword == "size":
                    try:
                        self.size = int(value)
                    except ValueError:
                        self.size = 0
                elif keyword == "linkpath":
                    self.linkname = value
                elif keyword == "mtime":
                    try:
                        self.mtime = float(value)
                    except ValueError:
                        self.mtime = 0.0
                elif keyword == "uid":
                    try:
                        self.uid = int(value)
                    except ValueError:
                        self.uid = 0
                elif keyword == "gid":
                    try:
                        self.gid = int(value)
                    except ValueError:
                        self.gid = 0
                elif keyword == "uname":
                    self.uname = value
                elif keyword == "gname":
                    self.gname = value
            self.pax_headers = dict(pax_headers)

    def frombuf(buf: bytes, encoding: str, errors: str, dircheck: bool = True) -> TarInfo:
        """A TarInfo from a 512 byte header block."""
        if len(buf) == 0:
            raise EmptyHeaderError("empty header")
        if len(buf) != BLOCKSIZE:
            raise TruncatedHeaderError("truncated header")
        if buf.count(NUL) == BLOCKSIZE:
            raise EOFHeaderError("end of file header")
        chksum = nti(buf[148:156])
        u, sg = calc_chksums(buf)
        if chksum != u and chksum != sg:
            raise InvalidHeaderError("bad checksum")
        obj = TarInfo()
        obj.name = nts(buf[0:100], encoding, errors)
        obj.mode = nti(buf[100:108])
        obj.uid = nti(buf[108:116])
        obj.gid = nti(buf[116:124])
        obj.size = nti(buf[124:136])
        obj.mtime = float(nti(buf[136:148]))
        obj.chksum = chksum
        obj.type = buf[156:157]
        obj.linkname = nts(buf[157:257], encoding, errors)
        obj.uname = nts(buf[265:297], encoding, errors)
        obj.gname = nts(buf[297:329], encoding, errors)
        obj.devmajor = nti(buf[329:337])
        obj.devminor = nti(buf[337:345])
        prefix = nts(buf[345:500], encoding, errors)
        if dircheck and obj.type == AREGTYPE and obj.name.endswith("/"):
            obj.type = DIRTYPE
        if obj.isdir():
            obj.name = obj.name.rstrip("/")
        if prefix and obj.type not in GNU_TYPES:
            obj.name = prefix + "/" + obj.name
        return obj

    def _create_header(name: str, nums: tuple[int, int, int, int, int], filetype: bytes, linkname: str,
                       magic: bytes, uname: str, gname: str, devmajor: int, devminor: int, prefix: str, format: int,
                       encoding: str, errors: str) -> bytes:
        """A header block; nums: mode, uid, gid, size, mtime."""
        mode, uid, gid, size, mtime = nums
        if filetype == CHRTYPE or filetype == BLKTYPE:
            dmaj = itn(devmajor, 8, format)
            dmin = itn(devminor, 8, format)
        else:
            dmaj = stn("", 8, encoding, errors)
            dmin = stn("", 8, encoding, errors)
        parts = [stn(name, 100, encoding, errors), itn(mode & 0o7777, 8, format), itn(uid, 8, format),
                 itn(gid, 8, format), itn(size, 12, format), itn(mtime, 12, format), b"        ", filetype,
                 stn(linkname, 100, encoding, errors), magic, stn(uname, 32, encoding, errors),
                 stn(gname, 32, encoding, errors), dmaj, dmin, stn(prefix, 155, encoding, errors)]
        buf = b"".join(parts)
        buf = buf + (BLOCKSIZE - len(buf)) * NUL
        chksum = calc_chksums(buf)[0]
        return buf[:-364] + ("%06o" % chksum).encode("ascii") + NUL + buf[-357:]

    def _create_gnu_long_header(name: str, type: bytes, encoding: str, errors: str) -> bytes:
        data = name.encode(encoding, errors) + NUL
        return (_create_header("././@LongLink", (0, 0, 0, len(data), 0), type, "", GNU_MAGIC, "", "", 0, 0, "",
                               USTAR_FORMAT, encoding, errors) + _create_payload(data))

    def _create_pax_generic_header(pax_headers: dict[str, str], type: bytes, encoding: str) -> bytes:
        records = b""
        for keyword in pax_headers:
            kw = keyword.encode("utf-8")
            value = pax_headers[keyword].encode("utf-8")
            n = len(kw) + len(value) + 3
            p = 0
            while True:
                m = n + len(str(p))
                if m == p:
                    break
                p = m
            records += str(p).encode("ascii") + b" " + kw + b"=" + value + b"\n"
        return (_create_header("././@PaxHeader", (0, 0, 0, len(records), 0), type, "", POSIX_MAGIC, "", "", 0, 0, "",
                               USTAR_FORMAT, "ascii", "replace") + _create_payload(records))

    # ------------------------------------------------------------ member files

    class _MemberFile:
        """extractfile()'s file object: a member's data (read-only, seekable)."""

        def __init__(self, data: bytes, name: str) -> None:
            self._data = data
            self.name = name
            self.mode = "rb"
            self._pos = 0
            self.closed = False

        def _check(self) -> None:
            if self.closed:
                raise ValueError("I/O operation on closed file.")

        def read(self, size: int | None = -1) -> bytes:
            self._check()
            if size is None or size < 0:
                out = self._data[self._pos:]
            else:
                out = self._data[self._pos:self._pos + size]
            self._pos += len(out)
            return out

        def read1(self, size: int = -1) -> bytes:
            return self.read(size)

        def readline(self, size: int = -1) -> bytes:
            self._check()
            nl = self._data.find(b"\n", self._pos)
            end = len(self._data) if nl < 0 else nl + 1
            if size >= 0 and end - self._pos > size:
                end = self._pos + size
            out = self._data[self._pos:end]
            self._pos = end
            return out

        def readlines(self, hint: int = -1) -> list[bytes]:
            lines: list[bytes] = []
            total = 0
            while True:
                line = self.readline()
                if not line:
                    break
                lines.append(line)
                total += len(line)
                if 0 < hint <= total:
                    break
            return lines

        def __iter__(self):
            while True:
                line = self.readline()
                if not line:
                    return
                yield line

        def seek(self, pos: int, whence: int = 0) -> int:
            self._check()
            if whence == 0:
                self._pos = min(max(pos, 0), len(self._data))
            elif whence == 1:
                self._pos = min(max(self._pos + pos, 0), len(self._data))
            elif whence == 2:
                self._pos = max(min(len(self._data) + pos, len(self._data)), 0)
            else:
                raise ValueError("Invalid argument")
            return self._pos

        def tell(self) -> int:
            self._check()
            return self._pos

        def readable(self) -> bool:
            return True

        def writable(self) -> bool:
            return False

        def seekable(self) -> bool:
            return True

        def close(self) -> None:
            self.closed = True

        def __enter__(self) -> "_MemberFile":
            return self

        def __exit__(self, t, v, tb) -> None:
            self.close()

    # ------------------------------------------------------------ TarFile

    _FILTERS = ("fully_trusted", "tar", "data")

    class TarFile:
        """The TarFile Class provides an interface to tar archives."""

        def __init__(self, name: str | None = None, mode: str = "r", fileobj: _F = None, format: int | None = None,
                     dereference: bool | None = None, ignore_zeros: bool | None = None,
                     encoding: str | None = None, errors: str = "surrogateescape",
                     pax_headers: dict[str, str] | None = None, debug: int | None = None,
                     errorlevel: int | None = None) -> None:
            """Open an (uncompressed) tar archive name: mode 'r' (reading), 'a' (appending), 'w' or 'x'
            (writing); fileobj: the archive's file object instead."""
            if mode not in ("r", "a", "w", "x"):
                raise ValueError("mode must be 'r', 'a', 'w' or 'x'")
            self.mode = mode
            self.format = DEFAULT_FORMAT if format is None else format
            self.encoding = ENCODING if encoding is None else encoding
            self.errors = errors
            self.ignore_zeros = False if ignore_zeros is None else ignore_zeros
            self.dereference = False if dereference is None else dereference
            self.errorlevel = 1 if errorlevel is None else errorlevel
            self.debug = 0 if debug is None else debug
            self.extraction_filter: str | None = None
            self.closed = False
            self.members: list[TarInfo] = []
            self._loaded = False
            self.offset = 0
            self.pax_headers: dict[str, str] = {}
            self._comptype = "tar"
            self._compresslevel = 9
            self._data = b""
            self._pos = 0
            self._out: list[bytes] = []
            self._sink: Callable[[bytes], None] | None = None
            self.inodes: dict[tuple[int, int], str] = {}
            self.firstmember: TarInfo | None = None
            if fileobj is None:
                if name is None:
                    raise ValueError("nothing to open")
                nm = name
                if mode == "a" and not os.path.exists(nm):
                    self.mode = "w"
                if self.mode == "r" or self.mode == "a":
                    with io.open(nm, "rb") as fr:
                        self._data = fr.read()
                elif self.mode == "w":
                    with io.open(nm, "wb") as fw:
                        pass
                else:
                    with io.open(nm, "xb") as fx:
                        pass

                def to_file(b: bytes) -> None:
                    with io.open(nm, "wb") as f2:
                        f2.write(b)
                if self.mode != "r":
                    self._sink = to_file
            else:
                if name is None and hasattr(fileobj, "name"):
                    fname = fileobj.name
                    if isinstance(fname, str):
                        name = fname
                start = 0
                if self.mode == "r" or self.mode == "a":
                    start = fileobj.tell()
                    self._data = fileobj.read()

                def to_fileobj(b: bytes) -> None:
                    if self.mode == "a":
                        fileobj.seek(start)
                    fileobj.write(b)
                if self.mode != "r":
                    self._sink = to_fileobj
            self.name = os.path.abspath(name) if name else None
            if self.mode == "r":
                self.firstmember = self.next()
            elif self.mode == "a":
                while True:
                    self._pos = self.offset
                    try:
                        tarinfo1 = self._fromtarfile(True)
                        self.members.append(tarinfo1)
                    except EOFHeaderError:
                        break
                    except EmptyHeaderError:
                        break
                    except HeaderError as e:
                        raise ReadError(str(e))
                self._out.append(self._data[:self.offset])
            if self.mode in ("a", "w", "x"):
                self._loaded = True
                if pax_headers and self.format == PAX_FORMAT:
                    self.pax_headers = dict(pax_headers)
                    buf = _create_pax_generic_header(dict(pax_headers), XGLTYPE, "utf-8")
                    self._out.append(buf)
                    self.offset += len(buf)

        # ---- reading

        def _read(self, n: int) -> bytes:
            b = self._data[self._pos:self._pos + n]
            self._pos += len(b)
            return b

        def _fromtarfile(self, dircheck: bool) -> TarInfo:
            buf = self._read(BLOCKSIZE)
            obj = frombuf(buf, self.encoding, self.errors, dircheck)
            obj.offset = self._pos - BLOCKSIZE
            return self._proc_member(obj)

        def _proc_member(self, obj: TarInfo) -> TarInfo:
            if obj.type == GNUTYPE_LONGNAME or obj.type == GNUTYPE_LONGLINK:
                return self._proc_gnulong(obj)
            elif obj.type == XHDTYPE or obj.type == XGLTYPE or obj.type == SOLARIS_XHDTYPE:
                return self._proc_pax(obj)
            return self._proc_builtin(obj)

        def _proc_builtin(self, obj: TarInfo) -> TarInfo:
            obj.offset_data = self._pos
            offset = obj.offset_data
            if obj.isreg() or obj.type not in SUPPORTED_TYPES:
                offset += _block(obj.size)
            self.offset = offset
            obj._apply_pax_info(self.pax_headers)
            if obj.isdir():
                obj.name = obj.name.rstrip("/")
            return obj

        def _proc_gnulong(self, obj: TarInfo) -> TarInfo:
            buf = self._read(_block(obj.size))
            try:
                nxt = self._fromtarfile(False)
            except HeaderError as e:
                raise SubsequentHeaderError(str(e))
            nxt.offset = obj.offset
            if obj.type == GNUTYPE_LONGNAME:
                nxt.name = nts(buf, self.encoding, self.errors)
            else:
                nxt.linkname = nts(buf, self.encoding, self.errors)
            if nxt.isdir():
                nxt.name = nxt.name.removesuffix("/")
            return nxt

        def _proc_pax(self, obj: TarInfo) -> TarInfo:
            buf = self._read(_block(obj.size))
            if obj.type == XGLTYPE:
                pax_headers = self.pax_headers
            else:
                pax_headers = dict(self.pax_headers)
            pos = 0
            while len(buf) > pos and buf[pos] != 0:
                sp = buf.find(b" ", pos)
                if sp < 0 or sp - pos > 20 or sp == pos or not buf[pos:sp].isdigit():
                    raise InvalidHeaderError("invalid header")
                length = int(buf[pos:sp])
                if length < 5 or pos + length > len(buf):
                    raise InvalidHeaderError("invalid header")
                end = pos + length - 1
                kv = buf[sp + 1:end]
                eq = kv.find(b"=")
                if eq <= 0 or buf[end] != 0x0A:
                    raise InvalidHeaderError("invalid header")
                keyword = kv[:eq].decode("utf-8", self.errors)
                value = kv[eq + 1:].decode("utf-8", self.errors)
                pax_headers[keyword] = value
                pos += length
            try:
                nxt = self._fromtarfile(False)
            except HeaderError as e:
                raise SubsequentHeaderError(str(e))
            if obj.type == XHDTYPE or obj.type == SOLARIS_XHDTYPE:
                nxt._apply_pax_info(pax_headers)
                nxt.offset = obj.offset
                if "size" in pax_headers:
                    offset = nxt.offset_data
                    if nxt.isreg() or nxt.type not in SUPPORTED_TYPES:
                        offset += _block(nxt.size)
                    self.offset = offset
            return nxt

        def next(self) -> TarInfo | None:
            """The next member of the archive (None: no more)."""
            self._check("ra")
            if self._pos != self.offset:
                if self.offset == 0:
                    return None
                if self.offset > len(self._data):
                    raise ReadError("unexpected end of data")
                self._pos = self.offset
            tarinfo: TarInfo | None = None
            while True:
                try:
                    tarinfo = self._fromtarfile(True)
                except EOFHeaderError as e:
                    if self.ignore_zeros:
                        self.offset += BLOCKSIZE
                        self._pos = self.offset
                        continue
                    tarinfo = None
                except InvalidHeaderError as e:
                    if self.ignore_zeros:
                        self.offset += BLOCKSIZE
                        self._pos = self.offset
                        continue
                    elif self.offset == 0:
                        raise ReadError(str(e))
                    tarinfo = None
                except EmptyHeaderError:
                    if self.offset == 0:
                        raise ReadError("empty file")
                    tarinfo = None
                except TruncatedHeaderError as e:
                    if self.offset == 0:
                        raise ReadError(str(e))
                    tarinfo = None
                except SubsequentHeaderError as e:
                    raise ReadError(str(e))
                break
            if tarinfo is not None:
                self.members.append(tarinfo)
            else:
                self._loaded = True
            return tarinfo

        def _load(self) -> None:
            if not self._loaded:
                while self.next() is not None:
                    pass
                self._loaded = True

        def _check(self, mode: str | None = None) -> None:
            if self.closed:
                raise OSError("%s is closed" % "TarFile")
            if mode is not None and self.mode not in mode:
                raise OSError("bad operation for mode %r" % self.mode)

        def getmembers(self) -> list[TarInfo]:
            """The members of the archive as a list of TarInfo objects."""
            self._check()
            self._load()
            return self.members

        def getnames(self) -> list[str]:
            """The members of the archive as a list of their names."""
            return [t.name for t in self.getmembers()]

        def _getmember(self, name: str, upto: TarInfo | None = None) -> TarInfo | None:
            members = self.getmembers()
            skipping = upto is not None
            for i in range(len(members) - 1, -1, -1):
                m = members[i]
                if skipping:
                    if m is upto:
                        skipping = False
                    continue
                if m.name == name:
                    return m
            return None

        def getmember(self, name: str) -> TarInfo:
            """The TarInfo of member name (KeyError: none); its last one when there are several."""
            t = self._getmember(name.rstrip('/'))
            if t is None:
                raise KeyError("filename %r not found" % name)
            return t

        def __iter__(self):
            index = 0
            while True:
                if index < len(self.members):
                    t = self.members[index]
                elif self._loaded:
                    return
                else:
                    t1 = self.next()
                    if t1 is None:
                        self._loaded = True
                        return
                    t = t1
                index += 1
                yield t

        def _member_of(self, member: _M) -> TarInfo:
            if isinstance(member, str):
                return self.getmember(member)
            else:
                return member

        def _find_link_target(self, tarinfo: TarInfo) -> TarInfo:
            if tarinfo.issym():
                linkname = "/".join([x for x in [os.path.dirname(tarinfo.name), tarinfo.linkname] if x])
                limit: TarInfo | None = None
            else:
                linkname = tarinfo.linkname
                limit = tarinfo
            m = self._getmember(os.path.normpath(linkname), limit)
            if m is None:
                raise KeyError("linkname %r not found" % linkname)
            return m

        def extractfile(self, member: _M) -> _MemberFile | None:
            """A file object of member's data (a name or a TarInfo); None for what is not a file or a link."""
            self._check("r")
            tarinfo = self._member_of(member)
            for _ in range(32):
                if tarinfo.isreg() or tarinfo.type not in SUPPORTED_TYPES:
                    return _MemberFile(self._data[tarinfo.offset_data:tarinfo.offset_data + tarinfo.size],
                                       tarinfo.name)
                elif tarinfo.islnk() or tarinfo.issym():
                    tarinfo = self._find_link_target(tarinfo)
                else:
                    return None
            return None

        # ---- writing

        def gettarinfo(self, name: str | None = None, arcname: str | None = None, fileobj: _F = None) -> TarInfo | None:
            """A TarInfo for the file name (from os.stat()); arcname: its name in the archive."""
            self._check("awx")
            if name is None:
                if fileobj is None:
                    raise ValueError("gettarinfo() needs a name")
                else:
                    name = str(fileobj.name)
            if arcname is None:
                arcname = name
            arcname = arcname.replace(os.sep, "/").lstrip("/")
            tarinfo = TarInfo()
            statres = os.stat(name) if self.dereference else os.lstat(name)
            linkname = ""
            stmd = statres.st_mode
            if stat.S_ISREG(stmd):
                inode = (statres.st_ino, statres.st_dev)
                if not self.dereference and statres.st_nlink > 1 and inode in self.inodes and \
                        arcname != self.inodes[inode]:
                    ftype = LNKTYPE
                    linkname = self.inodes[inode]
                else:
                    ftype = REGTYPE
                    if inode[0]:
                        self.inodes[inode] = arcname
            elif stat.S_ISDIR(stmd):
                ftype = DIRTYPE
            elif stat.S_ISFIFO(stmd):
                ftype = FIFOTYPE
            elif stat.S_ISLNK(stmd):
                ftype = SYMTYPE
                linkname = os.readlink(name)
            elif stat.S_ISCHR(stmd):
                ftype = CHRTYPE
            elif stat.S_ISBLK(stmd):
                ftype = BLKTYPE
            else:
                return None
            tarinfo.name = arcname
            tarinfo.mode = stmd
            tarinfo.uid = statres.st_uid
            tarinfo.gid = statres.st_gid
            tarinfo.size = statres.st_size if ftype == REGTYPE else 0
            tarinfo.mtime = statres.st_mtime
            tarinfo.type = ftype
            tarinfo.linkname = linkname
            return tarinfo

        def _write(self, b: bytes) -> None:
            self._out.append(b)

        def addfile(self, tarinfo: TarInfo, fileobj: _F = None) -> None:
            """Add the TarInfo tarinfo to the archive (fileobj: its data, tarinfo.size bytes)."""
            self._check("awx")
            if fileobj is None:
                if tarinfo.isreg() and tarinfo.size != 0:
                    raise ValueError("fileobj not provided for non zero-size regular file")
            t = tarinfo._copy()
            buf = t.tobuf(self.format, self.encoding, self.errors)
            self._write(buf)
            self.offset += len(buf)
            if fileobj is not None:
                data = fileobj.read(t.size)
                if len(data) < t.size:
                    raise OSError("unexpected end of data")
                self._write(data)
                blocks, remainder = divmod(t.size, BLOCKSIZE)
                if remainder > 0:
                    self._write(NUL * (BLOCKSIZE - remainder))
                    blocks += 1
                self.offset += blocks * BLOCKSIZE
            self.members.append(t)

        def add(self, name: str, arcname: str | None = None, recursive: bool = True, *, filter: _P = None) -> None:
            """Add the file name (a directory: with what is in it; filter(tarinfo): the TarInfo to add,
            or None: not this one)."""
            self._check("awx")
            if arcname is None:
                arcname = name
            if self.name is not None and os.path.abspath(name) == self.name:
                return
            tarinfo = self.gettarinfo(name, arcname)
            if tarinfo is None:
                return
            if filter is not None:
                tarinfo = filter(tarinfo)
                if tarinfo is None:
                    return
            if tarinfo.isreg():
                with io.open(name, "rb") as f:
                    self.addfile(tarinfo, f)
            elif tarinfo.isdir():
                self.addfile(tarinfo)
                if recursive:
                    for f2 in sorted(os.listdir(name)):
                        self.add(os.path.join(name, f2), os.path.join(arcname, f2), recursive, filter=filter)
            else:
                self.addfile(tarinfo)

        def close(self) -> None:
            """Close the TarFile (writing: the end of the archive is written, then the file)."""
            if self.closed:
                return
            self.closed = True
            if self.mode in ("a", "w", "x"):
                self._write(NUL * (BLOCKSIZE * 2))
                self.offset += BLOCKSIZE * 2
                remainder = self.offset % RECORDSIZE
                if remainder > 0:
                    self._write(NUL * (RECORDSIZE - remainder))
                data = b"".join(self._out)
                if self._comptype == "gz":
                    data = gzip.compress(data, self._compresslevel)
                sink = self._sink
                if sink is not None:
                    sink(data)

        def __enter__(self) -> "TarFile":
            self._check()
            return self

        def __exit__(self, type, value, traceback) -> None:
            self.close()

        # ---- listing, extracting

        def list(self, verbose: bool = True, *, members: list[TarInfo] | None = None) -> None:
            """Print a table of contents (verbose: like ls -l)."""
            self._check()
            ms = members if members is not None else self.getmembers()
            for tarinfo in ms:
                if verbose:
                    modetype = 0
                    if tarinfo.type == REGTYPE:
                        modetype = stat.S_IFREG
                    elif tarinfo.type == SYMTYPE:
                        modetype = stat.S_IFLNK
                    elif tarinfo.type == FIFOTYPE:
                        modetype = stat.S_IFIFO
                    elif tarinfo.type == CHRTYPE:
                        modetype = stat.S_IFCHR
                    elif tarinfo.type == DIRTYPE:
                        modetype = stat.S_IFDIR
                    elif tarinfo.type == BLKTYPE:
                        modetype = stat.S_IFBLK
                    print(stat.filemode(modetype | tarinfo.mode), end=' ')
                    print("%s/%s" % (tarinfo.uname or str(tarinfo.uid), tarinfo.gname or str(tarinfo.gid)), end=' ')
                    if tarinfo.ischr() or tarinfo.isblk():
                        print("%10s" % ("%d,%d" % (tarinfo.devmajor, tarinfo.devminor)), end=' ')
                    else:
                        print("%10d" % tarinfo.size, end=' ')
                    lt = time.localtime(tarinfo.mtime)
                    print("%d-%02d-%02d %02d:%02d:%02d" % (lt.tm_year, lt.tm_mon, lt.tm_mday, lt.tm_hour, lt.tm_min,
                                                         lt.tm_sec), end=' ')
                print(tarinfo.name + ("/" if tarinfo.isdir() else ""), end=' ')
                if verbose:
                    if tarinfo.issym():
                        print("-> " + tarinfo.linkname, end=' ')
                    if tarinfo.islnk():
                        print("link to " + tarinfo.linkname, end=' ')
                print()

        def _filter_name(self, filter: str | None) -> str:
            f = filter if filter is not None else self.extraction_filter
            if f is None:
                return "data"
            if f not in _FILTERS:
                raise ValueError("filter %r not found" % f)
            return f

        def _filtered(self, member: TarInfo, path: str, fname: str) -> tuple[TarInfo, int]:
            """(the member to extract, its mode; -1: leave it) after filter fname."""
            if fname == "fully_trusted":
                return (member, member.mode)
            for_data = fname == "data"
            t = member._copy()
            name = member.name
            dest_path = os.path.realpath(path, strict=os.path.ALLOW_MISSING)
            if name.startswith('/') or name.startswith(os.sep):
                name = member.name.lstrip('/' + os.sep)
                t.name = name
            if os.path.isabs(name):
                raise AbsolutePathError(member)
            target_path = os.path.realpath(os.path.join(dest_path, name), strict=os.path.ALLOW_MISSING)
            if os.path.commonpath([target_path, dest_path]) != dest_path:
                raise OutsideDestinationError(member, target_path)
            mode = member.mode & 0o755
            if for_data:
                if member.isreg() or member.islnk():
                    if not mode & 0o100:
                        mode &= ~0o111
                    mode |= 0o600
                elif member.isdir() or member.issym():
                    mode = -1
                else:
                    raise SpecialFileError(member)
            if mode >= 0:
                t.mode = mode
            if for_data:
                if member.islnk() or member.issym():
                    if os.path.isabs(member.linkname):
                        raise AbsoluteLinkError(member)
                    if target_path == dest_path:
                        raise OutsideDestinationError(member, target_path)
                    normalized = os.path.normpath(member.linkname)
                    t.linkname = normalized
                    if member.issym():
                        link_dir = os.path.dirname(name.rstrip('/' + os.sep))
                        tp = os.path.join(dest_path, link_dir, normalized)
                    else:
                        tp = os.path.join(dest_path, normalized)
                    tp = os.path.realpath(tp, strict=os.path.ALLOW_MISSING)
                    if os.path.commonpath([tp, dest_path]) != dest_path:
                        raise LinkOutsideDestinationError(member, tp)
            return (t, mode)

        def extractall(self, path: str = ".", members: list[TarInfo] | None = None, *, numeric_owner: bool = False,
                       filter: str | None = None) -> None:
            """Extract all members (or those given) to the directory path; filter: 'data' (the default),
            'tar' or 'fully_trusted'."""
            fname = self._filter_name(filter)
            directories: list[tuple[TarInfo, int]] = []
            ms = members if members is not None else self.getmembers()
            for member in ms:
                tm = self._filtered(member, path, fname)
                tarinfo, mode = tm
                if tarinfo.isdir():
                    directories.append(tm)
                self._extract_one(tarinfo, mode, path, not tarinfo.isdir())
            directories.sort(key=lambda a: a[0].name, reverse=True)
            for tarinfo, mode in directories:
                dirpath = os.path.join(path, tarinfo.name)
                try:
                    st = os.lstat(dirpath)
                except FileNotFoundError:
                    continue
                if not stat.S_ISDIR(st.st_mode):
                    continue
                self._utime(tarinfo, dirpath)
                self._chmod(mode, dirpath)

        def extract(self, member: _M, path: str = "", set_attrs: bool = True, *, numeric_owner: bool = False,
                    filter: str | None = None) -> None:
            """Extract member (a name or a TarInfo) to the directory path."""
            fname = self._filter_name(filter)
            tarinfo, mode = self._filtered(self._member_of(member), path, fname)
            self._extract_one(tarinfo, mode, path, set_attrs)

        def _extract_one(self, tarinfo: TarInfo, mode: int, path: str, set_attrs: bool) -> None:
            self._check("r")
            targetpath = os.path.join(path, tarinfo.name).rstrip("/")
            upperdirs = os.path.dirname(targetpath)
            if upperdirs and not os.path.exists(upperdirs):
                os.makedirs(upperdirs, exist_ok=True)
            if tarinfo.isreg():
                self._makefile(tarinfo, targetpath)
            elif tarinfo.isdir():
                try:
                    os.mkdir(targetpath, 0o700)
                except FileExistsError:
                    if not os.path.isdir(targetpath):
                        raise
            elif tarinfo.islnk() or tarinfo.issym():
                self._makelink(tarinfo, targetpath, path)
            elif tarinfo.isfifo() or tarinfo.ischr() or tarinfo.isblk():
                raise ExtractError("special devices not supported by system")
            else:
                self._makefile(tarinfo, targetpath)
            if set_attrs and not tarinfo.issym():
                self._chmod(mode, targetpath)
                self._utime(tarinfo, targetpath)

        def _makefile(self, tarinfo: TarInfo, targetpath: str) -> None:
            data = self._data[tarinfo.offset_data:tarinfo.offset_data + tarinfo.size]
            if len(data) != tarinfo.size:
                raise ReadError("unexpected end of data")
            with io.open(targetpath, "wb") as f:
                f.write(data)

        def _makelink(self, tarinfo: TarInfo, targetpath: str, root: str) -> None:
            try:
                if tarinfo.issym():
                    if os.path.lexists(targetpath):
                        os.unlink(targetpath)
                    os.symlink(tarinfo.linkname, targetpath)
                    return
                link_target = os.path.join(root, tarinfo.linkname)
                if os.path.exists(link_target):
                    if os.path.lexists(targetpath):
                        os.unlink(targetpath)
                    os.link(link_target, targetpath)
                    return
            except OSError:
                pass
            target = self._find_link_target(tarinfo)
            if target.isreg():
                self._makefile(target, targetpath)

        def _chmod(self, mode: int, targetpath: str) -> None:
            if mode < 0:
                return
            try:
                os.chmod(targetpath, mode)
            except OSError:
                raise ExtractError("could not change mode")

        def _utime(self, tarinfo: TarInfo, targetpath: str) -> None:
            try:
                os.utime(targetpath, (tarinfo.mtime, tarinfo.mtime))
            except OSError:
                raise ExtractError("could not change modification time")

    def _gunzip(data: bytes) -> bytes:
        try:
            return gzip.decompress(data)
        except OSError:
            raise ReadError("not a gzip file")
        except EOFError:
            raise ReadError("not a gzip file")

    def open(name: str | None = None, mode: str = "r", fileobj: _F = None, bufsize: int = RECORDSIZE, *,
             format: int | None = None, encoding: str | None = None, errors: str = "surrogateescape",
             pax_headers: dict[str, str] | None = None, ignore_zeros: bool = False, dereference: bool = False,
             errorlevel: int = 1, compresslevel: int = 9) -> TarFile:
        """Open a tar archive: 'r' / 'r:*' (reading, gzip found by itself), 'r:', 'r:gz', 'w' / 'w:',
        'w:gz', 'x', 'x:gz', 'a' (the stream modes 'r|...' / 'w|...' are the same here)."""
        if not name and fileobj is None:
            raise ValueError("nothing to open")
        m = mode.replace("|", ":")
        filemode = m
        comptype = "*" if m == "r" else "tar"
        if ":" in m:
            filemode, comptype = m.split(":", 1)
            filemode = filemode or "r"
            comptype = comptype or ("*" if filemode == "r" else "tar")
        if filemode not in ("r", "a", "w", "x"):
            raise ValueError("undiscernible mode")
        if comptype == "bz2":
            raise CompressionError("bz2 module is not available")
        if comptype == "xz":
            raise CompressionError("lzma module is not available")
        if comptype == "zst":
            raise CompressionError("compression.zstd module is not available")
        if comptype not in ("*", "tar", "gz"):
            raise CompressionError("unknown compression type %r" % comptype)
        if filemode == "r" and comptype != "tar":
            if fileobj is not None:
                raw = fileobj.read()
            else:
                with io.open(str(name), "rb") as f:
                    raw = f.read()
            if comptype == "gz":
                raw = _gunzip(raw)
            else:                                          # (r, r:*: each method in turn, as CPython)
                msgs: list[str] = []
                if raw[:2] == b"\x1f\x8b":
                    try:
                        raw2 = _gunzip(raw)
                        return TarFile(name, "r", io.BytesIO(raw2), format=format, encoding=encoding, errors=errors,
                                       ignore_zeros=ignore_zeros, dereference=dereference, errorlevel=errorlevel)
                    except ReadError as e:
                        msgs.append("- method gz: ReadError(%r)" % str(e))
                else:
                    msgs.append("- method gz: ReadError(%r)" % ("empty file" if not raw else "not a gzip file"))
                msgs.append("- method bz2: CompressionError('bz2 module is not available')")
                msgs.append("- method xz: CompressionError('lzma module is not available')")
                msgs.append("- method zst: CompressionError('compression.zstd module is not available')")
                try:
                    return TarFile(name, "r", io.BytesIO(raw), format=format, encoding=encoding, errors=errors,
                                   ignore_zeros=ignore_zeros, dereference=dereference, errorlevel=errorlevel)
                except ReadError as e:
                    msgs.append("- method tar: ReadError(%r)" % str(e))
                raise ReadError("file could not be opened successfully:\n" + "\n".join(msgs))
            t = TarFile(name, "r", io.BytesIO(raw), format=format, encoding=encoding, errors=errors,
                        ignore_zeros=ignore_zeros, dereference=dereference, errorlevel=errorlevel)
            return t
        if filemode == "a" and comptype == "gz":
            raise ValueError("mode must be 'r', 'w' or 'x'")
        if fileobj is None:
            t = TarFile(name, filemode, format=format, encoding=encoding, errors=errors, pax_headers=pax_headers,
                        ignore_zeros=ignore_zeros, dereference=dereference, errorlevel=errorlevel)
        else:
            t = TarFile(name, filemode, fileobj, format=format, encoding=encoding, errors=errors,
                        pax_headers=pax_headers, ignore_zeros=ignore_zeros, dereference=dereference,
                        errorlevel=errorlevel)
        if comptype == "gz":
            t._comptype = "gz"
            t._compresslevel = compresslevel
        return t

    def is_tarfile(name: _F) -> bool:
        """Whether name (a path or a file object) is a tar archive this module can read."""
        try:
            if isinstance(name, str):
                t = open(name)
            else:
                t = open(fileobj=name)
            t.close()
            return True
        except TarError:
            return False
