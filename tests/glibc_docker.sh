#!/bin/sh
# Compiled programs that use the C library (ctypes) against a real i386 glibc,
# in a Docker container: tests/typed/ctypes_linux.mpy (its output must be the
# expected one) and examples/fastapi/main.py served to a few requests.
#
#   IMAGE=<image> FASM=<fasm> sh tests/glibc_docker.sh
#
# IMAGE: any image with bash and an i386 C library (/lib/ld-linux.so.2), e.g.
# Ubuntu with libc6-i386 (gcc-multilib); FASM, RUN as for run_typed_tests.sh.
MINIPY=${MINIPY:-./minipy}
FASM=${FASM:-fasm}
IMAGE=${IMAGE:-minipy-ubuntu-test}
OUT=${OUT:-build/glibc}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
"$MINIPY" --compile --target linux --fasm "$FASM" tests/typed/ctypes_linux.mpy -o "$OUT/ctypes_linux" >/dev/null || exit 1
"$MINIPY" --compile --target linux --fasm "$FASM" examples/fastapi/main.py -o "$OUT/fastapi_main" >/dev/null || exit 1
cp tests/typed/expected/ctypes_linux.out "$OUT/ctypes_linux.expected"
cat > "$OUT/check.sh" <<'EOF'
#!/bin/bash
cd /w
fail=0
./ctypes_linux > ctypes_linux.got 2>&1
if cmp -s ctypes_linux.got ctypes_linux.expected; then echo "PASS ctypes_linux"; else echo "FAIL ctypes_linux"; diff ctypes_linux.expected ctypes_linux.got; fail=1; fi
./fastapi_main > server.log 2>&1 &
pid=$!
sleep 1
ask() {
    exec 3<>/dev/tcp/127.0.0.1/8000
    printf "%s %s HTTP/1.1\r\nHost: localhost\r\n\r\n" "$1" "$2" >&3
    tail -n 1 <&3
    exec 3>&-
}
check() {
    got=$(ask "$1" "$2")
    if [ "$got" = "$3" ]; then echo "PASS $1 $2"; else echo "FAIL $1 $2: $got"; fail=1; fi
}
check GET / '{"Hello":"World"}'
check GET /items/5 '{"item_id":5,"q":null}'
check GET '/items/5?q=somequery' '{"item_id":5,"q":"somequery"}'
check GET /items/foo '{"detail":[{"type":"int_parsing","loc":["path","item_id"],"msg":"Input should be a valid integer, unable to parse string as an integer","input":"foo"}]}'
check POST / '{"detail":"Method Not Allowed"}'
kill $pid
sed 's/^/    /' server.log
exit $fail
EOF
docker run --rm -v "$OUT":/w "$IMAGE" bash /w/check.sh
