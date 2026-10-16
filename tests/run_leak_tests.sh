#!/bin/sh
# Leak checks of compiled programs: each tests/leaks/<name>.py takes a
# number of rounds as its argument; it is compiled with --count-allocs and run
# for a few rounds and for many, and the heap blocks still live at its end
# (the "[live blocks: N, ...]" report) must be the same - reference counting
# and the cycle collector (src/aot/runtime/gc.asm, rt_gc_root) free all the rest.
#
#   PLAINPY compiler (default ./plainpy); TARGET (default: macos on a Mac,
#   else linux), FASM, RUN as for tests/run_typed_tests.sh; OUT (build/leaks)
#
# usage: sh tests/run_leak_tests.sh [tests/leaks/name.py ...]
PLAINPY=${PLAINPY:-./plainpy}
FASM=${FASM:-fasm}
if [ -z "$TARGET" ]; then [ "$(uname -s)" = Darwin ] && TARGET=macos || TARGET=linux; fi
OUT=${OUT:-build/leaks}
mkdir -p "$OUT"
pass=0; fail=0; failed=""
files="$*"
[ -n "$files" ] || files=$(ls tests/leaks/*.py)
for f in $files; do
    n=$(basename "$f" .py)
    if ! "$PLAINPY" --compile --count-allocs --target "$TARGET" --fasm "$FASM" "$f" -o "$OUT/$n" >"$OUT/$n.log" 2>&1; then
        fail=$((fail+1)); failed="$failed $n"; echo "FAIL $n (does not compile)"; head -5 "$OUT/$n.log"; continue
    fi
    small=$($RUN "$OUT/$n" 10 2>&1 | sed -n 's/.*\[live blocks: \([0-9]*\),.*/\1/p')
    big=$($RUN "$OUT/$n" 2000 2>&1 | sed -n 's/.*\[live blocks: \([0-9]*\),.*/\1/p')
    if [ -n "$small" ] && [ "$small" = "$big" ]; then pass=$((pass+1))
    else fail=$((fail+1)); failed="$failed $n"; echo "FAIL $n: live blocks after 10 rounds: $small, after 2000: $big"; fi
done
echo "leak tests ($TARGET): $pass passed, $fail failed"
[ $fail -eq 0 ] || { echo "failed:$failed"; exit 1; }
