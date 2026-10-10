# logging (CPython's): loggers, handlers, formatters (% { $ styles), filters, adapters, basicConfig, lastResort
import sys
sys.stderr = sys.stdout
import logging, sys, io
logging.warning("root warning %s", 1)
logging.info("not shown")
log = logging.getLogger("app.sub")
print(log.name, log.level, log.getEffectiveLevel(), log.parent.name, logging.getLevelName(20), logging.getLevelName("ERROR"))
buf = io.StringIO()
h = logging.StreamHandler(buf)
h.setFormatter(logging.Formatter("%(levelname)s:%(name)s:%(funcName)s:%(lineno)d:%(message)s"))
log.addHandler(h)
log.setLevel(logging.DEBUG)
log.propagate = False
def work():
    log.debug("debug %d", 5)
    log.info("info %s", "x")
    log.warning("warn")
work()
try:
    1 / 0
except ZeroDivisionError:
    log.exception("failed")
print(buf.getvalue().splitlines()[:4])
print(buf.getvalue().splitlines()[-1])
f2 = logging.Formatter("{levelname} {name} {message}", style="{")
h.setFormatter(f2); log.error("brace %s", "style")
f3 = logging.Formatter("$levelname $message", style="$")
h.setFormatter(f3); log.critical("dollar")
print(buf.getvalue().splitlines()[-2:])
class F(logging.Filter):
    def filter(self, r):
        return "keep" in r.getMessage()
h.addFilter(F())
n = len(buf.getvalue())
log.info("drop this"); log.info("keep this")
print(buf.getvalue()[n:].strip())
adapter = logging.LoggerAdapter(log, {"user": "u1"})
h.filters.clear()
h.setFormatter(logging.Formatter("%(message)s %(user)s"))
adapter.info("adapted")
print(buf.getvalue().splitlines()[-1])
print(logging.getLogger("app") is logging.getLogger("app"), logging.getLogger("app.sub").parent is logging.getLogger("app"))
print(logging.root.level, logging.WARNING, logging.getLevelNamesMapping()["INFO"], logging.Logger.manager.loggerDict.keys().__len__() >= 2)
r = logging.LogRecord("n", logging.INFO, "p.py", 3, "m %s", ("a",), None)
print(r.getMessage(), r.levelname, r.filename, r.module, r.lineno, r.threadName, r.processName)
logging.disable(logging.CRITICAL)
log.critical("disabled")
logging.disable(logging.NOTSET)
print(buf.getvalue().splitlines()[-1])
log.handlers.clear()
logging.basicConfig(level=logging.INFO, format="%(levelname)s|%(message)s", stream=sys.stdout, force=True)
logging.info("basic %d", 42)
logging.getLogger("other").warning("other logger")
nl = logging.getLogger("quiet"); nl.addHandler(logging.NullHandler()); nl.propagate = False; nl.error("nothing")
logging.shutdown()
print("done")
