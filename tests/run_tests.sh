#!/bin/sh
# MiniPy test runner (golden-file regression).
#
# For each standalone test tests/<name>.mpy, run the interpreter, capture
# stdout+stderr and compare against tests/expected/<name>.out.
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
# file names in tracebacks are machine-independent.
cd "$ROOT" || exit 1

# Standalone tests only. Excludes helper module utils.mpy and the import_path/
# directory (both are import targets, not tests on their own).
TESTS="advanced_runtime_test cpythonish_test draw error_test exceptions_test \
expression_ast_test for_bool_test fs_import_test raise_test statement_ast_test \
syntax_test test operators_test expressions_test assignment_test functions_test builtins_test literals_test methods_test exc_types_test stmts_test oop_test singleline_test imports_test sys_test exc_across_calls_test thread_test loop_stack_test interp_features_test annotations_test except_star_test template_strings_test"

# Programs the compiled tests run as well: the interpreter's output must equal
# the compiled one (tests/typed/expected/<name>.out).
CROSS="kolibri_lib_test:kos_decode multiline_test:multiline kolibri_ui_test:files_kolibri json_dumps_test:json_dumps records_test:records fastapi_handle_test:fastapi_handle frontend_test:frontend"

UPDATE=0
[ "${1:-}" = "--update" ] && UPDATE=1

mkdir -p "$EXP_DIR"

run_one() {
    "$MINIPY" "tests/$1.mpy" 2>&1
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

for pair in $CROSS; do
    t=${pair%%:*}; typed=${pair#*:}
    [ "$UPDATE" -eq 1 ] && continue
    got=$(run_one "$t")
    if [ "$got" = "$(cat "$ROOT/tests/typed/expected/$typed.out")" ]; then
        echo "PASS  $t (= compiled $typed)"
        pass=$((pass+1))
    else
        echo "FAIL  $t (differs from compiled $typed)"
        printf '%s\n' "$got" | diff -u "$ROOT/tests/typed/expected/$typed.out" - | sed 's/^/    /' | head -40
        fail=$((fail+1)); failed="$failed $t"
    fi
done

echo "----------------------------------------"
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ] || { echo "failing:$failed"; exit 1; }
