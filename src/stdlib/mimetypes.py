"""Guess the MIME type of a file (CPython's mimetypes): guess_type(url), guess_file_type(path),
guess_extension(type), guess_all_extensions(type), add_type(type, ext), init(files), MimeTypes;
the tables suffix_map, encodings_map, types_map, common_types. init() reads the system's
mime.types files (knownfiles) as CPython does."""
import os
import posixpath
from typing import TypeVar

_F = TypeVar("_F")

__all__ = [
    "knownfiles", "inited", "MimeTypes",
    "guess_type", "guess_file_type", "guess_all_extensions", "guess_extension",
    "add_type", "init", "read_mime_types",
    "suffix_map", "encodings_map", "types_map", "common_types"
]

knownfiles = [
    "/etc/mime.types",
    "/etc/httpd/mime.types",                    # Mac OS X
    "/etc/httpd/conf/mime.types",               # Apache
    "/etc/apache/mime.types",                   # Apache 1
    "/etc/apache2/mime.types",                  # Apache 2
    "/usr/local/etc/httpd/conf/mime.types",
    "/usr/local/lib/netscape/mime.types",
    "/usr/local/etc/httpd/conf/mime.types",     # Apache 1.2
    "/usr/local/etc/mime.types",                # Apache 1.3
    ]

inited = False


class MimeTypes:
    """MIME-types datastore: from mime.types-style files; the MIME type of a filename or URL,
    the extension of a MIME type."""

    def __init__(self, filenames: _F = (), strict: bool = True) -> None:
        if not inited:
            init()
        self.encodings_map: dict[str, str] = dict(_encodings_map_default)
        self.suffix_map: dict[str, str] = dict(_suffix_map_default)
        empty1: dict[str, str] = {}
        empty2: dict[str, str] = {}
        self.types_map = (empty1, empty2)                   # (non-strict, strict)
        inv1: dict[str, list[str]] = {}
        inv2: dict[str, list[str]] = {}
        self.types_map_inv = (inv1, inv2)
        for ext, type in _types_map_default.items():
            self.add_type(type, ext, True)
        for ext, type in _common_types_default.items():
            self.add_type(type, ext, False)
        for name in filenames:
            self.read(name, strict)

    def _map(self, strict: bool) -> dict[str, str]:
        return self.types_map[1] if strict else self.types_map[0]

    def _inv(self, strict: bool) -> dict[str, list[str]]:
        return self.types_map_inv[1] if strict else self.types_map_inv[0]

    def add_type(self, type: str, ext: str, strict: bool = True) -> None:
        """A mapping between a type and an extension (a new type replaces the extension's)."""
        if not type:
            return
        self._map(strict)[ext] = type
        inv = self._inv(strict)
        if type not in inv:
            inv[type] = []
        exts = inv[type]
        if ext not in exts:
            exts.append(ext)

    def guess_type(self, url: str, strict: bool = True) -> tuple[str | None, str | None]:
        """(type, encoding) of a URL or path: ('text/html', None), ('application/x-tar', 'gzip')."""
        import urllib.parse
        p = urllib.parse.urlparse(url)
        if p.scheme and len(p.scheme) > 1:
            scheme = p.scheme
            url = p.path
        else:
            return self.guess_file_type(url, strict=strict)
        if scheme == 'data':
            comma = url.find(',')
            if comma < 0:
                return None, None
            semi = url.find(';', 0, comma)
            if semi >= 0:
                type = url[:semi]
            else:
                type = url[:comma]
            if '=' in type or '/' not in type:
                type = 'text/plain'
            return type, None
        return self._guess_file_type(url, strict)

    def guess_file_type(self, path: str, *, strict: bool = True) -> tuple[str | None, str | None]:
        """Like guess_type(), of a file path."""
        return self._guess_file_type(path, strict)

    def _guess_file_type(self, path: str, strict: bool) -> tuple[str | None, str | None]:
        base, ext = posixpath.splitext(path)
        while ext.lower() in self.suffix_map:
            base, ext = posixpath.splitext(base + self.suffix_map[ext.lower()])
        encoding: str | None = None
        if ext in self.encodings_map:
            encoding = self.encodings_map[ext]
            base, ext = posixpath.splitext(base)
        ext = ext.lower()
        types_map = self._map(True)
        if ext in types_map:
            return types_map[ext], encoding
        elif strict:
            return None, encoding
        types_map = self._map(False)
        if ext in types_map:
            return types_map[ext], encoding
        return None, encoding

    def guess_all_extensions(self, type: str, strict: bool = True) -> list[str]:
        """The extensions (with their dot) of a MIME type."""
        type = type.lower()
        extensions = list(self._inv(True).get(type, []))
        if not strict:
            for ext in self._inv(False).get(type, []):
                if ext not in extensions:
                    extensions.append(ext)
        return extensions

    def guess_extension(self, type: str, strict: bool = True) -> str | None:
        """An extension of a MIME type (None: none known)."""
        extensions = self.guess_all_extensions(type, strict)
        if not extensions:
            return None
        return extensions[0]

    def read(self, filename: str, strict: bool = True) -> None:
        """Reads a mime.types-format file."""
        with open(filename, encoding='utf-8') as fp:
            self._read_lines(fp.read().splitlines(), strict)

    def _read_lines(self, lines: list[str], strict: bool) -> None:
        for line in lines:
            words = line.split()
            for i in range(len(words)):
                if words[i][0] == '#':
                    del words[i:]
                    break
            if not words:
                continue
            for suff in words[1:]:
                self.add_type(words[0], '.' + suff, strict)

    def read_windows_registry(self, strict: bool = True) -> None:
        pass


