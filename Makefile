# Machine-local overrides (cross-toolchain paths, etc.) live in an untracked
# config.local.mk. Copy config.local.mk.example to config.local.mk and edit it;
# values set there win over the ?= defaults below. See README.
-include config.local.mk

# the version the programs report (-v) and the packages carry; a release's
# tag sets it (VERSION=1.2.3)
VERSION ?= 0.1.0

CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -O2
HOSTCC ?= cc
BUILD_DIR ?= build
HOST_TARGET ?= plainpy
HOST_COMPILER ?= plainpyc
HOST_LIB ?= plainpy.lib
# the executables lose their symbols (strip -x on macOS, -s elsewhere; `make debug` keeps them)
HOST_STRIP ?= $(if $(filter Darwin,$(shell uname -s)),strip -x,strip -s)

# ---------------------------------------------------------------------------
# Source layout
#   src/*.c                platform-independent core: parser, frontend tree, files,
#                          the standard library (deflated), main.c: plainpy
#   src/interp/*           the interpreter: bytecode compiler, VM, objects, built-ins
#   src/interp/methods/*   ... the methods of the built-in types
#   src/interp/modules/*   ... import and the modules written in C
#   src/aot/*              the typed compiler, plainpyc (plainpy --compile runs it): main,
#                          driver, peephole optimizer, kpack, the C translation (macos)
#   src/aot/types/*        ... its type checker
#   src/aot/codegen/*      ... its code generator
#   src/aot/runtime/*.asm  ... the runtime routines of compiled programs
#   src/targets/*          a folder per platform: what runs there and how plainpyc
#                          compiles for it (src/targets/README.md)
#   src/targets/posix/*    plainpy and plainpyc on Linux and macOS (POSIX/stdio),
#                          Linux system calls on other hosts (lx_emul.c)
#   src/targets/kolibri/*  KolibriOS: plainpy's backend, the console of compiled programs
# Every .c is compiled separately to a .o and linked. Headers live next to
# their .c; all includes resolve through -Isrc. Two programs: plainpy (the
# interpreter) and plainpyc (the compiler); neither carries the other.
# ---------------------------------------------------------------------------

INTERP_SRC = $(patsubst src/%.c,%,$(sort $(wildcard src/interp/*.c src/interp/*/*.c)))
AOT_SRC    = $(addprefix aot/,driver imports preludes namespaces pkgcheck rtlib peep kpack x2c) \
             $(patsubst src/%.c,%,$(sort $(wildcard src/aot/types/*.c src/aot/codegen/*.c))) \
             targets/targets $(patsubst src/%.c,%,$(sort $(wildcard src/targets/*/target.c)))
COMMON_SRC = util fs inflate ast py_lex py_parse py_dump py_front
PLAINPY_SRC = $(COMMON_SRC) $(INTERP_SRC) main
PLAINPYC_SRC = $(COMMON_SRC) $(AOT_SRC) aot/main
HOST_PLATFORM_SRC    = targets/posix/startup targets/posix/fs_host targets/posix/thread
KOLIBRI_PLATFORM_SRC = targets/kolibri/startup targets/kolibri/console targets/kolibri/fs_kolibri targets/kolibri/syscall targets/kolibri/thread \
                       targets/kolibri/ports_libc

HEADERS  = $(wildcard src/*.h src/targets/*.h src/targets/*/*.h src/interp/*.h src/interp/*/*.h src/aot/*.h src/aot/*/*.h)
INCLUDES = -Isrc -I$(BUILD_DIR)/gen

# plainpy is three files: plainpy, plainpyc and plainpy.lib, their data - the
# standard library written in Python, the runtime routines of compiled
# programs, the start of the C programs of the macos target. tools/pack.c
# writes plainpy.lib (the Python modules without comments and docstrings, each
# thing deflated) and the id the programs check it by; src/inflate.c inflates
# what is needed. The same file serves every platform.
PACK = $(BUILD_DIR)/tools/pack
PACK_SRC = tools/pack.c src/py_lex.c src/util.c src/inflate.c src/aot/kpack.c

