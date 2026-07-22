CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -Wno-clobbered -O2
BUILD_DIR ?= build
HOST_TARGET ?= minipy

MINIPY_SOURCES = \
	minipy.c \
	src/00_prelude.c \
	src/01_values.c \
	src/02_containers.c \
	src/03_bytecode.c \
	src/04_lexer.c \
	src/05_frontend.c \
	src/06_parser.c \
	src/07_vm.c \
	src/08_fs.c \
	src/08_fs_host.c \
	src/08_fs_kolibri.c

.PHONY: all test clean kolibrios kolibri

all: $(HOST_TARGET)

$(HOST_TARGET): $(MINIPY_SOURCES)
	$(CC) $(CFLAGS) minipy.c -o $@

test: $(HOST_TARGET)
	./$(HOST_TARGET) tests/test.mpy

# KolibriOS build
KOS32_PREFIX = /home/simba/private/ports
KOS32_SDK = $(KOS32_PREFIX)/sdk
KOS32_BINDIR = $(KOS32_SDK)/bin
KOS32_CC = $(KOS32_BINDIR)/i586-kolibrios-gcc
KOS32_OBJCOPY = $(KOS32_BINDIR)/i586-kolibrios-objcopy
KOS_APP_LDS = kos-app-fix.lds
KOS_IMPORT_DIR = /hd0/1/import_path
KOS_BUILD_DIR = $(BUILD_DIR)/kolibri
KOS_OBJ = $(KOS_BUILD_DIR)/minipy.o
KOS_BIN = $(KOS_BUILD_DIR)/minipy
KOS_MAP = $(KOS_BUILD_DIR)/minipy.map
KOS_SDK_LIBDIR = /home/simba/private/kolibrios-sdk/libraries/newlib/libc
KOS_NEWLIB_INC = $(KOS_SDK)/include

KOS_CFLAGS = -std=c99 -Wall -Wextra -Wno-clobbered -O2 -fomit-frame-pointer -fno-stack-protector
KOS_CFLAGS += -DMPY_PLATFORM_KOLIBRI=1 -DMPY_PLATFORM_KOLIBRIOS=1 -DMPY_FS_KOLIBRI=1
KOS_CFLAGS += -DMPY_FS_DEFAULT_IMPORT_DIR=\"$(KOS_IMPORT_DIR)\"
KOS_CFLAGS += -DMPY_DEFAULT_SCRIPT=\"$(KOS_IMPORT_DIR)/main.mpy\"
KOS_CFLAGS += -U__WIN32__ -U_Win32 -U_WIN32 -U__MINGW32__ -UWIN32
KOS_CFLAGS += -I$(KOS_NEWLIB_INC)

kolibrios: $(KOS_BIN)

$(KOS_BUILD_DIR):
	mkdir -p $@

$(KOS_OBJ): $(MINIPY_SOURCES) | $(KOS_BUILD_DIR)
	$(KOS32_CC) $(KOS_CFLAGS) -c minipy.c -o $@

$(KOS_BIN): $(KOS_OBJ)
	PATH=$(KOS32_BINDIR):$$PATH $(KOS32_CC) $(KOS_CFLAGS) -nostdlib \
		-Wl,-static -Wl,-S -Wl,-T,$(KOS_APP_LDS) -Wl,-Map,$(KOS_MAP) -Wl,--image-base,0 -Wl,--enable-auto-import \
		-Wl,-L$(KOS32_SDK)/i586-kolibrios/lib -Wl,-L$(KOS_SDK_LIBDIR) \
		-Wl,--allow-multiple-definition \
		-o $@ $< \
		-Wl,--start-group -lc -lc.dll -ldll -lgcc -Wl,--end-group \
		$(KOS_SDK_LIBDIR)/crt/pseudo-reloc.o

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -f $(HOST_TARGET)
	rm -rf $(BUILD_DIR)