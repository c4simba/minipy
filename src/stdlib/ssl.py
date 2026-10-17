"""TLS for sockets: the system's OpenSSL 3 (libssl, libcrypto) called through ctypes - the same
module in the interpreter and in compiled programs. Linux: libssl.so.3 (compiled programs are
i386: the i386 library, loaded when TLS is first used); macOS: Homebrew's openssl@3; other
folders: PLAINPY_LIBRARY_PATH. What CPython's ssl has for TLS clients and servers -
SSLContext, create_default_context, wrap_socket, the peer's certificate checked against the
system's CAs or given ones and its host name (SNI sent) - but no callbacks (SNI, PSK, a key's
password asked for), sessions or MemoryBIO."""

import sys

if sys.platform not in ("linux", "darwin"):
    raise ImportError("no ssl on " + sys.platform + ": plainpy's TLS is the system's OpenSSL 3 (libssl), which programs here do not have")

import socket
import ctypes

# ---------------------------------------------------------------- the libraries

if sys._compiled:
    if sys.platform == "darwin":
        _ssl = ctypes.CDLL("libssl.3.dylib")
        _crypto = ctypes.CDLL("libcrypto.3.dylib")
    else:
        _ssl = ctypes.CDLL("libssl.so.3")
        _crypto = ctypes.CDLL("libcrypto.so.3")
else:
    def _load(name: str):
        try:
            return ctypes.CDLL(name)
        except OSError as e:
            raise ImportError("ssl needs OpenSSL 3 (" + str(e) + "; macOS: brew install openssl@3, Linux: its libssl3 package; PLAINPY_LIBRARY_PATH names other folders)")
    if sys.platform == "darwin":
        _ssl = _load("libssl.3.dylib")
        _crypto = _load("libcrypto.3.dylib")
    else:
        _ssl = _load("libssl.so.3")
        _crypto = _load("libcrypto.so.3")

_ssl.TLS_client_method.restype = ctypes.c_void_p
_ssl.TLS_server_method.restype = ctypes.c_void_p
_ssl.SSL_CTX_new.restype = ctypes.c_void_p
_ssl.SSL_CTX_new.argtypes = [ctypes.c_void_p]
_ssl.SSL_CTX_free.restype = None
_ssl.SSL_CTX_free.argtypes = [ctypes.c_void_p]
_ssl.SSL_CTX_set_verify.restype = None
_ssl.SSL_CTX_set_verify.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
_ssl.SSL_CTX_set_default_verify_paths.argtypes = [ctypes.c_void_p]
_ssl.SSL_CTX_load_verify_locations.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
_ssl.SSL_CTX_use_certificate_chain_file.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_ssl.SSL_CTX_use_PrivateKey_file.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
_ssl.SSL_CTX_check_private_key.argtypes = [ctypes.c_void_p]
_ssl.SSL_CTX_set_cipher_list.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_ssl.SSL_CTX_set_alpn_protos.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
_ssl.SSL_CTX_ctrl.restype = ctypes.c_long
_ssl.SSL_CTX_ctrl.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
_ssl.SSL_CTX_get_cert_store.restype = ctypes.c_void_p
_ssl.SSL_CTX_get_cert_store.argtypes = [ctypes.c_void_p]
_ssl.SSL_new.restype = ctypes.c_void_p
_ssl.SSL_new.argtypes = [ctypes.c_void_p]
_ssl.SSL_free.restype = None
_ssl.SSL_free.argtypes = [ctypes.c_void_p]
_ssl.SSL_set_fd.argtypes = [ctypes.c_void_p, ctypes.c_int]
_ssl.SSL_set_connect_state.restype = None
_ssl.SSL_set_connect_state.argtypes = [ctypes.c_void_p]
_ssl.SSL_set_accept_state.restype = None
_ssl.SSL_set_accept_state.argtypes = [ctypes.c_void_p]
_ssl.SSL_ctrl.restype = ctypes.c_long
_ssl.SSL_ctrl.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
_ssl.SSL_set1_host.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_ssl.SSL_do_handshake.argtypes = [ctypes.c_void_p]
_ssl.SSL_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
_ssl.SSL_write.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
_ssl.SSL_pending.argtypes = [ctypes.c_void_p]
_ssl.SSL_get_error.argtypes = [ctypes.c_void_p, ctypes.c_int]
_ssl.SSL_shutdown.argtypes = [ctypes.c_void_p]
_ssl.SSL_get_verify_result.restype = ctypes.c_long
_ssl.SSL_get_verify_result.argtypes = [ctypes.c_void_p]
_ssl.SSL_get_version.restype = ctypes.c_char_p
_ssl.SSL_get_version.argtypes = [ctypes.c_void_p]
_ssl.SSL_get_current_cipher.restype = ctypes.c_void_p
_ssl.SSL_get_current_cipher.argtypes = [ctypes.c_void_p]
_ssl.SSL_CIPHER_get_name.restype = ctypes.c_char_p
_ssl.SSL_CIPHER_get_name.argtypes = [ctypes.c_void_p]
_ssl.SSL_CIPHER_get_version.restype = ctypes.c_char_p
_ssl.SSL_CIPHER_get_version.argtypes = [ctypes.c_void_p]
_ssl.SSL_CIPHER_get_bits.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_ssl.SSL_get1_peer_certificate.restype = ctypes.c_void_p
_ssl.SSL_get1_peer_certificate.argtypes = [ctypes.c_void_p]