_db: list[MimeTypes] = []


def _the_db() -> MimeTypes:
    if not _db:
        init()
    return _db[0]


def guess_type(url: str, strict: bool = True) -> tuple[str | None, str | None]:
    """(type, encoding) of a URL or path."""
    return _the_db().guess_type(url, strict)


def guess_file_type(path: str, *, strict: bool = True) -> tuple[str | None, str | None]:
    """(type, encoding) of a file path."""
    return _the_db().guess_file_type(path, strict=strict)


def guess_all_extensions(type: str, strict: bool = True) -> list[str]:
    return _the_db().guess_all_extensions(type, strict)


def guess_extension(type: str, strict: bool = True) -> str | None:
    return _the_db().guess_extension(type, strict)


def add_type(type: str, ext: str, strict: bool = True) -> None:
    _the_db().add_type(type, ext, strict)


def init(files: list[str] | None = None) -> None:
    """Reads the mime.types files (knownfiles, and files)."""
    global suffix_map, types_map, encodings_map, common_types, inited
    inited = True
    db = MimeTypes()
    names = list(knownfiles)
    if files is not None:
        names.extend(files)
    for file in names:
        if os.path.isfile(file):
            try:
                db.read(file)
            except (OSError, UnicodeDecodeError):
                pass
    encodings_map = db.encodings_map
    suffix_map = db.suffix_map
    types_map = db.types_map[1]
    common_types = db.types_map[0]
    del _db[:]
    _db.append(db)


def read_mime_types(file: str) -> dict[str, str] | None:
    try:
        f = open(file, encoding='utf-8')
    except OSError:
        return None
    with f:
        db = MimeTypes()
        db._read_lines(f.read().splitlines(), True)
        return db.types_map[1]


