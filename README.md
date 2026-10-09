# MiniPy

MiniPy is a compact bytecode compiler and stack-based virtual machine for a Python-shaped language, written from scratch in C. It has no external runtime dependencies beyond the standard C library. The project is designed to be portable and can target both host systems (Linux, macOS) and KolibriOS.

## Pipeline

```text
source code
  → full Python 3.14 parser            (src/py_lex.c, src/py_parse.c: CPython's grammar, its `ast` tree)
  → the compilers' frontend tree       (src/py_front.c)
  → bytecode compiler → stack VM       (src/compiler.c, src/expr_compiler.c, src/vm*.c)
    or the typed compiler → native executable  (src/aot_*.c, see Compiled mode)
```

The parser reads all of Python 3.14 the way CPython does - precedence,
f-strings (conversions, format specs, `{x=}`), implicit string concatenation,
`;`, parenthesized imports, keyword-only parameters, decorators, every
statement - so both modes see a program exactly as CPython would. What a mode
does not run (`match`, `x = yield`, `:=`, starred assignment, ...) is a
compile error at its line.

## Features

- `global` / `nonlocal` declarations backed by shared closure dictionaries
- Function default arguments (literal defaults)
- Decorators: `@name`, `@obj.attr`, factories with arguments (`@win.button("OK", x=10)`), stacked
- Python's line joining: statements continue inside brackets and after a `\`; triple-quoted strings may span lines; adjacent string literals are joined
- `sorted` / `min` / `max` / `list.sort` with `key=` and `reverse=`; `from typing import ...` and annotated attributes (`self.items: list[str] = []`) are accepted (annotations are not evaluated)
- `print(a, b, sep=..., end=...)`; bytes literals (`b"..."`, the same bytes as a `str`); `match` / `case` are soft keywords
- `async def` / `await`: a coroutine runs when it is called (`await x` is `x`); `asyncio.run`, `sleep`, `gather`, `create_task`
- Built-in `json` (`dumps` with `separators=` / `ensure_ascii=`, `loads`), `minipy` (`endpoint`) and, on host builds, `ctypes` (`lib/ctypes.mpy` over dlopen)
- Modules are found next to the importing file, then in `MINIPYPATH`, then in `lib/` next to the `minipy` executable (`.mpy`, then `.py`)
- Exception objects: `BaseException`, `RuntimeError`, `StopIteration`; user classes derived from them (`class NotFound(Exception)`, `super().__init__(message)`) are raised and caught as themselves
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
./minipy --dump-ast tests/statement_ast_test.mpy       # the parser's tree, as CPython's ast.dump (also --pyast)
./minipy --dump-bytecode tests/statement_ast_test.mpy
./minipy --fs-info
```

`tests/pyast_check.py` compares `--pyast` with CPython's own `ast` over whole
files or directories (the standard library, site-packages).

### Compile to a native executable

See [Compiled mode](#compiled-mode-ahead-of-time-fasm) below: statically
typed programs become small i386 executables for Linux or KolibriOS, or
native macOS executables.

## Compiled mode (ahead-of-time, fasm)

Besides interpreting, `minipy` compiles **statically typed** programs to small
native executables:

```sh
./minipy --compile --target linux app.mpy     # Linux i386 executable ./app
./minipy --compile --target kolibri app.mpy   # KolibriOS application ./app
./minipy --compile --target macos app.mpy     # native macOS executable ./app (via C, see below)
./minipy --compile -S app.mpy                 # only the assembly listing app.asm
```

Without `--target` the program is compiled for the system `minipy` runs on
(`macos` on a Mac, `linux` elsewhere, `kolibri` on KolibriOS).

```text
app.mpy + every imported module
  -> full Python parser + frontend tree (src/py_parse.c, src/py_front.c; as the interpreter)
  -> type check and inference            (src/aot_types.c)
  -> one fasm listing app.asm, i386      (src/aot_codegen.c)
     + the runtime routines it uses      (src/aot_rtlib.asm)
  -> fasm -> app   (no linker, no libc, no runtime library to install)
     macos: -> the listing as C, app.c (src/aot_x2c.c) -> cc -> app
