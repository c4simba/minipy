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
- Decorators: `@name`, `@obj.attr`, factories with arguments (`@win.button("OK", x=10)`), stacked
- Python's line joining: statements continue inside brackets and after a `\`; triple-quoted strings may span lines; adjacent string literals are joined
- `sorted` / `min` / `max` / `list.sort` with `key=` and `reverse=`; `from typing import ...` and annotated attributes (`self.items: list[str] = []`) are accepted (annotations are not evaluated)
- Exception objects: `BaseException`, `RuntimeError`, `StopIteration`
- `try` / `except` / `finally` with protected bytecode regions
- `iter()` / `next()` builtins and `for` loops via iterator objects
- Object protocol hooks: `__len__`, `__add__`, `__eq__`, `__getitem__`, `__setitem__`, `__contains__`, `__iter__`, `__next__`
- Pluggable filesystem backend for imports (host stdio, KolibriOS/newlib, embeddable)

## Limitations

- Default arguments are literal-only
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
emitted, so `print("Hello, world!")` is a 201-byte ELF (552 bytes on
KolibriOS, which includes the console connection).

### Memory: reference counts the compiler leaves out

Reference counting is what keeps shared objects alive, so it stays; but the
compiler does not count where it can prove a count does not matter:

- reading a variable, a field or an item, temporaries and arguments are
  lent, not counted;
- a loop variable borrows its item from the container when the container
  keeps every item for the whole loop: a fresh one nothing else refers to
  (`text.split()`, `sorted(xs)`, a comprehension, a generator call), an
  immutable one (`str`, `tuple`), or any container if the loop's body runs no
  user code and removes nothing (`pop`, `remove`, `del`, `xs[i] = ...`);
- a variable's reference moves on its last use (backward liveness over the
  function, loops to a fixpoint): `return x`, `y = x`, `xs.append(x)`,
  `yield x` take it without an incref, and the slot needs no decref;
- a function that keeps a parameter (`self.name = name` as its last use -
  constructors) takes it owned: callers hand temporaries over, so
  `Item(s + "!", [s, s])` needs neither the increfs in `__init__` nor the
  decrefs after the call.

On `tests/typed/memory.mpy` (373 000 allocations) this cuts the increfs from
225 000 to 123 000 and the decrefs from 495 000 to 393 000 (most of the rest
free objects); the code gets a little smaller too. `MPY_NO_BORROW=1`,
`MPY_NO_MOVE=1` and `MPY_NO_CONSUME=1` in the compiler's environment switch
each part off, for comparisons with `--count-allocs`.

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
  (keys: `int`, `str`, tuples or objects), `tuple[A, B, ...]`, `tuple[T, ...]`,
  `Callable[[A, B], R]` (functions as values), `Iterator[T]` /
  `Generator[T, None, None]` (generators), classes (also `"Class"` forward
  references, `module.Class`), `sys.buffer`; `typing`'s `List`, `Dict`, `Set`,
  `Tuple`, `Optional[T]` (just `T`: `None` is its zero value), `Iterable`,
  `Sequence`, `Mapping` are understood too;
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