def _default_mime_types():
    global suffix_map, _suffix_map_default
    global encodings_map, _encodings_map_default
    global types_map, _types_map_default
    global common_types, _common_types_default

    suffix_map = _suffix_map_default = {
        '.svgz': '.svg.gz',
        '.tgz': '.tar.gz',
        '.taz': '.tar.gz',
        '.tz': '.tar.gz',
        '.tbz2': '.tar.bz2',
        '.txz': '.tar.xz',
        }

    encodings_map = _encodings_map_default = {
        '.gz': 'gzip',
        '.Z': 'compress',
        '.bz2': 'bzip2',
        '.xz': 'xz',
        '.br': 'br',
        }

    # Before adding new types, make sure they are either registered with IANA,
    # at https://www.iana.org/assignments/media-types/media-types.xhtml
    # or extensions, i.e. using the x- prefix

    # If you add to these, please keep them sorted by mime type.
    # Make sure the entry with the preferred file extension for a particular mime type
    # appears before any others of the same mimetype.
    types_map = _types_map_default = {
        '.js'     : 'text/javascript',
        '.mjs'    : 'text/javascript',
        '.epub'   : 'application/epub+zip',
        '.gz'     : 'application/gzip',
        '.json'   : 'application/json',
        '.webmanifest': 'application/manifest+json',
        '.doc'    : 'application/msword',
        '.dot'    : 'application/msword',
        '.wiz'    : 'application/msword',
        '.nq'     : 'application/n-quads',
        '.nt'     : 'application/n-triples',
        '.bin'    : 'application/octet-stream',
        '.a'      : 'application/octet-stream',
        '.dll'    : 'application/octet-stream',
        '.exe'    : 'application/octet-stream',
        '.o'      : 'application/octet-stream',
        '.obj'    : 'application/octet-stream',
        '.so'     : 'application/octet-stream',
        '.oda'    : 'application/oda',
        '.ogx'    : 'application/ogg',
        '.pdf'    : 'application/pdf',
        '.p7c'    : 'application/pkcs7-mime',
        '.ps'     : 'application/postscript',
        '.ai'     : 'application/postscript',
        '.eps'    : 'application/postscript',
        '.trig'   : 'application/trig',
        '.m3u'    : 'application/vnd.apple.mpegurl',
        '.m3u8'   : 'application/vnd.apple.mpegurl',
        '.xls'    : 'application/vnd.ms-excel',
        '.xlb'    : 'application/vnd.ms-excel',
        '.eot'    : 'application/vnd.ms-fontobject',
        '.ppt'    : 'application/vnd.ms-powerpoint',
        '.pot'    : 'application/vnd.ms-powerpoint',
        '.ppa'    : 'application/vnd.ms-powerpoint',
        '.pps'    : 'application/vnd.ms-powerpoint',
        '.pwz'    : 'application/vnd.ms-powerpoint',
        '.odg'    : 'application/vnd.oasis.opendocument.graphics',
        '.odp'    : 'application/vnd.oasis.opendocument.presentation',
        '.ods'    : 'application/vnd.oasis.opendocument.spreadsheet',
        '.odt'    : 'application/vnd.oasis.opendocument.text',
        '.pptx'   : 'application/vnd.openxmlformats-officedocument.presentationml.presentation',
        '.xlsx'   : 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
        '.docx'   : 'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
        '.rar'    : 'application/vnd.rar',
        '.wasm'   : 'application/wasm',
        '.7z'     : 'application/x-7z-compressed',
        '.bcpio'  : 'application/x-bcpio',
        '.cpio'   : 'application/x-cpio',
        '.csh'    : 'application/x-csh',
        '.deb'    : 'application/x-debian-package',
        '.dvi'    : 'application/x-dvi',
        '.gtar'   : 'application/x-gtar',
        '.hdf'    : 'application/x-hdf',
        '.h5'     : 'application/x-hdf5',
        '.latex'  : 'application/x-latex',
        '.mif'    : 'application/x-mif',
        '.cdf'    : 'application/x-netcdf',
        '.nc'     : 'application/x-netcdf',
        '.p12'    : 'application/x-pkcs12',
        '.php'    : 'application/x-httpd-php',
        '.pfx'    : 'application/x-pkcs12',
        '.ram'    : 'application/x-pn-realaudio',
        '.pyc'    : 'application/x-python-code',
        '.pyo'    : 'application/x-python-code',
        '.rpm'    : 'application/x-rpm',
        '.sh'     : 'application/x-sh',
        '.shar'   : 'application/x-shar',
        '.swf'    : 'application/x-shockwave-flash',
        '.sv4cpio': 'application/x-sv4cpio',
        '.sv4crc' : 'application/x-sv4crc',
        '.tar'    : 'application/x-tar',
        '.tcl'    : 'application/x-tcl',
        '.tex'    : 'application/x-tex',
        '.texi'   : 'application/x-texinfo',
        '.texinfo': 'application/x-texinfo',
        '.roff'   : 'application/x-troff',
        '.t'      : 'application/x-troff',
        '.tr'     : 'application/x-troff',
        '.man'    : 'application/x-troff-man',
        '.me'     : 'application/x-troff-me',
        '.ms'     : 'application/x-troff-ms',
        '.ustar'  : 'application/x-ustar',
        '.src'    : 'application/x-wais-source',
        '.xsl'    : 'application/xml',
        '.rdf'    : 'application/xml',
        '.wsdl'   : 'application/xml',
        '.xpdl'   : 'application/xml',
        '.yaml'   : 'application/yaml',
        '.yml'    : 'application/yaml',
        '.zip'    : 'application/zip',
        '.3gp'    : 'audio/3gpp',
        '.3gpp'   : 'audio/3gpp',
        '.3g2'    : 'audio/3gpp2',
        '.3gpp2'  : 'audio/3gpp2',
        '.aac'    : 'audio/aac',
        '.adts'   : 'audio/aac',
        '.loas'   : 'audio/aac',
        '.ass'    : 'audio/aac',
        '.au'     : 'audio/basic',
        '.snd'    : 'audio/basic',
        '.flac'   : 'audio/flac',
        '.mka'    : 'audio/matroska',
        '.m4a'    : 'audio/mp4',
        '.mp3'    : 'audio/mpeg',
        '.mp2'    : 'audio/mpeg',
        '.ogg'    : 'audio/ogg',
        '.opus'   : 'audio/opus',
        '.aif'    : 'audio/x-aiff',
        '.aifc'   : 'audio/x-aiff',
        '.aiff'   : 'audio/x-aiff',
        '.ra'     : 'audio/x-pn-realaudio',
        '.wav'    : 'audio/vnd.wave',
        '.otf'    : 'font/otf',
        '.ttf'    : 'font/ttf',
        '.weba'   : 'audio/webm',
        '.woff'   : 'font/woff',
        '.woff2'  : 'font/woff2',
        '.avif'   : 'image/avif',
        '.bmp'    : 'image/bmp',
        '.emf'    : 'image/emf',
        '.fits'   : 'image/fits',
        '.g3'     : 'image/g3fax',
        '.gif'    : 'image/gif',
        '.ief'    : 'image/ief',
        '.jp2'    : 'image/jp2',
        '.jpg'    : 'image/jpeg',
        '.jpe'    : 'image/jpeg',
        '.jpeg'   : 'image/jpeg',
        '.jpm'    : 'image/jpm',
        '.jpx'    : 'image/jpx',
        '.heic'   : 'image/heic',
        '.heif'   : 'image/heif',
        '.png'    : 'image/png',
        '.svg'    : 'image/svg+xml',
        '.t38'    : 'image/t38',
        '.tiff'   : 'image/tiff',
        '.tif'    : 'image/tiff',
        '.tfx'    : 'image/tiff-fx',
        '.ico'    : 'image/vnd.microsoft.icon',
        '.webp'   : 'image/webp',
        '.wmf'    : 'image/wmf',
        '.ras'    : 'image/x-cmu-raster',
        '.pnm'    : 'image/x-portable-anymap',
        '.pbm'    : 'image/x-portable-bitmap',
        '.pgm'    : 'image/x-portable-graymap',
        '.ppm'    : 'image/x-portable-pixmap',
        '.rgb'    : 'image/x-rgb',
        '.xbm'    : 'image/x-xbitmap',
        '.xpm'    : 'image/x-xpixmap',
        '.xwd'    : 'image/x-xwindowdump',
        '.eml'    : 'message/rfc822',
        '.mht'    : 'message/rfc822',
        '.mhtml'  : 'message/rfc822',
        '.nws'    : 'message/rfc822',
        '.gltf'   : 'model/gltf+json',
        '.glb'    : 'model/gltf-binary',
        '.stl'    : 'model/stl',
        '.css'    : 'text/css',
        '.csv'    : 'text/csv',
        '.html'   : 'text/html',
        '.htm'    : 'text/html',
        '.md'     : 'text/markdown',
        '.markdown': 'text/markdown',
        '.n3'     : 'text/n3',
        '.txt'    : 'text/plain',
        '.bat'    : 'text/plain',
        '.c'      : 'text/plain',
        '.h'      : 'text/plain',
        '.ksh'    : 'text/plain',
        '.pl'     : 'text/plain',
        '.srt'    : 'text/plain',
        '.rtx'    : 'text/richtext',
        '.rtf'    : 'text/rtf',
        '.tsv'    : 'text/tab-separated-values',
        '.vtt'    : 'text/vtt',
        '.py'     : 'text/x-python',
        '.rst'    : 'text/x-rst',
        '.etx'    : 'text/x-setext',
        '.sgm'    : 'text/x-sgml',
        '.sgml'   : 'text/x-sgml',
        '.vcf'    : 'text/x-vcard',
        '.xml'    : 'text/xml',
        '.mkv'    : 'video/matroska',
        '.mk3d'   : 'video/matroska-3d',
        '.mp4'    : 'video/mp4',
        '.mpeg'   : 'video/mpeg',
        '.m1v'    : 'video/mpeg',
        '.mpa'    : 'video/mpeg',
        '.mpe'    : 'video/mpeg',
        '.mpg'    : 'video/mpeg',
        '.ogv'    : 'video/ogg',
        '.mov'    : 'video/quicktime',
        '.qt'     : 'video/quicktime',
        '.webm'   : 'video/webm',
        '.avi'    : 'video/vnd.avi',
        '.m4v'    : 'video/x-m4v',
        '.wmv'    : 'video/x-ms-wmv',
        '.movie'  : 'video/x-sgi-movie',
        }

    # These are non-standard types, commonly found in the wild.  They will
    # only match if strict=0 flag is given to the API methods.

    # Please sort these too
    common_types = _common_types_default = {
        '.rtf' : 'application/rtf',
        '.apk' : 'application/vnd.android.package-archive',
        '.midi': 'audio/midi',
        '.mid' : 'audio/midi',
        '.jpg' : 'image/jpg',
        '.pict': 'image/pict',
        '.pct' : 'image/pict',
        '.pic' : 'image/pict',
        '.xul' : 'text/xul',
        }


_default_mime_types()
