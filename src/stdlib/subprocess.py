"""Subprocesses with accessible I/O streams (CPython's subprocess): run, Popen, call, check_call,
check_output, getoutput, getstatusoutput, CompletedProcess, CalledProcessError, TimeoutExpired,
PIPE, STDOUT, DEVNULL, list2cmdline.

Linux (and the hosts where minipy runs Linux's system calls): fork + execve, pipes, waitpid.
KolibriOS: the program is started (function 70.7) with the arguments as one string; no pipes, and
its exit status is not known (0 when it has ended).

Compiled programs: run() / check_output() give str with text=True (or universal_newlines=True, or an
encoding) - the compiler picks the text version for those literal arguments -, else bytes; a Popen's
communicate() gives bytes."""
import sys
import os
import time
import select
import _os
from typing import Generic, TypeVar

__all__ = ["Popen", "PIPE", "STDOUT", "call", "check_call", "getstatusoutput", "getoutput", "check_output", "run",
           "CalledProcessError", "DEVNULL", "SubprocessError", "TimeoutExpired", "CompletedProcess", "list2cmdline"]

_T = TypeVar("_T")
_A = TypeVar("_A")
_I = TypeVar("_I")

PIPE = -1
STDOUT = -2
DEVNULL = -3

_PTR = 4 if sys._compiled else (8 if sys.maxsize > 2 ** 32 else 4)       # (an address in a system call's array)


class SubprocessError(Exception):
    pass


def _signal_name(sig: int) -> str:
    names = {1: "SIGHUP", 2: "SIGINT", 3: "SIGQUIT", 4: "SIGILL", 6: "SIGABRT", 8: "SIGFPE", 9: "SIGKILL",
             11: "SIGSEGV", 13: "SIGPIPE", 14: "SIGALRM", 15: "SIGTERM"}
    return names.get(sig, "")


if sys._compiled:
    def _cmd(args: _A) -> str:
        return str(args)

    def _out_b(b: bytes | None) -> str | None:
        return None if b is None else b.decode("utf-8", "replace")

if not sys._compiled:
    def _cmd(args):
        return args

    def _out_b(b):
        return b


class CalledProcessError(SubprocessError):
    """Raised when run() is called with check=True and the process returns a non-zero exit status (compiled
    programs: cmd, output and stderr are text)."""

    def __init__(self, returncode: int, cmd: str, output: str | None = None, stderr: str | None = None) -> None:
        if sys._compiled:
            super().__init__("")
        else:
            super().__init__(returncode, cmd)
        self.returncode = returncode
        self.cmd = cmd
        self.output = output
        self.stderr = stderr

    def __str__(self) -> str:
        if self.returncode and self.returncode < 0:
            name = _signal_name(-self.returncode)
            if name:
                return "Command '%s' died with <Signals.%s: %d>." % (self.cmd, name, -self.returncode)
            return "Command '%s' died with unknown signal %d." % (self.cmd, -self.returncode)
        return "Command '%s' returned non-zero exit status %d." % (self.cmd, self.returncode)

    @property
    def stdout(self) -> str | None:
        """Alias for output attribute, to match stderr"""
        return self.output


class TimeoutExpired(SubprocessError):
    """This exception is raised when the timeout expires while waiting for a child process."""

    def __init__(self, cmd: str, timeout: float, output: str | None = None, stderr: str | None = None) -> None:
        if sys._compiled:
            super().__init__("")
        else:
            super().__init__(cmd, timeout)
        self.cmd = cmd
        self.timeout = timeout
        self.output = output
        self.stderr = stderr

    def __str__(self) -> str:
        return "Command '%s' timed out after %s seconds" % (self.cmd, self.timeout)

    @property
    def stdout(self) -> str | None:
        return self.output