_crypto.OpenSSL_version.restype = ctypes.c_char_p
_crypto.OpenSSL_version.argtypes = [ctypes.c_int]
_crypto.OpenSSL_version_num.restype = ctypes.c_ulong
_crypto.ERR_get_error.restype = ctypes.c_ulong
_crypto.ERR_peek_error.restype = ctypes.c_ulong
_crypto.ERR_clear_error.restype = None
_crypto.ERR_reason_error_string.restype = ctypes.c_char_p
_crypto.ERR_reason_error_string.argtypes = [ctypes.c_ulong]
_crypto.X509_verify_cert_error_string.restype = ctypes.c_char_p
_crypto.X509_verify_cert_error_string.argtypes = [ctypes.c_long]
_crypto.X509_free.restype = None
_crypto.X509_free.argtypes = [ctypes.c_void_p]
_crypto.X509_get_subject_name.restype = ctypes.c_void_p
_crypto.X509_get_subject_name.argtypes = [ctypes.c_void_p]
_crypto.X509_get_issuer_name.restype = ctypes.c_void_p
_crypto.X509_get_issuer_name.argtypes = [ctypes.c_void_p]
_crypto.X509_get_version.restype = ctypes.c_long
_crypto.X509_get_version.argtypes = [ctypes.c_void_p]
_crypto.X509_get0_notBefore.restype = ctypes.c_void_p
_crypto.X509_get0_notBefore.argtypes = [ctypes.c_void_p]
_crypto.X509_get0_notAfter.restype = ctypes.c_void_p
_crypto.X509_get0_notAfter.argtypes = [ctypes.c_void_p]
_crypto.X509_get_serialNumber.restype = ctypes.c_void_p
_crypto.X509_get_serialNumber.argtypes = [ctypes.c_void_p]
_crypto.X509_get_ext_by_NID.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
_crypto.X509_get_ext.restype = ctypes.c_void_p
_crypto.X509_get_ext.argtypes = [ctypes.c_void_p, ctypes.c_int]
_crypto.X509V3_EXT_print.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int]
_crypto.X509_NAME_print_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_ulong]
_crypto.ASN1_TIME_print.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_crypto.i2a_ASN1_INTEGER.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_crypto.i2d_X509_bio.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_crypto.PEM_read_bio_X509.restype = ctypes.c_void_p
_crypto.PEM_read_bio_X509.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
_crypto.d2i_X509_bio.restype = ctypes.c_void_p
_crypto.d2i_X509_bio.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_crypto.X509_STORE_add_cert.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_crypto.BIO_s_mem.restype = ctypes.c_void_p
_crypto.BIO_new.restype = ctypes.c_void_p
_crypto.BIO_new.argtypes = [ctypes.c_void_p]
_crypto.BIO_new_mem_buf.restype = ctypes.c_void_p
_crypto.BIO_new_mem_buf.argtypes = [ctypes.c_void_p, ctypes.c_int]
_crypto.BIO_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
_crypto.BIO_free.argtypes = [ctypes.c_void_p]

# ---------------------------------------------------------------- names CPython's ssl has

PROTOCOL_TLS = 2
PROTOCOL_TLS_CLIENT = 16
PROTOCOL_TLS_SERVER = 17
PROTOCOL_SSLv23 = PROTOCOL_TLS
CERT_NONE = 0
CERT_OPTIONAL = 1
CERT_REQUIRED = 2
OP_ALL = 0x80000850
OP_NO_SSLv2 = 0
OP_NO_SSLv3 = 0x02000000
OP_NO_TLSv1 = 0x04000000
OP_NO_TLSv1_1 = 0x10000000
OP_NO_COMPRESSION = 0x00020000
HAS_SNI = True
HAS_ALPN = True
HAS_TLSv1_3 = True
SSL_ERROR_SSL = 1
SSL_ERROR_WANT_READ = 2
SSL_ERROR_WANT_WRITE = 3
SSL_ERROR_SYSCALL = 5
SSL_ERROR_ZERO_RETURN = 6
SSL_ERROR_EOF = 8
if sys._compiled:                     # (no C call when the module is imported: http.client imports it)
    OPENSSL_VERSION = "OpenSSL 3"
    OPENSSL_VERSION_NUMBER = 0x30000000
