"""The store: every package minipy downloaded, once per machine.

    ~/.minipy/store/<name>-<version>-<sha256[:16]>/   a wheel unpacked (read only)
    ~/.minipy/cache/<sha256>.whl                       the wheel as downloaded

(MINIPY_HOME instead of ~/.minipy.) A project uses the store through links in
its .minipy/site - nothing copied, no virtual environment; `minipy vendor`
copies them instead (a project to take elsewhere, KolibriOS)."""
import hashlib
import os
import shutil
import zipfile
from . import index as idx


class StoreError(Exception):
    pass


def home():
    h = os.environ.get("MINIPY_HOME", "")
    if h:
        return h
    return os.path.join(os.environ.get("HOME", "."), ".minipy")


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def path_of(pkg):
    """The folder a locked package is unpacked in."""
    return os.path.join(home(), "store", "%s-%s-%s" % (pkg.name.lower().replace("-", "_"), pkg.version, pkg.sha256[:16]))


def fetch(pkg):
    """The package's folder in the store: downloaded (its sha256 checked) and
    unpacked unless it is there already."""
    dest = path_of(pkg)
    if os.path.exists(os.path.join(dest, ".minipy-store")):
        return dest
    cache = os.path.join(home(), "cache")
    os.makedirs(cache, exist_ok=True)
    whl = os.path.join(cache, pkg.sha256 + ".whl")
    if not os.path.exists(whl) or _sha256(whl) != pkg.sha256:
        try:
            idx.download(pkg.url, whl)
        except idx.FetchError as e:
            raise StoreError(str(e))
        got = _sha256(whl)
        if got != pkg.sha256:
            os.remove(whl)
            raise StoreError("%s: sha256 %s, but the lock file says %s - not used" % (pkg.filename, got, pkg.sha256))
    tmp = dest + ".part"
    if os.path.exists(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    try:
        z = zipfile.ZipFile(whl)
        for name in z.namelist():
            if name.startswith("/") or ".." in name.split("/"):
                raise StoreError("%s: unsafe path in the wheel: %s" % (pkg.filename, name))
        z.extractall(tmp)
        z.close()
    except (zipfile.BadZipFile, OSError) as e:
        raise StoreError("%s: cannot unpack: %s" % (pkg.filename, e))
    with open(os.path.join(tmp, ".minipy-store"), "w") as f:
        f.write("%s %s %s\n" % (pkg.name, pkg.version, pkg.sha256))
    if os.path.exists(dest):
        shutil.rmtree(dest)
    os.replace(tmp, dest)
    return dest


def top_levels(folder):
    """The top-level modules and packages a wheel installs: [(name, entry)]."""
    out = []
    for entry in sorted(os.listdir(folder)):
        if entry.endswith(".dist-info") or entry.endswith(".data") or entry.startswith(".") or entry == "__pycache__":
            continue
        p = os.path.join(folder, entry)
        if os.path.isdir(p):
            if entry.isidentifier():
                out.append((entry, entry))
        elif entry.endswith(".py") and entry[:-3].isidentifier():
            out.append((entry[:-3], entry))
    return out


def extensions(folder):
    """C extension modules in a wheel (it would not be a pure-Python one)."""
    out = []
    for d, dirs, files in os.walk(folder):
        for f in files:
            if f.endswith(".so") or f.endswith(".pyd") or f.endswith(".dylib"):
                out.append(os.path.relpath(os.path.join(d, f), folder))
    return out


def modules(folder):
    """The modules of a package's folder: [(dotted name, path relative to the folder)]."""
    out = []
    for name, entry in top_levels(folder):
        p = os.path.join(folder, entry)
        if not os.path.isdir(p):
            out.append((name, entry))
            continue
        for d, dirs, files in os.walk(p):
            dirs.sort()
            rel = os.path.relpath(d, folder)
            parts = rel.split(os.sep)
            if any(not x.isidentifier() for x in parts):
                continue
            for f in sorted(files):
                if not f.endswith(".py"):
                    continue
                stem = f[:-3]
                if stem == "__init__":
                    out.append((".".join(parts), os.path.join(rel, f)))
                elif stem != "__main__" and stem.isidentifier():
                    out.append((".".join(parts + [stem]), os.path.join(rel, f)))
    return out


def link_site(site, entries, copy=False):
    """Make site hold exactly entries: [(name in site, path in the store)] -
    links, or copies (copy, or where links cannot be made)."""
    if os.path.islink(site) or os.path.isfile(site):
        os.remove(site)
    os.makedirs(site, exist_ok=True)
    wanted = dict(entries)
    for e in os.listdir(site):
        p = os.path.join(site, e)
        if e in wanted and not copy and os.path.islink(p) and os.readlink(p) == wanted[e]:
            del wanted[e]
            continue
        if os.path.islink(p) or os.path.isfile(p):
            os.remove(p)
        else:
            shutil.rmtree(p)
    for name in sorted(wanted):
        src = wanted[name]
        dst = os.path.join(site, name)
        if not copy:
            try:
                os.symlink(src, dst)
                continue
            except (OSError, AttributeError, NotImplementedError):
                pass
        if os.path.isdir(src):
            shutil.copytree(src, dst)
        else:
            shutil.copyfile(src, dst)
