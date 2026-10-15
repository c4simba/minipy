# subprocess: run (capture, text, shell, input, check, timeout, cwd, env), check_output, call, check_call,
# getoutput / getstatusoutput, Popen (pipes, communicate, wait with a timeout, kill), errors

import subprocess
import sys

r = subprocess.run(["echo", "hello", "world"], capture_output=True)
print(r.returncode, r.stdout, r.stderr)
r2 = subprocess.run(["sh", "-c", "echo out; echo err 1>&2; exit 3"], capture_output=True, text=True)
print(r2.returncode, repr(r2.stdout), repr(r2.stderr))
print(r2)
r3 = subprocess.run("echo $((6*7))", shell=True, stdout=subprocess.PIPE, text=True)
print(repr(r3.stdout), r3.stderr)
print(subprocess.check_output(["tr", "a-z", "A-Z"], input=b"shout\n"))
print(repr(subprocess.check_output(["cat"], input="text in\n", text=True)))
try:
    subprocess.run(["false"], check=True)
except subprocess.CalledProcessError as e:
    print("CalledProcessError:", e, e.returncode)
try:
    subprocess.run(["no-such-program-xyz"])
except FileNotFoundError as e:
    print("FileNotFoundError:", e.errno, e.filename)
print(subprocess.call(["sh", "-c", "exit 5"]), subprocess.check_call(["true"]))
print(subprocess.getoutput("echo one; echo two"), subprocess.getstatusoutput("exit 2"))
p = subprocess.Popen(["sh", "-c", "read x; echo got $x; exit 1"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
out, err = p.communicate(b"data\n")
print(out, err, p.returncode, p.poll())
p2 = subprocess.Popen(["sleep", "5"])
try:
    p2.wait(timeout=0.1)
except subprocess.TimeoutExpired as e:
    print("TimeoutExpired:", e)
p2.kill()
print(p2.wait())
try:
    subprocess.run(["sleep", "5"], timeout=0.1)
except subprocess.TimeoutExpired as e:
    print("TimeoutExpired:", e.timeout)
r4 = subprocess.run(["pwd"], capture_output=True, text=True, cwd="/")
print(r4.stdout.strip())
r5 = subprocess.run(["sh", "-c", "echo $FOO"], capture_output=True, text=True, env={"FOO": "bar", "PATH": "/bin:/usr/bin"})
print(r5.stdout.strip())
print(subprocess.list2cmdline(["a b", "c\"d", "e"]))
