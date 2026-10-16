// plainpy in a worker: the interpreter (wasm/plainpy.js, make wasm) runs what
// the page sends, a piece at a time, and sends its output back as it comes.
//   page -> worker  {type: "run", code}
//   worker -> page  {type: "ready", version} | {type: "out", text, err} | {type: "done", rc}
//                   rc: 0 done, 1 an exception, 2 more lines wanted
"use strict";
importScripts("wasm/plainpy.js");

let out = "", err = "";
function flush() {
  if (out) { postMessage({ type: "out", text: out, err: false }); out = ""; }
  if (err) { postMessage({ type: "out", text: err, err: true }); err = ""; }
}
const decoder = new TextDecoder();
function sink(isErr) {
  let bytes = [];
  return function (c) {
    if (c === null) return;
    bytes.push(c & 255);
    if (c === 10 || bytes.length >= 512) {
      const t = decoder.decode(new Uint8Array(bytes)); bytes = [];
      if (isErr) err += t; else out += t;
      flush();
    }
  };
}

createPlainpy({
  locateFile: (f) => "wasm/" + f,
  preRun: [function (M) {
    // no input() here: a worker cannot wait for the page's next line
    M.FS.init(() => null, sink(false), sink(true));
  }],
}).then((M) => {
  M.ccall("ppy_web_start", "number", [], []);
  self.onmessage = (e) => {
    if (e.data.type !== "run") return;
    let rc;
    try {
      rc = M.ccall("ppy_web_run", "number", ["string"], [e.data.code]);
    } catch (x) {                                    // (the browser's own stack ran out, say: this session is over)
      flush();
      postMessage({ type: "out", text: "\nplainpy stopped: " + x + "\n", err: true });
      postMessage({ type: "crashed" });
      return;
    }
    flush();
    postMessage({ type: "done", rc: rc });
  };
  postMessage({ type: "ready" });
}, (e) => postMessage({ type: "out", text: "plainpy did not start: " + e + "\n", err: true }));
