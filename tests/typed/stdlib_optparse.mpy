# optparse: the classic option parser (compiled: Values gets a typed field per destination)
import optparse
from optparse import OptionParser, OptionGroup, make_option
import sys


class Parser(OptionParser):
    def error(self, msg: str) -> None:
        print("error:", msg)
        raise SystemExit(2)


p = Parser(usage="%prog [options] FILE...", version="%prog 1.2", prog="demo",
           description="Process some files, the way demo does it, with options that change how.",
           epilog="See the manual for more.")
p.add_option("-v", "--verbose", action="store_true", dest="verbose", help="talk more")
p.add_option("-q", "--quiet", action="store_false", dest="verbose", help="talk less")
p.add_option("-n", "--num", type="int", default=3, help="how many [default: %default]")
p.add_option("-r", "--ratio", type="float", metavar="R", help="a ratio")
p.add_option("-o", "--out", metavar="FILE", help="write the result to FILE, which is created when it does not exist yet and replaced otherwise")
p.add_option("-I", "--include", action="append", dest="incs", help="a directory to search")
p.add_option("-d", action="count", dest="debug", help="more debugging")
p.add_option("--mode", type="choice", choices=["fast", "slow"], default="fast", help="speed: %default")
p.add_option("--fast", action="store_const", const=1, dest="speed")
p.add_option("--slow", action="store_const", const=2, dest="speed")
p.add_option("--point", type="int", nargs=2, help="x and y")
p.add_option("--tag", action="append_const", const="t", dest="tags")
g = OptionGroup(p, "Dangerous Options", "Caution: use these options at your own risk.")
g.add_option("-g", "--go", action="store_true", help="go for it")
p.add_option_group(g)
p.set_defaults(debug=0)

opts, args = p.parse_args(["-v", "-n", "0x10", "--out=x.txt", "-I", "a", "-Ib", "--incl", "c", "-dd", "f1",
                           "--fast", "--slow", "--point", "3", "4", "--tag", "--tag", "-r", "2.5", "--mo", "slow", "f2"])
print(opts)
print(args, opts.verbose, opts.num, opts.ratio, opts.out, opts.incs, opts.debug, opts.mode, opts.speed, opts.point,
      opts.tags, opts.go)
opts, args = p.parse_args(["-q", "--", "-v", "x"])
print(opts, args)
opts, args = p.parse_args([])
print(opts, args, opts.num + 1, opts.debug + 1)
print(p.format_help())
p.print_usage()
print(repr(p.get_usage()), p.get_version(), p.get_prog_name(), p.has_option("-v"), p.has_option("-z"),
      p.get_option("--num"), p.get_option("-x"))

for bad in [["-z"], ["--nu", "x"], ["--num"], ["-n"], ["--mode", "medium"], ["-r", "abc"], ["--verbose=yes"],
            ["--point", "1"], ["--o", "y"]]:
    try:
        p.parse_args(bad)
    except SystemExit as e:
        print("exit", e.code)
for flag in ["--help", "--version"]:
    try:
        p.parse_args([flag])
    except SystemExit as e:
        print("exit", e.code)

p2 = OptionParser(prog="t", option_list=[make_option("-a", action="store_true", default=False),
                                         make_option("-b", "--bee", default="x", help="bee [%default]")],
                  add_help_option=False, formatter=optparse.TitledHelpFormatter())
p2.disable_interspersed_args()
o2, a2 = p2.parse_args(["-a", "pos", "-b", "y"])
print(o2, a2, o2.a, o2.bee)
print(p2.format_help())
p2.set_default("bee", "z")
print(p2.get_default_values(), p2.parse_args([])[0])

p3 = OptionParser(conflict_handler="resolve", prog="r")
p3.add_option("-x", "--ex", action="store_true")
p3.add_option("-x", "--xx", action="store_true", help="the second x")
p3.remove_option("--ex")
print(p3.format_option_help())
try:
    OptionParser().add_option("x")
except optparse.OptionError as e:
    print("OptionError:", e)
try:
    q = OptionParser()
    q.add_option("-k")
    q.add_option("-k")
except optparse.OptionConflictError as e:
    print("OptionConflictError:", e)
try:
    OptionParser().add_option("--zz", action="bogus")
except optparse.OptionError as e:
    print("OptionError:", e)