else:
    OPENSSL_VERSION = _crypto.OpenSSL_version(0) or "OpenSSL"
    OPENSSL_VERSION_NUMBER = _crypto.OpenSSL_version_num()


class TLSVersion:
    MINIMUM_SUPPORTED = -2
    SSLv3 = 0x300
    TLSv1 = 0x301
    TLSv1_1 = 0x302
    TLSv1_2 = 0x303
    TLSv1_3 = 0x304
    MAXIMUM_SUPPORTED = -1


class Purpose:
    SERVER_AUTH = "SERVER_AUTH"         # (a client's context: it checks servers)
    CLIENT_AUTH = "CLIENT_AUTH"


class SSLError(OSError):
    """An error of the TLS library: reason ("CERTIFICATE_VERIFY_FAILED"), library ("SSL")."""

    def __init__(self, code: int = 1, message: str = "", reason: str | None = None, library: str | None = None):
        super().__init__(message)
        self.errno = code
        self.strerror = message
        self.reason = reason
        self.library = library

    def __str__(self) -> str:
        return self.strerror if self.strerror is not None else ""


class SSLZeroReturnError(SSLError):
    pass


class SSLWantReadError(SSLError):
    pass


class SSLWantWriteError(SSLError):
    pass


class SSLSyscallError(SSLError):
    pass


class SSLEOFError(SSLError):
    pass


class SSLCertVerificationError(SSLError):
    """The peer's certificate did not pass: verify_code, verify_message (OpenSSL's)."""

    def __init__(self, code: int = 1, message: str = "", reason: str | None = None, library: str | None = None):
        super().__init__(code, message, reason, library)
        self.verify_code = 0
        self.verify_message = ""


CertificateError = SSLCertVerificationError

_LIBS = {2: "SYS", 6: "EVP", 9: "PEM", 11: "X509", 13: "ASN1", 20: "SSL", 32: "BIO", 34: "X509V3"}


def _error(what: str, ssl_handle: int = 0, host: str | None = None) -> SSLError:
    """The error OpenSSL queued (the first one; the queue emptied), as CPython words it."""
    code = _crypto.ERR_get_error()
    e = code
    while e != 0:                                   # (the rest: not needed)
        e = _crypto.ERR_get_error()
    if code == 0:
        return SSLError(1, what)
    text = _crypto.ERR_reason_error_string(code)
    if text is None:
        text = "error " + str(code & 0x7FFFFF)
    lib = _LIBS.get((code >> 23) & 0xFF, "SSL")
    reason = text.upper().replace(" ", "_").replace("-", "_")
    msg = "[" + lib + ": " + reason + "] " + text
    if reason == "CERTIFICATE_VERIFY_FAILED" and ssl_handle != 0:
        vc = _ssl.SSL_get_verify_result(ssl_handle)
        vm = _crypto.X509_verify_cert_error_string(vc) or ""
        if vc == 62 and host is not None:             # X509_V_ERR_HOSTNAME_MISMATCH, IP_ADDRESS_MISMATCH: as CPython words them
            vm = "Hostname mismatch, certificate is not valid for '" + host + "'."
        elif vc == 64 and host is not None:
            vm = "IP address mismatch, certificate is not valid for '" + host + "'."
        msg += ": " + vm
        ce = SSLCertVerificationError(1, msg, reason, lib)
        ce.verify_code = vc
        ce.verify_message = vm
        return ce
    return SSLError(1, msg, reason, lib)


def _readable(path: str) -> None:
    """FileNotFoundError and the like, as CPython raises them, before OpenSSL is given the file."""
    f = open(path, "rb")
    f.close()


def _bio_text(bio: int) -> str:
    """What a memory BIO holds (then freed)."""
    out: list[bytes] = []
    buf = sys.buffer(1024)
    while True:
        n = _crypto.BIO_read(bio, buf, 1024)
        if n <= 0:
            break
        out.append(sys.peek_bytes(buf, 0, n))
    _crypto.BIO_free(bio)
    return b"".join(out).decode("utf-8", "replace")


# ---------------------------------------------------------------- certificates as CPython shows them