# The runtime routines of compiled programs are fasm source, the files of
# src/aot/runtime joined in this order (a program gets its routines in it;
# src/aot/rtlib.c includes the generated file).
RTLIB_ASM = $(addprefix src/aot/runtime/,$(addsuffix .asm,system exceptions memory gc str unicode_tables unicode \
            str_index str_search str_methods bytes codecs int_bytes strbuild float int64 int_text int_math none kinds \
            list tuple set dict object file asyncio generator closure time math float_ops input)) \
            src/targets/kolibri/console.asm
ifneq ($(filter-out $(RTLIB_ASM),$(wildcard src/aot/runtime/*.asm src/targets/*/*.asm)),)
$(error RTLIB_ASM (Makefile) does not list $(filter-out $(RTLIB_ASM),$(wildcard src/aot/runtime/*.asm src/targets/*/*.asm)))
endif
RTLIB_TXT = $(BUILD_DIR)/gen/rtlib.asm
# ... and the C files of the targets that make C (src/targets/*): the start of
# every translated program (the runtime of a hosted one, of one for a port),
# the port's interface and ports - name:file, the .c files a file includes
# put in it (tools/cinline.awk)
C_TEXT_SRC = x2c_rt:src/targets/macos/x2c_host.c x2c_port:src/targets/c/x2c_port.c port_h:src/targets/c/ppy_port.h \
             port_posix:src/targets/c/port_posix.c port_template:src/targets/c/port_template.c \
             esp32_port:src/targets/esp32/port_esp32.c esp32_cmake:src/targets/esp32/project/CMakeLists.txt \
             esp32_main_cmake:src/targets/esp32/project/main/CMakeLists.txt esp32_sdkconfig:src/targets/esp32/project/sdkconfig.defaults
C_TEXT_DEPS = $(foreach t,$(C_TEXT_SRC),$(word 2,$(subst :, ,$(t)))) src/aot/x2c_machine.c src/aot/x2c_float.c \
              src/targets/posix/lx_emul.c src/targets/posix/devsim.c tools/cinline.awk
C_TEXTS = $(BUILD_DIR)/gen/text/.stamp
# ... and the standard library modules written in Python (src/pystdlib.c)
STDLIB_PY  = $(shell find src/stdlib -name '*.py')
# Each program carries only the modules it can use (a module and its submodules):
# plainpyc knows typing, functools, abc, json, collections.abc itself and compiles
# none of the interpreter's own modules ...
INTERP_ONLY_MODULES  = typing _typing _ppy_typing _ppy_generic annotationlib functools abc json \
                       collections.abc _py_abc _colorize _ppy_pkg
# ... and plainpy does natively what these do for compiled programs
COMPILE_ONLY_MODULES = _sysio _oserror _ppy_iter _ppy_exit _sysargs _ppy_jsonr _mathx
LIB    = $(BUILD_DIR)/gen/plainpy.lib
LIB_ID = $(BUILD_DIR)/gen/libid.h
# the version (rewritten only when VERSION changes: the programs are built again then)
VERSION_H = $(BUILD_DIR)/gen/version.h

.PHONY: all test test-update test-typed test-leaks test-pkg test-port test-bytecode test-ssl test-kolibri-ssl bench clean kolibrios kolibrios-debug clean-kolibri debug \
        install uninstall dist deb rpm kolibrios-dist kolibri-tls print-version wasm wasm-node FORCE

all: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)

$(PACK): $(PACK_SRC) src/py_lex.h src/py_ast.h src/inflate.h src/util.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c99 -O2 -Isrc $(PACK_SRC) -o $@

$(RTLIB_TXT): $(RTLIB_ASM)
	@mkdir -p $(dir $@)
	cat $(RTLIB_ASM) > $@

$(C_TEXTS): $(C_TEXT_DEPS)
	@mkdir -p $(dir $@)
	@for t in $(C_TEXT_SRC); do awk -f tools/cinline.awk "$${t#*:}" > "$(dir $@)$${t%%:*}.txt" || exit 1; done
	@touch $@

