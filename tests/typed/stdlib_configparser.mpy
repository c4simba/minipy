import configparser
import io

text = """
[DEFAULT]
ServerAliveInterval = 45
Compression = yes
CompressionLevel = 9
ForwardX11 = yes

[forge.example]
User = hg

[topsecret.server.example]
Port = 50022
ForwardX11 = no
path = /home/%(User)s/data
User = admin

[paths]
home = /home/me
data = %(home)s/data   ; inline comment kept
# a comment
multi = first line
  second line
"""
cfg = configparser.ConfigParser()
cfg.read_string(text)
print(cfg.sections(), cfg.defaults()["compression"])
print(cfg["forge.example"]["user"], cfg["forge.example"]["compression"], cfg.get("topsecret.server.example", "port"))
print(cfg.getint("topsecret.server.example", "port") + 1, cfg.getboolean("topsecret.server.example", "forwardx11"), cfg.getfloat("DEFAULT", "compressionlevel"))
print(cfg.get("topsecret.server.example", "path"), cfg.get("paths", "data"), repr(cfg.get("paths", "multi")))
print(cfg.get("paths", "data", raw=True), cfg.has_section("paths"), cfg.has_option("paths", "home"), cfg.has_option("paths", "nope"))
print(sorted(cfg.options("forge.example")), cfg.get("forge.example", "missing", fallback="dflt"), cfg["paths"].get("home"))
print(list(cfg["forge.example"].keys()), "user" in cfg["forge.example"], len(cfg["paths"]))
cfg.add_section("new")
cfg.set("new", "key", "value")
cfg["other"] = {"a": "1", "b": "two"}
cfg.remove_option("paths", "multi")
out = io.StringIO()
cfg.write(out)
print(out.getvalue())
for bad in ("[s]\nx = 1\nx = 2\n", "no section\n", "[s]\n[s]\n"):
    try:
        configparser.ConfigParser().read_string(bad)
    except configparser.Error as e:
        print(type(e).__name__ + ":", e)
try:
    cfg.get("nosuch", "x")
except configparser.NoSectionError as e:
    print("NoSectionError:", e)
try:
    cfg.get("paths", "zzz")
except configparser.NoOptionError as e:
    print("NoOptionError:", e)
ext = configparser.ConfigParser(interpolation=configparser.ExtendedInterpolation())
ext.read_string("[a]\nx = 1\n[b]\ny = ${a:x}/2\nz = ${y}+\n")
print(ext.get("b", "y"), ext.get("b", "z"))
raw = configparser.RawConfigParser()
raw.read_string("[r]\nv = %(nope)s\n")
print(raw.get("r", "v"))
try:
    cfg.set("new", "bad", "%(missing)s")
    cfg.get("new", "bad")
except configparser.InterpolationMissingOptionError as e:
    print("InterpolationMissingOptionError:", e)
