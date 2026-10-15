# MiniPy

MiniPy is a compact bytecode compiler and stack-based virtual machine for a Python-shaped language, written from scratch in C. It has no external runtime dependencies beyond the standard C library. The project is designed to be portable and can target both host systems (Linux, macOS) and KolibriOS.

## Install

The [releases](https://github.com/c4simba/minipy/releases) carry minipy built
for Linux (x86_64, aarch64), macOS (arm64) and KolibriOS; the latest is always
at the same addresses:

| system | download | |
|---|---|---|
| Debian, Ubuntu | [minipy_amd64.deb](https://github.com/c4simba/minipy/releases/latest/download/minipy_amd64.deb), [minipy_arm64.deb](https://github.com/c4simba/minipy/releases/latest/download/minipy_arm64.deb) | `sudo apt install ./minipy_amd64.deb` |
| Fedora, RHEL, openSUSE | [minipy.x86_64.rpm](https://github.com/c4simba/minipy/releases/latest/download/minipy.x86_64.rpm), [minipy.aarch64.rpm](https://github.com/c4simba/minipy/releases/latest/download/minipy.aarch64.rpm) | `sudo dnf install ./minipy.x86_64.rpm` |
| any Linux | [minipy-linux-x86_64.tar.gz](https://github.com/c4simba/minipy/releases/latest/download/minipy-linux-x86_64.tar.gz), [minipy-linux-aarch64.tar.gz](https://github.com/c4simba/minipy/releases/latest/download/minipy-linux-aarch64.tar.gz) | unpack anywhere: `bin/minipy` |
| macOS | [minipy-macos-arm64.tar.gz](https://github.com/c4simba/minipy/releases/latest/download/minipy-macos-arm64.tar.gz) | unpack anywhere: `bin/minipy` |
| KolibriOS | [minipy-kolibrios.zip](https://github.com/c4simba/minipy/releases/latest/download/minipy-kolibrios.zip) | the programs, `minipy.lib` and `lib/` in one folder |

```sh
curl -LO https://github.com/c4simba/minipy/releases/latest/download/minipy_amd64.deb
sudo apt install ./minipy_amd64.deb      # minipy, minipyc; fasm is suggested
minipy --version
```

From the sources (a C compiler and make):

```sh
make                    # minipy, minipyc, minipy.lib
sudo make install       # PREFIX=/usr/local: bin/minipy, bin/minipyc, lib/minipy/ (DESTDIR stages it)
sudo make uninstall
make deb                # build/minipy_<version>_<arch>.deb   (dpkg-deb)
make rpm                # build/minipy-<version>-1.<arch>.rpm (rpmbuild)
make dist               # build/minipy-<version>-<os>-<arch>.tar.gz
make kolibrios-dist     # build/minipy-<version>-kolibrios.zip (the KolibriOS toolchain: see KolibriOS build)
```

Compiling (`minipy --compile`) needs fasm on the PATH for the `linux` and
`kolibri` targets (`sudo apt install fasm`) and a C compiler for `macos`.
`VERSION=1.2.3` sets the version the programs report and the packages carry.
GitHub Actions (`.github/workflows/build.yml`) builds and tests all of it on
every push; a tag `v1.2.3` publishes the release 1.2.3.

## Pipeline

```text
source code
  → full Python 3.14 parser            (src/py_lex.c, src/py_parse.c: CPython's grammar, its `ast` tree)
  → the compilers' frontend tree       (src/py_front.c)
  → bytecode compiler → stack VM       (src/interp: compile.c, eval.c, ...)
    or the typed compiler → native executable  (src/aot, see Compiled mode)
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
- Built-in `json` (`dumps` with `separators=` / `ensure_ascii=`, `loads`), `minipy` (`endpoint`) and, on host builds, `ctypes` (`lib/ctypes.py` over dlopen)
- Modules are found next to the importing file, then in `MINIPYPATH`, then in `lib/` next to the `minipy` executable (installed: `<prefix>/lib/minipy/lib`)
- Exception objects: `BaseException`, `RuntimeError`, `StopIteration`; user classes derived from them (`class NotFound(Exception)`, `super().__init__(message)`) are raised and caught as themselves
- `try` / `except` / `finally` with protected bytecode regions
- `iter()` / `next()` builtins and `for` loops via iterator objects
- Object protocol hooks: `__len__`, `__add__`, `__eq__`, `__getitem__`, `__setitem__`, `__contains__`, `__iter__`, `__next__`
- Pluggable filesystem backend for imports (host stdio, KolibriOS/newlib, embeddable)

## Limitations

- Default arguments are literal-only
- Closures use shared dictionaries rather than CPython-style cell objects
- Cross-frame exception unwind relies on the recursive VM stack (a future trampoline VM would improve this)

## Source layout

```text
src/
  main.c                    minipy's command line: run a script (--compile runs minipyc)
  py_lex.c py_parse.c       the full Python parser (CPython's grammar, its ast tree)
  py_front.c ast.c ast.h    the frontend tree both modes work on
  util.c fs.c pystdlib.c    memory and text helpers, files and module search, src/stdlib embedded
  inflate.c                 inflating what tools/pack.c deflated into minipy.lib
  stdlib/*.py               library modules written in Python (both modes)
  stdlib/_mpy_pkg/          the package manager (minipy add / sync / check ...)
  platform/                 the host (POSIX) and KolibriOS backends
  interp/                   the interpreter
    interp.h                its objects and API
    compile.c eval.c        bytecode compiler and the eval loop
    call.c attr.c class.c   calls, attributes, classes
    gen.c                   generators, coroutines, async generators
    raise.c traceback.c     exceptions and their tracebacks
    obj.c ops.c             the object model, the collector; operators
    builtins.c format.c bytearray.c
    methods/                the methods of str, bytes, list, dict, set, int, float, object ...
    modules/                import, and the modules written in C (sys, math, json, thread, _ctypes)
  aot/                      the typed compiler, minipyc (minipy --compile runs it)
    aot.h model.h           its interface; the program model (types, modules, functions, classes)
    main.c                  minipyc's command line
    driver.c imports.c      options, the import pass, running fasm / kpack / cc
    preludes.c namespaces.c Python source and classes added to programs that need them
    pkgcheck.c              --compile --check: packages checked for compiled programs
    types/                  type checker: one file per subject (check.h)
    codegen/                code generator: one file per subject (gen.h)
    runtime/*.asm           the runtime routines of compiled programs, by subject
    peep.c                  peephole pass and outlining over the listing
    kpack.c                 LZMA packing of KolibriOS executables
    x2c.c x2c_rt.c          macos: the listing translated to C
tools/pack.c                the build's packer: minipy.lib (Python modules without comments and
                            docstrings, the runtime routines, x2c_rt.c; deflated), kpack
tools/kolibrios-toolchain.sh  the i586-kolibrios cross toolchain (KolibriOS build)
lib/*.py                    modules every program can import: kolibri (KolibriOS), ctypes, fastapi
.github/workflows/build.yml builds, tests and releases (Linux, macOS, KolibriOS)
```

A directory split into many files has one internal header (`run.h`,
`methods/methods.h`, `modules/modules.h`, `aot/driver.h`, `types/check.h`,
`codegen/gen.h`): the state its files share and the functions one file
defines for the others, grouped by that file. A function only its own file
uses stays `static` there. The runtime of compiled programs is the files of
`aot/runtime` joined in the order `RTLIB_ASM` in the Makefile lists them (a
program gets the routines it uses, in that order).

## Host build

### Prerequisites

- A C compiler (GCC or Clang)
- GNU Make

### Build and test

```sh
make          # minipy (the interpreter), minipyc (the compiler), minipy.lib
make test
```

minipy is three files, kept together in one folder (on every system):

```text
minipy       the interpreter (and the package manager)          674 KB  (macOS/arm64)
minipyc      the compiler; `minipy --compile ...` runs it        858 KB
minipy.lib   their data: the standard library written in Python, 892 KB
             the runtime routines of compiled programs
```

`minipy` and `minipyc` find `minipy.lib` next to themselves (symbolic links
resolved) or, installed, in `../lib/minipy` (`MINIPYLIB=path` names another
one), read it the first time they need
something of it, and take only the library of their own build (its id is
compiled into them). Modules only one of them uses are marked so in the file
(`INTERP_ONLY_MODULES` / `COMPILE_ONLY_MODULES` in the Makefile; minipyc says
"only available in the interpreter" for those). The file is the same for
every platform.

`tools/pack.c` (built with the host's C compiler, also for cross builds)
writes it: the modules without comments and docstrings - lines stay where they
were, so tracebacks point at the same lines; a docstring becomes `pass`, so
`__doc__` of the library's own functions and classes is None, as with
`python -OO` - and every module and text deflated on its own; `src/inflate.c`
inflates a module the first time it is imported (the whole library takes
~20 ms). The executables are stripped (`make debug` keeps the symbols). One
6.1 MB program before.

### Run a script

```sh
./minipy tests/test.py
```

### Diagnostic flags

```sh
./minipy --dump-ast tests/statement_ast_test.py       # the parser's tree, as CPython's ast.dump (also --pyast)
./minipy --dump-bytecode tests/statement_ast_test.py
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
./minipy --compile --target linux app.py     # Linux i386 executable ./app
./minipy --compile --target kolibri app.py   # KolibriOS application ./app
./minipy --compile --target macos app.py     # native macOS executable ./app (via C, see below)
./minipy --compile -S app.py                 # only the assembly listing app.asm
```

Without `--target` the program is compiled for the system `minipy` runs on
(`macos` on a Mac, `linux` elsewhere, `kolibri` on KolibriOS).

```text
app.py + every imported module
  -> full Python parser + frontend tree (src/py_parse.c, src/py_front.c; as the interpreter)
  -> type check and inference            (src/aot/types)
  -> one fasm listing app.asm, i386      (src/aot/codegen)
     each function rewritten shorter, repeated call sequences shared  (src/aot/peep.c)
     + the runtime routines it uses      (src/aot/runtime)
  -> fasm -> app   (no linker, no libc, no runtime library to install)
     kolibri: -> LZMA-packed as kpack packs (src/aot/kpack.c); the kernel unpacks it
     macos: -> the listing as C, app.c (src/aot/x2c.c) -> cc -> app
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
emitted, so `print("Hello, world!")` is a 201-byte ELF (472 bytes on
KolibriOS, packed; 552 unpacked, which includes the console connection).

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

On `tests/typed/memory.py` (373 000 allocations) this cuts the increfs from
225 000 to 123 000 and the decrefs from 495 000 to 393 000 (most of the rest
free objects); the code gets a little smaller too. `MPY_NO_BORROW=1`,
`MPY_NO_MOVE=1` and `MPY_NO_CONSUME=1` in the compiler's environment switch
each part off, for comparisons with `--count-allocs`.

### Memory: cycles

Objects that refer to each other - kids and their `parent`, two objects in
each other's dicts, an object holding its own bound method
(`self.on_click = self.click`), a list a closure captures and holds - keep
each other's counts above zero once nothing else refers to them. The types
tell which objects can be in such a cycle at all: the compiler makes a graph
of a node per class, per function whose values are closures and one for
function values, with an edge wherever a field (through lists, dicts, sets
and tuples), a capture or a function value (a closure, a bound method) can
refer to such an object, and finds its strongly connected components. Only
the objects of a node in a cycle there - and the containers and cells that
can hold them - get a mark in their heap block's header when made; a
program without such types links nothing of what follows and runs as
before.

When a marked object loses a reference but lives on, the runtime notes it.
Once 1000 are noted (more when few turn out to be garbage), it looks at
them all - Bacon and Rajan's synchronous cycle collection: the references
among the marked objects they lead to are taken off the counts; whatever
still has a count is referred to from outside and stays, with everything
it leads to; the rest refers only to itself and is freed (the strings,
lists and live objects it refers to are released as usual). `gc.collect()`
runs it at once and returns how many objects it freed. Exception chains
(`__context__`, `__cause__`) do not count: `raise` keeps them chains, as
CPython does. Generators and tasks are not looked into: a cycle through
one stays. On `tests/leaks/cycles.py` (each kind of cycle above, 2000
rounds) 158 000 blocks stayed allocated before; now as many as after 10
rounds.

`MPY_NO_GC=1` in the compiler's environment leaves the cycle collector out;
`MPY_GC_STRESS=1` makes the program collect at every noted object (the test
suite passes that way too); `MPY_GC_TRACE=1` lists the classes and closures
it found. `sh tests/run_leak_tests.sh` compiles each `tests/leaks/*.py`
with `--count-allocs` and checks that the blocks still allocated at the end
do not depend on the number of rounds.

### Constants made once

CPython keeps one object each for the small ints (-5 to 256), the
one-character strings, the empty string, tuple and bytes, and every tuple of
constants in a function. In compiled code numbers, `True`, `False` and `None`
are not objects at all - they live in registers and slots, so no int is ever
allocated. One-character strings (`s[i]`, `chr(n)`, iterating a str) and the
empty string are static objects, and so is a tuple of constants -
`(1, 2, 3)`, `("a", "bb")` in `for x in ("a", "bb")`, `x in (1, 3, 5)`,
nested ones, negative numbers, `None` for an optional item: one object
written into the program instead of a new one built each time the
expression runs (equal ones are the same object, as in CPython; `+=` on a
`tuple[T, ...]` makes a new one instead of extending it in place). A loop
that did `t = (1, 2, 3)`, `for w in ("a", "bb", "ccc")` and
`i % 7 in (1, 3, 5)` made 6 allocations a round; now none.

The interpreter keeps its numbers in its values too, and shares what CPython
shares: the empty str, bytes and tuple, the str of each code point below 256
and the bytes of each byte value are made once (a loop over a text's
characters runs 2.3 times as fast), and a tuple display of constants is one
constant of its code object.

### Code size: templates, shared code, kpack

The generator writes every construct from a small template (a value in eax,
edx:eax for an int, the left operand of an operator pushed, arguments stored
one by one), so where two templates meet the code is longer than it needs to
be. These things shorten it:

- a peephole pass over each function (src/aot/peep.c), on a backward
  liveness analysis of the registers and flags over its jumps: an int
  operand that is a constant or a variable goes straight into the operation
  (`i + 1` is `add eax,1 / adc edx,0 / jo`, not a push, a load and an add
  from the stack), `//` and `%` by a constant are a shift, a mask or one or
  two `div`s instead of the general 64-bit division, arguments made only of
  loads are pushed, a stored value is not loaded back, jumps to jumps and to
  the next line go, and so do results nobody reads;
- shared code: `obj.name` on a possible None jumps to one `NA<k>` per
  attribute name, and the message is made when it happens
  (`rt_attrerr_none`); `self` is not checked at all inside a method that
  only ever runs on an object; a list display appends through
  `rt_list_add32`; the same instructions before the same call, wherever they
  recur, become one routine `OL<k>` that ends in a jump to the callee;
- less of the standard library: the code pages, utf-16 and utf-32 of
  `_codecs_more.py` (17 KB) come in only when the program's code - not its
  comments - names one of them in a string or gives an encoding that comes
  from elsewhere to what takes one (`s.encode(enc)`, `open(f, encoding=x)`);
- kolibri: the executable is packed as KolibriOS's kpack packs one (LZMA
  with its call/jump preprocessing; src/aot/kpack.c), and the kernel unpacks
  it as it loads it. `--no-pack` leaves it as fasm wrote it.

The desktop of Nectar (github.com/c4simba/nectar; then still in Windows XP's look) went from 470 835 bytes to 315 169, and
93 600 packed; its start menu alone from 381 275 to 254 897, and 78 561
packed. 22 KB of each drop are the fonts' glyphs, now stored at 1 bit a
pixel where they are not anti-aliased; packed, that makes no difference
(LZMA had squeezed the old one-letter-a-pixel strings as well).

`MPY_NO_PEEP=1` (all of the peephole pass), `MPY_NO_OUTLINE=1` and
`MPY_SELF_CHECKS=1` in the compiler's environment switch them off; `MPY_PEEP_STATS=1` counts the rewrites, and
`MPY_PEEP_MAX=n` stops after the first n (to find a wrong one by bisection).

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
- a dict keeps its entries in insertion order, as CPython's: an array of
  them and an index of hashes; `del d[k]` and `d.pop(k)` leave a hole in
  the array (constant time) that goes when the table fills up or something
  walks the entries in order (a loop, `keys()`, `==`, printing);
- `int`/`bool` values are widened where a `float` is expected; a subclass
  instance is accepted where its base class is expected;
- `None` is the zero value of the type it meets: `0`, `0.0`, `""`, a null
  object, and a fresh empty container for `list`/`dict`/`set`; `x is None`
  tests for that zero/empty value;
- unannotated parameters and empty literals get their types from how they are
  used; whatever stays unknown must be annotated (`cannot infer the type of
  parameter 'v'; add a type annotation`). A module function that the program
  never calls and whose parameter types are therefore unknown is left out
  instead (libraries such as `lib/kolibri.py` keep working).

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
`kui`, a KolibriOS UI toolkit (in [c4simba/nectar](https://github.com/c4simba/nectar)), turns a paint function into the
window itself:

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

A module is looked for next to the importing file, then in the packages of
the program's project (`.minipy/site`, see [Packages](#packages-from-pypi)),
then in each folder of `MINIPYPATH` (separated by `:` or `;`), then in `lib/`
next to the `minipy` executable, then in the main program's folder (so the
modules of a project's packages import its top-level modules, as with
CPython's `sys.path[0]`) - the interpreter does the same. A module is
`name.py` or `name/__init__.py` (plain Python files: the same code runs in
CPython), so `minipy --compile main.py` builds `main`. A module's names are
also the ones it imports: `from .config import Item`, where `config` has
`Item` from `kui.item`, works in whatever order the modules come:

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
a value. The interpreter has the same API on host builds (`lib/ctypes.py`
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
`sys.addr(buf)` build the structures system calls take (`size`: 1 to 4
bytes, little-endian); `sys.peek_at(address,
size)`, `sys.poke_at(address, value, size)`, `sys.peek_str_at(address, n)`,
`sys.poke_str_at(address, s)` and `sys.cstr_at(address, max)` (a
zero-terminated string) read and write memory the system hands out by address.
`sys.exit(code)` ends the program. The same functions exist in the KolibriOS
build of the interpreter, which returns the registers as unsigned numbers
(compiled code: signed 32-bit ones). `lib/kolibri.py` wraps the
KolibriOS system functions with them (next section).

### Building and testing

The linux and kolibri targets need [fasm](https://flatassembler.net) (1.7x).
Their programs are i386: they run on x86/x86-64 Linux (no 32-bit libraries
needed) and on KolibriOS. The macos target needs only a C compiler.

```sh
make                  # minipy (the interpreter) and minipyc (the compiler)
make test-typed       # compile tests/typed/*.py, run them, compare with tests/typed/expected
                      # (on a Mac natively: TARGET=macos; elsewhere TARGET=linux)
make test-leaks       # tests/leaks/*.py: nothing stays allocated that the work left behind
make test-pkg         # the package manager on a package index of its own (no network)
```

`--fasm`, `--fasm-args` (or `MPY_FASM`, `MPY_FASM_ARGS`) choose the
assembler; `--stack` sets a KolibriOS application's stack size and
`--no-pack` keeps it unpacked. The
`tests/typed/err_*.py` tests check the compiler's diagnostics; most of the
other expected outputs are byte-for-byte CPython's.

### Speed against CPython

`make bench` (`python3 tests/bench/run.py [--interp] [name ...]`) compiles
the programs of `tests/bench/` - plain Python, a few seconds each in CPython -
and times them both ways (the best of three runs; their outputs must match):

| benchmark | what it does | CPython 3.14 | compiled | |
|---|---|---|---|---|
| `loops` | Collatz sequences: while, `%`, `//`, `*` on ints | 3.54 s | 0.23 s | 15.3x faster |
| `power` | modular exponentiation by squaring, `i ** 3`, `x ** 1.5` | 2.87 s | 0.92 s | 3.1x faster |
| `floats` | the Mandelbrot set, an integral of `sin(x) * sqrt(x)` | 1.99 s | 0.44 s | 4.6x faster |
| `primes` | a sieve over a list, the gaps between primes in a dict | 2.98 s | 1.18 s | 2.5x faster |
| `listcomp` | comprehensions: squares, a filter, tuples taken apart, strings, nested and flattening ones | 2.50 s | 0.74 s | 3.4x faster |
| `dicts` | int and str keys: filling, `in`, `[]`, `get`, counting, `items()`, `values()`, `del`, a comprehension | 2.27 s | 1.30 s | 1.7x faster |
| `trees` | trees whose nodes know their parent and kids: built, walked both ways, dropped (garbage cycles) | 1.73 s | 1.16 s | 1.5x faster |
| `clock` | `time.time()`, `monotonic()`, `perf_counter()`; `datetime.now()`, `localtime()`, `strftime()` | 2.25 s | 2.59 s | 1.2x slower |

(an Apple M-series Mac, the macos target, 2026-10-13.) `loops` owes much to
`n % 2` and `n // 2` being a mask and a shift. `dicts` took 195 s before
`del` left a hole instead of moving every later entry down and making the
index again (it was the 12 500 deletions of each round); in `trees` the cycle
collector frees every tree a round leaves behind - without it the program
ran in 0.5 s and kept 18 million blocks. In `clock` the clock calls
cost what CPython's do (50 to 100 ns against 58); the calendar is
CPython's C code against minipy's `src/stdlib/datetime.py` and `time.py`,
compiled: `datetime.now()`, `localtime()` and `time.strftime()` take about
twice CPython's time, `datetime.strftime()` about the same. (They took 3
times as long before `strftime` read its format once instead of on every
call, the clocks stopped making a (seconds, nanoseconds) pair and a buffer
on each call and division by a constant stopped being the general 64-bit
one: `clock` took 5.15 s.) On a Mac the macos target is i386 code
translated to C (registers and flags kept in C variables), and these times
include that translation's cost - the more so the bigger the program, which
is one C function: the same operation costs about twice as much once
`datetime` is imported as in a small program. On x86 (Linux, KolibriOS) the
same programs run as the i386 code itself. `--interp` adds minipy's
interpreter: as fast as CPython on `listcomp`, 1.1 to 1.9 times its time on
the other computing ones, 4.9 times on `trees` (an attribute is a dict
lookup) and 14 times on `clock`. It got there by `for a, b in enumerate(xs)`
(and `zip(xs, ys)`, `d.items()`) not making a tuple per round, small
objects' memory reused per size class, one walk of the heap per collection
with the objects sorted by a radix sort instead of `qsort`, and the eval
loop's `setjmp` no longer saving the signal mask with a system call on
macOS - before, `listcomp` took 12.6 s, `trees` 21.8 s and `clock` 61 s.

Without an x86 Linux (an arm64 Mac, say), `tests/x86run.py` runs the i386
programs - and the Linux fasm itself - in a small emulator built on
[unicorn](https://www.unicorn-engine.org) (`pip install unicorn`). It also runs
KolibriOS applications (packed ones unpacked as the kernel does): it plays the
shell's side of the console, maps system function 70 to host files, and fakes
a headless desktop (window calls are
logged; events come from `X86RUN_EVENTS`, by default "redraw, then the close
button"; `3:2` presses button 2, `2:113` the key `q`, `6:x/y/bits[/wheel]`
moves the mouse to (x, y) in the window with those function 37.3 bits; a
test's `tests/typed/<name>.events` file sets them). Behind
`lib/kolibri.py` it is a small deterministic KolibriOS written from the
system function documentation: a 1024x768 screen, a fixed clock, a few
processes, a network card, a clipboard, IPC, pipes, a RAM disk `/tmp0/1` with
a few files (other paths are host files); calls that change something are
logged as `[kos] ...`, drawing as `[gui] ...`
(`tests/typed/kos_api_kolibri.py` checks both directions):

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
of assembling the listing, translates it into C (`src/aot/x2c.c`, a static
recompiler for the fasm subset the compiler emits) that the system C compiler
builds into a native executable - arm64 on Apple silicon, no Rosetta, no fasm:

```sh
./minipy --compile hello.py && ./hello       # on a Mac: hello.asm, hello.c, ./hello
```

The C program keeps the i386 machine the listing was written for
(`src/aot/x2c_rt.c`, at the start of every such C file): 32-bit registers
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

## Packages from PyPI

```sh
minipy add requests 'attrs>=23'   # into pyproject.toml, then lock, download, check
minipy remove attrs
minipy lock                       # choose the versions again: pylock.toml
minipy sync                       # what pylock.toml names: into the store and .minipy/site, then the check
minipy check [PACKAGE...]         # the check again, with the full report
minipy list                       # the packages and where they can be used
minipy vendor                     # .minipy/site as copies: a project to take elsewhere (KolibriOS)
```

A project is the folder of its `pyproject.toml` (`minipy add` makes one):
its `[project] dependencies` (PEP 621) say what it needs, `pylock.toml`
(PEP 751) pins every package to a version and to one wheel with its sha256.
There is no virtual environment. A wheel is downloaded once per machine,
checked against the lock file's hash and unpacked into the store
(`~/.minipy/store`, or `$MINIPY_HOME/store`); a project links to it from
`.minipy/site`, which programs inside the project import from. Nothing of a
package runs while it is installed: only pure-Python wheels (`py3-none-any`)
are used - no C extension modules, no builds from source. Environment markers
see `implementation_name == "minipy"` and `python_version == "3.14"`, so the
pure-Python fallbacks of packages with C speedups are chosen. The index is
PyPI's JSON API (`--index URL`, `[tool.minipy] index-url` or
`MINIPY_INDEX_URL`, also a folder laid out like it: `file://...`);
downloads go through `curl`.

Every package goes through the **check for compiled programs**: the compiler
runs over all of its modules (`minipy --compile --check MODULE...`) and,
unlike a compilation, goes on after an error, so it reports everything it
cannot compile - a `Union` other than `T | None`, `Any`, an unknown type, a
dynamic feature - and, as *unclear*, every function it cannot check: one
nothing calls whose parameters have no types. Before that each module is
compiled for the interpreter; source minipy cannot run at all (an int literal
beyond 64 bits ...) marks the package as not running in minipy. A module
passes when its file has no error and nothing unclear and every module it
imports passes too:

```text
checking 2 packages for compiled programs...
  okpkg 1.2: passes the check - usable in compiled programs
  unionpkg 1.0: does not pass the check - interpreter only
    unionpkg/__init__.py:4: error: only Union[T, None] (Optional[T]) is supported in compiled code: one type per value
```

The interpreter runs every package. A compiled program may import only the
modules that passed (`.minipy/index`; the full reports in `.minipy/check/`):

```text
app.py:1: error: module 'unionpkg' (package unionpkg 1.0) did not pass the check for compiled programs:
see .minipy/check/unionpkg-1.0.txt (`minipy check unionpkg`)
```

`pyproject.toml` and `pylock.toml` belong to the project's sources;
`.minipy/` is made again by `minipy sync`.

## FastAPI

`lib/fastapi.py` and `lib/uvicorn.py` are FastAPI's routing API and a
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

`lib/uvicorn.py` picks its socket constants and structures by
`sys.platform` (Linux or macOS). `tests/fastapi_native.sh` compiles the
example for the machine it runs on, serves it on port 8000 and checks a few
answers with `curl` - then the same with the interpreter.

Path parameters (`{name}` in the path) and query parameters are `int`,
`float`, `bool` or `str`, with defaults; endpoints may be `async def`; results
are written as JSON (dicts and records, lists, numbers, strings, `None`,
objects as their fields); `HTTPException(status_code, detail)`; unknown paths
give 404, wrong methods 405, missing or invalid parameters 422 with FastAPI's
error details. `tests/typed/fastapi_handle.py` checks the answers against
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

## KolibriOS API: `lib/kolibri.py`

`lib/kolibri.py` is the KolibriOS system interface written in MiniPy on
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
`KernelVersion`, `RamInfo`, `SystemColors`, ...). `tests/typed/kos_decode.py`
runs compiled and, as `tests/kolibri_lib_test.py`, in the interpreter with
the same expected output.

```python
import kolibri as k

for e in k.list_folder("/sys"):
    print(e.name + (" <dir>" if e.is_folder() else " " + str(e.size)))
print("kernel " + str(k.kernel_version()) + ", " + str(k.free_ram_kb()) + " KB free")
k.clipboard_put_text("hello")
```

It is in `lib/`, next to the programs (installed: `<prefix>/lib/minipy/lib`),
so every program imports it as it is. Programs for KolibriOS with a user
interface - `kui`, a UI toolkit on it, the graphics library `kgfx`, PNG
decoding, KolibriOS's own fonts, and Nectar, a whole desktop made of them -
are in a repository of their own: [c4simba/nectar](https://github.com/c4simba/nectar).

## KolibriOS build

minipy and minipyc for KolibriOS are built with the `i586-kolibrios` GCC
cross toolchain of [KolibriOS's ports](https://git.kolibrios.org/KolibriOS/ports)
(binutils, GCC 13, newlib). `tools/kolibrios-toolchain.sh` builds it from that
repository's sources (a quarter of an hour; a C compiler, make, curl, bison,
flex, texinfo and the gmp / mpfr / mpc development files):

```sh
sh tools/kolibrios-toolchain.sh $HOME/kos-toolchain
make kolibrios KOS_PORTS=1 KOS32_SDK=$HOME/kos-toolchain       # build/kolibri/minipy, minipyc, minipy.lib
make kolibrios-dist KOS_PORTS=1 KOS32_SDK=$HOME/kos-toolchain  # build/minipy-<version>-kolibrios.zip
```

The CI does the same (the toolchain kept between runs) and puts
`minipy-kolibrios.zip` into every release.

`KOS_PORTS=1` takes newlib as the toolchain's one static `libc.a` (with
`libm.a`; `src/platform/kolibri/ports_libc.c` gives the two things it leaves
to the program). Without it the Makefile expects an SDK with newlib as
`libc.dll` (the layout under `/home/autobuild/tools/win32`):

```sh
make kolibrios KOS32_PREFIX=/home/autobuild/tools/win32          # sdk/bin/i586-kolibrios-gcc, ...
make kolibrios KOS32_SDK=/path/to/sdk KOS_SDK_LIBDIR=/path/to/newlib/libc
make kolibrios KOS_LIBS="-lc -lc.dll -ldll -lgcc" KOS_CRT=/path/to/crt/pseudo-reloc.o
```

Both link with `kos-app-fix.lds` (the toolchain's `kos-app.lds` with a 1 MB
stack and the `.idata` sections kept); the compiler driver turns the result
into a KolibriOS program (`objcopy -O binary`).

### Output

```text
build/kolibri/minipy        the interpreter      } kpacked: LZMA, unpacked by the kernel
build/kolibri/minipyc       the compiler         } as it loads them (KOS_KPACK=0: not)
build/kolibri/minipy.lib    their data (the same file as the host build's)
build/kolibri/minipy.map    (and minipyc.map)
```

Put the three files (and `lib/`) into one folder. The programs are packed
the way minipyc packs the KolibriOS programs it compiles (`src/aot/kpack.c`,
the format of the system's kpack). Both use the system's SHELL as their
console - through a ring buffer in the `{PID}-SHELL` shared memory
(`src/platform/kolibri/console.c`) - and minipyc runs `/sys/develop/fasm`.
Started without a script, the interpreter runs `main.py` of `KOS_IMPORT_DIR`
(`make kolibrios KOS_IMPORT_DIR=...`, default `/hd0/1/import_path`).

## Filesystem backend

Filesystem access is abstracted behind `src/fs.c` with swappable backends:

| File | Purpose |
|------|---------|
| `src/fs.c` | Common path/module helpers and backend dispatch |
| `src/platform/host/fs_host.c` | Default stdio backend for host builds |
| `src/platform/kolibri/fs_kolibri.c` | KolibriOS/newlib backend (enabled with `MPY_FS_KOLIBRI`) |

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
import utils        # from /rd/1/minipy/tests/test.py → /rd/1/minipy/tests/utils.py
import pkg.tools    # → pkg/tools.py relative to the importer directory
```

If a relative path cannot be opened and `MPY_FS_DEFAULT_IMPORT_DIR` is set, the FS layer retries from that fixed directory. The KolibriOS Makefile sets this from `KOS_IMPORT_DIR`.

### Adding a new backend

To target an embedded platform, add a new backend file implementing `mpy_fs_backend_read_file()` backed by ROM, flash, an RTOS VFS, or a fixed manifest. The compiler and VM require no changes.