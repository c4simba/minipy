import argparse
import sys

p = argparse.ArgumentParser(prog="tool", description="Does things.", epilog="Bye.")
p.add_argument("path", help="input file")
p.add_argument("-n", "--count", type=int, default=1, help="how many")
p.add_argument("-v", "--verbose", action="store_true", help="talk more")
p.add_argument("--mode", choices=["fast", "slow"], default="fast")
p.add_argument("--tag", action="append", help="tags")
p.add_argument("--ratio", type=float)
p.add_argument("files", nargs="*")
p.add_argument("-q", action="count", default=0)
print(p.format_help())
a = p.parse_args(["in.txt", "-n", "3", "-v", "a", "b", "--tag", "x", "--tag=y", "-qq"])
print(a)
print(a.path, a.count + 1, a.verbose, a.mode, a.tag, a.ratio, a.files, a.q)
b = p.parse_args(["x", "--ratio", "2.5", "--", "-f"])
print(b, b.ratio)
for argv in (["x", "--mode", "bad"], ["x", "-n", "zz"], [], ["x", "--nope"], ["x", "--count"], ["x", "--verb"]):
    print("--", argv, flush=True)
    try:
        p.parse_args(argv)
    except SystemExit as e:
        print("exit", e.code)

g = argparse.ArgumentParser(prog="g")
m = g.add_mutually_exclusive_group()
m.add_argument("--a", action="store_true")
m.add_argument("--b", action="store_true")
s = g.add_subparsers(dest="cmd", help="commands")
r = s.add_parser("run", help="run it")
r.add_argument("--fast", action="store_true")
s.add_parser("stop")
print(g.format_help())
for argv in (["run", "--fast"], ["stop"], [], ["--a", "--b"], ["nope"]):
    print("--", argv, flush=True)
    try:
        print(g.parse_args(argv))
    except SystemExit as e:
        print("exit", e.code)

h = argparse.ArgumentParser(prog="h", add_help=False)
h.add_argument("--level", type=int, choices=[1, 2, 3], required=True)
h.add_argument("names", nargs="+", metavar="NAME")
grp = h.add_argument_group("extra", "More settings.")
grp.add_argument("--opt", nargs="?", const="c", default="d")
print(h.format_usage(), end="")
print(h.parse_args(["--level", "2", "x", "y"]))
print(h.parse_args(["--level=3", "x", "--opt"]))
print(h.parse_args(["x", "--opt", "v", "--level", "1"]))
ns, rest = h.parse_known_args(["--level", "1", "n", "--zzz"])
print(ns, rest)
for argv in (["x"], ["--level", "5", "x"], ["--level", "1"]):
    print("--", argv, flush=True)
    try:
        h.parse_args(argv)
    except SystemExit as e:
        print("exit", e.code)
