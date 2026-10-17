#!/bin/sh
# TLS: src/stdlib/ssl.py over the system's OpenSSL 3. CPython's ssl is the other side
# (tests/ssl/server.py serves, tests/ssl/talk.py connects); plainpy's must do what CPython's does:
#
#   tests/ssl/client.py   a client (certificates checked, the host name, http.client, urlopen):
#                         CPython's output = the interpreter's = a compiled program's
#   tests/ssl/certs.py    the peer's certificate as CPython's ssl gives it (the interpreter)
#   tests/ssl/fields.py   a compiled program's certificate fields (tests/ssl/fields.out)
#   tests/ssl/serve.py    a TLS server in plainpy (interpreter, compiled), CPython's client
#
# Environment: PLAINPY (./plainpy), PYTHON (python3: with its ssl), OUT (build/ssl), TARGET of
# the compiled programs (macos on a Mac, linux elsewhere - an i386 program: it needs the i386
# libssl.so.3, libssl3:i386; none: the interpreter only), FASM.
PLAINPY=${PLAINPY:-./plainpy}
PYTHON=${PYTHON:-python3}
OUT=${OUT:-build/ssl}
if [ -z "$TARGET" ]; then
    if [ "$(uname -s)" = Darwin ]; then TARGET=macos; else TARGET=linux; fi
fi
FASM=${FASM:-fasm}
mkdir -p "$OUT"
pass=0; fail=0
check() {                        # name, expected file, got file
    if cmp -s "$2" "$3"; then pass=$((pass+1)); echo "PASS  $1"
    else fail=$((fail+1)); echo "FAIL  $1"; diff "$2" "$3" | head -20; fi
}
compile() {                      # source, output
    "$PLAINPY" --compile --target "$TARGET" --fasm "$FASM" "$1" -o "$2" > "$2.log" 2>&1 || { cat "$2.log"; return 1; }
}

rm -f "$OUT/server.port"
"$PYTHON" tests/ssl/server.py > "$OUT/server.port" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null' EXIT
i=0; while [ $i -lt 75 ] && ! [ -s "$OUT/server.port" ]; do sleep 0.2; i=$((i+1)); done      # (up to 15 s)
port=$(head -1 "$OUT/server.port")
case "$port" in ''|*[!0-9]*) echo "the TLS server of $PYTHON did not start:"; cat "$OUT/server.port"; exit 1;; esac

"$PYTHON" tests/ssl/client.py "$port" > "$OUT/client.want" 2>&1
"$PLAINPY" tests/ssl/client.py "$port" > "$OUT/client.got" 2>&1
check "client (interpreter)" "$OUT/client.want" "$OUT/client.got"
"$PYTHON" tests/ssl/certs.py "$port" > "$OUT/certs.want" 2>&1
"$PLAINPY" tests/ssl/certs.py "$port" > "$OUT/certs.got" 2>&1
check "certificate (interpreter)" "$OUT/certs.want" "$OUT/certs.got"

serve() {                        # command: tests/ssl/serve.py, talked to by tests/ssl/talk.py
    rm -f "$OUT/serve.port"
    $1 > "$OUT/serve.port" 2>&1 &
    pid=$!
    i=0; while [ $i -lt 75 ] && ! [ -s "$OUT/serve.port" ]; do sleep 0.2; i=$((i+1)); done
    "$PYTHON" tests/ssl/talk.py "$(head -1 "$OUT/serve.port")" 2>&1
    wait $pid
    echo "server: exit $?"
    tail -n +2 "$OUT/serve.port"
}
printf 'server: TLSv1.3 ((%scommonName%s, %slocalhost%s),)\ngot b%sONE%s\ngot b%sTWO WORDS%s\ngot b%sTHREE%s\nserver: exit 0\n' \
    "'" "'" "'" "'" "'" "'" "'" "'" "'" "'" > "$OUT/serve.want"
serve "$PLAINPY tests/ssl/serve.py" > "$OUT/serve.got"
check "server (interpreter)" "$OUT/serve.want" "$OUT/serve.got"

if [ "$TARGET" != none ]; then
    if compile tests/ssl/client.py "$OUT/client"; then
        "$OUT/client" "$port" > "$OUT/client.cgot" 2>&1
        check "client ($TARGET)" "$OUT/client.want" "$OUT/client.cgot"
    else fail=$((fail+1)); echo "FAIL  client ($TARGET): not compiled"; fi
    if compile tests/ssl/fields.py "$OUT/fields"; then
        "$OUT/fields" "$port" > "$OUT/fields.got" 2>&1
        check "certificate fields ($TARGET)" tests/ssl/fields.out "$OUT/fields.got"
    else fail=$((fail+1)); echo "FAIL  certificate fields ($TARGET): not compiled"; fi
    if compile tests/ssl/serve.py "$OUT/serve"; then
        serve "$OUT/serve" > "$OUT/serve.cgot"
        check "server ($TARGET)" "$OUT/serve.want" "$OUT/serve.cgot"
    else fail=$((fail+1)); echo "FAIL  server ($TARGET): not compiled"; fi
fi
echo "ssl tests: $pass passed, $fail failed"
[ $fail -eq 0 ]
