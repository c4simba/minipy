#!/usr/bin/env python3
"""Run i386 programs made by `minipy --compile` on any host (an arm64 Mac, say).

    python3 tests/x86run.py [--trace-syscalls] program [args...]

A small user-mode emulator on top of the unicorn CPU emulator
(`pip install unicorn`):

  - Linux ELF executables: the int 0x80 system calls compiled programs and the
    Linux fasm binary use (read, write, open, close, lseek, brk, mmap2, munmap,
    gettimeofday, nanosleep, exit, ...);
  - KolibriOS applications (MENUET01): the int 0x40 functions the runtime uses
    (heap, process info, sleep, time, debug board) and the shell side of the
    "<pid>-SHELL" console, so print() and input() go to stdout/stdin.

The exit status is the program's (KolibriOS: the value passed to rt_exit).
X86RUN_STATS=1 reports the memory the program mapped (a leak check).

With tests/run_typed_tests.sh:
    FASM="python3 tests/x86run.py /path/to/linux/fasm" RUN="python3 tests/x86run.py" \\
        sh tests/run_typed_tests.sh                 # TARGET=kolibri for the other target
"""
import errno, os, struct, sys, time
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_INTR, UC_PROT_ALL
from unicorn.x86_const import (UC_X86_REG_FPCW, UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP)

PAGE = 0x1000
def down(x): return x & ~(PAGE - 1)
def up(x): return (x + PAGE - 1) & ~(PAGE - 1)

class Exit(Exception):
    def __init__(self, code): self.code = code

