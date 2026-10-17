"""The package index: PyPI's JSON API (or a folder laid out like it).

    <index>/<name>/json             a project: its releases and their files
    <index>/<name>/<version>/json   a release: its dependencies (requires_dist)

Only wheels made of Python source qualify (py3-none-any): plainpy runs no C
extension modules and builds nothing. Downloads go through curl (many at once:
--parallel - not ssl, which needs OpenSSL 3); file:// URLs are read directly."""
import json
import os
import subprocess
from .versions import Version, parse
from .reqs import canonical

DEFAULT_INDEX = "https://pypi.org/pypi"


class FetchError(Exception):
    pass


def _file_path(url):
    return url[len("file://"):] if url.startswith("file://") else ""


def fetch(url):
    """The bytes at url."""
    path = _file_path(url)
    if path:
        try:
            with open(path, "rb") as f:
                return f.read()
        except OSError as e:
            raise FetchError("cannot read %s: %s" % (path, e))
    try:
        r = subprocess.run(["curl", "-fsSL", "--retry", "2", url], capture_output=True)
    except OSError as e:
        raise FetchError("cannot run curl (needed to download from %s): %s" % (url, e))
    if r.returncode != 0:
        msg = r.stderr.decode("utf-8", "replace").strip()
        if "404" in msg:
            raise FetchError("not found: " + url)
        raise FetchError("cannot download %s: %s" % (url, msg or "curl exit status %d" % r.returncode))
    return r.stdout


def fetch_many(urls, tmpdir):
    """{url: bytes} for the urls that could be read (curl --parallel: one
    process, several connections; file:// read directly)."""
    out = {}
    net = []
    for u in urls:
        if _file_path(u):
            try:
                out[u] = fetch(u)
            except FetchError:
                pass
        else:
            net.append(u)
    if not net:
        return out
    os.makedirs(tmpdir, exist_ok=True)
    args = ["curl", "-sSL", "--parallel", "--parallel-max", "16"]
    files = []
    for i in range(len(net)):
        f = os.path.join(tmpdir, "f%d.json" % i)
        files.append(f)
        args += ["-o", f, net[i]]
    try:
        subprocess.run(args, capture_output=True)
    except OSError:
        return out
    for u, f in zip(net, files):
        try:
            with open(f, "rb") as h:
                data = h.read()
            os.remove(f)
            if data[:1] == b"{":
                out[u] = data
        except OSError:
            pass
    return out


def download(url, dest):
    """Save url's bytes as file dest."""
    data = fetch(url)
    tmp = dest + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, dest)


def wheel_tags(filename):
    """(dist, version, python tags, abi, platform) of a wheel's file name, or None."""
    if not filename.endswith(".whl"):
        return None
    parts = filename[:-4].split("-")
    if len(parts) == 6:
        parts = parts[:2] + parts[3:]                 # (a build tag)
    if len(parts) != 5:
        return None
    return (parts[0], parts[1], parts[2].split("."), parts[3], parts[4])


def pure_python(filename):
    """A wheel plainpy can use: Python 3 source for any platform."""
    t = wheel_tags(filename)
    if t is None:
        return False
    if t[3] != "none" or t[4] != "any":
        return False
    for py in t[2]:
        if py in ("py3", "py30"):
            return True
        if (py.startswith("py3") or py.startswith("cp3")) and py[3:].isdigit() and int(py[3:]) <= 14:
            return True
    return False


class Release:
    def __init__(self, name, version, files):
        self.name = name
        self.version = version
        self.files = files                            # the release's files (PyPI's dicts)

    def wheel(self):
        """The release's pure-Python wheel (PyPI's dict), or None."""
        for f in self.files:
            if pure_python(f.get("filename", "")) and not f.get("yanked"):
                return f
        return None

    def has_sdist_only(self):
        return any(f.get("packagetype") == "sdist" for f in self.files) and self.wheel() is None


class Index:
    def __init__(self, url=""):
        self.url = (url or os.environ.get("PLAINPY_INDEX_URL", "") or DEFAULT_INDEX).rstrip("/")
        self.projects = {}
        self.deps = {}

    def project(self, name):
        key = canonical(name)
        if key not in self.projects:
            try:
                self.projects[key] = json.loads(fetch("%s/%s/json" % (self.url, key)))
            except FetchError as e:
                if str(e).startswith("not found"):
                    raise FetchError("no package named '%s' in %s" % (name, self.url))
                raise
        return self.projects[key]

    def releases(self, name):
        """The project's releases, newest first (those with unparsable versions left out)."""
        data = self.project(name)
        out = []
        for ver, files in data.get("releases", {}).items():
            v = parse(ver)
            if v is not None and files and not all(f.get("yanked") for f in files):
                out.append(Release(data["info"]["name"], v, files))
        out.sort(key=lambda r: r.version.key(), reverse=True)
        return out

    def prefetch(self, name, versions):
        """Read the dependencies of several releases at once (before they are asked for)."""
        key = canonical(name)
        want = [v for v in versions if (key, str(v)) not in self.deps]
        if len(want) < 2:
            return
        urls = {"%s/%s/%s/json" % (self.url, key, v): str(v) for v in want}
        got = fetch_many(list(urls), os.path.join(os.environ.get("TMPDIR", "/tmp"), "plainpy-index-%d" % os.getpid()))
        for u, data in got.items():
            try:
                info = json.loads(data)["info"]
            except (ValueError, KeyError, TypeError):
                continue
            self.deps[(key, urls[u])] = list(info.get("requires_dist") or [])

    def requires(self, name, version):
        """A release's requirement strings (Requires-Dist)."""
        key = (canonical(name), str(version))
        if key not in self.deps:
            data = self.project(name)
            if str(data.get("info", {}).get("version")) == str(version):
                info = data["info"]
            else:
                info = json.loads(fetch("%s/%s/%s/json" % (self.url, canonical(name), version)))["info"]
            self.deps[key] = list(info.get("requires_dist") or [])
        return self.deps[key]