class CompletedProcess(Generic[_T]):
    """A process that has finished running: args, returncode, stdout, stderr (None: not captured)."""

    def __init__(self, args: str, returncode: int, stdout: _T | None = None, stderr: _T | None = None,
                 args_repr: str = "") -> None:
        self.args = args
        self._args_repr = args_repr or repr(args)
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr

    def __repr__(self) -> str:
        args = ['args=' + self._args_repr, 'returncode={!r}'.format(self.returncode)]
        if self.stdout is not None:
            args.append('stdout={!r}'.format(self.stdout))
        if self.stderr is not None:
            args.append('stderr={!r}'.format(self.stderr))
        return "{}({})".format(type(self).__name__, ', '.join(args))

    def check_returncode(self) -> None:
        """Raise CalledProcessError if the exit code is non-zero."""
        if self.returncode:
            if sys._compiled:
                raise CalledProcessError(self.returncode, self.args)
            raise CalledProcessError(self.returncode, self.args, self.stdout, self.stderr)


def list2cmdline(seq: list[str]) -> str:
    """Translate a sequence of arguments into a command line string (MS C runtime rules)."""
    result: list[str] = []
    needquote = False
    for arg in seq:
        bs_buf: list[str] = []
        if result:
            result.append(' ')
        needquote = (" " in arg) or ("\t" in arg) or not arg
        if needquote:
            result.append('"')
        for c in arg:
            if c == '\\':
                bs_buf.append(c)
            elif c == '"':
                result.append('\\' * len(bs_buf) * 2)
                bs_buf = []
                result.append('\\"')
            else:
                if bs_buf:
                    result.extend(bs_buf)
                    bs_buf = []
                result.append(c)
        if bs_buf:
            result.extend(bs_buf)
        if needquote:
            result.extend(bs_buf)
            result.append('"')
    return ''.join(result)


# ---- the system calls (Linux's numbers)

def _check(r: int, filename: str | None = None) -> int:
    if r < 0:
        if filename is not None:
            raise OSError(-r, os.strerror(-r), filename)
        raise OSError(-r, os.strerror(-r))
    return r


def _pipe() -> tuple[int, int]:
    b = sys.buffer(8)
    _check(sys.syscall(331, b, 0x80000)[0])            # pipe2(O_CLOEXEC)
    return (sys.peek(b, 0, 4), sys.peek(b, 4, 4))


def _close(fd: int) -> None:
    if fd >= 0:
        sys.syscall(6, fd)


def _addr_array(strings: list[str]) -> tuple[buffer, list[buffer]]:
    """An array of the strings' addresses (NUL-terminated copies), then a 0."""
    keep: list[buffer] = []
    arr = sys.buffer(_PTR * (len(strings) + 1))
    for i in range(len(strings)):
        data = strings[i].encode() + b"\0"
        sb = sys.buffer(len(data))
        sys.poke_bytes(sb, 0, data)
        keep.append(sb)
        a = sys.addr(sb)
        sys.poke(arr, _PTR * i, a & 0xFFFFFFFF, 4)
        if _PTR == 8:
            sys.poke(arr, _PTR * i + 4, (a >> 32) & 0xFFFFFFFF, 4)
    return arr, keep


def _which(cmd: str, env: dict[str, str] | None) -> str:
    if "/" in cmd:
        return cmd
    path = (env if env is not None else os.environ).get("PATH", "/usr/local/bin:/usr/bin:/bin")
    for d in path.split(":"):
        full = (d or ".") + "/" + cmd
        if os.path.isfile(full) and os.access(full, os.X_OK):
            return full
    return cmd


