// plainpy as a command for node (make wasm-node: the interpreter's tests on
// its WebAssembly build). Its data: next to plainpy.js, or PLAINPYLIB /
// PLAINPYPATH from the environment.
Module.preRun = Module.preRun || [];
Module.preRun.push(function () {
  var path = require("path");
  ENV.PLAINPYLIB = process.env.PLAINPYLIB || path.join(__dirname, "plainpy.lib");
  ENV.PLAINPYPATH = process.env.PLAINPYPATH || path.join(__dirname, "lib");
  for (var k of ["HOME", "TZ", "PATH"]) if (process.env[k]) ENV[k] = process.env[k];
});
