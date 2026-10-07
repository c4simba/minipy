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

### Compile to a native executable

See [Compiled mode](#compiled-mode-ahead-of-time-fasm) below: statically
typed programs become small i386 executables for Linux or KolibriOS.

## Compiled mode (ahead-of-time, fasm)

Besides interpreting, `minipy` compiles **statically typed** programs to small
native executables:

```sh
./minipy --compile app.mpy                    # Linux i386 executable ./app
./minipy --compile --target kolibri app.mpy   # KolibriOS application ./app
./minipy --compile -S app.mpy                 # only the assembly listing app.asm
```

```text
app.mpy + every imported module
  -> lexer / AST (same frontend as the interpreter)
  -> type check and inference            (src/aot_types.c)
  -> one fasm listing app.asm, i386      (src/aot_codegen.c)
     + the runtime routines it uses      (src/aot_rtlib.asm)
  -> fasm -> app   (no linker, no libc, no runtime library to install)
```

Values are machine values: `int`/`bool` are 32-bit integers, `float` is an
x87 double, `str`/`list`/`dict`/`set`/objects are pointers to
reference-counted heap blocks. A class is a struct (fields at fixed offsets
plus a vtable for methods that subclasses override); `print` formats into a
buffer and makes one `write` system call (`int 0x80`) or, on KolibriOS, one
message to the shell console. Only the runtime routines a program needs are
emitted, so `print("Hello, world!")` is a 192-byte ELF (543 bytes on
KolibriOS, which includes the console connection).

### Types

Every variable, parameter, return value, field and container element has one
type, written as an annotation or inferred from the code (`i = 1` makes `i`
an `int`). Storing a value of another type is a compile error:

```python
def mean(xs: list[float]) -> float:    # annotations where inference has nothing to go on
    return sum(xs) / len(xs)

count = 0                              # int, inferred
count = "zero"                         # error: variable 'count' is int, cannot take str
```

- types: `int`, `bool`, `float`, `str`, `list[T]`, `set[T]`, `dict[K, V]`
  (keys: `int`, `str`, tuples or objects), `tuple[A, B, ...]`, classes (also
  `"Class"` forward references, `module.Class`), `sys.buffer`;
- `int`/`bool` values are widened where a `float` is expected; a subclass
  instance is accepted where its base class is expected;
- `None` is the zero value of the type it meets: `0`, `0.0`, `""`, a null
  object, and a fresh empty container for `list`/`dict`/`set`; `x is None`
  tests for that zero/empty value;
- unannotated parameters and empty literals get their types from how they are
  used; whatever stays unknown must be annotated (`cannot infer the type of
  parameter 'v'; add a type annotation`). A module function that the program
  never calls and whose parameter types are therefore unknown is left out
  instead (libraries such as `examples/kolibri.mpy` keep working).

Supported: functions (default values, keyword arguments, recursion,
`global`), classes with fields, methods, single inheritance, `super()`,
`@staticmethod`, `@property`, `__init__`, `__str__`/`__repr__`, `__len__`,
operator methods (`__add__ __sub__ __mul__ __truediv__ ... __neg__`,
`__eq__ __ne__ __lt__ __le__ __gt__ __ge__`, `__getitem__ __setitem__
__contains__`), constant class attributes (`Config.SIZE`), `isinstance`;
`if`/`while`/`for` (with `else`, `break`, `continue`) over `range`,
`reversed`, `enumerate`, `zip`, `dict.items()`, strings, tuples and
containers; list/set/dict comprehensions and generator arguments
(`sum(x * x for x in xs)`); slicing; tuples (`tuple[int, str]`, multiple
return values, unpacking, comparison, sorting); `%` formatting, f-strings,
`str.format` and `format()` with Python's format specs (fill, `<>^`, sign,
`#`, `0`, width, `,`, precision, `d s f e g x X o b c %`);
`sorted`/`list.sort` with `key=` (a lambda or a function, also `len`,
`str.lower`, ...) and `reverse=`, `min`/`max` with `key=`;
`try`/`except`/`else`/`finally` and `raise` (below); `async def` / `await`
(below); files (`open(path, "r"|"w"|"a")`, `read readline readlines write
close`, `for line in f`, `with open(...) as f`; on KolibriOS through system
function 70, written at `close()`); `assert`, `with x as y`, `del`; builtins
`len str repr int float bool abs min max ord chr sum sorted reversed any all
divmod pow hex bin oct format open isinstance input round list set dict`; the
usual `str`, `list`, `dict` and `set` methods.

Not supported in compiled code (compile errors): generators (`yield`),
`lambda` other than as a sort key, nested functions/classes, `*args`/`**kwargs`,
`nonlocal`, the `thread` module. `int` is 32-bit (it wraps on overflow).

### Exceptions