def _name_text(name: int) -> str:
    """A name in RFC 2253's text, most significant part last (CN=a,O=b,C=c)."""
    b = _crypto.BIO_new(_crypto.BIO_s_mem())
    _crypto.X509_NAME_print_ex(b, name, 0, 0x1110313)   # XN_FLAG_RFC2253, but UTF-8 as it is (not ASN1_STRFLGS_ESC_MSB)
    return _bio_text(b)


def _cert_fields(x: int) -> dict[str, str]:
    """The certificate's fields as OpenSSL prints them: subject, issuer (RFC 2253), version,
    serialNumber, notBefore, notAfter, subjectAltName ("DNS:a, IP Address:1.2.3.4")."""
    d: dict[str, str] = {}
    d["subject"] = _name_text(_crypto.X509_get_subject_name(x))
    d["issuer"] = _name_text(_crypto.X509_get_issuer_name(x))
    d["version"] = str(_crypto.X509_get_version(x) + 1)
    b = _crypto.BIO_new(_crypto.BIO_s_mem())
    _crypto.i2a_ASN1_INTEGER(b, _crypto.X509_get_serialNumber(x))
    d["serialNumber"] = _bio_text(b)
    b = _crypto.BIO_new(_crypto.BIO_s_mem())
    _crypto.ASN1_TIME_print(b, _crypto.X509_get0_notBefore(x))
    d["notBefore"] = _bio_text(b)
    b = _crypto.BIO_new(_crypto.BIO_s_mem())
    _crypto.ASN1_TIME_print(b, _crypto.X509_get0_notAfter(x))
    d["notAfter"] = _bio_text(b)
    k = _crypto.X509_get_ext_by_NID(x, 85, -1)        # NID_subject_alt_name
    if k >= 0:
        b = _crypto.BIO_new(_crypto.BIO_s_mem())
        _crypto.X509V3_EXT_print(b, _crypto.X509_get_ext(x, k), 0, 0)
        d["subjectAltName"] = _bio_text(b)
    return d


if not sys._compiled:                 # (CPython's shapes: tuples of tuples - one type per value in compiled code)
    def _rdns(text: str):
        """RFC 2253's text (CN=a,O=b): CPython's ((('organizationName', 'b'),), (('commonName', 'a'),))."""
        names = {"CN": "commonName", "O": "organizationName", "OU": "organizationalUnitName", "C": "countryName",
                 "ST": "stateOrProvinceName", "L": "localityName", "emailAddress": "emailAddress", "DC": "domainComponent",
                 "serialNumber": "serialNumber", "street": "streetAddress", "UID": "userId"}
        parts = []
        cur = ""
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == "\\" and i + 1 < len(text):
                cur += text[i + 1]
                i += 2
                continue
            if ch == ",":
                parts.append(cur)
                cur = ""
            else:
                cur += ch
            i += 1
        if cur:
            parts.append(cur)
        rdns = []
        for p in reversed(parts):                     # (RFC 2253 lists them from the last)
            k, sep, v = p.partition("=")
            rdns.append(((names.get(k, k), v),))
        return tuple(rdns)

    def _cert_dict(x: int):
        """getpeercert() as CPython's: subject, issuer, version, serialNumber, notBefore, notAfter, subjectAltName."""
        f = _cert_fields(x)
        d = {"subject": _rdns(f["subject"]), "issuer": _rdns(f["issuer"]), "version": int(f["version"]),
             "serialNumber": f["serialNumber"], "notBefore": f["notBefore"], "notAfter": f["notAfter"]}
        if "subjectAltName" in f:
            alt = []
            for item in f["subjectAltName"].split(", "):
                kind, sep, value = item.partition(":")
                alt.append((kind, value.strip()))
            d["subjectAltName"] = tuple(alt)
        return d


def _der(x: int) -> bytes:
    b = _crypto.BIO_new(_crypto.BIO_s_mem())
    _crypto.i2d_X509_bio(b, x)
    out: list[bytes] = []
    buf = sys.buffer(4096)
    while True:
        n = _crypto.BIO_read(b, buf, 4096)
        if n <= 0:
            break
        out.append(sys.peek_bytes(buf, 0, n))
    _crypto.BIO_free(b)
    return b"".join(out)


# ---------------------------------------------------------------- contexts

