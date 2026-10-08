#!/bin/sh
# Tests of the typed compiler (minipy --compile).
#
#   tests/typed/<name>.mpy      compiled (an i386 Linux executable by default) and run; its
#                               output (stdout+stderr, plus "[exit N]" when N != 0)
#                               must equal tests/typed/expected/<name>.out.
#                               <name>.in, when present, is fed to stdin;
#                               <name>.events: the GUI events x86run delivers;
#                               <name>.requests: the HTTP requests its fake C
#                               library's sockets receive (one per line).
#                               expected/<name>.<target>.out, when present, is
#                               the expectation for that target instead.
#   tests/typed/err_<name>.mpy  must be rejected by the compiler with the
#                               diagnostic in tests/typed/expected/err_<name>.err.
#
# Environment:
#   MINIPY   compiler            (default ./minipy)
#   FASM     fasm executable     (default fasm)
#   RUN      prefix for running the i386 programs, e.g. an emulator (default none)
#   TARGET   linux (default), kolibri or macos; tests named *_linux / *_kolibri
#            run only for that target (*_linux also for macos, unless they need
#            the scripted HTTP clients of x86run: <name>.requests). macos
#            programs are native: no FASM, no RUN.
#   OUT      scratch directory   (default build/typed)
#   UPDATE=1 rewrite the expected files from the current results
#
# usage: sh tests/run_typed_tests.sh [tests/typed/name.mpy ...]
MINIPY=${MINIPY:-./minipy}
FASM=${FASM:-fasm}
TARGET=${TARGET:-linux}
OUT=${OUT:-build/typed}
DIR=tests/typed
EXP=$DIR/expected
mkdir -p "$OUT" "$EXP"
pass=0; fail=0; failed=""
# tests/typed/lib holds verbatim copies of the example modules the tests import
for m in kolibri kui files; do
    if ! cmp -s "examples/$m.mpy" "$DIR/lib/$m.mpy"; then
        fail=$((fail+1)); failed="$failed lib/$m"; echo "FAIL $DIR/lib/$m.mpy is not a copy of examples/$m.mpy"
    fi
done
files="$*"
[ -n "$files" ] || files=$(ls $DIR/*.mpy)
for f in $files; do
    n=$(basename "$f" .mpy)
    case "$n" in
    *_linux) [ "$TARGET" = linux ] || { [ "$TARGET" = macos ] && [ ! -f "$DIR/$n.requests" ]; } || continue;;
    *_kolibri) [ "$TARGET" = kolibri ] || continue;;
    esac
    case "$n" in
    err_*)
        "$MINIPY" --compile -S "$f" -o "$OUT/$n" >"$OUT/$n.err" 2>&1
        rc=$?
        sed -e "s|^$DIR/||" "$OUT/$n.err" > "$OUT/$n.got"
        if [ "$UPDATE" = 1 ]; then cp "$OUT/$n.got" "$EXP/$n.err"; fi
        if [ $rc -ne 0 ] && cmp -s "$OUT/$n.got" "$EXP/$n.err"; then pass=$((pass+1))
        else fail=$((fail+1)); failed="$failed $n"; echo "FAIL $n (compiler exit $rc)"; diff "$EXP/$n.err" "$OUT/$n.got" | head -10; fi
        ;;
    *)
        if ! "$MINIPY" --compile --target "$TARGET" --fasm "$FASM" "$f" -o "$OUT/$n" >"$OUT/$n.log" 2>&1; then
            fail=$((fail+1)); failed="$failed $n"; echo "FAIL $n (does not compile)"; head -10 "$OUT/$n.log"; continue
        fi
        inp=/dev/null; [ -f "$DIR/$n.in" ] && inp="$DIR/$n.in"
        ev=1,3; [ -f "$DIR/$n.events" ] && ev=$(cat "$DIR/$n.events")   # scripted GUI events for x86run
        rq=; [ -f "$DIR/$n.requests" ] && rq="$DIR/$n.requests"          # scripted HTTP clients for x86run
        X86RUN_EVENTS="$ev" X86RUN_REQUESTS="$rq" $RUN "$OUT/$n" <"$inp" >"$OUT/$n.got" 2>&1
        rc=$?
        [ $rc -ne 0 ] && echo "[exit $rc]" >>"$OUT/$n.got"
        exp="$EXP/$n.out"; [ -f "$EXP/$n.$TARGET.out" ] && exp="$EXP/$n.$TARGET.out"
        if [ "$UPDATE" = 1 ]; then cp "$OUT/$n.got" "$exp"; fi
        if cmp -s "$OUT/$n.got" "$exp"; then pass=$((pass+1))
        else fail=$((fail+1)); failed="$failed $n"; echo "FAIL $n"; diff "$exp" "$OUT/$n.got" | head -20; fi
        ;;
    esac
done
echo "typed tests ($TARGET): $pass passed, $fail failed"
[ $fail -eq 0 ] || { echo "failed:$failed"; exit 1; }
