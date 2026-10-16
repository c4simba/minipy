#!/bin/sh
# The i586-kolibrios cross toolchain of KolibriOS's ports
# (https://git.kolibrios.org/KolibriOS/ports): binutils, gcc (C) and newlib,
# built from that repository's sources into PREFIX - what `make kolibrios`
# compiles plainpy for KolibriOS with:
#
#   sh tools/kolibrios-toolchain.sh [PREFIX]          (default build/kos-toolchain)
#   make kolibrios KOS32_PREFIX=PREFIX
#
# It takes a quarter of an hour; the CI keeps PREFIX between runs (keyed by
# PORTS_REF). Needs a C compiler, make, curl, bison, flex, texinfo and the
# development files of gmp, mpfr and mpc (Debian: libgmp-dev libmpfr-dev
# libmpc-dev).
#
#   PORTS_REF  the ports commit (default: the one this was made with)
#   WORK       where the sources are unpacked and built (default build/kos-toolchain-src)
#   JOBS       parallel jobs (default: the processors)
set -eu
PORTS_REF=${PORTS_REF:-97d61121350a60caa61e610d9ef98cb1b3aa2998}
PREFIX=${1:-$(pwd)/build/kos-toolchain}
WORK=${WORK:-$(pwd)/build/kos-toolchain-src}
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
TARGET=i586-kolibrios

mkdir -p "$PREFIX" "$WORK"
PREFIX=$(cd "$PREFIX" && pwd)
cd "$WORK"
if [ ! -d ports/toolchain/gcc ]; then
    [ -f ports.tar.gz ] || curl -fL --retry 3 -o ports.tar.gz "https://git.kolibrios.org/KolibriOS/ports/archive/$PORTS_REF.tar.gz"
    tar xzf ports.tar.gz ports/toolchain ports/libraries      # (the ports themselves are not needed)
fi
SRC=$WORK/ports
export PATH="$PREFIX/bin:$PATH"

echo "== binutils"
mkdir -p binutils && cd binutils
"$SRC/toolchain/binutils/configure" --target=$TARGET --prefix="$PREFIX" --disable-werror --disable-nls \
    --disable-intl --disable-sim --disable-gdb --enable-shared >configure.log
make -j"$JOBS" >make.log
make install-strip >install.log
cd ..

echo "== newlib headers, the program linker script"
mkdir -p "$PREFIX/$TARGET/include" "$PREFIX/$TARGET/lib"
cp -R "$SRC/libraries/newlib/newlib/libc/include/." "$PREFIX/$TARGET/include/"
cp "$SRC/toolchain/kos-app.lds" "$PREFIX/$TARGET/lib/"

echo "== gcc"
mkdir -p gcc && cd gcc
"$SRC/toolchain/gcc/configure" --target=$TARGET --with-sysroot="$PREFIX/$TARGET" --prefix="$PREFIX" \
    --disable-multilib --disable-nls --enable-shared --enable-languages=c >configure.log
make -j"$JOBS" all-gcc all-target-libgcc >make.log
make install-strip-gcc install-target-libgcc >install.log
cd ..

echo "== newlib"
mkdir -p newlib && cd newlib
CFLAGS=-Os "$SRC/libraries/newlib/newlib/configure" --host=$TARGET --prefix="$PREFIX" >configure.log
make -j"$JOBS" >make.log
make install >install.log
cd ..

echo "built $PREFIX/bin/$TARGET-gcc"
