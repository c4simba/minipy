"""The platform the program runs on (CPython's platform): system(), machine(), node(), release(),
version(), uname(), platform(), architecture(), python_version() ..."""
import os
import sys

__all__ = ["system", "node", "release", "version", "machine", "processor", "uname", "platform", "architecture",
           "python_version", "python_version_tuple", "python_implementation", "python_compiler", "python_build",
           "libc_ver", "mac_ver"]


class uname_result:
    """(system, node, release, version, machine) and processor."""

    def __init__(self, system: str, node: str, release: str, version: str, machine: str) -> None:
        self.system = system
        self.node = node
        self.release = release
        self.version = version
        self.machine = machine

    @property
    def processor(self) -> str:
        return processor()

    def __getitem__(self, i: int) -> str:
        return [self.system, self.node, self.release, self.version, self.machine, self.processor][i]

    def __len__(self) -> int:
        return 6

    def __iter__(self):
        for x in [self.system, self.node, self.release, self.version, self.machine, self.processor]:
            yield x

    def __repr__(self) -> str:
        return ("uname_result(system=" + repr(self.system) + ", node=" + repr(self.node) + ", release=" +
                repr(self.release) + ", version=" + repr(self.version) + ", machine=" + repr(self.machine) + ")")


_cache: list[uname_result] = []


def uname() -> uname_result:
    if not _cache:
        u = os.uname()
        _cache.append(uname_result(u.sysname, u.nodename, u.release, u.version, u.machine))
    return _cache[0]


def system() -> str:
    """'Linux', 'Darwin', 'KolibriOS' ..."""
    return uname().system


def node() -> str:
    return uname().node


def release() -> str:
    return uname().release


def version() -> str:
    return uname().version


def machine() -> str:
    """'i686', 'x86_64', 'arm64' ..."""
    return uname().machine


def processor() -> str:
    m = uname().machine
    if sys.platform == "darwin":
        return "arm" if m == "arm64" else "i386"
    return m if sys.platform == "kolibrios" else ""


def architecture(executable: str = "", bits: str = "", linkage: str = "") -> tuple[str, str]:
    """(bits, linkage) of the program: ('64bit', 'Mach-O'), ('32bit', 'ELF') ..."""
    m = machine()
    b = "64bit" if m in ("x86_64", "arm64", "aarch64", "amd64") else "32bit"
    if sys.platform == "darwin":
        return b, "Mach-O"
    if sys.platform == "kolibrios":
        return b, ""
    return b, "ELF"


def mac_ver(release: str = "", versioninfo: tuple[str, str, str] = ("", "", ""), machine: str = "") -> tuple[str, tuple[str, str, str], str]:
    if sys.platform != "darwin":
        return release, versioninfo, machine
    r = _sw_vers()
    return (r if r else release), versioninfo, globals_machine()


def globals_machine() -> str:
    return uname().machine


def _sw_vers() -> str:
    try:
        with open("/System/Library/CoreServices/SystemVersion.plist") as f:
            text = f.read()
    except OSError:
        return ""
    k = text.find("<key>ProductVersion</key>")
    if k < 0:
        return ""
    a = text.find("<string>", k)
    b = text.find("</string>", a)
    return text[a + 8:b] if a >= 0 and b > a else ""


def libc_ver(executable: str | None = None, lib: str = "", version: str = "", chunksize: int = 16384) -> tuple[str, str]:
    return lib, version


def platform(aliased: bool = False, terse: bool = False) -> str:
    """One line naming the platform: 'Linux-6.1.0-x86_64-with-glibc2.36', 'macOS-15.0-arm64-arm-64bit' ..."""
    s = system()
    if s == "Darwin":
        r = _sw_vers()
        if r:
            if terse:
                return "macOS-" + r
            bits, linkage = architecture()
            return "macOS-" + r + "-" + machine() + "-" + processor() + "-" + bits + "-" + linkage
    if terse:
        return s + "-" + release()
    return s + "-" + release() + "-" + machine() + ("-" + processor() if processor() else "")


def python_implementation() -> str:
    return "CPython"


def python_version() -> str:
    return "3.14.0"


def python_version_tuple() -> tuple[str, str, str]:
    return "3", "14", "0"


def python_compiler() -> str:
    return "minipy"


def python_build() -> tuple[str, str]:
    return "main", ""


def python_branch() -> str:
    return ""


def python_revision() -> str:
    return ""