# plainpy.lib and its id (libid.h, compiled into the programs)
$(LIB): $(STDLIB_PY) $(RTLIB_TXT) $(C_TEXTS) $(PACK) Makefile
	@mkdir -p $(dir $@)
	$(PACK) lib $@ $(LIB_ID).tmp src/stdlib $(addprefix -i ,$(INTERP_ONLY_MODULES)) $(addprefix -c ,$(COMPILE_ONLY_MODULES)) \
		-t rtlib $(RTLIB_TXT) $(foreach t,$(C_TEXT_SRC),-t $(word 1,$(subst :, ,$(t))) $(BUILD_DIR)/gen/text/$(word 1,$(subst :, ,$(t))).txt) \
		$(sort $(STDLIB_PY))
	cmp -s $(LIB_ID).tmp $(LIB_ID) || cp $(LIB_ID).tmp $(LIB_ID)
$(LIB_ID): $(LIB) ; @:      # (a recipe: make looks at libid.h again after the lib is made, so the programs are built again)

$(HOST_LIB): $(LIB)
	cp $< $@

$(VERSION_H): FORCE
	@mkdir -p $(dir $@)
	@echo '#define PPY_VERSION "$(VERSION)"' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp
$(BUILD_DIR)/host/main.o $(BUILD_DIR)/host/aot/main.o $(BUILD_DIR)/kolibri/main.o $(BUILD_DIR)/kolibri/aot/main.o: $(VERSION_H)

# Verbose host build (same logging switches as kolibrios-debug), handy for
# reproducing debug output on the development machine.
debug:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) -DPPY_DEBUG=1 -DPPY_FS_DEBUG=1" HOST_STRIP=true

# ---------------------------- Host build -----------------------------------
HOST_OBJ  = $(addprefix $(BUILD_DIR)/host/,$(addsuffix .o,$(PLAINPY_SRC) $(HOST_PLATFORM_SRC))) $(BUILD_DIR)/host/pystdlib.o
HOST_OBJC = $(addprefix $(BUILD_DIR)/host/,$(addsuffix .o,$(PLAINPYC_SRC) $(HOST_PLATFORM_SRC))) $(BUILD_DIR)/host/pystdlib_compile.o

$(BUILD_DIR)/host/pystdlib.o: $(LIB_ID)
$(BUILD_DIR)/host/pystdlib_compile.o: src/pystdlib.c $(LIB_ID) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -DPPY_LIB_COMPILER=1 -c $< -o $@

# (CPPFLAGS and LDFLAGS: what a distribution's build adds - debian/rules)
$(BUILD_DIR)/host/%.o: src/%.c $(HEADERS) src/targets/posix/lx_emul.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(INCLUDES) -c $< -o $@

# math functions are in -lm outside macOS; ctypes in the interpreter
# (src/interp/modules/ctypes.c): dlopen is in -ldl on older glibc
HOST_LDLIBS ?= -lm $(if $(filter Linux,$(shell uname -s)),-ldl,)

$(HOST_TARGET): $(HOST_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -pthread $^ -o $@ $(HOST_LDLIBS)
	$(HOST_STRIP) $@

$(HOST_COMPILER): $(HOST_OBJC)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -lm
	$(HOST_STRIP) $@

# ---------------------------- Tests -----------------------------------------
test: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	@sh tests/run_tests.sh

# Typed compiler (plainpy --compile). On a Mac the tests are native programs
# (TARGET=macos: a C compiler is all it takes); elsewhere they need fasm and an
# i386-capable Linux to run them (TARGET=kolibri / RUN=<emulator> / FASM=...
# see the script).
TYPED_TARGET ?= $(if $(filter Darwin,$(shell uname -s)),macos,linux)
test-typed: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	@TARGET=$${TARGET:-$(TYPED_TARGET)} sh tests/run_typed_tests.sh

# leak checks: tests/leaks/*.py compiled with --count-allocs, the blocks still
# allocated at the end the same after few rounds as after many (the cycle collector)
test-leaks: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	@TARGET=$${TARGET:-$(TYPED_TARGET)} sh tests/run_leak_tests.sh

# the c target: exact float text, the program's C freestanding, the typed suite on its posix port
test-port: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	@sh tests/run_port_tests.sh

# TLS (ssl: the system's OpenSSL 3) against CPython's ssl - the interpreter, compiled programs (TARGET=none: not those)
test-ssl: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	@sh tests/run_ssl_tests.sh

# TLS on KolibriOS (mbedtls.obj), a compiled client and server in QEMU against CPython's:
# KOS_IMG=<a KolibriOS floppy image>, qemu-system-i386, mtools, fasm (FASM=...)
test-kolibri-ssl: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB) $(BUILD_DIR)/kolibri/mbedtls.obj
	@sh tests/run_kolibri_ssl_tests.sh

