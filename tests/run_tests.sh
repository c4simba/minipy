#!/bin/sh
# MiniPy test runner (golden-file regression).
#
# For each standalone test tests/<name>.mpy, run the interpreter, capture
# stdout+stderr with the "[minipy] ..." debug lines filtered out, and compare
# against tests/expected/<name>.out.
#
# Usage:
#   tests/run_tests.sh            compare against golden files (default)
#   tests/run_tests.sh --update   regenerate the golden files
#
# MINIPY env var overrides the interpreter path (default: ./minipy).

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MINIPY=${MINIPY:-"$ROOT/minipy"}
EXP_DIR="$ROOT/tests/expected"

# Run from the repo root and reference scripts by a relative path, so the
# "Script: tests/<name>.mpy" banner in the output is machine-independent.
cd "$ROOT" || exit 1

# Standalone tests only. Excludes helper module utils.mpy and the import_path/
# directory (both are import targets, not tests on their own).
TESTS="advanced_runtime_test cpythonish_test draw error_test exceptions_test \
expression_ast_test for_bool_test fs_import_test raise_test statement_ast_test \
syntax_test test operators_test expressions_test assignment_test functions_test builtins_test literals_test methods_test exc_types_test stmts_test"

UPDATE=0
[ "${1:-}" = "--update" ] && UPDATE=1

mkdir -p "$EXP_DIR"

run_one() {
    # Run the interpreter and strip nondeterministic/debug noise.
    "$MINIPY" "tests/$1.mpy" 2>&1 | grep -v '^\[minipy\]'
}

if [ "$UPDATE" -eq 1 ]; then
    for t in $TESTS; do
        run_one "$t" > "$EXP_DIR/$t.out"
        echo "updated $t.out"
    done
    exit 0
fi

pass=0
fail=0
failed=""
for t in $TESTS; do
    exp="$EXP_DIR/$t.out"
    got=$(run_one "$t")
    if [ ! -f "$exp" ]; then
        echo "MISSING golden: $t (run with --update)"
        fail=$((fail+1)); failed="$failed $t"
        continue
    fi
    if [ "$got" = "$(cat "$exp")" ]; then
        echo "PASS  $t"
        pass=$((pass+1))
    else
        echo "FAIL  $t"
        printf '%s\n' "$got" | diff -u "$exp" - | sed 's/^/    /'
        fail=$((fail+1)); failed="$failed $t"
    fi
done

echo "----------------------------------------"
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ] || { echo "failing:$failed"; exit 1; }
