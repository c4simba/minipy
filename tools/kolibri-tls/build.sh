#!/bin/sh
# plainpy's mbedtls.obj for KolibriOS: the KolibriOS port of mbedTLS 2.16.6
# (programs/develop/libraries/kos_mbedtls of the KolibriOS repository: its sources
# and config.h) built with LLVM - each source to LLVM IR, llvm-link into one
# module, one i386 COFF object out of it: the library format function 68.19
# loads (an EXPORTS table of name / address pairs, relocations DIR32 / REL32).
# It needs no other library: kos_libc.c is the C library mbedTLS calls (the
# port's build imports libc.obj and network.obj), kos_net.c sends and receives
# through KolibriOS's sockets (function 75). It exports what plainpy's ssl
# calls, as the port's mbedtls.obj names it (mbedtls_init included: nothing to
# do here), plus ppy_entropy (seed bytes: RDRAND, else the time stamp counter's
# jitter) and ppy_net_send_nb / ppy_net_recv_nb (sockets with a timeout).
#
#   sh tools/kolibri-tls/build.sh [OUT]       OUT: build/kolibri-tls/mbedtls.obj
#   make kolibri-tls                          the same, kpacked: build/kolibri/mbedtls.obj
# Environment: KOS_MBEDTLS (a checkout of the port: else cloned here, sparse),
# LLVM (the folder of clang, llvm-link, llvm-nm, llvm-objcopy, opt: Homebrew's,
# else the one clang on the PATH is in), EXPORTS=all (the port's whole API).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:-$ROOT/build/kolibri-tls/mbedtls.obj}
W=$ROOT/build/kolibri-tls
if [ -z "$LLVM" ]; then
    if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then LLVM=/opt/homebrew/opt/llvm/bin
    else LLVM=$(dirname "$(command -v clang)"); fi
fi
mkdir -p "$W/bc" "$(dirname "$OUT")"
if [ -z "$KOS_MBEDTLS" ]; then
    KOS_MBEDTLS=$W/kolibrios/programs/develop/libraries/kos_mbedtls
    if [ ! -d "$KOS_MBEDTLS/library" ]; then
        rm -rf "$W/kolibrios"
        git clone --depth 1 --filter=blob:none --sparse https://github.com/KolibriOS/kolibrios.git "$W/kolibrios"
        (cd "$W/kolibrios" && git sparse-checkout set programs/develop/libraries/kos_mbedtls)
    fi
fi
RES=$("$LLVM/clang" -print-resource-dir)
CF="-target i686-pc-windows-gnu -U_WIN32 -U_WIN64 -U__MINGW32__ -U__MINGW64__ -U__WIN32__ -UWIN32 -U__CYGWIN__
    -D__KOLIBRI__ -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe
    -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-exceptions -fno-ident -fno-common
    -DMBEDTLS_USER_CONFIG_FILE=\"kos_user_config.h\" -nostdinc -isystem $RES/include -I$HERE/include -I$KOS_MBEDTLS/include -w"
n=0
for f in "$KOS_MBEDTLS"/library/*.c "$HERE"/kos_libc.c "$HERE"/kos_net.c "$HERE"/kos_extra.c; do
    b=$(basename "$f" .c)
    case "$b" in net_sockets) continue;; esac            # (kos_net.c instead: function 75)
    "$LLVM/clang" $CF -emit-llvm -c "$f" -o "$W/bc/$b.bc"
    n=$((n+1))
done
"$LLVM/llvm-link" "$W"/bc/*.bc -o "$W/lib.bc"
# the exports: the functions src/stdlib/ssl.py calls (_tls.name, _kfunc("name")) - the rest of
# mbedTLS's code, used by none of them, is left out (a third of the library); EXPORTS=all: the
# port's (library/mbedtls_export.asm), for other programs' ctypes
"$LLVM/llvm-nm" --defined-only "$W/lib.bc" | awk '$2 ~ /^[TW]$/ {print $3}' | sed 's/^_//' | sort -u > "$W/defined.txt"   # (the windows target's names: _name)
if [ "$EXPORTS" = all ]; then
    sed -n 's/^extrn \([A-Za-z_0-9]*\).*/\1/p' "$KOS_MBEDTLS/library/mbedtls_export.asm" | sort -u | comm -12 - "$W/defined.txt" > "$W/wanted.txt"
else
    { grep -o '_tls\.[a-z_0-9]*' "$ROOT/src/stdlib/ssl.py" | sed 's/^_tls\.//'
      grep -o '"\(mbedtls\|ppy\)_[a-z_0-9]*"' "$ROOT/src/stdlib/ssl.py" | tr -d '"'; } | sort -u > "$W/wanted.txt"
    missing=$(comm -23 "$W/wanted.txt" "$W/defined.txt")
    if [ -n "$missing" ]; then echo "src/stdlib/ssl.py calls what the library lacks:"; echo "$missing"; exit 1; fi
fi
{ cat "$W/wanted.txt"; printf "mbedtls_init\nppy_entropy\nppy_net_send_nb\nppy_net_recv_nb\nmbedtls_net_init\nmbedtls_net_free\nmbedtls_net_send\nmbedtls_net_recv\n"; } | sort -u > "$W/exports.txt"
{ echo '/* the EXPORTS table function 68.19 hands out (made by tools/kolibri-tls/build.sh) */'
  while read -r s; do echo "extern char $s[];"; done < "$W/exports.txt"
  echo 'const struct { const char *name; void *address; } EXPORTS[] = {'
  while read -r s; do echo "    {\"$s\", $s},"; done < "$W/exports.txt"
  echo '    {0, 0}'
  echo '};'; } > "$W/exports.c"
"$LLVM/clang" $CF -emit-llvm -c "$W/exports.c" -o "$W/exports.bc"
"$LLVM/llvm-link" "$W/lib.bc" "$W/exports.bc" -o "$W/all.bc"
# all but EXPORTS made internal, what nothing reaches dropped; kos_libc.c's functions stay public:
# the code generator calls some of them by name (memcpy, __udivdi3, ...)
{ echo EXPORTS; "$LLVM/llvm-nm" --defined-only "$W/bc/kos_libc.bc" | awk '$2 ~ /^[TW]$/ {print $3}' | sed 's/^_//'; } > "$W/public.txt"
"$LLVM/opt" -passes='internalize,globaldce' -internalize-public-api-file="$W/public.txt" "$W/all.bc" -o "$W/small.bc"
"$LLVM/clang" -target i686-pc-windows-gnu -O2 -mno-stack-arg-probe -c "$W/small.bc" -o "$W/raw.obj"
"$LLVM/llvm-objcopy" --strip-debug --remove-section=.drectve --remove-section=.llvm_addrsig --remove-section=.deplibs "$W/raw.obj" "$OUT"
undef=$("$LLVM/llvm-nm" -u "$OUT" || true)
if [ -n "$undef" ]; then echo "$OUT: undefined (the C library kos_libc.c lacks):"; echo "$undef"; exit 1; fi
echo "$OUT: $(wc -c < "$OUT" | tr -d ' ') bytes, $n sources, $(wc -l < "$W/exports.txt" | tr -d ' ') exports"
