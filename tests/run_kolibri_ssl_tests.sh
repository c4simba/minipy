#!/bin/sh
# ssl on KolibriOS, in QEMU: tests/ssl/kolibri.py compiled for the kolibri target and run in a real
# KolibriOS - plainpy's client of CPython's server (tests/ssl/server.py on this machine), then
# plainpy's server for CPython's client (tests/ssl/talk.py, through QEMU's port forwarding). What
# the program writes (kout.txt) must be tests/ssl/kolibri.out, what CPython's client prints the
# lines below.
#
# Needs qemu-system-i386, mtools (mtype, mcopy), a KolibriOS floppy image (KOS_IMG: kolibri.img of
# builds.kolibrios.org/en_US/latest-img.7z), mbedtls.obj (TLS: make kolibri-tls) and fasm (FASM).
# OUT/hd is the VM's /hd0/1; a copy of the image starts the program at boot. WAIT: the seconds the
# VM gets for each half (300: QEMU without KVM is slow); PORT: the host's end of the server's port.
# KOS_PLAINPY=<the folder of a KolibriOS build: plainpy, plainpy.lib> runs the program in plainpy
# there (the interpreter's ctypes and ssl) instead of compiling it.
PLAINPY=${PLAINPY:-./plainpy}
PYTHON=${PYTHON:-python3}
OUT=${OUT:-build/kolibri-ssl}
FASM=${FASM:-fasm}
TLS=${TLS:-build/kolibri/mbedtls.obj}
WAIT=${WAIT:-300}
PORT=${PORT:-18443}
if [ ! -f "$KOS_IMG" ]; then echo "KOS_IMG=<a KolibriOS floppy image> (builds.kolibrios.org/en_US/latest-img.7z: kolibri.img)"; exit 1; fi
if [ ! -f "$TLS" ]; then echo "$TLS: not built (make kolibri-tls)"; exit 1; fi
rm -rf "$OUT"
mkdir -p "$OUT/hd"
if [ -n "$KOS_PLAINPY" ]; then
    cp "$KOS_PLAINPY/plainpy" "$KOS_PLAINPY/plainpy.lib" tests/ssl/kolibri.py "$OUT/hd/"
    mkdir -p "$OUT/hd/lib" && cp lib/*.py "$OUT/hd/lib/"
    run='/hd0/1/plainpy /hd0/1/kolibri.py'
    mode=interpreter
else
    "$PLAINPY" --compile --target kolibri --fasm "$FASM" tests/ssl/kolibri.py -o "$OUT/hd/ktest" > "$OUT/compile.log" 2>&1 || { cat "$OUT/compile.log"; exit 1; }
    rm -f "$OUT/hd/ktest.asm"
    run='/hd0/1/ktest ""'
    mode=compiled
fi
cp tests/ssl/cert.pem tests/ssl/key.pem "$OUT/hd/"
cp "$TLS" "$OUT/hd/mbedtls.obj"
cp "$KOS_IMG" "$OUT/run.img"
mtype -i "$OUT/run.img" ::/SETTINGS/AUTORUN.DAT > "$OUT/autorun.dat" || exit 1
printf '%s 0\r\n' "$run" >> "$OUT/autorun.dat"
mcopy -o -i "$OUT/run.img" "$OUT/autorun.dat" ::/SETTINGS/AUTORUN.DAT || exit 1

"$PYTHON" tests/ssl/server.py > "$OUT/server.port" 2>&1 &
server=$!
qemu=""
talk=""
trap 'kill $server $qemu $talk 2>/dev/null' EXIT
i=0; while [ $i -lt 75 ] && ! [ -s "$OUT/server.port" ]; do sleep 0.2; i=$((i+1)); done
head -1 "$OUT/server.port" > "$OUT/hd/port.txt"
qemu-system-i386 -m 256 -drive "file=$OUT/run.img,format=raw,if=floppy" -boot a -drive "file=fat:rw:$OUT/hd,format=raw,if=ide,index=0" \
    -vga std -display none -nic "user,model=e1000,hostfwd=tcp:127.0.0.1:$PORT-:8443" > "$OUT/qemu.log" 2>&1 &
qemu=$!
upto() {                         # a line of kout.txt, the seconds to wait for it
    i=0; while [ $i -lt $(($2 * 2)) ] && ! grep -q "^$1" "$OUT/hd/kout.txt" 2>/dev/null; do sleep 0.5; i=$((i+1)); done
}
upto listening "$WAIT"
"$PYTHON" tests/ssl/talk.py "$PORT" > "$OUT/talk.got" 2>&1 &
talk=$!
upto done "$WAIT"
kill $qemu $talk 2>/dev/null

pass=0; fail=0
check() {                        # name, expected file, got file
    if cmp -s "$2" "$3"; then pass=$((pass+1)); echo "PASS  $1"
    else fail=$((fail+1)); echo "FAIL  $1"; diff "$2" "$3" | head -20; fi
}
check "client and server (kolibrios, $mode)" tests/ssl/kolibri.out "$OUT/hd/kout.txt"
printf 'server: TLSv1.2 ((%scommonName%s, %slocalhost%s),)\ngot b%sONE%s\ngot b%sTWO WORDS%s\ngot b%sTHREE%s\n' \
    "'" "'" "'" "'" "'" "'" "'" "'" "'" "'" > "$OUT/talk.want"
check "CPython's client of it" "$OUT/talk.want" "$OUT/talk.got"
echo "kolibrios ssl tests: $pass passed, $fail failed"
[ $fail -eq 0 ]