# bytecode files: the tests with plainpy.lib's modules as bytecode, and each test run from its .ppyc
test-bytecode: $(HOST_TARGET) $(HOST_LIB)
	@sh tests/run_bytecode_tests.sh

# the package manager (plainpy add / sync / check ...) on a package index of its own
test-pkg: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	@TARGET=$${TARGET:-$(TYPED_TARGET)} sh tests/run_pkg_tests.sh

test-update: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	@sh tests/run_tests.sh --update

# compiled programs against CPython: tests/bench/*.py timed both ways (python3)
bench: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	@python3 tests/bench/run.py

# ---------------------------- KolibriOS build -------------------------------
# Cross-toolchain locations. Overridable via config.local.mk, the environment,
# or the command line (defaults follow the common autobuild layout; see README).
KOS32_PREFIX  ?= /home/autobuild/tools/win32
KOS32_SDK     ?= $(KOS32_PREFIX)/sdk
KOS32_BINDIR  ?= $(KOS32_SDK)/bin
KOS32_CC      ?= $(KOS32_BINDIR)/i586-kolibrios-gcc
KOS32_OBJCOPY ?= $(KOS32_BINDIR)/i586-kolibrios-objcopy
KOS_APP_LDS    = kos-app-fix.lds
KOS_IMPORT_DIR = /hd0/1/import_path
KOS_BUILD_DIR  = $(BUILD_DIR)/kolibri
KOS_BIN        = $(KOS_BUILD_DIR)/plainpy
KOS_BINC       = $(KOS_BUILD_DIR)/plainpyc
KOS_LIB        = $(KOS_BUILD_DIR)/plainpy.lib
# the programs are kpacked, as plainpyc packs the programs it compiles (LZMA; the
# kernel unpacks them while loading them); KOS_KPACK=0 leaves them as they are
KOS_KPACK     ?= 1
# KOS_PORTS=1: the toolchain tools/kolibrios-toolchain.sh builds from
# KolibriOS/ports (KOS32_SDK=<where it is>: what the CI uses) - newlib as one
# static libc.a in the toolchain's sysroot, a few things it leaves out given
# by src/targets/kolibri/ports_libc.c. Otherwise an SDK with newlib as
# libc.dll (KOS_SDK_LIBDIR) and its crt.
KOS_PORTS ?= 0
ifeq ($(KOS_PORTS),1)
KOS_SDK_LIBDIR ?= $(KOS32_SDK)/i586-kolibrios/lib
KOS_NEWLIB_INC ?= $(KOS32_SDK)/i586-kolibrios/include
KOS_LIBS ?= -lc -lm -lgcc
KOS_CRT  ?=
else
KOS_SDK_LIBDIR ?= $(KOS32_SDK)/libraries/newlib/libc
KOS_NEWLIB_INC ?= $(KOS32_SDK)/include
KOS_LIBS ?= -lc -lc.dll -ldll -lgcc
KOS_CRT  ?= $(KOS_SDK_LIBDIR)/crt/pseudo-reloc.o
endif