class _Pipe:
    """One end of a pipe to the child: a binary file (read / readline / write / close)."""

    def __init__(self, fd: int, writing: bool) -> None:
        self.fd = fd
        self.writing = writing
        self.closed = False
        self._buf = b""

    def fileno(self) -> int:
        return self.fd

    def _recv(self) -> bytes:
        b = sys.buffer(65536)
        while True:
            r = sys.syscall(3, self.fd, b, 65536)[0]
            if r != -4:
                break
        n = _check(r)
        return sys.peek_bytes(b, 0, n)

    def read(self, size: int = -1) -> bytes:
        out = self._buf
        self._buf = b""
        while size < 0 or len(out) < size:
            chunk = self._recv()
            if not chunk:
                break
            out += chunk
        if size >= 0 and len(out) > size:
            self._buf = out[size:]
            out = out[:size]
        return out

    def readline(self) -> bytes:
        while b"\n" not in self._buf:
            chunk = self._recv()
            if not chunk:
                break
            self._buf += chunk
        i = self._buf.find(b"\n")
        end = len(self._buf) if i < 0 else i + 1
        line = self._buf[:end]
        self._buf = self._buf[end:]
        return line

    def __iter__(self):
        while True:
            line = self.readline()
            if not line:
                return
            yield line

    def write(self, data: bytes) -> int:
        done = 0
        while done < len(data):
            part = data[done:done + 65536]
            b = sys.buffer(len(part))
            sys.poke_bytes(b, 0, part)
            r = sys.syscall(4, self.fd, b, len(part))[0]
            if r == -4:
                continue
            done += _check(r)
        return len(data)

    def flush(self) -> None:
        pass

    def close(self) -> None:
        if not self.closed:
            self.closed = True
            _close(self.fd)

    def __enter__(self) -> "_Pipe":
        return self

    def __exit__(self, et, ev, tb) -> None:
        self.close()


