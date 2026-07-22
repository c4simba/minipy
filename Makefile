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

CORE_SRC = util qstr gc value containers bytecode lexer ast frontparser \
           compiler expr_compiler fs vm vm_ops vm_exc vm_builtins vm_methods main
HOST_PLATFORM_SRC    = platform/host/startup platform/host/fs_host
KOLIBRI_PLATFORM_SRC = platform/kolibri/startup platform/kolibri/console platform/kolibri/fs_kolibri platform/kolibri/syscall

HEADERS  = $(wildcard src/*.h) $(wildcard src/platform/*.h)
INCLUDES = -Isrc

.PHONY: all test test-update clean kolibrios

all: $(HOST_TARGET)

# ---------------------------- Host build -----------------------------------
HOST_OBJ = $(addprefix $(BUILD_DIR)/host/,$(addsuffix .o,$(CORE_SRC) $(HOST_PLATFORM_SRC)))

$(BUILD_DIR)/host/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(HOST_TARGET): $(HOST_OBJ)
	$(CC) $(CFLAGS) $^ -o $@

# ---------------------------- Tests -----------------------------------------
test: $(HOST_TARGET)
	@sh tests/run_tests.sh

test-update: $(HOST_TARGET)
	@sh tests/run_tests.sh --update

# ---------------------------- KolibriOS build -------------------------------
KOS32_PREFIX  = /home/simba/private/ports
KOS32_SDK     = $(KOS32_PREFIX)/sdk
KOS32_BINDIR  = $(KOS32_SDK)/bin
KOS32_CC      = $(KOS32_BINDIR)/i586-kolibrios-gcc
KOS32_OBJCOPY = $(KOS32_BINDIR)/i586-kolibrios-objcopy
KOS_APP_LDS    = kos-app-fix.lds
KOS_IMPORT_DIR = /hd0/1/import_path
KOS_BUILD_DIR  = $(BUILD_DIR)/kolibri
KOS_BIN        = $(KOS_BUILD_DIR)/minipy
KOS_MAP        = $(KOS_BUILD_DIR)/minipy.map
KOS_SDK_LIBDIR = /home/simba/private/kolibrios-sdk/libraries/newlib/libc
KOS_NEWLIB_INC = $(KOS32_SDK)/include

KOS_CFLAGS = -std=c99 -Wall -Wextra -Wno-clobbered -O2 -fomit-frame-pointer -fno-stack-protector
KOS_CFLAGS += -DMPY_PLATFORM_KOLIBRI=1 -DMPY_PLATFORM_KOLIBRIOS=1 -DMPY_FS_KOLIBRI=1
KOS_CFLAGS += -DMPY_FS_DEFAULT_IMPORT_DIR=\"$(KOS_IMPORT_DIR)\"
KOS_CFLAGS += -DMPY_DEFAULT_SCRIPT=\"$(KOS_IMPORT_DIR)/main.mpy\"
KOS_CFLAGS += -U__WIN32__ -U_Win32 -U_WIN32 -U__MINGW32__ -UWIN32
KOS_CFLAGS += -I$(KOS_NEWLIB_INC)

KOS_OBJ = $(addprefix $(KOS_BUILD_DIR)/,$(addsuffix .o,$(CORE_SRC) $(KOLIBRI_PLATFORM_SRC)))

$(KOS_BUILD_DIR)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(KOS32_CC) $(KOS_CFLAGS) $(INCLUDES) -c $< -o $@

kolibrios: $(KOS_BIN)

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
