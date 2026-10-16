// The "Try it" terminal: plainpy's interpreter (WebAssembly, try-worker.js)
// answering what is typed, as Python's REPL does. Enter runs a line; a block
// (after a line ending in ":") ends with an empty line; Shift+Enter adds a
// line; Up/Down go through the history; Ctrl+L clears; Stop ends a run that
// takes too long (and starts a new session).
(function () {
  "use strict";
  const box = document.getElementById("repl");
  if (!box) return;
  const out = box.querySelector(".repl-out");
  const line = box.querySelector(".repl-line");
  const input = box.querySelector(".repl-in");
  const promptEl = box.querySelector(".repl-prompt");
  const startBtn = box.querySelector(".repl-start");
  const stopBtn = box.querySelector('[data-act="stop"]');
  const clearBtn = box.querySelector('[data-act="clear"]');
  const examples = document.querySelectorAll("[data-repl-code]");

  let worker = null, ready = false, busy = false, buffer = [], queued = null;
  let history = [];
  try { history = JSON.parse(localStorage.getItem("plainpy-repl-history") || "[]"); } catch (e) { history = []; }
  let hpos = history.length;

  function write(text, cls) {
    const span = document.createElement("span");
    if (cls) span.className = cls;
    span.textContent = text;
    out.appendChild(span);
    out.scrollTop = out.scrollHeight;
  }
  function setPrompt(p) { promptEl.textContent = p; }
  function grow() { input.rows = Math.min(12, input.value.split("\n").length); }

  function start(quiet) {
    ready = false; busy = false; buffer = [];
    worker = new Worker("try-worker.js");
    worker.onmessage = (e) => {
      const m = e.data;
      if (m.type === "ready") {
        ready = true;
        if (!quiet) write("plainpy · Python 3.14, compiled to WebAssembly, running in this page\n" +
                          "Enter runs a line; a block ends with an empty line. (input() has no keyboard here.)\n", "dim");
        line.hidden = false; setPrompt(">>> "); stopBtn.disabled = true;
        input.focus({ preventScroll: true });
        if (queued) { const q = queued; queued = null; run(q, true); }
      } else if (m.type === "out") {
        write(m.text, m.err ? "err" : "");
      } else if (m.type === "crashed") {
        worker.terminate(); worker = null;
        write("[a new session starts]\n", "dim");
        start(true);
      } else if (m.type === "done") {
        busy = false; stopBtn.disabled = true;
        if (m.rc === 2) { setPrompt("... "); }
        else { buffer = []; setPrompt(">>> "); }
        line.hidden = false;
        input.focus({ preventScroll: true });
      }
    };
    worker.onerror = (e) => { write("plainpy stopped: " + (e.message || "an error") + "\n", "err"); };
  }

  function remember(code) {
    if (!code.trim() || history[history.length - 1] === code) { hpos = history.length; return; }
    history.push(code);
    if (history.length > 200) history = history.slice(-200);
    hpos = history.length;
    try { localStorage.setItem("plainpy-repl-history", JSON.stringify(history)); } catch (e) { /* (private mode) */ }
  }

  // Does the Python text go on? Open brackets or a triple-quoted string, a
  // line ending in "\" or ":", or a block not yet ended by an empty line.
  function goesOn(lines) {
    const text = lines.join("\n");
    let depth = 0, q = null, i = 0;
    while (i < text.length) {
      const c = text[i];
      if (q) {
        if (c === "\\") { i += 2; continue; }
        if (q.length === 3 ? text.startsWith(q, i) : c === q) { i += q.length; q = null; continue; }
        if (q.length === 1 && c === "\n") q = null;
        i++; continue;
      }
      if (c === "#") { while (i < text.length && text[i] !== "\n") i++; continue; }
      if (c === '"' || c === "'") { q = text.startsWith(c + c + c, i) ? c + c + c : c; i += q.length; continue; }
      if ("([{".includes(c)) depth++;
      else if (")]}".includes(c)) depth = Math.max(0, depth - 1);
      i++;
    }
    if (q && q.length === 3) return true;
    if (depth > 0) return true;
    const last = lines[lines.length - 1];
    if (/\\\s*$/.test(last)) return true;
    if (/:\s*(#.*)?$/.test(last)) return true;
    return lines.length > 1 && last.trim() !== "";
  }

  function run(code, echo) {
    if (!ready) { queued = code; if (!worker) start(); return; }
    if (echo) code.split("\n").forEach((l, k) => write((k ? "... " : ">>> ") + l + "\n", "in"));
    busy = true; line.hidden = true; stopBtn.disabled = false;
    worker.postMessage({ type: "run", code: code });
  }

  function submit() {
    const text = input.value;
    input.value = ""; grow();
    if (text.includes("\n")) {                          // a pasted block: as it is
      text.split("\n").forEach((l, k) => write((k || buffer.length ? "... " : ">>> ") + l + "\n", "in"));
      buffer = buffer.concat(text.split("\n"));
      remember(text);
      run(buffer.join("\n"), false);
      return;
    }
    write(promptEl.textContent + text + "\n", "in");
    if (!buffer.length && !text.trim()) return;
    buffer.push(text);
    if (buffer.length === 1) remember(text); else remember(buffer.join("\n"));
    if (text.trim() !== "" && goesOn(buffer)) { setPrompt("... "); return; }
    run(buffer.join("\n"), false);
  }

  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); if (!busy && ready) submit(); return; }
    if (e.key === "Enter" && e.shiftKey) { setTimeout(grow, 0); return; }
    if (e.key === "l" && e.ctrlKey) { e.preventDefault(); out.textContent = ""; return; }
    const atStart = input.selectionStart === 0 || !input.value.slice(0, input.selectionStart).includes("\n");
    const atEnd = !input.value.slice(input.selectionEnd).includes("\n");
    if (e.key === "ArrowUp" && atStart && hpos > 0) { e.preventDefault(); input.value = history[--hpos]; grow(); }
    else if (e.key === "ArrowDown" && atEnd && hpos < history.length) { e.preventDefault(); hpos++; input.value = hpos < history.length ? history[hpos] : ""; grow(); }
  });
  input.addEventListener("input", grow);
  box.querySelector(".repl-screen").addEventListener("click", () => { if (!line.hidden && !window.getSelection().toString()) input.focus({ preventScroll: true }); });

  startBtn.addEventListener("click", () => { startBtn.hidden = true; write("loading plainpy (about 2 MB)…\n", "dim"); start(false); });
  stopBtn.addEventListener("click", () => {
    if (!busy) return;
    worker.terminate(); worker = null;
    write("\n[stopped: a new session starts]\n", "dim");
    start(true);
  });
  clearBtn.addEventListener("click", () => { out.textContent = ""; if (!line.hidden) input.focus({ preventScroll: true }); });
  examples.forEach((b) => b.addEventListener("click", () => {
    const code = b.getAttribute("data-repl-code").replace(/\\n/g, "\n");
    if (!worker) { startBtn.hidden = true; write("loading plainpy (about 2 MB)…\n", "dim"); queued = code; start(false); box.scrollIntoView({ block: "nearest" }); return; }
    if (busy) return;
    buffer = []; setPrompt(">>> ");
    remember(code);
    run(code, true);
  }));
})();
