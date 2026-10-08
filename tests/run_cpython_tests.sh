#!/bin/sh
# Tests of the cpython target (minipy --compile --target cpython): every
# tests/cpython/*.py is run by the Python itself (the expected output) and
# compiled to a native executable with it; the outputs must be equal.
#
#   PYTHON=<python 3.14> sh tests/run_cpython_tests.sh [tests/cpython/name.py ...]
#
# PYTHON   the Python (default python3; a venv's python brings its packages)
# OUT      scratch directory (default build/cpython-tests)
# FLAGS    more minipy options (e.g. "--stdlib")
MINIPY=${MINIPY:-./minipy}
PYTHON=${PYTHON:-python3}
OUT=${OUT:-build/cpython-tests}
mkdir -p "$OUT"
pass=0; fail=0; failed=""
files="$*"
[ -n "$files" ] || files=$(ls tests/cpython/*.py)
for f in $files; do
    n=$(basename "$f" .py)
    (cd "$(dirname "$f")" && "$PYTHON" -W ignore::SyntaxWarning "$n.py") > "$OUT/$n.expected" 2>&1
    if ! "$MINIPY" --compile --target cpython --python "$PYTHON" $FLAGS "$f" -o "$OUT/$n" > "$OUT/$n.log" 2>&1; then
        fail=$((fail+1)); failed="$failed $n"; echo "FAIL $n (does not compile)"; tail -5 "$OUT/$n.log"; continue
    fi
    (cd "$(dirname "$f")" && "../../$OUT/$n") > "$OUT/$n.got" 2>&1
    if cmp -s "$OUT/$n.expected" "$OUT/$n.got"; then pass=$((pass+1))
    else fail=$((fail+1)); failed="$failed $n"; echo "FAIL $n"; diff "$OUT/$n.expected" "$OUT/$n.got" | head -20; fi
done
echo "cpython target tests: $pass passed, $fail failed"
[ $fail -eq 0 ] || { echo "failed:$failed"; exit 1; }