class Popen:
    """Execute a child program in a new process: Popen(["ls", "-l"], stdout=PIPE)."""

    if not sys._compiled:
        def __init__(self, args, bufsize=-1, executable=None, stdin=None, stdout=None, stderr=None, preexec_fn=None,
                     close_fds=True, shell=False, cwd=None, env=None, universal_newlines=None, startupinfo=None,
                     creationflags=0, restore_signals=True, start_new_session=False, pass_fds=(), *, user=None,
                     group=None, extra_groups=None, encoding=None, errors=None, text=None, umask=-1, pipesize=-1,
                     process_group=None):
            self._start(args, executable, stdin, stdout, stderr, shell, cwd, env, universal_newlines, encoding,
                        errors, text)

    if sys._compiled:
        def __init__(self, args, bufsize: int = -1, executable: str | None = None, stdin: int | None = None,
                     stdout: int | None = None, stderr: int | None = None, close_fds: bool = True, shell: bool = False,
                     cwd: str | None = None, env: dict[str, str] | None = None, universal_newlines: bool | None = None,
                     *, encoding: str | None = None, errors: str | None = None, text: bool | None = None) -> None:
            self._start(args, executable, stdin, stdout, stderr, shell, cwd, env, universal_newlines, encoding,
                        errors, text)

    def _start(self, args, executable: str | None, stdin: int | None, stdout: int | None, stderr: int | None,
               shell: bool, cwd: str | None, env: dict[str, str] | None, universal_newlines: bool | None,
               encoding: str | None, errors: str | None, text: bool | None) -> None:
        self.args = args
        self.text_mode = bool(text) or bool(universal_newlines) or encoding is not None or errors is not None
        self.encoding = encoding or "utf-8"
        self.errors = errors or "strict"
        self.returncode: int | None = None
        self.stdin: _Pipe | None = None
        self.stdout: _Pipe | None = None
        self.stderr: _Pipe | None = None
        self.pid = -1
        if isinstance(args, str):
            argv = ["/bin/sh", "-c", args] if shell else [args]
        else:
            argv = [str(a) for a in args]
            if shell:
                argv = ["/bin/sh", "-c"] + argv
        if not argv:
            raise IndexError("list index out of range")
        prog = executable if executable is not None else argv[0]
        if sys.platform == "kolibrios":
            if stdin == PIPE or stdout == PIPE or stderr == PIPE:
                raise OSError(95, "pipes to a process are not available on KolibriOS")
            self.pid = _kolibri_start(prog, argv[1:])
            return
        self._spawn(prog, argv, cwd, env, stdin, stdout, stderr)

    def _spawn(self, prog: str, argv: list[str], cwd: str | None, env: dict[str, str] | None, stdin: int | None,
               stdout: int | None, stderr: int | None) -> None:
        path = _which(prog, env)
        envd = env if env is not None else dict(os.environ)
        argv_arr, keep1 = _addr_array(argv)
        env_arr, keep2 = _addr_array([k + "=" + envd[k] for k in envd])
        pathb = sys.buffer(len(path.encode()) + 1)
        sys.poke_bytes(pathb, 0, path.encode() + b"\0")
        cwdb = sys.buffer(len(cwd.encode()) + 1 if cwd is not None else 1)
        if cwd is not None:
            sys.poke_bytes(cwdb, 0, cwd.encode() + b"\0")
        # the child's ends (-1: inherited)
        c_in, c_out, c_err = -1, -1, -1
        p_in, p_out, p_err = -1, -1, -1
        devnull = -1
        if stdin == DEVNULL or stdout == DEVNULL or stderr == DEVNULL:
            devnull = os.open(os.devnull, os.O_RDWR)
        if stdin == PIPE:
            c_in, p_in = _pipe()
        elif stdin == DEVNULL:
            c_in = devnull
        elif stdin is not None and stdin >= 0:
            c_in = stdin
        if stdout == PIPE:
            p_out, c_out = _pipe()
        elif stdout == DEVNULL:
            c_out = devnull
        elif stdout is not None and stdout >= 0:
            c_out = stdout
        if stderr == PIPE:
            p_err, c_err = _pipe()
        elif stderr == DEVNULL:
            c_err = devnull
        elif stderr == STDOUT:
            c_err = c_out if c_out >= 0 else 1
        elif stderr is not None and stderr >= 0:
            c_err = stderr
        err_r, err_w = _pipe()                     # the child's exec error (none: closed by the exec)
        sys.stdout.flush()
        sys.stderr.flush()
        pid = sys.syscall(2)[0]                    # fork
        if pid == 0:
            # the child
            if c_in >= 0:
                sys.syscall(63, c_in, 0)
            if c_out >= 0:
                sys.syscall(63, c_out, 1)
            if c_err >= 0:
                sys.syscall(63, c_err, 2)
            code = 0
            if cwd is not None:
                code = sys.syscall(12, cwdb)[0]
            if code >= 0:
                code = sys.syscall(11, pathb, argv_arr, env_arr)[0]
            eb = sys.buffer(8)
            sys.poke(eb, 0, -code, 4)
            sys.poke(eb, 4, 1 if cwd is not None and code != 0 and sys.syscall(33, cwdb, 0)[0] < 0 else 0, 4)
            sys.syscall(4, err_w, eb, 8)
            sys.syscall(1, 255)
        _check(pid)
        self.pid = pid
        _close(err_w)
        for fd in (c_in, c_out, c_err):
            if fd >= 0 and fd != devnull and fd not in (stdin, stdout, stderr):
                _close(fd)
        if devnull >= 0:
            _close(devnull)
        eb = sys.buffer(8)
        got = 0
        while True:
            r = sys.syscall(3, err_r, eb, 8)[0]
            if r != -4:
                got = r
                break
        _close(err_r)
        if got >= 4:
            errno_ = sys.peek(eb, 0, 4)
            cwd_failed = got >= 8 and sys.peek(eb, 4, 4) != 0
            self._waitpid(0)
            for fd in (p_in, p_out, p_err):
                _close(fd)
            if cwd_failed and cwd is not None:
                raise OSError(errno_, os.strerror(errno_), cwd)
            raise OSError(errno_, os.strerror(errno_), prog)
        if p_in >= 0:
            self.stdin = _Pipe(p_in, True)
        if p_out >= 0:
            self.stdout = _Pipe(p_out, False)
        if p_err >= 0:
            self.stderr = _Pipe(p_err, False)

    def _waitpid(self, options: int) -> bool:
        """(waits for the child - options 1: WNOHANG) True: it has ended (returncode set)."""
        if self.returncode is not None:
            return True
        if sys.platform == "kolibrios":
            if _kolibri_running(self.pid):
                if options & 1:
                    return False
                while _kolibri_running(self.pid):
                    time.sleep(0.01)
            self.returncode = 0
            return True
        st = sys.buffer(4)
        while True:
            r = sys.syscall(7, self.pid, st, options)[0]
            if r != -4:
                break
        if r == 0:
            return False
        if r < 0:
            self.returncode = 0                  # (already waited for)
            return True
        status = sys.peek(st, 0, 4)
        if status & 0x7f == 0:
            self.returncode = (status >> 8) & 0xff
        elif status & 0xff == 0x7f:
            return False
        else:
            self.returncode = -(status & 0x7f)
        return True

    def poll(self) -> int | None:
        """Check if child process has terminated: its returncode, or None."""
        self._waitpid(1)
        return self.returncode

    def wait(self, timeout: float | None = None) -> int:
        """Wait for child process to terminate: its returncode."""
        if timeout is None:
            self._waitpid(0)
        else:
            deadline = time.monotonic() + timeout
            delay = 0.0005
            while not self._waitpid(1):
                left = deadline - time.monotonic()
                if left <= 0:
                    raise TimeoutExpired(_cmd(self.args), timeout)
                delay = min(delay * 2, left, 0.05)
                time.sleep(delay)
        rc = self.returncode
        return rc if rc is not None else 0

    def communicate(self, input: bytes | None = None, timeout: float | None = None) -> tuple[bytes | None, bytes | None]:
        """Send input to stdin, read stdout and stderr until end-of-file, wait for the process to terminate:
        (stdout_data, stderr_data) - None for those not piped."""
        deadline = None if timeout is None else time.monotonic() + timeout
        stdin = self.stdin
        if stdin is not None:
            if input:
                try:
                    stdin.write(input)
                except OSError:
                    pass
            stdin.close()
        outs: list[bytes] = []
        errs: list[bytes] = []
        readers: list[_Pipe] = []
        out_pipe = self.stdout
        err_pipe = self.stderr
        if out_pipe is not None:
            readers.append(out_pipe)
        if err_pipe is not None:
            readers.append(err_pipe)
        while readers:
            left = None if deadline is None else deadline - time.monotonic()
            if left is not None and left <= 0:
                raise TimeoutExpired(_cmd(self.args), timeout if timeout is not None else 0.0,
                                     _out_b(b"".join(outs)) if out_pipe else None, _out_b(b"".join(errs)) if err_pipe else None)
            ready = select.select(readers, [], [], left)[0]
            for p in ready:
                chunk = p._recv()
                if not chunk:
                    readers.remove(p)
                    p.close()
                elif p is out_pipe:
                    outs.append(chunk)
                else:
                    errs.append(chunk)
        if deadline is None:
            self.wait()
        else:
            try:
                self.wait(max(deadline - time.monotonic(), 0.0))
            except TimeoutExpired:
                raise TimeoutExpired(_cmd(self.args), timeout if timeout is not None else 0.0)
        return (b"".join(outs) if out_pipe is not None else None, b"".join(errs) if err_pipe is not None else None)

    def send_signal(self, sig: int) -> None:
        """Send a signal to the process."""
        if self.returncode is None:
            if sys.platform == "kolibrios":
                sys.syscall(18, 18, self.pid)
            else:
                sys.syscall(37, self.pid, sig)

    def terminate(self) -> None:
        """Terminate the process with SIGTERM"""
        self.send_signal(15)

    def kill(self) -> None:
        """Kill the process with SIGKILL"""
        self.send_signal(9)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, value, traceback) -> None:
        for p in (self.stdout, self.stderr, self.stdin):
            if p is not None:
                p.close()
        self.wait()

    def __repr__(self) -> str:
        return "<Popen: returncode: {} args: {!r}>".format(self.returncode, self.args)


