# MiniPy

MiniPy is a compact bytecode compiler and stack-based virtual machine for a Python-shaped language, written from scratch in C. It has no external runtime dependencies beyond the standard C library. The project is designed to be portable and can target both host systems (Linux, macOS) and KolibriOS.

## Pipeline

```text
source code
  → lexer
  → expression AST + typed statement AST
  → symbol-table traversal
  → AST-driven bytecode compiler
  → stack VM
```

## Features

- `global` / `nonlocal` declarations backed by shared closure dictionaries
- Function default arguments (literal defaults)
- Simple `@name` decorators
- Exception objects: `BaseException`, `RuntimeError`, `StopIteration`
- `try` / `except` / `finally` with protected bytecode regions
- `iter()` / `next()` builtins and `for` loops via iterator objects
- Object protocol hooks: `__len__`, `__add__`, `__eq__`, `__getitem__`, `__setitem__`, `__contains__`, `__iter__`, `__next__`
- Pluggable filesystem backend for imports (host stdio, KolibriOS/newlib, embeddable)

## Limitations

- Default arguments are literal-only
- Decorators are simple-name only (no arbitrary decorator expressions)
- Closures use shared dictionaries rather than CPython-style cell objects
- Cross-frame exception unwind relies on the recursive VM stack (a future trampoline VM would improve this)

## Host build

### Prerequisites

- A C compiler (GCC or Clang)
- GNU Make

### Build and test

```sh
make
make test
```

### Run a script

```sh
./minipy tests/test.mpy
```

### Diagnostic flags

```sh
./minipy --dump-ast tests/statement_ast_test.mpy
./minipy --dump-symbols tests/advanced_runtime_test.mpy
./minipy --dump-bytecode tests/statement_ast_test.mpy
./minipy --fs-info
```

## KolibriOS build

The Makefile provides `kolibrios` and `kolibri` targets for the kos32 GCC toolchain. Defaults assume the common KolibriOS toolchain layout under `/home/autobuild/tools/win32`, but every important path is overridable.

### Build

```sh
make kolibrios
```

With a custom SDK location:

```sh
make kolibrios \
  KOS32_PREFIX=/home/autobuild/tools/win32 \
  KOS_SDK=/home/autobuild/tools/win32/sdk
```

### Output

```text
build/kolibri/minipy.o
build/kolibri/minipy.elf
build/kolibri/minipy
build/kolibri/minipy.map
```

### Toolchain overrides

```sh
make kolibrios KOS32_CC=kos32-gcc KOS32_LD=kos32-ld KOS32_OBJCOPY=kos32-objcopy
make kolibrios KOS_NEWLIB_INC=/path/to/sdk/sources/newlib/libc/include
make kolibrios KOS_APP_LDS=/path/to/sdk/sources/newlib/app.lds
make kolibrios KOS_LIBDIR=/home/autobuild/tools/win32/mingw32/lib
make kolibrios KOS_LIBS="-lapp -lc.dll -lgcc"
make kolibrios KOS_IMPORT_DIR=/hd0/1/import_path
make kolibrios KOS_DEFAULT_SCRIPT=/hd0/1/import_path/main.mpy
```

`KOS_DEFAULT_SCRIPT` is used when the KolibriOS build is launched without command-line arguments (e.g. as a GUI app). The default value is `/hd0/1/import_path/main.mpy`.

### Diagnostics

```sh
make kolibrios-config     # show resolved toolchain configuration
make kolibrios-dryrun     # print commands without executing
make kolibrios-hostcheck  # verify toolchain availability
```

## Filesystem backend

Filesystem access is abstracted behind `src/08_fs.c` with swappable backends:

| File | Purpose |
|------|---------|
| `src/08_fs.c` | Common path/module helpers and backend dispatch |
| `src/08_fs_host.c` | Default stdio backend for host builds |
| `src/08_fs_kolibri.c` | KolibriOS/newlib backend (enabled with `MPY_FS_KOLIBRI`) |

### Public API

```c
mpy_fs_read_file(path)
mpy_fs_try_read_file(path, &error_message)
mpy_fs_dirname(path)
mpy_fs_module_path(importer_dir, module_name)
mpy_fs_backend_name()
```

### Import resolution (KolibriOS)

Paths are normalized to forward slashes. Simple imports are resolved relative to the importing file:

```python
import utils        # from /rd/1/minipy/tests/test.mpy → /rd/1/minipy/tests/utils.mpy
import pkg.tools    # → pkg/tools.mpy relative to the importer directory
```

If a relative path cannot be opened and `MPY_FS_DEFAULT_IMPORT_DIR` is set, the FS layer retries from that fixed directory. The KolibriOS Makefile sets this from `KOS_IMPORT_DIR`.

### Adding a new backend

To target an embedded platform, add a new backend file implementing `mpy_fs_backend_read_file()` backed by ROM, flash, an RTOS VFS, or a fixed manifest. The compiler and VM require no changes.