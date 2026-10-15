# argparse (CPython's): options, positionals, groups, subparsers, help formatting, errors
import argparse, sys
sys.stderr = sys.stdout
p = argparse.ArgumentParser(prog="tool", description="A test tool.", epilog="Bye.")
p.add_argument("files", nargs="*", help="input files")
p.add_argument("-v", "--verbose", action="store_true", help="talk more")
p.add_argument("-n", "--count", type=int, default=3, help="how many (default: %(default)s)")
p.add_argument("--mode", choices=["fast", "slow"], default="fast")
p.add_argument("-o", dest="output", metavar="FILE")
p.add_argument("--tag", action="append", default=[])
p.add_argument("-q", action="count", default=0)
g = p.add_mutually_exclusive_group()
g.add_argument("--json", action="store_true")
g.add_argument("--xml", action="store_true")
a = p.parse_args(["a.txt", "b.txt", "-v", "-n", "5", "--mode", "slow", "-o", "out", "--tag", "x", "--tag", "y", "-qq", "--json"])
print(a)
print(vars(a)["files"], a.verbose, a.count, a.mode, a.output, a.tag, a.q, a.json, a.xml)
print(p.format_usage(), end="")
print(p.format_help())
for bad in [["-n", "x"], ["--mode", "medium"], ["--json", "--xml"], ["--unknown"], ["-n"]]:
    try:
        p.parse_args(bad)
    except SystemExit as e:
        print("exit", e.code)
ns, rest = p.parse_known_args(["f", "--zzz", "1"])
print(ns.files, rest)
sub = argparse.ArgumentParser(prog="git")
sp = sub.add_subparsers(dest="cmd", required=True)
c1 = sp.add_parser("commit", help="record changes")
c1.add_argument("-m", required=True)
c2 = sp.add_parser("push")
c2.add_argument("remote", nargs="?", default="origin")
print(sub.parse_args(["commit", "-m", "msg"]), sub.parse_args(["push"]))
try:
    sub.parse_args([])
except SystemExit as e:
    print("exit", e.code)
print(sub.format_help())
p2 = argparse.ArgumentParser(prog="calc", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
p2.add_argument("x", type=float)
p2.add_argument("--op", default="add", help="operation")
p2.add_argument("--flag", action=argparse.BooleanOptionalAction, default=True)
print(p2.parse_args(["2.5", "--no-flag"]))
print(p2.format_help())
p3 = argparse.ArgumentParser(prog="v", add_help=False)
p3.add_argument("--version", action="version", version="%(prog)s 1.0")
try:
    p3.parse_args(["--version"])
except SystemExit as e:
    print("exit", e.code)
