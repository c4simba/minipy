# Machine-local overrides (cross-toolchain paths, etc.) live in an untracked
# config.local.mk. Copy config.local.mk.example to config.local.mk and edit it;
# values set there win over the ?= defaults below. See README.
-include config.local.mk

CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -Wno-clobbered -O2
BUILD_DIR ?= build
HOST_TARGET ?= minipy

# ---------------------------------------------------------------------------
# Source layout
#   src/*.c                platform-independent core
#   src/platform/host/*    hosted (POSIX/stdio) backend
#   src/platform/kolibri/* KolibriOS backend
# Every .c is compiled separately to a .o and linked. Headers live next to
# their .c; all includes resolve through -Isrc.
# ---------------------------------------------------------------------------

CORE_SRC = util fs ast i_obj i_ops i_compile i_eval i_format i_methods i_builtins i_modules \
           aot_driver aot_types aot_codegen aot_rtlib aot_x2c py_lex py_parse py_dump py_front main
HOST_PLATFORM_SRC    = platform/host/startup platform/host/fs_host platform/host/thread
KOLIBRI_PLATFORM_SRC = platform/kolibri/startup platform/kolibri/console platform/kolibri/fs_kolibri platform/kolibri/syscall platform/kolibri/thread

HEADERS  = $(wildcard src/*.h) $(wildcard src/platform/*.h)
INCLUDES = -Isrc -I$(BUILD_DIR)/gen

# The runtime routines of compiled programs are fasm source; minipy carries
# them as a C string (src/aot_rtlib.c includes the generated file).
RTLIB_INC = $(BUILD_DIR)/gen/aot_rtlib.inc
# ... and the start of the C programs of the macos target (src/aot_x2c.c)
X2C_INC = $(BUILD_DIR)/gen/aot_x2c_rt.inc

.PHONY: all test test-update test-typed clean kolibrios kolibrios-debug clean-kolibri debug

all: $(HOST_TARGET)

$(RTLIB_INC): src/aot_rtlib.asm
	@mkdir -p $(dir $@)
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $< > $@

$(X2C_INC): src/aot_x2c_rt.c
	@mkdir -p $(dir $@)
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $< > $@

# Verbose host build (same logging switches as kolibrios-debug), handy for
# reproducing debug output on the development machine.
debug:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) -DMPY_DEBUG=1 -DMPY_FS_DEBUG=1"

# ---------------------------- Host build -----------------------------------
HOST_OBJ = $(addprefix $(BUILD_DIR)/host/,$(addsuffix .o,$(CORE_SRC) $(HOST_PLATFORM_SRC)))

$(BUILD_DIR)/host/aot_rtlib.o: $(RTLIB_INC)
$(BUILD_DIR)/host/aot_x2c.o: $(X2C_INC)

$(BUILD_DIR)/host/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -pthread $(INCLUDES) -c $< -o $@

# ctypes in the interpreter (vm_stdlib.c): dlopen is in -ldl on older glibc
HOST_LDLIBS ?= $(if $(filter Linux,$(shell uname -s)),-ldl,)

$(HOST_TARGET): $(HOST_OBJ)
	$(CC) $(CFLAGS) -pthread $^ -o $@ $(HOST_LDLIBS)

# ---------------------------- Tests -----------------------------------------
test: $(HOST_TARGET)
	@sh tests/run_tests.sh

# Typed compiler (minipy --compile). On a Mac the tests are native programs
# (TARGET=macos: a C compiler is all it takes); elsewhere they need fasm and an
# i386-capable Linux to run them (TARGET=kolibri / RUN=<emulator> / FASM=...
# see the script).
TYPED_TARGET ?= $(if $(filter Darwin,$(shell uname -s)),macos,linux)
test-typed: $(HOST_TARGET)
	@TARGET=$${TARGET:-$(TYPED_TARGET)} sh tests/run_typed_tests.sh

test-update: $(HOST_TARGET)
	@sh tests/run_tests.sh --update

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
KOS_BIN        = $(KOS_BUILD_DIR)/minipy
KOS_MAP        = $(KOS_BUILD_DIR)/minipy.map
KOS_SDK_LIBDIR ?= $(KOS32_SDK)/libraries/newlib/libc
KOS_NEWLIB_INC ?= $(KOS32_SDK)/include

KOS_CFLAGS = -std=c99 -Wall -Wextra -Wno-clobbered -O2 -fomit-frame-pointer -fno-stack-protector
KOS_CFLAGS += -DMPY_PLATFORM_KOLIBRI=1 -DMPY_PLATFORM_KOLIBRIOS=1 -DMPY_FS_KOLIBRI=1
KOS_CFLAGS += -DMPY_FS_DEFAULT_IMPORT_DIR=\"$(KOS_IMPORT_DIR)\"
KOS_CFLAGS += -DMPY_DEFAULT_SCRIPT=\"$(KOS_IMPORT_DIR)/main.mpy\"
KOS_CFLAGS += -U__WIN32__ -U_Win32 -U_WIN32 -U__MINGW32__ -UWIN32
KOS_CFLAGS += -I$(KOS_NEWLIB_INC)
KOS_CFLAGS += $(EXTRA_KOS_CFLAGS)      # kolibrios-debug injects -DMPY_DEBUG / -DMPY_FS_DEBUG here

KOS_OBJ = $(addprefix $(KOS_BUILD_DIR)/,$(addsuffix .o,$(CORE_SRC) $(KOLIBRI_PLATFORM_SRC)))

$(KOS_BUILD_DIR)/aot_rtlib.o: $(RTLIB_INC)
$(KOS_BUILD_DIR)/aot_x2c.o: $(X2C_INC)

$(KOS_BUILD_DIR)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(KOS32_CC) $(KOS_CFLAGS) $(INCLUDES) -c $< -o $@

kolibrios: $(KOS_BIN)

# Verbose KolibriOS build: enables MPY_DEBUG (compile + syscall trace) and
# MPY_FS_DEBUG (f70 file I/O trace). Objects are rebuilt from clean so the debug
# defines actually take effect (they change every translation unit).
kolibrios-debug:
	$(MAKE) clean-kolibri
	$(MAKE) kolibrios EXTRA_KOS_CFLAGS="-DMPY_DEBUG=1 -DMPY_FS_DEBUG=1"
	@echo "built KolibriOS DEBUG image ($(KOS_BIN)) with logging enabled"

clean-kolibri:
	rm -rf $(KOS_BUILD_DIR)

$(KOS_BIN): $(KOS_OBJ)
	PATH=$(KOS32_BINDIR):$$PATH $(KOS32_CC) $(KOS_CFLAGS) -nostdlib \
		-Wl,-static -Wl,-S -Wl,-T,$(KOS_APP_LDS) -Wl,-Map,$(KOS_MAP) -Wl,--image-base,0 -Wl,--enable-auto-import \
		-Wl,-L$(KOS32_SDK)/i586-kolibrios/lib -Wl,-L$(KOS_SDK_LIBDIR) \
		-Wl,--allow-multiple-definition \
		-o $@ $(KOS_OBJ) \
		-Wl,--start-group -lc -lc.dll -ldll -lgcc -Wl,--end-group \
		$(KOS_SDK_LIBDIR)/crt/pseudo-reloc.o

clean:
	rm -f $(HOST_TARGET)
	rm -rf $(BUILD_DIR)
