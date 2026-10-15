# fileinput (lines of several files, counters), filecmp (cmp, cmpfiles, dircmp report), sched (events in time
# order with priorities, cancel, a fake clock)
import filecmp
import fileinput
import os
import sched

base = "build/typed_ffs"
for d in (base, base + "/a", base + "/b", base + "/a/sub", base + "/b/sub"):
    if not os.path.isdir(d):
        os.makedirs(d)
files = {"a/same.txt": "same\n", "b/same.txt": "same\n", "a/diff.txt": "one\n", "b/diff.txt": "three\n",
         "a/only_a.txt": "a\n", "b/only_b.txt": "b\n", "a/sub/x.txt": "x\n", "b/sub/x.txt": "yy\n", "f1.txt": "l1\nl2\n",
         "f2.txt": "m1\n"}
for name in files:
    with open(base + "/" + name, "w") as f:
        f.write(files[name])

with fileinput.input([base + "/f1.txt", base + "/f2.txt"]) as fi:
    for line in fi:
        print(fileinput.filename().split("/")[-1], fileinput.lineno(), fileinput.filelineno(), fileinput.isfirstline(),
              repr(line))
lines = [line.strip() for line in fileinput.FileInput(base + "/f1.txt")]
print(lines)

print(filecmp.cmp(base + "/a/same.txt", base + "/b/same.txt"), filecmp.cmp(base + "/a/diff.txt", base + "/b/diff.txt"),
      filecmp.cmp(base + "/a/diff.txt", base + "/b/diff.txt", shallow=False))
print(filecmp.cmpfiles(base + "/a", base + "/b", ["same.txt", "diff.txt", "nothing.txt"]))
dc = filecmp.dircmp(base + "/a", base + "/b")
print(dc.left_only, dc.right_only, dc.common, dc.common_files, dc.common_dirs, dc.same_files, dc.diff_files)
dc.report()
dc.report_full_closure()
print(sorted(dc.subdirs.keys()), dc.subdirs["sub"].diff_files)

clock = [0.0]


def now() -> float:
    return clock[0]


def wait(t: float) -> None:
    clock[0] += t


log: list[str] = []


def act(name: str, n: int) -> None:
    log.append("%s %d at %.1f" % (name, n, clock[0]))


s = sched.scheduler(now, wait)
s.enter(2, 1, act, ("late", 1))
s.enter(1, 2, act, ("low", 2))
s.enter(1, 1, act, ("high", 3))
e = s.enter(1.5, 1, act, ("cancelled", 4))
s.enterabs(0.5, 1, act, argument=("abs", 5))
print(len(s.queue), s.empty(), [ev.time for ev in s.queue])
s.cancel(e)
print(s.run(blocking=False), log)
s.run()
print(log, s.empty())

for name in files:
    os.remove(base + "/" + name)
for d in (base + "/a/sub", base + "/b/sub", base + "/a", base + "/b", base):
    os.rmdir(d)
