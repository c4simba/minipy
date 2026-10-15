import thread

log: list[str] = []
lk = thread.lock()

def note(s: str) -> None:
    thread.acquire(lk)
    log.append(s)
    thread.release(lk)

def slow(name: str, delay: float) -> None:
    thread.sleep(delay)
    note(name)

hs = [thread.start(slow, ("c", 0.3)), thread.start(slow, ("a", 0.1)), thread.start(slow, ("b", 0.2))]
thread.sleep(0.15)
note("main")
for h in hs:
    thread.join(h)
print(log)

def late() -> None:
    thread.sleep(0.05)
    print("late thread done")

thread.start(late)
print("main done")
