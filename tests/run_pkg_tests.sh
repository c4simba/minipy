#!/bin/sh
# Tests of the package manager (minipy add / remove / lock / sync / check /
# list / vendor, src/stdlib/_mpy_pkg) without the network: a package index of
# its own (tests/packages/make_index.py: wheels and PyPI's JSON API as files),
# a project using it (tests/packages/proj), what the check finds, the
# interpreter running every package and the compiler only the modules that
# passed. The output, the scratch folder's path replaced by <out>, must equal
# tests/packages/expected.out.
#
#   MINIPY  minipy (default ./minipy); TARGET (default: macos on a Mac, else
#   linux), FASM, RUN as for tests/run_typed_tests.sh; OUT (build/pkgtest);
#   UPDATE=1 rewrites the expected output
MINIPY=$(cd "$(dirname "${MINIPY:-./minipy}")" && pwd)/$(basename "${MINIPY:-./minipy}")
FASM=${FASM:-fasm}
if [ -z "$TARGET" ]; then [ "$(uname -s)" = Darwin ] && TARGET=macos || TARGET=linux; fi
OUT=${OUT:-build/pkgtest}
EXP=tests/packages/expected.out
rm -rf "$OUT"
mkdir -p "$OUT/proj"
OUT=$(cd "$OUT" && pwd)
cp tests/packages/proj/*.py "$OUT/proj/"
"$MINIPY" tests/packages/make_index.py "$OUT" || exit 1
export MINIPY_HOME="$OUT/home"
export MINIPY_INDEX_URL="file://$OUT/index"
run() {
    echo "\$ minipy $*"
    "$MINIPY" "$@" 2>&1
    echo "[exit $?]"
}
compile() {
    echo "\$ minipy --compile $1"
    if "$MINIPY" --compile --target "$TARGET" --fasm "$FASM" "$1.py" -o "$1" 2>&1; then $RUN "./$1" 2>&1; fi
}
(
cd "$OUT/proj" || exit 1
run add okpkg mixpkg unionpkg untypedpkg
echo "--- pyproject.toml"; cat pyproject.toml
echo "--- pylock.toml (names, versions, check)"; grep -E '^(name|version|check|modules-ok|requirements) = ' pylock.toml
echo "--- .minipy/index"; cat .minipy/index
run list
echo "\$ minipy main.py"; "$MINIPY" main.py 2>&1
compile app
compile bad
compile dyn
run check unionpkg
run add cpkg
run add "okpkg>=5"
echo "--- pyproject.toml (unchanged)"; grep -c '"' pyproject.toml
run remove untypedpkg
run list
rm -rf .minipy/site
echo "(offline: the store has everything the lock file names)"
MINIPY_INDEX_URL=file:///nonexistent run sync
compile app
run vendor
echo "--- .minipy/site after vendor"; for e in .minipy/site/*; do [ -L "$e" ] && echo "$(basename "$e"): link" || echo "$(basename "$e"): copy"; done
) | sed "s#$OUT#<out>#g" > "$OUT/got.out"
if [ "${UPDATE:-0}" = 1 ]; then cp "$OUT/got.out" "$EXP"; echo "updated $EXP"; exit 0; fi
if diff "$EXP" "$OUT/got.out" > "$OUT/diff.txt"; then echo "package tests: passed"
else echo "package tests: FAILED"; head -40 "$OUT/diff.txt"; exit 1; fi