```

The listing names its labels after the program: `main.read_item` (a
function), `fastapi.FastAPI.get` (a method), `main.read_root.endpoint` (the
adapter of an endpoint), `main.app` (a global), `kolibri.Window.vtable`;
equal names - methods of the same name in different modules' classes of the
same name, say - get `_2`, `_3`, ... appended. Labels of compiler temporaries
stay short (`L12`, `S3` strings, `FC0` float constants).

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

- a dict literal with str-literal keys whose values have types no one type
  covers (`{"item_id": 5, "q": "x"}`) is a *record*: a dict of fixed keys,
  indexed by key literals (`r["q"]`), printed, compared and written as JSON
  like a dict (not iterated, not extended); a function returns one shape;
- `Optional[str]` (`str | None`, `Union[str, None]`) is a `str` whose `None`
  prints as `None` and is written as `null` by `json.dumps` (a plain `str`'s
  zero value is `""`);
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

`sys` (below), `asyncio`, `json` (`json.dumps(value, separators=..., ensure_ascii=...)`:
written by type at compile time, CPython's output, objects as their fields),
`ctypes` (next section), `minipy` (`Endpoint`, `endpoint`: see FastAPI), `math` (`sqrt sin cos tan asin acos atan atan2 exp
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
`if`, loop or `try`, and not after other statements. The one exception is an
`if __name__ == "__main__":` block at the top level: in the program's main
module it runs (its imports join the module's others), in an imported module
it is left out.

A module is looked for next to the importing file, then in each folder of
`MINIPYPATH` (separated by `:` or `;`), then in `lib/` next to the `minipy`
executable - the interpreter does the same. `name.mpy`, `name/__init__.mpy`,
`name.py` and `name/__init__.py` are accepted, so `minipy --compile main.py`
builds `main`:

```sh
MINIPYPATH=$HOME/mylibs:/opt/shared ./minipy --compile app.py
```

### C libraries: `ctypes`

On the Linux and macOS targets a program calls C functions the way CPython's
`ctypes` does. For Linux `minipy --compile` makes a dynamically linked
executable (`/lib/ld-linux.so.2` loads the libraries and fills in the
functions' addresses; `fflush(NULL)` runs at exit); on macOS each call is a
native call with the signature its declarations give (see [macOS](#macos-target-the-listing-as-c)):

```python
import ctypes

libc = ctypes.CDLL("libc.so.6")              # a module-level name, a literal library name
libc.strlen.restype = ctypes.c_size_t        # declarations: module-level statements
libc.strlen.argtypes = [ctypes.c_char_p]
getenv = libc.getenv                         # an alias shares them
getenv.restype = ctypes.c_char_p             # a char * result is copied into a str (NULL: None)
libm = ctypes.CDLL("libm.so.6")
libm.pow.restype = ctypes.c_double

buf = ctypes.create_string_buffer(64)
libc.snprintf(buf, 64, b"%d %s", 7, "seven")
print(libc.strlen("hello"), getenv("HOME"), ctypes.string_at(ctypes.addressof(buf)), libm.pow(2.0, 10.0))
```

Calls are cdecl with the stack aligned to 16 bytes. Arguments: `int`/`bool`
(`c_int` without argtypes), `float` (`c_double`), `str` / bytes and buffers
(their bytes, NUL-terminated), `None` (NULL); with argtypes `c_int c_uint
c_long c_size_t c_ssize_t c_short c_byte c_char c_bool c_longlong c_double
c_float c_char_p c_void_p` (and the `c_intN` / `c_uintN` names). Results as
the restype says (`c_int` by default, `None` for void). Also
`create_string_buffer`, `string_at`, `addressof` and `get_errno`. Not
supported: callbacks (C calling Python), structures and pointers as ctypes
objects (use buffers and `sys.peek`/`sys.poke`), passing a library around as
a value. The interpreter has the same API on host builds (`lib/ctypes.mpy`
over dlopen; up to 8 integer/pointer arguments, variadic functions need
argtypes for their fixed arguments, as CPython on arm64 Macs).

### System calls: the `sys` module

`sys.platform` is a constant (`"linux"` / `"kolibrios"` / `"darwin"`; the
interpreter gives its host's, as CPython does).
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

The linux and kolibri targets need [fasm](https://flatassembler.net) (1.7x).
Their programs are i386: they run on x86/x86-64 Linux (no 32-bit libraries
needed) and on KolibriOS. The macos target needs only a C compiler.

```sh
make                  # minipy (interpreter + compiler)
make test-typed       # compile tests/typed/*.mpy, run them, compare with tests/typed/expected
                      # (on a Mac natively: TARGET=macos; elsewhere TARGET=linux)
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

Dynamically linked programs (ctypes) run under `tests/x86run.py` too: their
imports point at a small fake C library written in Python (string, stdio,
`snprintf`, `getenv`, `open`, errno, sockets). Its sockets serve scripted HTTP
clients - `X86RUN_REQUESTS` names a file of requests, one per line (`GET
/items/5?q=x`), a test's `tests/typed/<name>.requests` sets it - and print
each answer. `tests/glibc_docker.sh` runs the ctypes test and the FastAPI
example against a real i386 glibc in a Docker image (`IMAGE=`, e.g. Ubuntu with
`libc6-i386`).

`--count-allocs` makes a program report at exit how many heap blocks are
still allocated and how many allocations, `incref` and `decref` calls it made
(a leak check: the count of live blocks must not grow with the work done).

On KolibriOS itself the KolibriOS build of `minipy` compiles the same way,
running `/sys/develop/fasm` with KolibriOS fasm's `infile,outfile,path`
arguments; nothing else has to be installed.

### macOS target: the listing as C

`--target macos` compiles the program exactly as for Linux and then, instead
of assembling the listing, translates it into C (`src/aot_x2c.c`, a static
recompiler for the fasm subset the compiler emits) that the system C compiler
builds into a native executable - arm64 on Apple silicon, no Rosetta, no fasm:

```sh
./minipy --compile hello.mpy && ./hello       # on a Mac: hello.asm, hello.c, ./hello
```

The C program keeps the i386 machine the listing was written for
(`src/aot_x2c_rt.c`, at the start of every such C file): 32-bit registers
and flags (kept lazily: what the last instruction compared, so `cmp` + `jl`
becomes a C comparison), the x87 stack in double precision (which compiled
code selects anyway), and a 4 GiB guest address space reserved in one
mapping. Every instruction becomes a few C statements in one function;
jumps and calls to labels are `goto`s, and the labels whose addresses are
used as values (return addresses, function values, exception handlers) are
reached through a computed-goto table. Linux system calls (`int 0x80`) run on
macOS's (`read`, `write`, `open` with Linux's flags, `mmap2` from the guest
space, `gettimeofday`, `nanosleep`, ...), so `sys.syscall` with Linux numbers
works too. The runtime routines that compute in the x87's 64-bit precision -
float formatting and parsing, `exp`, `**` - are C functions there.

ctypes calls are native: each call site carries its C signature, the
function is looked up with `dlsym` when first called (`libc.so.6`,
`libm.so.6`, ... are the system's C library; `libfoo.so.N` is tried as
`libfoo.dylib`), and arguments are converted to C types - `c_long`,
`c_size_t`, `c_ssize_t` are 64-bit there, the variadic functions of the C
library (`printf`, `snprintf`, `open`, ...) get their variadic arguments the
arm64 way even without `argtypes`. Pointers are guest addresses (C sees the
host address of the same bytes); a `char *` result outside the guest memory
is copied into it, another pointer becomes a handle the program can pass
back.

`-O1` builds a program in a second or two; `--cc`, `--cc-args` (or `MPY_CC`,
`MPY_CC_ARGS`, default `-O1 -w {in} -o {out} -lm`) choose the compiler and
its arguments. `TARGET=macos sh tests/run_typed_tests.sh` runs the typed
tests natively (the `*_linux` ones too, except those that need the scripted
HTTP clients of `tests/x86run.py`); `tests/typed/expected/<name>.<target>.out`
overrides a test's expected output for one target (`sys_linux.macos.out`:
`darwin`).

## FastAPI

`lib/fastapi.mpy` and `lib/uvicorn.mpy` are FastAPI's routing API and a
uvicorn-like HTTP/1.1 server written in MiniPy (the server over libc sockets
through ctypes). FastAPI's own hello world compiles unchanged:

```python
from typing import Union

from fastapi import FastAPI

app = FastAPI()


@app.get("/")
def read_root():
    return {"Hello": "World"}


@app.get("/items/{item_id}")
def read_item(item_id: int, q: Union[str, None] = None):
    return {"item_id": item_id, "q": q}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
```

```sh
./minipy --compile --target linux examples/fastapi/main.py   # a 28 KB i386 Linux executable
./minipy --compile examples/fastapi/main.py                  # on a Mac: a native macOS one
./main                                          # or: ./minipy examples/fastapi/main.py (interpreter, host)
curl 'http://localhost:8000/items/5?q=somequery'   # {"item_id":5,"q":"somequery"}
```

`lib/uvicorn.mpy` picks its socket constants and structures by
`sys.platform` (Linux or macOS). `tests/fastapi_native.sh` compiles the
example for the machine it runs on, serves it on port 8000 and checks a few
answers with `curl` - then the same with the interpreter.

Path parameters (`{name}` in the path) and query parameters are `int`,
`float`, `bool` or `str`, with defaults; endpoints may be `async def`; results
are written as JSON (dicts and records, lists, numbers, strings, `None`,
objects as their fields); `HTTPException(status_code, detail)`; unknown paths
give 404, wrong methods 405, missing or invalid parameters 422 with FastAPI's
error details. `tests/typed/fastapi_handle.mpy` checks the answers against
real FastAPI's (its TestClient on the same app): equal status codes and bodies,
compiled for Linux and KolibriOS and in the interpreter. Not supported:
request bodies (pydantic models), dependencies, response models, OpenAPI /
`/docs`, middleware; the server handles one connection at a time.

How it works: `app.get(path)` returns a `Callable[[Endpoint], Endpoint]`, and
passing a function where a `minipy.Endpoint` is expected makes the compiler
write an adapter for it in Python - parameters converted from a dict of
strings by their annotations, the function called (an async one directly: the
server runs in a task), the result `json.dumps`ed - so every route has one
type. In the interpreter `minipy.endpoint(f)` does the same at run time.

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
mpy_fs_find_module(importer_dir, module_name)   /* + MINIPYPATH and <minipy>/lib */
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