#!/bin/sh
# examples/fastapi/main.py compiled for the machine this runs on (on a Mac:
# the macos target, a native executable) and served to a few requests on
# 127.0.0.1:8000 with curl; then the same with the interpreter.
#
#   sh tests/fastapi_native.sh
MINIPY=${MINIPY:-./minipy}
OUT=${OUT:-build/fastapi-native}
mkdir -p "$OUT"
"$MINIPY" --compile examples/fastapi/main.py -o "$OUT/main" >/dev/null || exit 1
fail=0
check() {
    got=$(curl -s -X "$1" "http://127.0.0.1:8000$2")
    if [ "$got" = "$3" ]; then echo "PASS $1 $2"; else echo "FAIL $1 $2: $got"; fail=1; fi
}
serve() {                           # $1: what, then the command
    what=$1; shift
    echo "-- $what"
    "$@" > "$OUT/server.log" 2>&1 &
    pid=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do curl -s -o /dev/null http://127.0.0.1:8000/ && break; sleep 0.3; done
    check GET / '{"Hello":"World"}'
    check GET /items/5 '{"item_id":5,"q":null}'
    check GET '/items/5?q=somequery' '{"item_id":5,"q":"somequery"}'
    check GET /items/foo '{"detail":[{"type":"int_parsing","loc":["path","item_id"],"msg":"Input should be a valid integer, unable to parse string as an integer","input":"foo"}]}'
    check POST / '{"detail":"Method Not Allowed"}'
    kill $pid; wait $pid 2>/dev/null
    sed 's/^/    /' "$OUT/server.log"
}
serve compiled "$OUT/main"
serve interpreter "$MINIPY" examples/fastapi/main.py
exit $fail
