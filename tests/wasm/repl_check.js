// The REPL of the website's terminal (src/targets/wasm/web.c), driven as
// site/try-worker.js drives it, on the web build: node tests/wasm/repl_check.js
// (after make wasm). Prints each piece, its status and what it printed;
// exits 1 when one is not what it should be.
"use strict";
const path = require("path");
const dir = path.join(__dirname, "..", "..", "build", "wasm");
const createPlainpy = require(path.join(dir, "plainpy.js"));
let out = "";
const sink = (c) => { if (c !== null) out += String.fromCharCode(c); };
createPlainpy({ locateFile: (f) => path.join(dir, f), preRun: [(M) => M.FS.init(() => null, sink, sink)] }).then((M) => {
  M.ccall("ppy_web_start", "number", [], []);
  const cases = [
    ["1 + 2", 0, "3\n"],
    ["x = [n * n for n in range(5)]", 0, ""],
    ["x", 0, "[0, 1, 4, 9, 16]\n"],
    ["_ + [25]", 0, "[0, 1, 4, 9, 16, 25]\n"],
    ["None", 0, ""],
    ["def f(n):", 2, ""],
    ["def f(n):\n    return n * 3", 0, ""],
    ["f(14)", 0, "42\n"],
    ['s = """a', 2, ""],
    ["{'a': 1}['b']", 1, "KeyError: 'b'"],
    ["x[0]", 0, "0\n"],
    ["import json\nprint(json.dumps({'k': [1, 2.5, None]}))", 0, '{"k": [1, 2.5, null]}\n'],
    ["from machine import Pin\nPin(2, Pin.OUT).on()", 0, "[pin 2] out\n[pin 2] = 1\n"],
    ["def r(n):\n    return r(n + 1)\n\nr(0)", 1, "RecursionError"],
    ["input('name? ')", 1, "EOFError"],
    ["import sys\nsys.exit(3)", 0, "sys.exit()"],
    ["print('still here', sum(range(10)))", 0, "still here 45\n"],
    ["for i in range(3):\n    print(i)\n", 0, "0\n1\n2\n"],
  ];
  let bad = 0;
  for (const [code, want, text] of cases) {
    out = "";
    const rc = M.ccall("ppy_web_run", "number", ["string"], [code]);
    const ok = rc === want && (text === "" ? out === "" : out.includes(text));
    if (!ok) bad++;
    console.log((ok ? "ok   " : "FAIL ") + JSON.stringify(code) + " -> " + rc + " " + JSON.stringify(out.length > 90 ? out.slice(0, 40) + " ... " + out.slice(-40) : out));
  }
  console.log(bad ? bad + " failed" : "repl: all " + cases.length + " as they should be");
  process.exit(bad ? 1 : 0);
});