class SSLContext:
    """What connections share: the side (client, server), the certificates trusted, ours."""

    def __init__(self, protocol: int = PROTOCOL_TLS_CLIENT):
        self.protocol = protocol
        server = protocol == PROTOCOL_TLS_SERVER
        self._ctx = _ssl.SSL_CTX_new(_ssl.TLS_server_method() if server else _ssl.TLS_client_method())
        if self._ctx == 0:
            raise _error("cannot make an SSL context")
        self._check_hostname = protocol == PROTOCOL_TLS_CLIENT
        self._verify_mode = CERT_REQUIRED if protocol == PROTOCOL_TLS_CLIENT else CERT_NONE
        self._keep: list[bytes] = []                   # (data OpenSSL refers to: kept as long as the context)
        self._apply_verify()
        _ssl.SSL_CTX_ctrl(self._ctx, 123, 0x303, None)  # SSL_CTRL_SET_MIN_PROTO_VERSION: TLS 1.2, as CPython
        self.options = OP_ALL | OP_NO_SSLv3 | OP_NO_COMPRESSION

    def _apply_verify(self) -> None:
        mode = 0
        if self._verify_mode != CERT_NONE:
            mode = 1                                   # SSL_VERIFY_PEER
            if self.protocol == PROTOCOL_TLS_SERVER and self._verify_mode == CERT_REQUIRED:
                mode |= 2                              # SSL_VERIFY_FAIL_IF_NO_PEER_CERT
        _ssl.SSL_CTX_set_verify(self._ctx, mode, None)

    @property
    def verify_mode(self) -> int:
        return self._verify_mode

    @verify_mode.setter
    def verify_mode(self, value: int) -> None:
        if value == CERT_NONE and self._check_hostname:
            raise ValueError("Cannot set verify_mode to CERT_NONE when check_hostname is enabled.")
        self._verify_mode = value
        self._apply_verify()

    @property
    def check_hostname(self) -> bool:
        return self._check_hostname

    @check_hostname.setter
    def check_hostname(self, value: bool) -> None:
        if value and self._verify_mode == CERT_NONE:
            self._verify_mode = CERT_REQUIRED
            self._apply_verify()
        self._check_hostname = value

    def set_default_verify_paths(self) -> None:
        """The system's certificate authorities (OpenSSL's folder; SSL_CERT_FILE, SSL_CERT_DIR)."""
        if _ssl.SSL_CTX_set_default_verify_paths(self._ctx) != 1:
            raise _error("cannot load the default certificates")

    def load_default_certs(self, purpose: str = Purpose.SERVER_AUTH) -> None:
        self.set_default_verify_paths()

    def load_verify_locations(self, cafile: str | None = None, capath: str | None = None, cadata=None) -> None:
        """Certificate authorities to trust: a PEM file, a folder of them (c_rehash), PEM text or DER bytes."""
        if cafile is None and capath is None and cadata is None:
            raise TypeError("cafile, capath and cadata cannot be all omitted")
        if cafile is not None:
            _readable(cafile)
        if cafile is not None or capath is not None:
            if _ssl.SSL_CTX_load_verify_locations(self._ctx, cafile, capath) != 1:
                e = _error("cannot load the certificates")
                if e.reason == "NO_SUCH_FILE" or e.reason == "SYSTEM_LIB":
                    raise FileNotFoundError(2, str(e))
                raise e
        if cadata is not None:
            if isinstance(cadata, str):
                self._add_cadata(cadata.encode("ascii"), True)
            else:
                self._add_cadata(cadata, False)

    def _add_cadata(self, data: bytes, pem: bool) -> None:
        """Certificates of cadata: PEM text, or DER (one after another)."""
        self._keep.append(data)
        store = _ssl.SSL_CTX_get_cert_store(self._ctx)
        bio = _crypto.BIO_new_mem_buf(data, len(data))
        n = 0
        while True:
            if pem:
                x = _crypto.PEM_read_bio_X509(bio, None, None, None)
            else:
                x = _crypto.d2i_X509_bio(bio, None)
            if x == 0:
                break
            _crypto.X509_STORE_add_cert(store, x)
            _crypto.X509_free(x)
            n += 1
        _crypto.BIO_free(bio)
        _crypto.ERR_clear_error()                      # (the end of the data: an error OpenSSL queues)
        if n == 0:
            raise SSLError(1, "no certificate in cadata")

    def load_cert_chain(self, certfile: str, keyfile: str | None = None, password: str | None = None) -> None:
        """This side's certificate (and the chain after it) and its private key (PEM files)."""
        if password is not None:
            raise NotImplementedError("load_cert_chain(): keys with a password are not supported")
        _readable(certfile)
        if keyfile is not None:
            _readable(keyfile)
        if _ssl.SSL_CTX_use_certificate_chain_file(self._ctx, certfile) != 1:
            raise _error("cannot load the certificate " + certfile)
        if _ssl.SSL_CTX_use_PrivateKey_file(self._ctx, keyfile if keyfile is not None else certfile, 1) != 1:   # SSL_FILETYPE_PEM
            raise _error("cannot load the private key")
        if _ssl.SSL_CTX_check_private_key(self._ctx) != 1:
            raise _error("the private key does not match the certificate")

    def set_ciphers(self, ciphers: str) -> None:
        if _ssl.SSL_CTX_set_cipher_list(self._ctx, ciphers) != 1:
            raise SSLError(1, "No cipher can be selected.")

    def set_alpn_protocols(self, protocols: list[str]) -> None:
        wire = b""
        for p in protocols:
            b = p.encode("ascii")
            if len(b) == 0 or len(b) > 255:
                raise SSLError(1, "ALPN protocols must be 1 to 255 in length")
            wire += bytes([len(b)]) + b
        self._keep.append(wire)
        if _ssl.SSL_CTX_set_alpn_protos(self._ctx, wire, len(wire)) != 0:
            raise _error("cannot set the ALPN protocols")

    def _version(self, cmd: int, value: int) -> None:
        v = value
        if value == TLSVersion.MINIMUM_SUPPORTED or value == TLSVersion.MAXIMUM_SUPPORTED:
            v = 0
        _ssl.SSL_CTX_ctrl(self._ctx, cmd, v, None)

    @property
    def minimum_version(self) -> int:
        v = _ssl.SSL_CTX_ctrl(self._ctx, 130, 0, None)   # SSL_CTRL_GET_MIN_PROTO_VERSION
        return v if v != 0 else TLSVersion.MINIMUM_SUPPORTED

    @minimum_version.setter
    def minimum_version(self, value: int) -> None:
        self._version(123, value)

    @property
    def maximum_version(self) -> int:
        v = _ssl.SSL_CTX_ctrl(self._ctx, 131, 0, None)
        return v if v != 0 else TLSVersion.MAXIMUM_SUPPORTED

    @maximum_version.setter
    def maximum_version(self, value: int) -> None:
        self._version(124, value)

    def wrap_socket(self, sock: socket.socket, server_side: bool = False, do_handshake_on_connect: bool = True,
                    suppress_ragged_eofs: bool = True, server_hostname: str | None = None) -> "SSLSocket":
        """The socket, now talking TLS (sock gives its connection up to it)."""
        if server_side and server_hostname is not None:
            raise ValueError("server_hostname can only be specified in client mode")
        if self._check_hostname and not server_side and not server_hostname:
            raise ValueError("check_hostname requires server_hostname")
        return SSLSocket(self, sock, server_side, server_hostname, do_handshake_on_connect, suppress_ragged_eofs)

    def __del__(self) -> None:
        if self._ctx != 0:
            _ssl.SSL_CTX_free(self._ctx)
            self._ctx = 0