class Emu:
    def __init__(self, path, argv, trace=False):
        self.trace = trace
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.pages = set()
        self.mapped = self.peak = 0
        self.code = None
        data = open(path, 'rb').read()
        self.kolibri = data[:8] == b'MENUET01'
        self.shell = None
        uc.hook_add(UC_HOOK_INTR, self.intr)
        uc.reg_write(UC_X86_REG_FPCW, 0x37F)                # what a new process gets
        if self.kolibri:
            _, start, i_end, mem_end, stack_top, _, _ = struct.unpack_from('<7I', data, 8)
            self.map(0, up(max(mem_end, len(data))))
            uc.mem_write(0, data)
            uc.reg_write(UC_X86_REG_ESP, stack_top)
            self.entry = start
            self.heap_next = 0x10000000
            return
        if data[:4] != b'\x7fELF': raise SystemExit('x86run: not an ELF file or a KolibriOS application: ' + path)
        entry, phoff = struct.unpack_from('<II', data, 24)
        phentsize, phnum = struct.unpack_from('<HH', data, 42)
        top = 0
        for i in range(phnum):
            p_type, p_off, p_vaddr, _, p_filesz, p_memsz, _, _ = struct.unpack_from('<8I', data, phoff + i * phentsize)
            if p_type != 1: continue                         # PT_LOAD
            self.map(down(p_vaddr), up(p_vaddr + p_memsz))
            uc.mem_write(p_vaddr, data[p_off:p_off + p_filesz])
            top = max(top, up(p_vaddr + p_memsz))
        self.brk = self.brk_min = top
        self.mmap_next = 0x40000000
        stack_top, stack_size = 0xC0000000, 0x800000
        self.map(stack_top - stack_size, stack_top)
        sp = stack_top - 0x100                               # argv strings, then argc/argv/envp/auxv
        ptrs = []
        for a in argv:
            b = a.encode() + b'\0'
            sp -= len(b); uc.mem_write(sp, b); ptrs.append(sp)
        sp &= ~15
        words = [len(argv)] + ptrs + [0, 0, 0, 0]          # argv NULL, envp NULL, AT_NULL
        sp -= 4 * len(words)
        uc.mem_write(sp, struct.pack('<%dI' % len(words), *words))
        uc.reg_write(UC_X86_REG_ESP, sp)
        self.entry = entry

    def map(self, start, end):
        run = None
        for p in range(start // PAGE, end // PAGE):
            if p in self.pages:
                if run is not None: self.uc.mem_map(run * PAGE, (p - run) * PAGE, UC_PROT_ALL); run = None
                continue
            self.pages.add(p)
            if run is None: run = p
        if run is not None: self.uc.mem_map(run * PAGE, (end // PAGE - run) * PAGE, UC_PROT_ALL)

    def unmap(self, start, end):
        for p in range(start // PAGE, end // PAGE):
            if p in self.pages:
                self.uc.mem_unmap(p * PAGE, PAGE); self.pages.discard(p)

    def cstr(self, addr):
        out = b''
        while True:
            chunk = bytes(self.uc.mem_read(addr, 64))
            i = chunk.find(b'\0')
            if i >= 0: return (out + chunk[:i]).decode('utf-8', 'replace')
            out += chunk; addr += 64

    def intr(self, uc, intno, _):
        if intno == 0x40 and self.kolibri:
            r = [uc.reg_read(x) for x in (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI)]
            try:
                ret = self.kcall(*r)
            except Exit as e:
                self.shell_poll(); self.code = e.code; uc.emu_stop(); return
            if self.trace: sys.stderr.write('[kos %d.%d -> %r]\n' % (r[0] if r[0] < 2**31 else r[0] - 2**32, r[1], ret))
            if ret is not None: uc.reg_write(UC_X86_REG_EAX, ret & 0xFFFFFFFF)
            return
        if intno != 0x80:
            eip = uc.reg_read(UC_X86_REG_EIP)
            sys.stderr.write('x86run: interrupt %#x at %#x\n' % (intno, eip))
            self.code = 139; uc.emu_stop(); return
        r = [uc.reg_read(x) for x in (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP)]
        try:
            ret = self.syscall(*r)
        except Exit as e:
            self.code = e.code; uc.emu_stop(); return
        except OSError as e:
            ret = -(e.errno or errno.EIO)
        if self.trace: sys.stderr.write('[sys %d %x %x %x -> %d]\n' % (r[0], r[1], r[2], r[3], ret if ret < 2**31 else ret - 2**32))
        uc.reg_write(UC_X86_REG_EAX, ret & 0xFFFFFFFF)

    # ---- Linux (int 0x80)
    def syscall(self, n, b, c, d, si, di, bp):
        uc = self.uc
        s32 = lambda x: x - 2**32 if x >= 2**31 else x
        if n in (1, 252): raise Exit(b & 0xFF)               # exit, exit_group
        if n == 3:                                           # read
            data = os.read(b, d)
            uc.mem_write(c, data); return len(data)
        if n == 4: return os.write(b, bytes(uc.mem_read(c, d)))
        if n == 5:                                           # open
            host = {0: os.O_RDONLY, 1: os.O_WRONLY, 2: os.O_RDWR}[c & 3]
            if c & 0o100: host |= os.O_CREAT
            if c & 0o1000: host |= os.O_TRUNC
            if c & 0o2000: host |= os.O_APPEND
            return os.open(self.cstr(b), host, d & 0o777)
        if n == 6: os.close(b); return 0
        if n == 10: os.unlink(self.cstr(b)); return 0
        if n == 13:                                          # time
            t = int(time.time())
            if b: uc.mem_write(b, struct.pack('<I', t))
            return t
        if n == 19: return os.lseek(b, s32(c), d)
        if n == 20: return 4242                              # getpid
        if n == 45:                                          # brk
            if b <= self.brk_min: return self.brk
            self.map(up(self.brk), up(b)); self.brk = b; return b
        if n == 54: return -errno.ENOTTY                     # ioctl
        if n == 78:                                          # gettimeofday
            t = time.time()
            if b: uc.mem_write(b, struct.pack('<II', int(t), int((t % 1) * 1e6)))
            return 0
        if n == 91:                                          # munmap
            self.unmap(down(b), up(b + c)); self.mapped -= up(c); return 0
        if n in (90, 192):                                   # mmap, mmap2
            if n == 90: b, c, d, si, di, bp = struct.unpack('<6I', bytes(uc.mem_read(b, 24)))
            size = up(c)
            addr = self.mmap_next; self.mmap_next += size + PAGE
            self.map(addr, addr + size)
            self.mapped += size; self.peak = max(self.peak, self.mapped)
            if not (si & 0x20) and s32(di) >= 0:             # a file mapping: read it in
                off = bp * (PAGE if n == 192 else 1)
                os.lseek(di, off, 0); uc.mem_write(addr, os.read(di, c))
            return addr
        if n == 125: return 0                                # mprotect
        if n == 162:                                         # nanosleep
            sec, nsec = struct.unpack('<II', bytes(uc.mem_read(b, 8)))
            time.sleep(sec + nsec / 1e9); return 0
        if n in (174, 175): return 0                         # rt_sigaction, rt_sigprocmask
        sys.stderr.write('x86run: unsupported syscall %d\n' % n)
        return -errno.ENOSYS

    # ---- KolibriOS (int 0x40): the functions the runtime uses, and the shell console
    SHELL_RING, SHELL_SIZE = 1040, 15344
    def kcall(self, a, b, c, d, si, di):
        uc = self.uc
        if a == 0xFFFFFFFF: raise Exit(b & 0xFF)             # -1: terminate
        if a == 5:                                           # sleep (1/100 s)
            self.shell_poll(); time.sleep(b / 100.0); return None
        if a == 26 and b == 9: return int(time.monotonic() * 100) & 0xFFFFFFFF
        if a == 9:                                           # process info: PID at +30
            info = bytearray(1024); struct.pack_into('<I', info, 30, 4242)
            uc.mem_write(b, bytes(info)); return 1
        if a == 63 and b == 1: os.write(2, bytes([c & 0xFF])); return None
        if a == 68:
            if b == 1: self.shell_poll(); return None        # yield
            if b == 11: return 0x1000000                     # heap init
            if b == 12:                                      # alloc
                size = up(c + 16); addr = self.heap_next; self.heap_next += size + PAGE
                self.map(addr, addr + size); self.mapped += size; self.peak = max(self.peak, self.mapped)
                return addr
            if b == 13: return 1                             # free
            if b == 22:                                      # open shared memory
                name = self.cstr(c)
                size = up(d); addr = self.heap_next; self.heap_next += size + PAGE
                self.map(addr, addr + size)
                if name.endswith('-SHELL'): self.shell = addr
                return addr
            if b == 23: return 0
        if a == 70: return self.f70(b)                       # file system: host files
        gui = self.gui(a, b, c, d, si, di)
        if gui is not False: return gui
        sys.stderr.write('x86run: unsupported KolibriOS function %d.%d\n' % (a if a < 2**31 else a - 2**32, b))
        return 0xFFFFFFFF

    # A headless desktop: window calls are logged, events come from X86RUN_EVENTS
    # ("1,3" by default: redraw, then a press of button 1 - the close box).
    def gui(self, a, b, c, d, si, di):
        if not hasattr(self, 'events'):
            self.events = [int(x) for x in os.environ.get('X86RUN_EVENTS', '1,3').split(',') if x]
        def log(text): os.write(1, ('[gui] ' + text + '\n').encode())
        if a == 12: log('redraw %s' % ('begin' if b == 1 else 'end')); return None
        if a == 0: log('window x=%d w=%d y=%d h=%d style=%#x caption=%r' % (b >> 16, b & 0xFFFF, c >> 16, c & 0xFFFF, d, self.cstr(di) if di else '')); return None
        if a == 4: log('text x=%d y=%d color=%#x %r' % (b >> 16, b & 0xFFFF, c & 0xFFFFFF, bytes(self.uc.mem_read(d, si)).decode('latin-1') if si else '')); return None
        if a == 8: log('button x=%d w=%d y=%d h=%d id=%d color=%#x' % (b >> 16, b & 0xFFFF, c >> 16, c & 0xFFFF, d, si)); return None
        if a == 13: log('bar x=%d w=%d y=%d h=%d color=%#x' % (b >> 16, b & 0xFFFF, c >> 16, c & 0xFFFF, d)); return None
        if a == 71: log('title %r' % self.cstr(c)); return None
        if a in (10, 11, 23):
            self.shell_poll()
            if self.events: return self.events.pop(0)
            raise Exit(3)                                    # out of scripted events
        if a == 17: return 1 << 8                            # the pressed button: 1
        if a == 2: return 1                                  # no key
        return False

    def f70(self, info):
        """System function 70 on host files: 0 read, 2 create/rewrite, 3 write at, 5 info."""
        uc = self.uc
        sub, off, hi, size, buf = struct.unpack('<5I', bytes(uc.mem_read(info, 20)))
        path = self.cstr(info + 20) if bytes(uc.mem_read(info + 20, 1)) != b'\0' else self.cstr(struct.unpack('<I', bytes(uc.mem_read(info + 21, 4)))[0])
        try:
            if sub == 0:
                with open(path, 'rb') as fh:
                    fh.seek(off); data = fh.read(size)
                uc.mem_write(buf, data); uc.reg_write(UC_X86_REG_EBX, len(data))
                return 0 if len(data) == size else 6
            if sub == 2:
                with open(path, 'wb') as fh: fh.write(bytes(uc.mem_read(buf, size)))
                uc.reg_write(UC_X86_REG_EBX, size); return 0
            if sub == 3:
                with open(path, 'r+b') as fh:
                    fh.seek(off); fh.write(bytes(uc.mem_read(buf, size)))
                uc.reg_write(UC_X86_REG_EBX, size); return 0
            if sub == 5:
                st = os.stat(path)
                bdfe = bytearray(40); struct.pack_into('<Q', bdfe, 32, st.st_size)
                uc.mem_write(buf, bytes(bdfe)); return 0
        except FileNotFoundError:
            return 5
        except OSError:
            return 10
        return 2                                             # not supported

    def shell_poll(self):
        """The shell's side: take the frames the program wrote to the ring."""
        if not self.shell: return
        uc, base = self.uc, self.shell
        wp, rp = struct.unpack('<II', bytes(uc.mem_read(base, 8)))
        ring = bytes(uc.mem_read(base + self.SHELL_RING, self.SHELL_SIZE))
        def take(n):
            nonlocal rp
            out = bytes(ring[(rp + i) % self.SHELL_SIZE] for i in range(n))
            rp = (rp + n) % self.SHELL_SIZE
            return out
        while rp != wp:
            cmd = take(1)[0]; lo, hi = take(2); payload = take(lo | hi << 8)
            if cmd == 3:                                     # print
                os.write(1, payload.split(b'\0')[0])
            elif cmd == 5:                                   # read a line
                line = sys.stdin.buffer.readline().rstrip(b'\n')[:1023]
                uc.mem_write(base + 16, line + b'\0')
                uc.mem_write(base + 12, struct.pack('<I', len(line)))
                uc.mem_write(base + 8, struct.pack('<I', 1))
            elif cmd == 1:                                   # exit
                uc.mem_write(base + 8, struct.pack('<I', 1))
        uc.mem_write(base + 4, struct.pack('<I', rp))

    def run(self):
        try:
            self.uc.emu_start(self.entry, 0xFFFFFFFF)
        except UcError as e:
            eip = self.uc.reg_read(UC_X86_REG_EIP)
            sys.stderr.write('x86run: %s at eip=%#x\n' % (e, eip))
            return 139
        if os.environ.get('X86RUN_STATS'):
            sys.stderr.write('[x86run: mapped now %d KiB, peak %d KiB]\n' % (self.mapped // 1024, self.peak // 1024))
        return self.code if self.code is not None else 0

def main():
    args = sys.argv[1:]
    trace = False
    if args and args[0] == '--trace-syscalls': trace = True; args = args[1:]
    if not args: raise SystemExit(__doc__)
    sys.exit(Emu(args[0], args, trace).run())

if __name__ == '__main__':
    main()