def _kolibri_start(prog: str, args: list[str]) -> int:
    """fn 70.7: start the program (its arguments as one string): its PID."""
    cmd = " ".join(args).encode() + b"\0"
    argb = sys.buffer(len(cmd))
    sys.poke_bytes(argb, 0, cmd)
    path = prog.encode()
    blk = sys.buffer(25 + len(path) + 1)
    sys.poke(blk, 0, 7, 4)
    sys.poke(blk, 4, 0, 4)
    sys.poke(blk, 8, sys.addr(argb), 4)
    sys.poke(blk, 12, 0, 4)
    sys.poke(blk, 16, 0, 4)
    sys.poke(blk, 20, 0, 1)
    sys.poke_bytes(blk, 25, path + b"\0")
    sys.poke(blk, 21, sys.addr(blk) + 25, 4)
    r = sys.syscall(70, blk)[0]
    if r < 0:
        raise OSError(2, "No such file or directory", prog)
    return r


def _kolibri_running(pid: int) -> bool:
    return sys.syscall(18, 21, pid)[0] != 0          # fn 18.21: the slot of the PID (0: none)


# ---- functions

def _run_bytes(args: _A, *, stdin: int | None = None, input: bytes | None = None, stdout: int | None = None,
               stderr: int | None = None, capture_output: bool = False, shell: bool = False, cwd: str | None = None,
               timeout: float | None = None, check: bool = False, env: dict[str, str] | None = None,
               text: bool | None = None, universal_newlines: bool | None = None, encoding: str | None = None,
               errors: str | None = None, executable: str | None = None) -> CompletedProcess[bytes]:
    if input is not None:
        if stdin is not None:
            raise ValueError('stdin and input arguments may not both be used.')
        stdin = PIPE
    if capture_output:
        if stdout is not None or stderr is not None:
            raise ValueError('stdout and stderr arguments may not be used with capture_output.')
        stdout = PIPE
        stderr = PIPE
    p = Popen(args, stdin=stdin, stdout=stdout, stderr=stderr, shell=shell, cwd=cwd, env=env, executable=executable)
    try:
        out, err = p.communicate(input, timeout=timeout)
    except TimeoutExpired:
        p.kill()
        p.wait()
        raise
    rc = p.poll()
    result: CompletedProcess[bytes] = CompletedProcess(_cmd(args), rc if rc is not None else 0, out, err, repr(args))
    if check and result.returncode:
        raise CalledProcessError(result.returncode, _cmd(args), _out_b(out), _out_b(err))
    return result


