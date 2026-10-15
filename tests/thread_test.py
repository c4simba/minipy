# Threads under a GIL: 4 workers each add 1000 to a shared counter, guarded by a
# lock, so the total is deterministic (4000) despite real concurrent execution.

import thread

count = [0]
lk = thread.lock()

def worker():
    i = 0
    while i < 1000:
        thread.acquire(lk)
        count[0] = count[0] + 1
        thread.release(lk)
        i = i + 1

handles = []
n = 0
while n < 4:
    handles.append(thread.start(worker))
    n = n + 1

for h in handles:
    thread.join(h)

print(count[0])

# thread.start accepts an args tuple too
def show(label, value):
    thread.acquire(lk)
    print(label + "=" + str(value))
    thread.release(lk)

thread.join(thread.start(show, ("answer", 42)))
print("done")
