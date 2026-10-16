// The website's "Try it" terminal in a real browser: headless Chrome driven
// over the DevTools protocol (node 22's WebSocket). Serve site/ (with
// site/wasm from make wasm) first, then:
//   node tests/wasm/page_check.js http://localhost:8765/ shot.png
// Starts plainpy, types a few lines, presses two examples, prints the
// terminal's text and saves a screenshot of it.
"use strict";
const { spawn } = require("child_process");
const fs = require("fs");
const url = process.argv[2] || "http://localhost:8765/";
const shot = process.argv[3] || "build/try.png";
const chrome = process.env.CHROME || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const port = 9333;
const proc = spawn(chrome, ["--headless=new", "--disable-gpu", "--hide-scrollbars", `--remote-debugging-port=${port}`,
  "--user-data-dir=build/chrome-profile", "--window-size=1280,1600", "about:blank"], { stdio: "ignore" });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  let ws;
  for (let i = 0; i < 50 && !ws; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
      const page = list.find((t) => t.type === "page");
      if (page) ws = new WebSocket(page.webSocketDebuggerUrl);
    } catch (e) { await sleep(200); }
  }
  await new Promise((r) => ws.addEventListener("open", r));
  let id = 0; const waiting = new Map();
  ws.addEventListener("message", (m) => { const d = JSON.parse(m.data); if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); } });
  const send = (method, params = {}) => new Promise((r) => { const n = ++id; waiting.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async (expr) => (await send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true })).result.result.value;
  const until = async (expr, ms) => { for (let t = 0; t < ms; t += 100) { if (await js(expr)) return true; await sleep(100); } return false; };

  await send("Page.enable");
  await send("Page.navigate", { url: url + "#try" });
  await sleep(1500);
  await js(`document.querySelector('#repl .repl-start').click()`);
  const started = await until(`!document.querySelector('#repl .repl-line').hidden`, 20000);
  console.log("started:", started);
  const type = async (line) => {
    await js(`(() => { const i = document.querySelector('#repl .repl-in'); i.value = ${JSON.stringify(line)};
      i.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter', bubbles: true})); })()`);
    await until(`!document.querySelector('#repl .repl-line').hidden`, 10000);
  };
  for (const l of ["1 + 2", "def sq(n):", "    return n * n", "", "[sq(i) for i in range(8)]", "'plain' + 'py'", "{'a': 1}['b']"]) await type(l);
  await js(`document.querySelectorAll('[data-repl-code]')[2].click()`);   // json
  await sleep(300); await until(`!document.querySelector('#repl .repl-line').hidden`, 10000);
  await js(`document.querySelectorAll('[data-repl-code]')[3].click()`);   // the blinking LED (sleeps)
  await sleep(300); await until(`!document.querySelector('#repl .repl-line').hidden`, 15000);
  const text = await js(`document.querySelector('#repl .repl-out').textContent`);
  console.log(text);
  await js(`document.querySelector('#repl').scrollIntoView({block: 'center'})`);
  await sleep(300);
  const box = await js(`(() => { const r = document.querySelector('#try').getBoundingClientRect(); return {x: r.x, y: r.y + scrollY, w: r.width, h: r.height}; })()`);
  const img = await send("Page.captureScreenshot", { format: "png", captureBeyondViewport: true, clip: { x: box.x, y: box.y, width: box.w, height: box.h, scale: 1 } });
  fs.writeFileSync(shot, Buffer.from(img.result.data, "base64"));
  console.log("screenshot:", shot);
  ws.close(); proc.kill();
  process.exit(started && text.includes(">>> 1 + 2") && text.includes("3\n") && text.includes("[0, 1, 4, 9, 16, 25, 36, 49]") && text.includes("[pin 2] = 1") ? 0 : 1);
})().catch((e) => { console.error(e); proc.kill(); process.exit(1); });