KOS_CFLAGS = -std=c99 -Wall -Wextra -O2 -fomit-frame-pointer -fno-stack-protector
KOS_CFLAGS += -DPPY_PLATFORM_KOLIBRI=1 -DPPY_PLATFORM_KOLIBRIOS=1 -DPPY_FS_KOLIBRI=1
KOS_CFLAGS += -DPPY_FS_DEFAULT_IMPORT_DIR=\"$(KOS_IMPORT_DIR)\"
KOS_CFLAGS += -DPPY_DEFAULT_SCRIPT=\"$(KOS_IMPORT_DIR)/main.py\"
KOS_CFLAGS += -U__WIN32__ -U_Win32 -U_WIN32 -U__MINGW32__ -UWIN32
KOS_CFLAGS += -I$(KOS_NEWLIB_INC)
KOS_CFLAGS += $(if $(filter 1,$(KOS_PORTS)),-DPPY_KOS_PORTS_LIBC=1)
KOS_CFLAGS += $(EXTRA_KOS_CFLAGS)      # kolibrios-debug injects -DPPY_DEBUG / -DPPY_FS_DEBUG here

KOS_OBJ  = $(addprefix $(KOS_BUILD_DIR)/,$(addsuffix .o,$(PLAINPY_SRC) $(KOLIBRI_PLATFORM_SRC))) $(KOS_BUILD_DIR)/pystdlib.o
KOS_OBJC = $(addprefix $(KOS_BUILD_DIR)/,$(addsuffix .o,$(PLAINPYC_SRC) $(KOLIBRI_PLATFORM_SRC))) $(KOS_BUILD_DIR)/pystdlib_compile.o

$(KOS_BUILD_DIR)/pystdlib.o: $(LIB_ID)
$(KOS_BUILD_DIR)/pystdlib_compile.o: src/pystdlib.c $(LIB_ID) $(HEADERS)
	@mkdir -p $(dir $@)
	$(KOS32_CC) $(KOS_CFLAGS) $(INCLUDES) -DPPY_LIB_COMPILER=1 -c $< -o $@

$(KOS_BUILD_DIR)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(KOS32_CC) $(KOS_CFLAGS) $(INCLUDES) -c $< -o $@

kolibrios: $(KOS_BIN) $(KOS_BINC) $(KOS_LIB)

$(KOS_LIB): $(LIB)
	@mkdir -p $(dir $@)
	cp $< $@

# Verbose KolibriOS build: enables PPY_DEBUG (compile + syscall trace) and
# PPY_FS_DEBUG (f70 file I/O trace). Objects are rebuilt from clean so the debug
# defines actually take effect (they change every translation unit).
kolibrios-debug:
	$(MAKE) clean-kolibri
	$(MAKE) kolibrios EXTRA_KOS_CFLAGS="-DPPY_DEBUG=1 -DPPY_FS_DEBUG=1"
	@echo "built KolibriOS DEBUG images ($(KOS_BIN), $(KOS_BINC)) with logging enabled"

clean-kolibri:
	rm -rf $(KOS_BUILD_DIR)

$(KOS_BIN) $(KOS_BINC): $(KOS_BUILD_DIR)/%: 
	PATH=$(KOS32_BINDIR):$$PATH $(KOS32_CC) $(KOS_CFLAGS) -nostdlib \
		-Wl,-static -Wl,-s -Wl,-T,$(KOS_APP_LDS) -Wl,-Map,$@.map -Wl,--image-base,0 -Wl,--enable-auto-import \
		-Wl,-L$(KOS32_SDK)/i586-kolibrios/lib -Wl,-L$(KOS_SDK_LIBDIR) \
		-Wl,--allow-multiple-definition \
		-o $@ $(filter %.o,$^) \
		-Wl,--start-group $(KOS_LIBS) -Wl,--end-group \
		$(KOS_CRT)
	$(if $(filter 1,$(KOS_KPACK)),$(PACK) kpack $@ $@)

$(KOS_BIN): $(KOS_OBJ) $(PACK)
$(KOS_BINC): $(KOS_OBJC) $(PACK)