`try`/`except`/`else`/`finally`, `raise X(message)`, bare `raise`, `except
(A, B) as e` work as in Python. Exceptions are objects of the built-in classes
(`Exception`, `ValueError`, `KeyError`, `IndexError`, `ZeroDivisionError`,
`RuntimeError`, `AttributeError`, `AssertionError`, ... - the usual hierarchy)
or of user classes derived from them; `str(e)` is the message. Run-time errors
(index out of range, missing key, division by zero, attribute of `None`,
failed `assert`, `int("x")`) raise the matching built-in exception. An uncaught
exception prints `Name: message` to stderr and exits with status 1. A `try`
costs a handler record in the frame; programs that never handle exceptions
keep the smaller "print and exit" error paths.

### async / await

`async def` functions, `await`, and a built-in `asyncio` module:
`asyncio.run(main())`, `await asyncio.sleep(seconds)`,
`asyncio.create_task(coro())` (a `Task[T]`: `await task`, `task.done()`,
`task.result()`) and `await asyncio.gather(a(), b(), ...)` (a list of the
results). Tasks are stackful: each runs on its own 64 KiB stack, `await
coroutine()` is a plain call, and a task gives the CPU back to the event loop
when it sleeps or waits for another task. The loop sleeps with `nanosleep`
(Linux) or system function 5 (KolibriOS) when every task is waiting for time.

### Built-in modules

`sys` (below), `asyncio`, `math` (`sqrt sin cos tan asin acos atan atan2 exp
log log10 log2 pow hypot fmod fabs floor ceil trunc degrees radians gcd isqrt
pi e tau`, on the x87), `time` (`time`, `monotonic`, `perf_counter`, `sleep`;
on KolibriOS `time()` counts from boot) and `random` (`seed random randint
randrange uniform choice shuffle`; xorshift32). Float arithmetic rounds to
double precision like CPython, and floats print like CPython (`repr`: the
shortest text that reads back the same).

### Imports

Imports are resolved at compile time: the compiler collects the entry script's
imports (and theirs, transitively) and emits every module into the one
listing; a module's body runs when it is first imported, as in Python.
Therefore every `import` / `from ... import` must be at the top of its module
(an optional docstring may precede them) - never inside a function, class,
`if`, loop or `try`, and not after other statements.

### System calls: the `sys` module

`sys.platform` is a constant (`"linux"` / `"kolibrios"`).
`sys.syscall(eax, ebx, ...)` (up to 6 integers, `str` or buffers - passed as
the address of their bytes) issues `int 0x80` on Linux or `int 0x40` on
KolibriOS and returns the registers `[eax, ebx, ecx, edx, esi, edi]` after the
call; `sys.syscall(...)[0]` reads just `eax` without building the list.
`examples/window.mpy` (with the unannotated `examples/kolibri.mpy` wrappers)
compiles unchanged: `minipy --compile --target kolibri examples/window.mpy`
gives a 1.9 KB KolibriOS application.
`sys.buffer(n | str)`, `sys.poke(buf, offset, value, size)`,
`sys.peek(buf, offset, size)`, `sys.poke_str`, `sys.peek_str` and
`sys.addr(buf)` build the structures system calls take, and `sys.exit(code)`
ends the program.

### Building and testing

Needs [fasm](https://flatassembler.net) (1.7x). Programs are i386: they run on
x86/x86-64 Linux (no 32-bit libraries needed) and on KolibriOS.

```sh
make                  # minipy (interpreter + compiler)
make test-typed       # compile tests/typed/*.mpy, run them, compare with tests/typed/expected
```

`--fasm`, `--fasm-args` (or `MPY_FASM`, `MPY_FASM_ARGS`) choose the
assembler; `--stack` sets a KolibriOS application's stack size. The
`tests/typed/err_*.mpy` tests check the compiler's diagnostics; most of the
other expected outputs are byte-for-byte CPython's.

Without an x86 Linux (an arm64 Mac, say), `tests/x86run.py` runs the i386
programs - and the Linux fasm itself - in a small emulator built on
[unicorn](https://www.unicorn-engine.org) (`pip install unicorn`). It also runs
KolibriOS applications: it plays the shell's side of the console, maps system
function 70 to host files, and fakes a headless desktop (window calls are
logged; events come from `X86RUN_EVENTS`, by default "redraw, then the close
button"):

```sh
X="python3 tests/x86run.py"
FASM="$X /path/to/fasm" RUN="$X" sh tests/run_typed_tests.sh
TARGET=kolibri FASM="$X /path/to/fasm" RUN="$X" sh tests/run_typed_tests.sh
```

On KolibriOS itself the KolibriOS build of `minipy` compiles the same way,
running `/sys/develop/fasm` with KolibriOS fasm's `infile,outfile,path`
arguments; nothing else has to be installed.

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