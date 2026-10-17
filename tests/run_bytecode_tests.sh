#!/bin/sh
# Bytecode files (src/interp/bytecode.c): the interpreter's tests run from bytecode.
#
#   1. plainpy.lib's modules compiled (plainpy --bytecode-lib): tests/run_tests.sh
#      with that library instead of the sources.
#   2. each test, and the modules it imports, compiled (plainpy --bytecode) and
#      the sources deleted: its output must be what tests/expected/<name>.out
#      says, but for the source lines of tracebacks (there is no source to show).
#
# Environment: PLAINPY (default ./plainpy), OUT (default build/bytecode),
# SKIP: tests left out (space-separated).
PLAINPY=${PLAINPY:-./plainpy}
OUT=${OUT:-build/bytecode}
case "$PLAINPY" in /*) ;; *) PLAINPY="$(pwd)/$PLAINPY";; esac
# what needs its own source: inspect.getsource, traceback's source lines, a test reading its file
SKIP="${SKIP:-} stdlib_inspect_test stdlib_traceback_test stdlib_bytearray_io_test"
rm -rf "$OUT"; mkdir -p "$OUT"
rc=0

echo "== 1. the standard library as bytecode"
"$PLAINPY" --bytecode-lib "$OUT/plainpy.lib" || exit 1
PLAINPYLIB="$OUT/plainpy.lib" PLAINPY="$PLAINPY" sh tests/run_tests.sh || rc=1

echo "== 2. the tests as bytecode, without their sources"
eval "$(sed -n '/^TESTS=/,/"$/p' tests/run_tests.sh)"          # (run_tests.sh's list)
cp -R tests "$OUT/tests"
( cd "$OUT" && for f in $(find tests -name '*.py' ! -path 'tests/typed/*' ! -path 'tests/bench/*'); do
      "$PLAINPY" --bytecode "$f" || echo "cannot compile $f"; rm -f "$f"; done )
# (a traceback's source lines: after its File line, the lines indented by 4)
strip_tb() { awk '/^ *File ".*", line [0-9]+, in / { print; src=1; next } src && /^    / { next } { src=0; print }'; }
pass=0; fail=0
for t in $TESTS; do
    case " $SKIP " in *" $t "*) continue;; esac
    [ -f "tests/expected/$t.out" ] || continue
    ( cd "$OUT" && "$PLAINPY" "tests/$t.ppyc" 2>&1 ) | strip_tb > "$OUT/$t.got"
    strip_tb < "tests/expected/$t.out" > "$OUT/$t.want"
    if cmp -s "$OUT/$t.want" "$OUT/$t.got"; then pass=$((pass+1))
    else fail=$((fail+1)); rc=1; echo "FAIL $t (bytecode)"; diff "$OUT/$t.want" "$OUT/$t.got" | head -10; fi
done
echo "bytecode tests: $pass passed, $fail failed"
exit $rc