# mbedtls.obj, the TLS library plainpy's ssl loads on KolibriOS (function 68.19):
# the KolibriOS port of mbedTLS 2.16 built with LLVM (tools/kolibri-tls/build.sh:
# clang, llvm-link, opt, llvm-objcopy - LLVM=<their folder>; no KolibriOS
# toolchain), kpacked. kolibrios-dist takes it when it is built.
KOS_TLS = $(KOS_BUILD_DIR)/mbedtls.obj
kolibri-tls: $(KOS_TLS)
$(KOS_TLS): tools/kolibri-tls/build.sh $(wildcard tools/kolibri-tls/*.c tools/kolibri-tls/include/*.h tools/kolibri-tls/include/sys/*.h) src/stdlib/ssl.py $(PACK)
	sh tools/kolibri-tls/build.sh $(BUILD_DIR)/kolibri-tls/mbedtls.obj
	@mkdir -p $(dir $@)
	$(if $(filter 1,$(KOS_KPACK)),$(PACK) kpack $(BUILD_DIR)/kolibri-tls/mbedtls.obj $@,cp $(BUILD_DIR)/kolibri-tls/mbedtls.obj $@)

# ---------------------------- WebAssembly (src/targets/wasm) ---------------
# The interpreter in a browser page (site/try.js), built with Emscripten's emcc
# (EMCC=..., or in its Docker image: emscripten/emsdk):
#   make wasm        build/wasm/plainpy.js, .wasm, .data (plainpy.lib and lib/ inside)
#   make wasm-node   build/wasm/node/plainpy: the same interpreter as a command for
#                    node, to run the tests on it: PLAINPY=build/wasm/node/plainpy sh tests/run_tests.sh
# (WebAssembly keeps locals where the collector's stack scan cannot see them: it collects
# between the REPL's pieces only - src/interp/obj.c, mp_gc_safe_point.)
EMCC     ?= emcc
WASM_DIR  = $(BUILD_DIR)/wasm
WASM_SRC  = $(addprefix src/,$(addsuffix .c,$(COMMON_SRC) $(INTERP_SRC) $(HOST_PLATFORM_SRC))) src/pystdlib.c
WASM_CFLAGS = -std=gnu99 -O2 -w -DPPY_WASM=1 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm $(INCLUDES)
WASM_LDFLAGS = -O2 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sSTACK_SIZE=8388608 -lm
WASM_OBJ  = $(patsubst src/%.c,$(WASM_DIR)/obj/%.o,$(WASM_SRC))
$(WASM_DIR)/obj/%.o: src/%.c $(HEADERS) src/targets/posix/lx_emul.c src/targets/posix/devsim.c
	@mkdir -p $(dir $@)
	$(EMCC) $(WASM_CFLAGS) -c $< -o $@
$(WASM_DIR)/obj/pystdlib.o: $(LIB_ID)
$(WASM_DIR)/obj/main.o: $(VERSION_H)
wasm: $(WASM_DIR)/plainpy.js
$(WASM_DIR)/plainpy.js: $(WASM_OBJ) $(WASM_DIR)/obj/targets/wasm/web.o $(LIB) $(LIB_PY)
	@mkdir -p $(WASM_DIR)/data/lib
	cp $(LIB) $(WASM_DIR)/data/plainpy.lib
	cp $(LIB_PY) $(WASM_DIR)/data/lib/
	$(EMCC) $(WASM_OBJ) $(WASM_DIR)/obj/targets/wasm/web.o -o $@ $(WASM_LDFLAGS) \
		-sMODULARIZE=1 -sEXPORT_NAME=createPlainpy -sENVIRONMENT=web,worker,node -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 \
		-sFORCE_FILESYSTEM=1 -sEXPORTED_FUNCTIONS=_ppy_web_start,_ppy_web_run -sEXPORTED_RUNTIME_METHODS=ccall,FS \
		--preload-file $(WASM_DIR)/data@/plainpy
wasm-node: $(WASM_DIR)/node/plainpy
$(WASM_DIR)/node/plainpy: $(WASM_OBJ) $(WASM_DIR)/obj/main.o src/targets/wasm/node_pre.js $(LIB) $(LIB_PY)
	@mkdir -p $(WASM_DIR)/node/lib
	cp $(LIB) $(WASM_DIR)/node/plainpy.lib
	cp $(LIB_PY) $(WASM_DIR)/node/lib/
	$(EMCC) $(WASM_OBJ) $(WASM_DIR)/obj/main.o -o $(WASM_DIR)/node/plainpy.js $(WASM_LDFLAGS) \
		-sENVIRONMENT=node -sNODERAWFS=1 -sEXIT_RUNTIME=1 --pre-js src/targets/wasm/node_pre.js
	printf '#!/bin/sh\nexec node "$$(dirname "$$0")/plainpy.js" "$$@"\n' > $@
	chmod +x $@

# ---------------------------- Installing, packages -------------------------
# make install puts the programs into $(PREFIX)/bin and their data into
# $(PREFIX)/lib/plainpy (plainpy.lib and lib/: the modules every program can
# import - kolibri.py, ctypes, fastapi ...), where the programs look when
# plainpy.lib is not next to them; DESTDIR stages it (packages):
#
#   make && sudo make install          (PREFIX=/usr/local)
#   sudo make uninstall
#   make dist                          a folder to unpack anywhere: bin/, lib/plainpy/
#   make deb / make rpm                Debian / RPM packages (dpkg-deb / rpmbuild)
#   make kolibrios-dist                the KolibriOS build, zipped (make kolibrios first;
#                                      make kolibri-tls before it: mbedtls.obj in it too)
#   tools/launchpad.sh                 source packages (debian/) for the Ubuntu PPA
#
# plainpy --compile needs fasm on the PATH for the linux and kolibri targets
# (the packages suggest it) and a C compiler for macos.
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
LIBDIR  ?= $(PREFIX)/lib/plainpy
DOCDIR  ?= $(PREFIX)/share/doc/plainpy
INSTALL ?= install
LIB_PY   = $(sort $(wildcard lib/*.py))
OS_NAME  = $(if $(filter Darwin,$(shell uname -s)),macos,$(shell uname -s | tr A-Z a-z))
ARCH     = $(shell uname -m)
DIST_DIR = $(BUILD_DIR)/dist
DIST     = plainpy-$(VERSION)-$(OS_NAME)-$(ARCH)
PKG_DIR  = $(BUILD_DIR)/pkg
URL     ?= https://github.com/c4simba/plainpy
LICENSE ?= MIT AND PSF-2.0
MAINTAINER ?= $(or $(shell git log -1 --format='%an <%ae>' 2>/dev/null),plainpy <$(URL)>)
SUMMARY  = A small Python: an interpreter and a compiler of typed programs
DESCRIPTION = plainpy runs Python programs with nothing but the C library; plainpy --compile compiles statically typed ones to small native executables (Linux i386 and KolibriOS through fasm, macOS through the C compiler).

print-version:
	@echo $(VERSION)

install: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR)/lib $(DESTDIR)$(DOCDIR)
	$(INSTALL) -m 755 $(HOST_TARGET) $(HOST_COMPILER) $(DESTDIR)$(BINDIR)/
	$(INSTALL) -m 644 $(HOST_LIB) $(DESTDIR)$(LIBDIR)/
	$(INSTALL) -m 644 $(LIB_PY) $(DESTDIR)$(LIBDIR)/lib/
	$(INSTALL) -m 644 README.md LICENSE LICENSE.PSF $(DESTDIR)$(DOCDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(notdir $(HOST_TARGET)) $(DESTDIR)$(BINDIR)/$(notdir $(HOST_COMPILER))
	rm -f $(DESTDIR)$(LIBDIR)/plainpy.lib $(addprefix $(DESTDIR)$(LIBDIR)/lib/,$(notdir $(LIB_PY)))
	rm -f $(DESTDIR)$(DOCDIR)/README.md $(DESTDIR)$(DOCDIR)/LICENSE $(DESTDIR)$(DOCDIR)/LICENSE.PSF
	-rmdir $(DESTDIR)$(LIBDIR)/lib $(DESTDIR)$(LIBDIR) $(DESTDIR)$(DOCDIR) 2>/dev/null

# a tarball of bin/ and lib/plainpy: unpacked anywhere it works (the programs
# find their data from where they are)
dist: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	rm -rf $(DIST_DIR)/$(DIST)
	$(MAKE) install DESTDIR=$(abspath $(DIST_DIR))/$(DIST) PREFIX= BINDIR=/bin LIBDIR=/lib/plainpy DOCDIR=/share/doc/plainpy
	tar -C $(DIST_DIR) -czf $(BUILD_DIR)/$(DIST).tar.gz $(DIST)
	@echo "built $(BUILD_DIR)/$(DIST).tar.gz"

DEB_ARCH = $(shell dpkg --print-architecture 2>/dev/null || echo $(ARCH))
DEB      = $(BUILD_DIR)/plainpy_$(VERSION)_$(DEB_ARCH).deb
deb: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	rm -rf $(PKG_DIR)/deb
	$(MAKE) install DESTDIR=$(abspath $(PKG_DIR))/deb PREFIX=/usr
	mkdir -p $(PKG_DIR)/deb/DEBIAN
	printf '%s\n' 'Package: plainpy' 'Version: $(VERSION)' 'Section: devel' 'Priority: optional' \
		'Architecture: $(DEB_ARCH)' 'Maintainer: $(MAINTAINER)' 'Depends: libc6' 'Suggests: fasm, gcc, curl' \
		'Homepage: $(URL)' 'Description: $(SUMMARY)' ' $(DESCRIPTION)' > $(PKG_DIR)/deb/DEBIAN/control
	dpkg-deb --root-owner-group --build $(PKG_DIR)/deb $(DEB)
	@echo "built $(DEB)"

RPM_VERSION = $(subst -,~,$(VERSION))
rpm: $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	rm -rf $(PKG_DIR)/rpm
	$(MAKE) install DESTDIR=$(abspath $(PKG_DIR))/rpm/root PREFIX=/usr
	mkdir -p $(PKG_DIR)/rpm/SPECS
	printf '%s\n' 'Name: plainpy' 'Version: $(RPM_VERSION)' 'Release: 1' 'Summary: $(SUMMARY)' \
		'License: $(LICENSE)' 'URL: $(URL)' 'Suggests: fasm' 'Suggests: curl' '%global debug_package %{nil}' \
		'%global _build_id_links none' '' '%description' '$(DESCRIPTION)' '' \
		'%install' 'cp -a $(abspath $(PKG_DIR))/rpm/root/. %{buildroot}/' '' \
		'%files' '/usr/bin/plainpy' '/usr/bin/plainpyc' '/usr/lib/plainpy' '%doc /usr/share/doc/plainpy' \
		> $(PKG_DIR)/rpm/SPECS/plainpy.spec
	rpmbuild --define '_topdir $(abspath $(PKG_DIR))/rpm' -bb $(PKG_DIR)/rpm/SPECS/plainpy.spec
	cp $(PKG_DIR)/rpm/RPMS/*/plainpy-$(RPM_VERSION)-1.*.rpm $(BUILD_DIR)/
	@echo "built $$(ls $(BUILD_DIR)/plainpy-$(RPM_VERSION)-1.*.rpm)"

# the KolibriOS build zipped: the programs, plainpy.lib and lib/ in one folder
# (put it anywhere: they find their data next to them)
KOS_DIST = plainpy-$(VERSION)-kolibrios
kolibrios-dist: $(KOS_BIN) $(KOS_BINC) $(KOS_LIB)
	rm -rf $(DIST_DIR)/$(KOS_DIST)
	mkdir -p $(DIST_DIR)/$(KOS_DIST)/lib
	cp $(KOS_BIN) $(KOS_BINC) $(KOS_LIB) $(wildcard $(KOS_TLS)) LICENSE LICENSE.PSF $(DIST_DIR)/$(KOS_DIST)/
	cp $(LIB_PY) $(DIST_DIR)/$(KOS_DIST)/lib/
	cd $(DIST_DIR) && rm -f ../$(KOS_DIST).zip && zip -qr ../$(KOS_DIST).zip $(KOS_DIST)
	@echo "built $(BUILD_DIR)/$(KOS_DIST).zip"

clean:
	rm -f $(HOST_TARGET) $(HOST_COMPILER) $(HOST_LIB)
	rm -rf $(BUILD_DIR)