Supported: functions (default values, keyword arguments, keyword-only
parameters, `*args`, `**kwargs`, `f(*seq)`, `f(**d)`, recursion, `global`),
lambdas, nested functions and closures (`nonlocal`), decorators, generators
(below), classes with fields, methods, single inheritance, `super()`,
`@staticmethod`, `@property`, `__init__`, `__str__`/`__repr__`, `__len__`,
`__iter__`,
operator methods (`__add__ __sub__ __mul__ __truediv__ ... __neg__`,
`__eq__ __ne__ __lt__ __le__ __gt__ __ge__`, `__getitem__ __setitem__
__contains__`), constant class attributes (`Config.SIZE`), `isinstance`;
`if`/`while`/`for` (with `else`, `break`, `continue`) over `range`,
`reversed`, `enumerate`, `zip`, `dict.items()`, strings, tuples, containers,
generators and objects with `__iter__`; list/set/dict comprehensions and
generator expressions; slicing; tuples (`tuple[int, str]`, multiple
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
divmod pow hex bin oct format open isinstance input round list set dict tuple
map filter iter next zip enumerate range` and `functools.reduce` (`map`,
`filter`, `zip`, `enumerate` and `range` used as values give lists; `dict()`
takes a dict or (key, value) pairs); `print(*xs, sep=...)`; the usual `str`,
`list`, `dict` and `set` methods.

Not supported in compiled code (compile errors): classes inside functions,
class decorators, `yield` as an expression (`x = yield`, `send()`), `return
value` in a generator, async generators, the `thread` module. Each name keeps
one type, so a function (or lambda) cannot take an `int` in one call and a
`str` in another - except decorators, which get an instance per use. `int` is
32-bit (it wraps on overflow).

### Functions as values, closures, decorators

Functions, lambdas, nested functions, methods bound to their object
(`obj.method`) and `Class.method` are values of type `Callable[[...], R]`: they
can be stored in variables, fields, lists and dicts, passed and returned. A nested function or lambda
keeps the variables of the enclosing function it uses: a variable bound once
before the closure is made is copied into it, anything else (a loop variable,
something assigned later or through `nonlocal`) is shared in a heap cell, so
late binding works as in Python. A function value is a small object (code,
name - `fn.__name__` - and the captured values); a function that captures
nothing is a static one.

```python
def make_counter():
    count = 0
    def step() -> int:
        nonlocal count
        count += 1
        return count
    return step

ops = {"+": lambda a, b: a + b, "*": lambda a, b: a * b}
print(ops["*"](6, 7), list(map(lambda w: w.upper(), ["a", "b"])))
```

Decorators work on module functions, nested functions and methods
(`name = d1(d2(function))`; on a method `obj.name(...)` then calls the
decorated value): plain ones (`@trace`), factories with arguments
(`@repeat(3)`, `@window.button("OK", x=10)`), stacked ones, and wrappers
written the usual way with `*args, **kwargs` (`@functools.wraps` is accepted).
A decorator function with unannotated parameters is generic: every use gets an
instance of its own, so one `@trace` can wrap functions of different
signatures. A decorator may return something other than a function:
`examples/kui.mpy`, a small KolibriOS UI toolkit, turns a paint function into
the window itself (see [KolibriOS API](#kolibrios-api-exampleskolibrimpy)):

```python
@window("Counter", 100, 100, 260, 160)
def counter(win: Window) -> None:          # paints the window; `counter` is the Window
    win.text(24, 36, "Count: " + str(count))

@counter.button("+1", x=20, y=80)
def increment() -> None:
    global count
    count += 1
    counter.redraw()

@counter.on_key("q")
def leave() -> None:
    counter.close()

counter.run()
```

### Generators

A function with `yield` (and `yield from`) is a generator function; calling it
makes a generator (`Iterator[T]`) that `for` loops, `next(g[, default])`,
`list()`, `sum()`, `sorted()`, `any()`/`all()` (lazily) and other
generators consume. Generator expressions are generators too, except where
they are consumed whole on the spot (`sum(x * x for x in xs)` becomes a loop).
`iter(xs)` makes one over a list. Each generator runs on a stack of its own
(64 KiB, like asyncio tasks); `next()` switches to it until it yields. An
exception raised in a generator reaches the code that called `next()`. A
generator dropped before it finishes releases what its frame holds (its
`finally` blocks do not run).

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
`sys.syscall(eax, ebx, ecx, edx, esi, edi, ebp)` (1 to 7 values: integers,
`str` or buffers - passed as the address of their bytes, always followed by a
0 byte - or `None`, a null pointer; `ebp` is 0 unless given) issues `int 0x80`
on Linux or `int 0x40` on KolibriOS and returns the registers `[eax, ebx, ecx,
edx, esi, edi]` after the call; `sys.syscall(...)[0]` reads just `eax` without
building the list. `sys.buffer(n | str)`, `sys.poke(buf, offset, value,
size)`, `sys.peek(buf, offset, size)`, `sys.poke_str`, `sys.peek_str` and
`sys.addr(buf)` build the structures system calls take; `sys.peek_at(address,
size)`, `sys.poke_at(address, value, size)`, `sys.peek_str_at(address, n)`,
`sys.poke_str_at(address, s)` and `sys.cstr_at(address, max)` (a
zero-terminated string) read and write memory the system hands out by address.
`sys.exit(code)` ends the program. The same functions exist in the KolibriOS
build of the interpreter, which returns the registers as unsigned numbers
(compiled code: signed 32-bit ones). `examples/kolibri.mpy` wraps the
KolibriOS system functions with them (next section).

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
button"; `3:2` presses button 2, `2:113` the key `q`, `6:x/y/bits[/wheel]`
moves the mouse to (x, y) in the window with those function 37.3 bits; a
test's `tests/typed/<name>.events` file sets them). Behind
`examples/kolibri.mpy` it is a small deterministic KolibriOS written from the
system function documentation: a 1024x768 screen, a fixed clock, a few
processes, a network card, a clipboard, IPC, pipes, a RAM disk `/tmp0/1` with
a few files (other paths are host files); calls that change something are
logged as `[kos] ...`, drawing as `[gui] ...`
(`tests/typed/kos_api_kolibri.mpy` checks both directions):

```sh
X="python3 tests/x86run.py"
FASM="$X /path/to/fasm" RUN="$X" sh tests/run_typed_tests.sh
TARGET=kolibri FASM="$X /path/to/fasm" RUN="$X" sh tests/run_typed_tests.sh
```

`--count-allocs` makes a program report at exit how many heap blocks are
still allocated and how many allocations, `incref` and `decref` calls it made
(a leak check: the count of live blocks must not grow with the work done).

On KolibriOS itself the KolibriOS build of `minipy` compiles the same way,
running `/sys/develop/fasm` with KolibriOS fasm's `infile,outfile,path`
arguments; nothing else has to be installed.

## KolibriOS API: `examples/kolibri.mpy`

`examples/kolibri.mpy` is the KolibriOS system interface written in MiniPy on
top of `sys.syscall`, after the kernel's `docs/sysfuncs.txt`; each wrapper
names its function (`fn 18.20` is function 18, subfunction 20). The same file
works compiled and in the interpreter. It covers windows and drawing (styles,
captions, text in the 6x9/8x16/UTF-8 fonts with scaling and backgrounds,
canvases, numbers, lines, pixels, images with palettes, blitting, window
shapes, moving, z-order, minimizing), events and their mask, buttons,
keyboard (keys decoded, scancode mode, modifiers, hotkeys, layouts,
languages), mouse (positions, buttons, events, wheel, cursors, settings),
screen and video parameters, skins, system colors and fonts, the desktop
background, time and date (BCD decoded) and setting them, uptime, CPU, RAM
and kernel information, processes and threads (`thread_info`, `threads()`,
priorities, killing), memory (heap, shared memory, `load_file`), drivers and
DLLs, the clipboard, IPC, the debug board and debugging, the speaker, PCI,
ports, MSRs, files (fn 70 for ASCII paths, fn 80 with UTF-8 otherwise: read,
write, append, info, attributes, folders with `FileInfo` entries, rename,
delete, symlinks, running programs, the current folder), network devices and
protocols (IP/DNS/gateway, ARP), sockets, futexes and pipes. Left out: what
the documentation marks outdated (18.11, 24.4/24.5, 64) and what needs the
address of machine code (51.1, 68.24).

What makes the two modes agree: register values are normalized with
`_s32()` and masked after shifts; constants with bit 31 set are written
negative (`-0x80000000`), since `0x80000000` is positive in the interpreter
and negative compiled; structures are built in buffers (a `str` cannot hold a
0 byte) and decoded into small classes (`ThreadInfo`, `FileInfo`,
`KernelVersion`, `RamInfo`, `SystemColors`, ...). `tests/typed/kos_decode.mpy`
runs compiled and, as `tests/kolibri_lib_test.mpy`, in the interpreter with
the same expected output.

```python
import kolibri as k

for e in k.list_folder("/sys"):
    print(e.name + (" <dir>" if e.is_folder() else " " + str(e.size)))
print("kernel " + str(k.kernel_version()) + ", " + str(k.free_ram_kb()) + " KB free")
k.clipboard_put_text("hello")
```

`examples/kui.mpy` builds a decorator UI on it (buttons, keys, clicks, double
clicks, the wheel, UTF-8 text); `examples/counter.mpy` and
`examples/files.mpy` - a file browser: a list with keyboard, mouse and wheel
navigation, and a panel with the entry's size, dates, attributes and the
first lines (or bytes) of a file - declare their windows with `@window(...)`.
`tests/typed/files_kolibri.mpy` runs the browser on the emulator's desktop.

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