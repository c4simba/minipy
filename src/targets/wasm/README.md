# wasm: the interpreter in a browser

The website's "Try it" terminal is plainpy's interpreter compiled to
WebAssembly with Emscripten - the same C as on Linux (the POSIX backend in
`../posix` on Emscripten's C library) with its standard library,
`plainpy.lib`, in Emscripten's in-memory file system.

```sh
make wasm          # build/wasm/plainpy.js, plainpy.wasm, plainpy.data (plainpy.lib and lib/)
make wasm-node     # build/wasm/node/plainpy: the same interpreter as a command for node
PLAINPY=build/wasm/node/plainpy sh tests/run_tests.sh    # the interpreter's tests on it
```

(`EMCC=...` names emcc; Emscripten 6.0.12 is what the website is built with:
`.github/workflows/pages.yml`.)

- [web.c](web.c) - the page's entry: `ppy_web_start()` once, then
  `ppy_web_run(code)` for each piece the user types, run in one `__main__`
  as Python's REPL runs it: an expression's value printed (and kept in `_`),
  an exception's traceback printed, `2` back when the piece is not complete
  yet (a block, a triple-quoted string).
- `../../site/try-worker.js` runs it in a Web Worker - a long loop does not
  stop the page, Stop ends it - and sends the output as it comes;
  `../../site/try.js` is the terminal.
- [node_pre.js](node_pre.js) - where the node command finds its data.

What differs from the interpreter on a computer: no threads (a page on
GitHub Pages cannot have SharedArrayBuffer), no `input()` from the
keyboard (a worker cannot wait for the page), no sockets, processes or C
libraries; files live in memory for the session. `machine` works with the
simulated board.

The interpreter's collector also looks for objects on the C stack;
WebAssembly keeps locals outside memory, where it cannot see them. So in
WebAssembly it collects only between two pieces of the REPL, when no C code
holds an object (`mp_gc_safe_point` in `src/interp/obj.c`): what one piece
throws away stays until it ends. (binaryen's `--spill-pointers` would make
every moment safe, but its bigger frames let the browser's stack hold only
about a hundred Python calls instead of some five hundred.)