def create_default_context(purpose: str = Purpose.SERVER_AUTH, cafile: str | None = None, capath: str | None = None,
                           cadata=None) -> SSLContext:
    """A client's context (servers checked against the system's certificate authorities, or the given
    ones, and their host names), or with Purpose.CLIENT_AUTH a server's."""
    ctx = SSLContext(PROTOCOL_TLS_SERVER if purpose == Purpose.CLIENT_AUTH else PROTOCOL_TLS_CLIENT)
    if cafile is not None or capath is not None or cadata is not None:
        ctx.load_verify_locations(cafile, capath, cadata)
    elif ctx.verify_mode != CERT_NONE:
        ctx.set_default_verify_paths()
    return ctx


def _create_unverified_context() -> SSLContext:
    """Nothing checked (the peer may be anyone): tests, or networks where that is known."""
    ctx = SSLContext(PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = CERT_NONE
    return ctx


_create_default_https_context = create_default_context


# ---------------------------------------------------------------- connections

class SSLSocket(socket.socket):
    """A socket whose bytes go through TLS: send / recv / sendall / close as a socket's."""

    def __init__(self, context: SSLContext, sock: socket.socket, server_side: bool = False, server_hostname: str | None = None,
                 do_handshake_on_connect: bool = True, suppress_ragged_eofs: bool = True):
        timeout = sock.gettimeout()
        super().__init__(sock.family, sock.type, sock.proto, sock.detach())
        self._timeout = timeout                        # (the descriptor's mode is the old socket's)
        self.context = context
        self.server_side = server_side
        self.server_hostname = server_hostname
        self.do_handshake_on_connect = do_handshake_on_connect
        self.suppress_ragged_eofs = suppress_ragged_eofs
        self._done = False
        self._ssl = _ssl.SSL_new(context._ctx)
        if self._ssl == 0:
            raise _error("cannot make a TLS connection")
        _ssl.SSL_set_fd(self._ssl, self._fd)
        if server_side:
            _ssl.SSL_set_accept_state(self._ssl)
        else:
            _ssl.SSL_set_connect_state(self._ssl)
            if server_hostname is not None:
                _ssl.SSL_ctrl(self._ssl, 55, 0, server_hostname)     # SSL_set_tlsext_host_name: SNI
                if context.check_hostname and _ssl.SSL_set1_host(self._ssl, server_hostname) != 1:
                    raise _error("cannot check the host name " + server_hostname)
        if do_handshake_on_connect and self._connected():
            self.do_handshake()

    def _connected(self) -> bool:
        try:
            self.getpeername()
            return True
        except OSError:
            return False

    def _again(self, r: int, writing: bool) -> int:
        """After an OpenSSL call gave r <= 0: 1 to call it again (once the socket is ready), 0 for the end
        of the data, else an exception."""
        err = _ssl.SSL_get_error(self._ssl, r)
        if err == SSL_ERROR_WANT_READ or err == SSL_ERROR_WANT_WRITE:
            if self._timeout is not None and self._timeout == 0.0:
                if err == SSL_ERROR_WANT_READ:
                    raise SSLWantReadError(SSL_ERROR_WANT_READ, "The operation did not complete (read)")
                raise SSLWantWriteError(SSL_ERROR_WANT_WRITE, "The operation did not complete (write)")
            self._wait(err == SSL_ERROR_WANT_WRITE)
            return 1
        if err == SSL_ERROR_ZERO_RETURN:
            return 0
        if err == SSL_ERROR_SYSCALL:
            e = _crypto.ERR_get_error()
            if e == 0:
                if self.suppress_ragged_eofs:
                    return 0
                raise SSLEOFError(SSL_ERROR_EOF, "EOF occurred in violation of protocol")
            raise SSLSyscallError(SSL_ERROR_SYSCALL, "Some I/O error occurred")
        if err == SSL_ERROR_SSL and (_crypto.ERR_peek_error() & 0x7FFFFF) == 294:   # SSL_R_UNEXPECTED_EOF_WHILE_READING:
            _crypto.ERR_clear_error()                                                # the peer closed without close_notify (OpenSSL 3)
            if self.suppress_ragged_eofs:
                return 0
            raise SSLEOFError(SSL_ERROR_EOF, "[SSL: UNEXPECTED_EOF_WHILE_READING] EOF occurred in violation of protocol")
        raise _error("TLS error", self._ssl, self.server_hostname)

    def do_handshake(self) -> None:
        """The TLS handshake: keys agreed, the peer's certificate checked."""
        self._check_open()
        while True:
            r = _ssl.SSL_do_handshake(self._ssl)
            if r == 1:
                self._done = True
                return
            if self._again(r, False) == 0:
                raise SSLEOFError(SSL_ERROR_EOF, "EOF occurred in violation of protocol")

    def connect(self, address: tuple[str, int]) -> None:
        super().connect(address)
        if self.do_handshake_on_connect:
            self.do_handshake()

    def connect_ex(self, address: tuple[str, int]) -> int:
        err = super().connect_ex(address)
        if err == 0 and self.do_handshake_on_connect:
            self.do_handshake()
        return err

    def accept(self) -> tuple[socket.socket, tuple[str, int]]:
        s, addr = super().accept()
        return (self.context.wrap_socket(s, True, self.do_handshake_on_connect, self.suppress_ragged_eofs), addr)

    def _ready(self) -> None:
        if not self._done:
            self.do_handshake()

    def send(self, data: bytes, flags: int = 0) -> int:
        """Some of data (TLS records): how much was sent."""
        self._check_open()
        if flags != 0:
            raise ValueError("non-zero flags not allowed in calls to send() on " + "SSLSocket")
        self._ready()
        if len(data) == 0:
            return 0
        while True:
            r = _ssl.SSL_write(self._ssl, data, len(data))
            if r > 0:
                return r
            if self._again(r, True) == 0:
                raise SSLZeroReturnError(SSL_ERROR_ZERO_RETURN, "TLS/SSL connection has been closed (EOF)")

    def sendall(self, data: bytes, flags: int = 0) -> None:
        done = 0
        while done < len(data):
            done += self.send(data[done:], flags)

    def write(self, data: bytes) -> int:
        return self.send(data)

    def recv(self, bufsize: int, flags: int = 0) -> bytes:
        """Up to bufsize bytes of what the peer sent (b"": it closed the connection)."""
        self._check_open()
        if flags != 0:
            raise ValueError("non-zero flags not allowed in calls to recv() on " + "SSLSocket")
        if bufsize < 0:
            raise ValueError("negative buffersize in recv")
        self._ready()
        if bufsize == 0:
            return b""
        buf = sys.buffer(bufsize)
        while True:
            r = _ssl.SSL_read(self._ssl, buf, bufsize)
            if r > 0:
                return sys.peek_bytes(buf, 0, r)
            if self._again(r, False) == 0:
                return b""

    def read(self, len: int = 1024) -> bytes:
        return self.recv(len)

    def pending(self) -> int:
        return _ssl.SSL_pending(self._ssl)

    def version(self) -> str | None:
        """The protocol agreed ("TLSv1.3"), or None before the handshake."""
        if not self._done:
            return None
        return _ssl.SSL_get_version(self._ssl)

    def cipher(self) -> tuple[str, str, int] | None:
        """(name, protocol, secret bits) of the connection's cipher."""
        if not self._done:
            return None
        c = _ssl.SSL_get_current_cipher(self._ssl)
        if c == 0:
            return None
        name = _ssl.SSL_CIPHER_get_name(c) or ""
        proto = _ssl.SSL_CIPHER_get_version(c) or ""
        return (name, proto, _ssl.SSL_CIPHER_get_bits(c, None))

    def selected_alpn_protocol(self) -> str | None:
        return None

    def _peer_cert(self) -> int:
        if not self._done:
            raise ValueError("handshake not done yet")
        return _ssl.SSL_get1_peer_certificate(self._ssl)

    def getpeercert_der(self) -> bytes | None:
        """The peer's certificate as DER bytes (getpeercert(True); compiled programs too), None if it sent none."""
        x = self._peer_cert()
        if x == 0:
            return None
        d = _der(x)
        _crypto.X509_free(x)
        return d

    if sys._compiled:
        def getpeercert(self, binary_form: bool = False) -> dict[str, str] | None:
            """The peer's certificate: CPython's keys, the values as text (one type per value in compiled
            code: subject "CN=a,O=b", subjectAltName "DNS:a, IP Address:1.2.3.4"); DER: getpeercert_der()."""
            if binary_form:
                raise ValueError("getpeercert(True): getpeercert_der() in compiled programs")
            x = self._peer_cert()
            if x == 0:
                return None
            d: dict[str, str] = {}
            if self.context.verify_mode != CERT_NONE:
                d = _cert_fields(x)
            _crypto.X509_free(x)
            return d
    else:
        def getpeercert(self, binary_form=False):
            """The peer's certificate: CPython's dict (subject, issuer, notAfter, subjectAltName ...), DER
            bytes with binary_form, None if it sent none ({} when it was not checked, as CPython)."""
            x = self._peer_cert()
            if x == 0:
                return None
            try:
                if binary_form:
                    return _der(x)
                if self.context.verify_mode == CERT_NONE:
                    return {}
                return _cert_dict(x)
            finally:
                _crypto.X509_free(x)

    def unwrap(self) -> socket.socket:
        """The TLS connection closed (close_notify), the plain socket given back."""
        if self._done and self._ssl != 0:
            _ssl.SSL_shutdown(self._ssl)
        self._free()
        return socket.socket(self.family, self.type, self.proto, self.detach())

    def _free(self) -> None:
        if self._ssl != 0:
            _ssl.SSL_free(self._ssl)
            self._ssl = 0

    def _real_close(self) -> None:
        if self._ssl != 0 and self._fd >= 0:
            if self._done:
                _ssl.SSL_shutdown(self._ssl)           # (close_notify: the peer knows the data ended here)
            self._free()
        super()._real_close()

    def __repr__(self) -> str:
        return "<ssl.SSLSocket fd=" + str(self._fd) + ", " + ("server" if self.server_side else "client") + ">"


def wrap_socket(sock: socket.socket, keyfile: str | None = None, certfile: str | None = None, server_side: bool = False,
                cert_reqs: int = CERT_NONE, ca_certs: str | None = None, server_hostname: str | None = None) -> SSLSocket:
    """The old module-level wrap_socket (removed in CPython 3.12): a context made for this socket."""
    ctx = SSLContext(PROTOCOL_TLS_SERVER if server_side else PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = cert_reqs
    if ca_certs is not None:
        ctx.load_verify_locations(ca_certs)
    if certfile is not None:
        ctx.load_cert_chain(certfile, keyfile)
    return ctx.wrap_socket(sock, server_side, True, True, server_hostname)


def get_default_verify_paths() -> tuple[str | None, str | None]:
    return (None, None)
