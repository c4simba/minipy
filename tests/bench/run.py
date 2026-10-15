#!/usr/bin/env python3
"""Compiled minipy against CPython: the same programs, timed both ways.

    python3 tests/bench/run.py [--runs N] [--interp] [name ...]

Each tests/bench/<name>.py is a plain Python program that runs for a few
seconds in CPython and prints a line that does not depend on the machine.
It is compiled with `./minipy --compile` (for the system minipy runs on:
macos on a Mac, linux elsewhere), then CPython and the compiled program run
--runs times each (3 by default); the best (lowest) wall time of each counts,
which leaves out most of what else the machine does. Their outputs must be
the same. --interp times minipy's interpreter too (`./minipy name.py`).

    benchmark   CPython    compiled   speed-up
    loops         3.51 s     1.15 s      3.1x
    ...

On an arm64 Mac the macos target is i386 code translated to C (src/aot/x2c.c:
registers, flags and memory of the i386 kept in C variables), so it is a
good deal slower than the same program run natively on an x86 machine (Linux
or KolibriOS).
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MINIPY = os.environ.get("MINIPY", os.path.join(ROOT, "minipy"))
OUT = os.path.join(ROOT, "build", "bench")
PYTHON = os.environ.get("PYTHON", sys.executable)


def best_time(cmd, runs):
    """(the lowest wall time of `runs` runs, the output of the last one)."""
    best = None
    out = b""
    for _ in range(runs):
        t = time.perf_counter()
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        dt = time.perf_counter() - t
        if p.returncode != 0:
            raise SystemExit("%s failed (exit %d):\n%s" % (" ".join(cmd), p.returncode, p.stdout.decode(errors="replace")))
        out = p.stdout
        best = dt if best is None else min(best, dt)
    return best, out


def main():
    args = sys.argv[1:]
    runs = 3
    interp = False
    names = []
    while args:
        a = args.pop(0)
        if a == "--runs":
            runs = int(args.pop(0))
        elif a == "--interp":
            interp = True
        elif a.startswith("-"):
            raise SystemExit(__doc__)
        else:
            names.append(a)
    if not names:
        names = sorted(f[:-3] for f in os.listdir(HERE) if f.endswith(".py") and f != "run.py")
    os.makedirs(OUT, exist_ok=True)
    print("CPython %s, %s; best of %d runs" % (sys.version.split()[0], PYTHON, runs))
    head = "%-10s %10s %10s %9s" % ("benchmark", "CPython", "compiled", "speed-up")
    if interp:
        head += " %12s" % "interpreter"
    print(head)
    total_py = total_c = 0.0
    for name in names:
        src = os.path.join(HERE, name + ".py")
        exe = os.path.join(OUT, name)
        c = subprocess.run([MINIPY, "--compile", src, "-o", exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if c.returncode != 0:
            print("%-10s does not compile:\n%s" % (name, c.stdout.decode(errors="replace")))
            continue
        tp, outp = best_time([PYTHON, src], runs)
        tc, outc = best_time([exe], runs)
        line = "%-10s %8.2f s %8.2f s %8.1fx" % (name, tp, tc, tp / tc)
        if interp:
            ti, outi = best_time([MINIPY, src], runs)
            line += " %10.2f s" % ti
            if outi != outp:
                line += "  (interpreter: other output)"
        if outc != outp:
            line += "  OUTPUTS DIFFER: %r / %r" % (outp.decode(errors="replace").strip(), outc.decode(errors="replace").strip())
        print(line, flush=True)
        total_py += tp
        total_c += tc
    if total_c > 0:
        print("%-10s %8.2f s %8.2f s %8.1fx" % ("all", total_py, total_c, total_py / total_c))


if __name__ == "__main__":
    main()