def _decoded(b: bytes | None, encoding: str | None, errors: str | None) -> str | None:
    if b is None:
        return None
    t = b.decode(encoding or "utf-8", errors or "strict")
    return t.replace("\r\n", "\n").replace("\r", "\n")


def _run_text(args: _A, *, stdin: int | None = None, input: str | None = None, stdout: int | None = None,
              stderr: int | None = None, capture_output: bool = False, shell: bool = False, cwd: str | None = None,
              timeout: float | None = None, check: bool = False, env: dict[str, str] | None = None,
              text: bool | None = None, universal_newlines: bool | None = None, encoding: str | None = None,
              errors: str | None = None, executable: str | None = None) -> CompletedProcess[str]:
    r = _run_bytes(args, stdin=stdin, input=input.encode(encoding or "utf-8") if input is not None else None,
                   stdout=stdout, stderr=stderr, capture_output=capture_output, shell=shell, cwd=cwd, timeout=timeout,
                   check=False, env=env, executable=executable)
    out = _decoded(r.stdout, encoding, errors)
    err = _decoded(r.stderr, encoding, errors)
    result: CompletedProcess[str] = CompletedProcess(_cmd(args), r.returncode, out, err, repr(args))
    if check and result.returncode:
        raise CalledProcessError(result.returncode, _cmd(args), out, err)
    return result


if not sys._compiled:
    def run(*popenargs, input=None, capture_output=False, timeout=None, check=False, **kwargs):
        """Run command with arguments and return a CompletedProcess instance."""
        args = popenargs[0] if popenargs else kwargs.pop("args")
        if kwargs.get("text") or kwargs.get("universal_newlines") or kwargs.get("encoding") or kwargs.get("errors"):
            return _run_text(args, input=input, capture_output=capture_output, timeout=timeout, check=check, **kwargs)
        return _run_bytes(args, input=input, capture_output=capture_output, timeout=timeout, check=check, **kwargs)

    def check_output(*popenargs, timeout=None, **kwargs):
        """Run command with arguments and return its output (CalledProcessError: a non-zero exit status)."""
        if 'input' in kwargs and kwargs['input'] is None:
            kwargs['input'] = '' if kwargs.get('universal_newlines') or kwargs.get('text') or kwargs.get('encoding') \
                or kwargs.get('errors') else b''
        return run(*popenargs, stdout=PIPE, timeout=timeout, check=True, **kwargs).stdout

