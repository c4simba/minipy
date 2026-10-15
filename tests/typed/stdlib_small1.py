import getopt
import keyword
import os
import platform
import secrets

print(keyword.kwlist[:5], len(keyword.kwlist), keyword.iskeyword("for"), keyword.iskeyword("print"), keyword.issoftkeyword("match"))
print(getopt.getopt(["-a", "-bval", "-c", "x", "--long=1", "--flag", "rest", "-z"], "ab:c:", ["long=", "flag"]))
print(getopt.gnu_getopt(["x", "-a", "--flag", "y"], "a", ["flag"]))
print(getopt.getopt(["--fl"], "", "flag"), getopt.getopt(["-a", "-"], "a"))
for bad in (["-q"], ["--fl"], ["-b"], ["--flag=1"]):
    try:
        getopt.getopt(bad, "ab:", ["flag", "flog"])
    except getopt.GetoptError as e:
        print("GetoptError:", e, e.opt)
t = secrets.token_hex(8)
u = secrets.token_urlsafe(9)
print(len(t), all(c in "0123456789abcdef" for c in t), len(u), len(secrets.token_bytes()), 0 <= secrets.randbelow(10) < 10)
print(secrets.choice(["x"]), secrets.randbits(4) < 16, secrets.compare_digest("abc", "abc"), secrets.compare_digest(b"a", b"b"))
try:
    secrets.randbelow(0)
except ValueError as e:
    print("ValueError:", e)
print(platform.system() == os.uname().sysname, platform.machine() == os.uname().machine, platform.node() == os.uname().nodename)
print(platform.uname().system == platform.system(), len(platform.architecture()), platform.python_implementation(), platform.python_version_tuple()[0])
