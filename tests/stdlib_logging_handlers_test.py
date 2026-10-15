# logging.handlers (rotating, memory, queue, watched, timed) and logging.config.dictConfig
import logging, logging.handlers, logging.config, os, sys, queue, io
sys.stderr = sys.stdout
d = "build/lh_test"
os.makedirs(d, exist_ok=True)
for f in os.listdir(d):
    os.remove(os.path.join(d, f))
log = logging.getLogger("rot")
log.propagate = False
log.setLevel(logging.DEBUG)
rh = logging.handlers.RotatingFileHandler(os.path.join(d, "r.log"), maxBytes=60, backupCount=2)
rh.setFormatter(logging.Formatter("%(levelname)s %(message)s"))
log.addHandler(rh)
for i in range(10):
    log.info("message number %d", i)
rh.close()
print(sorted(os.listdir(d)))
with open(os.path.join(d, "r.log")) as f:
    print(f.read().splitlines())
mh = logging.handlers.MemoryHandler(3, flushLevel=logging.ERROR, target=logging.StreamHandler(sys.stdout))
ml = logging.getLogger("mem"); ml.propagate = False; ml.addHandler(mh)
ml.warning("w1"); ml.warning("w2"); print("buffered", len(mh.buffer)); ml.error("e1"); print("after", len(mh.buffer))
q = queue.Queue()
qh = logging.handlers.QueueHandler(q)
ql = logging.getLogger("q"); ql.propagate = False; ql.addHandler(qh)
ql.warning("to queue %s", "arg")
rec = q.get_nowait()
print(rec.getMessage(), rec.msg, rec.args, rec.levelname)
out = io.StringIO()
listener = logging.handlers.QueueListener(q, logging.StreamHandler(out))
listener.start()
ql.error("via listener")
listener.stop()
print(out.getvalue().strip())
logging.config.dictConfig({
    "version": 1,
    "formatters": {"short": {"format": "%(name)s|%(levelname)s|%(message)s"}},
    "handlers": {"out": {"class": "logging.StreamHandler", "formatter": "short", "stream": "ext://sys.stdout", "level": "INFO"}},
    "loggers": {"cfg": {"handlers": ["out"], "level": "DEBUG", "propagate": False}},
})
cl = logging.getLogger("cfg")
cl.debug("hidden by handler level"); cl.info("configured")
bh = logging.handlers.BufferingHandler(2)
print(bh.shouldFlush(logging.makeLogRecord({"msg": "x"})), bh.capacity)
wh = logging.handlers.WatchedFileHandler(os.path.join(d, "w.log"))
wl = logging.getLogger("w"); wl.propagate = False; wl.addHandler(wh); wl.warning("watched"); wh.close()
print(open(os.path.join(d, "w.log")).read().strip())
th = logging.handlers.TimedRotatingFileHandler(os.path.join(d, "t.log"), when="D", backupCount=1)
print(th.when, th.interval, th.backupCount)
th.close()