if sys._compiled:
    def run(args: _A, *, stdin: int | None = None, input: bytes | None = None, stdout: int | None = None,
            stderr: int | None = None, capture_output: bool = False, shell: bool = False, cwd: str | None = None,
            timeout: float | None = None, check: bool = False, env: dict[str, str] | None = None,
            text: bool | None = None, universal_newlines: bool | None = None, encoding: str | None = None,
            errors: str | None = None, executable: str | None = None) -> CompletedProcess[bytes]:
        """Run command with arguments and return a CompletedProcess instance (text=True ...: _run_text)."""
        return _run_bytes(args, stdin=stdin, input=input, stdout=stdout, stderr=stderr, capture_output=capture_output,
                          shell=shell, cwd=cwd, timeout=timeout, check=check, env=env, executable=executable)

    def check_output(args: _A, *, stdin: int | None = None, input: bytes | None = None, stderr: int | None = None,
                     shell: bool = False, cwd: str | None = None, timeout: float | None = None,
                     env: dict[str, str] | None = None, text: bool | None = None, universal_newlines: bool | None = None,
                     encoding: str | None = None, errors: str | None = None, executable: str | None = None) -> bytes:
        """Run command with arguments and return its output (text=True ...: _check_output_text)."""
        out = _run_bytes(args, stdin=stdin, input=input, stdout=PIPE, stderr=stderr, shell=shell, cwd=cwd,
                         timeout=timeout, check=True, env=env, executable=executable).stdout
        return out if out is not None else b""


def _check_output_text(args: _A, *, stdin: int | None = None, input: str | None = None, stderr: int | None = None,
                       shell: bool = False, cwd: str | None = None, timeout: float | None = None,
                       env: dict[str, str] | None = None, text: bool | None = None, universal_newlines: bool | None = None,
                       encoding: str | None = None, errors: str | None = None, executable: str | None = None) -> str:
    out = _run_text(args, stdin=stdin, input=input, stdout=PIPE, stderr=stderr, shell=shell, cwd=cwd, timeout=timeout,
                    check=True, env=env, encoding=encoding, errors=errors, executable=executable).stdout
    return out if out is not None else ""


def call(args: _A, *, stdin: int | None = None, stdout: int | None = None, stderr: int | None = None,
         shell: bool = False, cwd: str | None = None, timeout: float | None = None,
         env: dict[str, str] | None = None) -> int:
    """Run command with arguments.  Wait for command to complete or timeout, then return the returncode."""
    p = Popen(args, stdin=stdin, stdout=stdout, stderr=stderr, shell=shell, cwd=cwd, env=env)
    try:
        return p.wait(timeout=timeout)
    except BaseException:
        p.kill()
        p.wait()
        raise


def check_call(args: _A, *, stdin: int | None = None, stdout: int | None = None, stderr: int | None = None,
               shell: bool = False, cwd: str | None = None, timeout: float | None = None,
               env: dict[str, str] | None = None) -> int:
    """Run command with arguments.  Wait for command to complete; a non-zero exit status: CalledProcessError."""
    retcode = call(args, stdin=stdin, stdout=stdout, stderr=stderr, shell=shell, cwd=cwd, timeout=timeout, env=env)
    if retcode:
        raise CalledProcessError(retcode, _cmd(args))
    return 0


def getstatusoutput(cmd: str, *, encoding: str | None = None, errors: str | None = None) -> tuple[int, str]:
    """Return (exitcode, output) of executing cmd in a shell (a trailing newline stripped)."""
    try:
        data = _check_output_text(cmd, shell=True, stderr=STDOUT, encoding=encoding, errors=errors)
        exitcode = 0
    except CalledProcessError as ex:
        out = ex.output
        data = out if isinstance(out, str) else ""
        exitcode = ex.returncode
    if data[-1:] == '\n':
        data = data[:-1]
    return exitcode, data


def getoutput(cmd: str, *, encoding: str | None = None, errors: str | None = None) -> str:
    """Return output (stdout and stderr) of executing cmd in a shell (a trailing newline stripped)."""
    return getstatusoutput(cmd, encoding=encoding, errors=errors)[1]
