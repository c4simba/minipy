#!/bin/sh
# The c target (src/targets/c): what make test-port checks.
#   1. floats to text and back without the C library (src/aot/x2c_float.c)
#      against the C library's, on random values and exact midpoints
#   2. a program's C built freestanding - no C library headers, no C library
#      functions: only the port's (ppy_port_*) left to link - and the
#      template port without a C library
#   3. the typed test suite on the c target with its posix port
# Environment: PLAINPY (./plainpy), CC (cc), N (random float values, 200000),
# QUICK=1 (no typed suite).
PLAINPY=${PLAINPY:-./plainpy}
CC=${CC:-cc}
N=${N:-200000}
OUT=build/port
mkdir -p "$OUT"
fail=0

echo "== floats"
$CC -O2 -Isrc tests/port/float_check.c -o "$OUT/float_check" -lm || exit 1
"$OUT/float_check" "$N" || fail=1
if command -v python3 >/dev/null 2>&1; then
    python3 tests/port/midpoints.py 8000 > "$OUT/midpoints.txt" && "$OUT/float_check" --strings "$OUT/midpoints.txt" || fail=1
fi

echo "== freestanding C"
inc=$(dirname "$($CC -print-file-name=include/stdint.h)")
"$PLAINPY" --compile --target c --port template tests/port/freestanding.py -o "$OUT/freestanding" >/dev/null || exit 1
FLAGS="-ffreestanding -nostdinc -isystem $inc -fno-stack-protector -O1 -w"
if $CC $FLAGS -c "$OUT/freestanding.c" -o "$OUT/freestanding.o" && $CC $FLAGS -DPPY_PORT_NO_LIBC -c "$OUT/ppy_port_template.c" -o "$OUT/template.o"; then
    other=$(nm -u "$OUT/freestanding.o" | awk '{print $NF}' | sed 's/^_//' | grep -v '^ppy_port_' | grep -vE '^(memcpy|memset|memmove)$')
    if [ -n "$other" ]; then echo "FAIL: the program's C needs more than the port: $other"; fail=1
    else echo "the program's C needs only: $(nm -u "$OUT/freestanding.o" | awk '{print $NF}' | sed 's/^_//' | tr '\n' ' ')"; fi
else echo "FAIL: does not build freestanding"; fail=1; fi
"$PLAINPY" --compile --target c --port posix tests/port/freestanding.py -o "$OUT/freestanding_posix" >/dev/null && "$OUT/freestanding_posix" > "$OUT/freestanding.got" 2>&1
if command -v python3 >/dev/null 2>&1; then
    python3 tests/port/freestanding.py > "$OUT/freestanding.want" 2>&1
    if cmp -s "$OUT/freestanding.got" "$OUT/freestanding.want"; then echo "its output: as CPython's"; else echo "FAIL: its output differs from CPython's"; diff "$OUT/freestanding.want" "$OUT/freestanding.got"; fail=1; fi
fi

if [ "$QUICK" != 1 ]; then
    echo "== typed suite on the c target (posix port)"
    TARGET=c OUT=build/typed-c PLAINPY="$PLAINPY" sh tests/run_typed_tests.sh || fail=1
fi
[ $fail -eq 0 ] && echo "port tests: passed" || { echo "port tests: FAILED"; exit 1; }
