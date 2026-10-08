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
            self.kinit(path)
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

    def cstr_bytes(self, addr):
        out = b''
        while True:
            chunk = bytes(self.uc.mem_read(addr, 64))
            i = chunk.find(b'\0')
            if i >= 0: return out + chunk[:i]
            out += chunk; addr += 64

    def cstr(self, addr): return self.cstr_bytes(addr).decode('utf-8', 'replace')

    def intr(self, uc, intno, _):
        if intno == 0x40 and self.kolibri:
            r = [uc.reg_read(x) for x in (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP)]
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

    # ---- KolibriOS (int 0x40): the functions the runtime uses, the shell console,
    # and a small deterministic KolibriOS behind examples/kolibri.mpy (written from
    # kernel/trunk/docs/sysfuncs.txt): a 1024x768 screen, 12:34:56 on 2026-10-08,
    # a few processes, a network card, a clipboard, ... Calls that change
    # something are logged as "[kos] ...", drawing as "[gui] ...". /tmp0/1 is a
    # RAM disk (a private host folder holding a few files); other paths are the
    # host's, relative ones from the current directory.
    SHELL_RING, SHELL_SIZE = 1040, 15344
    PID, SLOT = 4242, 4
    TMP0 = '/tmp0/1'
    TMP0_FILES = {'hello.txt': b'Hello from the RAM disk!\nSecond line.\n',
                  'docs/readme.txt': b'MiniPy on KolibriOS\n\nA file browser example.\n',
                  'docs/notes.txt': b'notes',
                  'docs/old/': None,
                  'kernel.mnt': bytes(range(256)) * 12,
                  'pics/': None}
    STAMP = (2026, 1, 2, 3, 4, 5)                            # every file's times (y, m, d, h, min, s)

    def kinit(self, path):
        self.board = []                                      # the debug board (fn 63)
        self.events = []
        for x in os.environ.get('X86RUN_EVENTS', '1,3').split(','):
            if x: code, _, arg = x.partition(':'); self.events.append((int(code), arg))
        self.button, self.key = 1, None
        self.mouse = (0, 0, 0, 0)                           # window x, y, fn 37.3 bits, wheel
        self.window = (0, 0, 0, 0)
        self.event_mask = 7
        name = os.path.splitext(os.path.basename(path))[0][:11]
        self.slots = {1: ('IDLE', 1, 0), 2: ('OS', 2, 0), 3: ('@TASKBAR', 3, 5),
                      4: (name, self.PID, 0), 5: ('', 0, 9), 6: ('SHELL', 4300, 5)}
        self.sizes = {}                                      # heap blocks
        self.shm = {}
        self.clipboard = [struct.pack('<III', 12 + 14, 0, 1) + b'fake clipboard']
        self.ipc = None
        self.cwd = self.TMP0
        self.attrs = {}                                      # host path -> attributes set by 70.6
        self.cursors = 0
        self.prev_cursor = 0
        self.futexes = {}
        self.pipe = b''
        self.sockets = 2
        self.tmp0 = None

    def rd(self, addr, n): return bytes(self.uc.mem_read(addr, n)) if n else b''
    def rd32(self, addr): return struct.unpack('<I', self.rd(addr, 4))[0]
    def wr(self, addr, data): self.uc.mem_write(addr, bytes(data))
    def wr32(self, addr, *vals): self.wr(addr, struct.pack('<%dI' % len(vals), *[v & 0xFFFFFFFF for v in vals]))
    def klog(self, text): self.shell_poll(); os.write(1, ('[kos] ' + text + '\n').encode('utf-8', 'replace'))
    def glog(self, text): self.shell_poll(); os.write(1, ('[gui] ' + text + '\n').encode('utf-8', 'replace'))
    def regs(self, eax, **rest):
        """eax is the return value; the other registers are set here."""
        names = {'ebx': UC_X86_REG_EBX, 'ecx': UC_X86_REG_ECX, 'edx': UC_X86_REG_EDX, 'esi': UC_X86_REG_ESI, 'edi': UC_X86_REG_EDI}
        for r, v in rest.items(): self.uc.reg_write(names[r], v & 0xFFFFFFFF)
        return eax

    def kstr(self, addr, enc=0):
        """A string argument: encoding `enc` (80's codes), or a leading byte 1-3, or cp866."""
        if enc == 0:
            first = self.rd(addr, 1)[0]
            if first in (1, 2, 3): enc, addr = first, addr + 1
            else: enc = 1
        if enc == 2:
            out = b''
            while True:
                w = self.rd(addr, 2); addr += 2
                if w == b'\0\0': return out.decode('utf-16-le', 'replace')
                out += w
        return self.cstr_bytes(addr).decode('cp866' if enc == 1 else 'utf-8', 'replace')

    def host_path(self, path):
        if path.lower() == self.TMP0 or path.lower().startswith(self.TMP0 + '/'):
            if self.tmp0 is None:
                import atexit, shutil, tempfile
                self.tmp0 = tempfile.mkdtemp(prefix='x86run-tmp0-')
                atexit.register(shutil.rmtree, self.tmp0, True)
                for name, data in self.TMP0_FILES.items():
                    p = os.path.join(self.tmp0, name)
                    os.makedirs(os.path.dirname(p), exist_ok=True)
                    if data is None: os.makedirs(p, exist_ok=True)
                    else:
                        with open(p, 'wb') as fh: fh.write(data)
            return self.tmp0 + path[len(self.TMP0):]
        return path

    def kalloc(self, size, data=None):
        size = up(size + 16); addr = self.heap_next; self.heap_next += size + PAGE
        self.map(addr, addr + size); self.mapped += size; self.peak = max(self.peak, self.mapped)
        self.sizes[addr] = size
        if data: self.wr(addr, data)
        return addr

    def kcall(self, a, b, c, d, si, di, bp):
        uc = self.uc
        if a == 0xFFFFFFFF: raise Exit(b & 0xFF)             # -1: terminate
        if a == 5:                                           # sleep (1/100 s)
            self.shell_poll(); time.sleep(b / 100.0); return None
        if a == 26 and b == 9: return int(time.monotonic() * 100) & 0xFFFFFFFF
        if a == 9: return self.thread_info(b, c)
        if a == 63 and b == 1: self.shell_poll(); os.write(2, bytes([c & 0xFF])); self.board.append(c & 0xFF); return None
        if a == 68:
            if b == 1: self.shell_poll(); return None        # yield
            if b == 11: return 0x1000000                     # heap init
            if b == 12: return self.kalloc(c)                # alloc
            if b == 13: return 1                             # free
            if b == 22:                                      # open shared memory
                name = self.cstr(c)
                if name.endswith('-SHELL'):
                    self.shell = self.kalloc(d); return self.shell
                if name in self.shm:
                    if si & 8: return self.regs(0, edx=10)
                    return self.regs(self.shm[name][0], edx=self.shm[name][1])
                if not si & 12: return self.regs(0, edx=5)
                self.shm[name] = (self.kalloc(d), d)
                self.klog('shared memory %r: %d bytes' % (name, d))
                return self.regs(self.shm[name][0], edx=0)
            if b == 23: self.klog('shared memory %r closed' % self.cstr(c)); return 0
        if a in (70, 80): return self.f70(b, a == 80)
        for part in (self.gui, self.api):
            r = part(a, b, c, d, si, di, bp)
            if r is not False: return r
        sys.stderr.write('x86run: unsupported KolibriOS function %d.%d\n' % (a if a < 2**31 else a - 2**32, b))
        return 0xFFFFFFFF

    def thread_info(self, buf, slot):
        if slot == 0xFFFFFFFF: slot = self.SLOT
        info = bytearray(1024)
        name, pid, status = self.slots.get(slot, ('', 0, 9))
        struct.pack_into('<IHH', info, 0, 10 * slot, slot, slot)
        info[10:21] = name.encode().ljust(11)[:11]
        if slot == self.SLOT:
            x, y, w, h = self.window
            struct.pack_into('<IIIIIII', info, 22, 0, 0x7FFFF, pid, x, y, w, h)
            struct.pack_into('<IIII', info, 54, 5, 24, max(w - 9, 0), max(h - 29, 0))
            struct.pack_into('<BIB', info, 70, 0, self.event_mask, 0)
        else:
            struct.pack_into('<I', info, 30, pid)
        struct.pack_into('<H', info, 50, status)
        self.wr(buf, info)
        return max(self.slots)

    # A headless desktop: window calls are logged, events come from X86RUN_EVENTS
    # ("1,3" by default: redraw, then a press of button 1 - the close box).
    # An event may carry what it delivers: "3:2" presses button 2, "2:113" key 'q',
    # "6:x/y/bits[/wheel]" the mouse at (x, y) in the window with those fn 37.3 bits.
    def gui(self, a, b, c, d, si, di, bp):
        log = self.glog
        xy = lambda v: (v >> 16, v & 0xFFFF)
        if a == 12: log('redraw %s' % ('begin' if b == 1 else 'end')); return None
        if a == 0:
            self.window = (b >> 16, c >> 16, b & 0xFFFF, c & 0xFFFF)
            log('window x=%d w=%d y=%d h=%d style=%#x caption=%r' % (b >> 16, b & 0xFFFF, c >> 16, c & 0xFFFF, d, self.cstr(di) if di else '')); return None
        if a == 4:
            text = self.cstr_bytes(d) if c & 0x80000000 else self.rd(d, si)
            extra = ''
            if c >> 24: extra = ' flags=%#x' % (c >> 24) + (' background=%#x' % di if c & 0x40000000 else '') + (' canvas %dx%d' % struct.unpack('<II', self.rd(di, 8)) if c & 0x08000000 else '')
            log('text x=%d y=%d color=%#x %r%s' % (b >> 16, b & 0xFFFF, c & 0xFFFFFF, text.decode('utf-8' if (c >> 28) & 3 == 3 else 'latin-1', 'replace'), extra)); return None
        if a == 8:
            if d & 0x80000000: log('button %d deleted' % (d & 0xFFFFFF)); return None
            log('button x=%d w=%d y=%d h=%d id=%d color=%#x' % (b >> 16, b & 0xFFFF, c >> 16, c & 0xFFFF, d, si)); return None
        if a == 13: log('bar x=%d w=%d y=%d h=%d color=%#x' % (b >> 16, b & 0xFFFF, c >> 16, c & 0xFFFF, d)); return None
        if a == 71:
            if c == 0: log('title removed'); return None
            log('title %r' % self.kstr(c, d if b == 2 else 0) + (' (encoding %d)' % d if b == 2 else '')); return None
        if a == 1: log('pixel x=%d y=%d %s' % (b, c, 'inverted' if d & 0x01000000 else 'color=%#x' % d)); return None
        if a == 38: log('line %d,%d - %d,%d %s' % (b >> 16, c >> 16, b & 0xFFFF, c & 0xFFFF, 'inverted' if d & 0x01000000 else 'color=%#x' % d)); return None
        if a == 7: log('image %dx%d at %d,%d bytes=%s' % (xy(c) + xy(d) + (self.rd(b, 6).hex(),))); return None
        if a == 25: log('background-layer image %dx%d at %d,%d bytes=%s' % (xy(c) + xy(d) + (self.rd(b, 8).hex(),))); return None
        if a == 65:
            log('image %dx%d at %d,%d bpp=%d palette=%s row_offset=%d first=%s' % (xy(c) + xy(d) + (si, self.rd(di, 8).hex() if di else '-', bp, self.rd(b, 4).hex()))); return None
        if a == 47:
            digits, base = (b >> 16) & 63, {0: 10, 1: 16, 2: 2}.get((b >> 8) & 255, 10)
            log('number %d base %d digits=%d%s at %d,%d color=%#x flags=%#x' % (c if c < 2**31 else c - 2**32, base, digits, ' no-leading-zeros' if b & 0x80000000 else '', d >> 16, d & 0xFFFF, si & 0xFFFFFF, si >> 24)); return None
        if a == 73:
            p = struct.unpack('<10I', self.rd(c, 40))
            log('blit flags=%#x to %d,%d %dx%d from %d,%d %dx%d row=%d first=%s' % ((b,) + p[:8] + (p[9], self.rd(p[8], 4).hex()))); return None
        if a == 67: log('move window x=%d y=%d w=%d h=%d' % tuple(v - 2**32 if v >= 2**31 else v for v in (b, c, d, si))); return None
        if a == 50:
            if b == 0: log('window shape %s' % (self.rd(c, 8).hex() if c else 'reset')); return None
            if b == 1: log('window shape scale %d' % c); return None
        if a == 40:
            old, self.event_mask = self.event_mask, b
            self.klog('event mask %#x' % b); return old
        if a in (10, 11, 23):
            self.shell_poll()
            if not self.events: raise Exit(3)                # out of scripted events
            code, arg = self.events.pop(0)
            if code == 3: self.button = int(arg) if arg else 1
            if code == 2: self.key = int(arg) if arg else None
            if code == 6:
                v = [int(x, 0) for x in arg.split('/')] + [0]
                self.mouse = (v[0], v[1], v[2], v[3])
            return code
        if a == 17: return self.button << 8                  # the pressed button (1 unless the event says)
        if a == 2:                                           # the key: al = 0, ah = its code (1: none)
            k, self.key = self.key, None
            return 1 if k is None else k << 8
        if a == 37:
            mx, my, bits, wheel = self.mouse
            if b == 0: return (max(self.window[0] + mx, 0) << 16) + max(self.window[1] + my, 0)
            if b == 1: return ((mx << 16) + my) & 0xFFFFFFFF
            if b == 2: return bits & 31
            if b == 3: return bits
            if b == 7: self.mouse = (mx, my, bits, 0); return wheel & 0xFFFF
            if b == 4:
                if d & 0xFFFF == 2: self.klog('cursor image, hotspot %d,%d' % (d >> 24, (d >> 16) & 255))
                else: self.klog('cursor file %r' % self.kstr(c))
                self.cursors += 1; return 0x4000 + self.cursors
            if b == 8: self.klog('cursor file %r (encoding %d)' % (self.kstr(c, d), d)); self.cursors += 1; return 0x4000 + self.cursors
            if b == 5: old, self.prev_cursor = self.prev_cursor, c; self.klog('cursor %#x' % c); return old
            if b == 6: self.klog('cursor %#x deleted' % c); return 0
        return False

    def api(self, a, b, c, d, si, di, bp):
        log = self.klog
        s32 = lambda v: v - 2**32 if v >= 2**31 else v
        bcd = lambda v: ((v >> 4) & 15) * 10 + (v & 15)
        if a == 3: return 0x563412                           # 12:34:56 (BCD: ss mm hh)
        if a == 29: return 0x081026                          # 2026-10-08 (BCD: dd mm yy)
        if a == 14: return (1023 << 16) | 767
        if a == 22:
            what = {0: 'time', 1: 'date', 2: 'day of week', 3: 'alarm'}[b]
            if b == 1: log('set date 20%02d-%02d-%02d' % (bcd(c), bcd(c >> 8), bcd(c >> 16)))
            elif b == 2: log('set day of week %d' % c)
            else: log('set %s %02d:%02d:%02d' % (what, bcd(c), bcd(c >> 8), bcd(c >> 16)))
            return 0
        if a == 15:
            if b == 1: log('background size %dx%d' % (c, d)); self.bg = (c, d); return None
            if b == 2: log('background pixel at offset %d color=%#x' % (c, d)); return None
            if b == 3: log('background redraw'); return None
            if b == 4: log('background mode %d' % c); return None
            if b == 5: log('background image %d bytes at offset %d: %s' % (si, d, self.rd(c, min(si, 6)).hex())); return None
            if b == 6: return self.kalloc(800 * 600 * 3, bytes([1, 2, 3]))
            if b == 7: return 1
            if b == 8: return self.regs((10 << 16) | 400, ebx=(20 << 16) | 300)
            if b == 9: log('background redraw %d,%d - %d,%d' % (c >> 16, d >> 16, c & 0xFFFF, d & 0xFFFF)); return None
        if a == 39:
            if b == 1: return (800 << 16) | 600
            if b == 2: return (c // 3) & 0xFFFFFF
            if b == 3:
                w, h = d >> 16, d & 0xFFFF
                self.wr(si, b''.join(struct.pack('<I', (c >> 16) + i) for i in range(w * h))); return 0
            if b == 4: return 2
        if a == 16: log('save ramdisk to floppy %d' % b); return 0
        if a == 18:
            if b in (1, 2, 3): log('%s slot %d' % ({1: 'unfocus', 2: 'kill', 3: 'focus'}[b], c)); return 0
            if b == 4: return 1000
            if b == 5: return 2400000000
            if b == 6: log('save ramdisk to %r' % self.kstr(c)); return 0
            if b == 7: return self.SLOT
            if b == 8: return 0 if c == 1 else log('speaker toggled')
            if b == 9: log('shutdown %d' % c); return 0
            if b == 10: log('minimize'); return None
            if b == 13: self.wr(c, bytes([0, 7, 7, 0, 0, 0]) + struct.pack('<HIHH', 40, 0xf26d5b28, 0, 1675)); return None
            if b == 14: return 0
            if b == 15: log('mouse centered'); return None
            if b == 16: return 1 << 20
            if b == 17: return 2 << 20
            if b == 18: log('kill pid %d' % c); return 0 if c in [p for _, p, _ in self.slots.values()] else -1
            if b == 19:
                if c in (0, 2, 6): return {0: 3, 2: 4, 6: 50}[c]
                log('mouse setting %d: %#x' % (c, d)); return None
            if b == 20: self.wr32(c, 524288, 262144, 77, 1 << 24, 1 << 23, 300, 20, 4096, 65536); return 524288
            if b == 21: return {p: s for s, (_, p, st) in self.slots.items() if st != 9}.get(c, 0)
            if b == 22: log('%s %s %d' % ('minimize' if c in (0, 1) else 'restore', 'slot' if c in (0, 2) else 'pid', d)); return 0
            if b == 23: log('minimize all'); return 2
            if b == 24: log('screen limits %dx%d' % (c, d)); return None
            if b == 25:
                if c == 1: return 0
                log('window z-position %d for %d' % (s32(si), s32(d))); return 1
        if a == 21: log('setting %d.%d = %d%s' % (b, c, d if b == 2 else c, '' if b != 2 or c == 9 else ' table ' + self.rd(d, 4).hex())); return 0
        if a == 26:
            if b == 2:
                if c == 9: return 4
                self.wr(d, bytes((c * 16 + i) & 255 for i in range(128))); return None
            if b == 5: return 1
            if b == 10: return self.regs(0x2A05F27B, edx=1)    # 5000000123 ns
            if b == 11: return 1
            if b == 12: return 0
        if a == 30:
            if b in (1, 4): self.cwd = self.kstr(c, d if b == 4 else 0); log('current folder %r' % self.cwd); return None
            if b in (2, 5):
                data = self.cwd.encode('utf-8' if b == 5 else 'cp866', 'replace') + b'\0'
                self.wr(c, data[:d]); return len(data)
            if b == 3: log('system folder %r = %r' % (self.cstr(c), self.cstr(c + 64))); return None
        if a == 34: return self.SLOT
        if a == 35: return ((b % 1024) << 16 | (b // 1024) << 8 | 0x40) & 0xFFFFFF
        if a == 36:
            w, h = c >> 16, c & 0xFFFF
            self.wr(b, bytes((i * 7) & 255 for i in range(w * h * 3))); return None
        if a == 46: log('%s ports %#x-%#x' % ('free' if b else 'reserve', c, d)); return 0
        if a == 48:
            if b == 3: self.wr32(c, *[0x101010 * i for i in range(10)]); return None
            if b == 4: return 22
            if b == 5: return self.regs(1023, ebx=739)       # left 0, right 1023, top 0, bottom 739
            if b == 7: return self.regs((6 << 16) | 72, ebx=(3 << 16) | 3)
            if b == 9: return 1
            if b == 11: return 9
            if b in (8, 13): log('skin %r' % self.kstr(c, d if b == 13 else 0)); return 0
            if b == 2: log('window colors %s' % ' '.join('%06x' % v for v in struct.unpack('<10I', self.rd(c, 40)))); return None
            log('screen setting 48.%d = %#x %#x' % (b, c, d)); return None
        if a == 49: log('APM function %#x' % d); return 0
        if a == 51:
            if b == 2: return self.SLOT
            if b == 3: return 2
            if b == 4: log('priority of %d = %d' % (s32(c), d)); return 2
        if a == 54:
            if b == 0: return len(self.clipboard)
            if b == 1:
                if c >= len(self.clipboard): return 1
                return self.kalloc(len(self.clipboard[c]), self.clipboard[c])
            if b == 2:
                data = self.rd(d, c); self.clipboard.append(data)
                log('clipboard += type %d, %r' % (struct.unpack_from('<I', data, 4)[0], data[12:].decode('utf-8', 'replace'))); return 0
            if b == 3: return 0 if self.clipboard and not self.clipboard.pop() is None else 1
            if b == 4: return 0
        if a == 55: log('speaker data %s' % self.rd(si, 4).hex()); return 0
        if a == 57: log('PCI BIOS al=%#x' % (bp & 255)); return 0
        if a == 60:
            if b == 1: self.ipc = (c, d); log('IPC area of %d bytes' % d); return 0
            if b == 2:
                if c != self.PID: return 0 if c in [p for _, p, _ in self.slots.values()] else 4
                if self.ipc is None: return 1
                area, size = self.ipc
                lock, used = struct.unpack('<II', self.rd(area, 8))
                if lock: return 2
                if used + 8 + si > size: return 3
                self.wr(area + used, struct.pack('<II', self.PID, si) + self.rd(d, si))
                self.wr32(area + 4, used + 8 + si)
                self.events.insert(0, (7, ''))                 # the receiver gets event 7
                return 0
        if a == 61:
            return {1: (1024 << 16) | 768, 2: 32, 3: 4096}[b]
        if a == 62:
            sub, bus, devfn, reg = b & 255, (b >> 8) & 255, (c >> 8) & 255, c & 255
            if sub == 0: return 0x0300
            if sub == 1: return 2
            if sub == 2: return 1
            space = bytearray(256)
            if bus == 0 and devfn == 1 << 3: space[0:12] = struct.pack('<IHHI', 0x70008086, 7, 0x0280, 0x06010000)
            if sub in (4, 5, 6):
                if bus == 0 and devfn != 1 << 3: return 0xFFFFFFFF
                n = {4: 1, 5: 2, 6: 4}[sub]
                return int.from_bytes(space[reg:reg + n], 'little')
            if sub in (8, 9, 10): log('PCI write %d:%d.%d reg %#x = %#x' % (bus, devfn >> 3, devfn & 7, reg, d)); return 0
        if a == 63 and b == 2:
            if not self.board: return self.regs(0, ebx=0)
            return self.regs(self.board.pop(0), ebx=1)
        if a == 66:
            if b == 1: self.keymode = c; log('keyboard mode %d' % c); return None
            if b == 2: return getattr(self, 'keymode', 0)
            if b == 3: return 0x80
            if b in (4, 5): log('hotkey %s scancode %d modifiers %#x' % ('set' if b == 4 else 'deleted', c, d)); return 0
            if b in (6, 7): log('input %s' % ('locked' if b == 6 else 'unlocked')); return None
        if a == 68:
            if b == 0: return 123456
            if b == 2: return {0: 0x6F0, 1: 0}.get(c, None) if c < 2 else log('cache %s' % ('on' if c == 2 else 'off'))
            if b == 3: return self.regs(0x11223344, ebx=0x55)
            if b == 4: log('MSR %#x = %#x:%#x' % (d, si, di)); return None
            if b == 14: self.wr32(c, 3, 1, 2, 3, 4, 5); return None
            if b == 16: return 0x8000 if self.cstr(c) == 'SOUND' else 0
            if b == 17:
                h, code, inp, isz, out, osz = struct.unpack('<6I', self.rd(c, 24))
                log('driver %#x code %d input %s' % (h, code, self.rd(inp, isz).hex()))
                self.wr(out, self.rd(inp, isz)[::-1][:osz]); return 0
            if b in (18, 19): log('DLL %r' % self.kstr(c, d if b == 18 else 0)); return 0
            if b == 20:
                new = self.kalloc(c)
                if d: self.wr(new, self.rd(d, min(c, self.sizes.get(d, 0))))
                return new
            if b == 21: log('driver PE %r %r' % (self.kstr(c), self.kstr(d))); return 0x9000
            if b == 26: log('release pages %#x+%d, %d bytes' % (c, d, si)); return None
            if b in (27, 28):
                try:
                    with open(self.host_path(self.kstr(c, d if b == 28 else 0)), 'rb') as fh: data = fh.read()
                except OSError: return self.regs(0, edx=0)
                return self.regs(self.kalloc(len(data), data), edx=len(data))
            if b == 29: return self.kalloc(c)
            if b == 30: log('driver %#x unloaded' % c); return 0
            if b == 31:
                if c == 1: return self.regs(0, ebx=0x9100, ecx=0x9200)
                if c == 2:
                    if d not in (0x9100, 0x9200): return 0xFFFFFFFF
                    name = b'SOUND' if d == 0x9100 else b'USBHID'
                    self.wr(di, name.ljust(16, b'\0') + struct.pack('<5I', 0x9200 if d == 0x9100 else 0x9000, 0x9000 if d == 0x9100 else 0x9100, 0x80000000, 0x80000100, 0x80000200)); return 0
        if a == 69:
            if b == 1: self.wr32(si, 0x1000, 0x202, 1, 2, 3, 4, 0x7FF0, 0x7FF8, 5, 6); return None
            if b == 6: self.wr(di, bytes(range(d))); return d
            if b == 7: log('debug write %d bytes at %#x: %s' % (d, si, self.rd(di, d).hex())); return d
            if b == 9: log('breakpoint thread %d edx=%#x at %#x' % (c, d, si)); return 0
            log('debug %d for %d' % (b, c)); return None
        if a == 72: log('message: event %d, %d' % (c, d)); return 0
        if a == 74:
            sub, dev = b & 255, (b >> 8) & 255
            if sub == 255: return 2
            if dev > 1: return 0xFFFFFFFF
            if sub == 0: return dev
            if sub == 1: self.wr(c, (b'loopback' if dev == 0 else b'fakenet0') + b'\0'); return 0
            if sub in (2, 3): log('device %d %s' % (dev, 'reset' if sub == 2 else 'stopped')); return 0
            if sub == 4: return 0x80001000 + dev
            if sub in (8, 9): return self.regs(0x2A05F200 + sub, ebx=1)
            if sub == 10: return 10
            return 100 * dev + sub
        if a == 75:
            sub = b & 255
            sa = lambda p: '%d.%d.%d.%d:%d' % (tuple(self.rd(p + 4, 4)) + (struct.unpack('>H', self.rd(p + 2, 2))[0],))
            if sub == 0: self.sockets += 1; log('socket %d: domain %d type %d protocol %d' % (self.sockets, c, d, si)); return self.regs(self.sockets, ebx=0)
            if sub == 1: log('socket %d closed' % c); return self.regs(0, ebx=0)
            if sub in (2, 4): log('socket %d %s %s' % (c, 'bound to' if sub == 2 else 'connected to', sa(d))); return self.regs(0, ebx=0)
            if sub == 3: log('socket %d listens, backlog %d' % (c, d)); return self.regs(0, ebx=0)
            if sub == 5:
                self.wr(d, struct.pack('<H', 2) + struct.pack('>H', 5555) + bytes([10, 0, 0, 2]) + bytes(8))
                self.sockets += 1; return self.regs(self.sockets, ebx=0)
            if sub == 6: log('socket %d sends %r flags %#x' % (c, self.rd(d, si).decode('latin-1'), di)); return self.regs(si, ebx=0)
            if sub == 7:
                reply = b'HTTP/1.0 200 OK\r\n'[:si]; self.wr(d, reply); return self.regs(len(reply), ebx=0)
            if sub == 8: log('socket %d option %d/%d = %d' % ((c,) + struct.unpack('<II', self.rd(d, 8)) + struct.unpack('<I', self.rd(d + 12, 4)))); return self.regs(0, ebx=0)
            if sub == 9: self.wr32(d + 12, 1); return self.regs(0, ebx=0)
            if sub == 10: return self.regs(7, ebx=8)
            return self.regs(0xFFFFFFFF, ebx=22)
        if a == 76:
            proto, dev, sub = b >> 16, (b >> 8) & 255, b & 255
            if dev > 1: return 0xFFFFFFFF
            if proto == 0 and sub == 0: return self.regs(0x12005452, ebx=0x5634)
            ipv4 = {2: 0x0A00A8C0, 4: 0x08080808, 6: 0x00FFFFFF, 8: 0x0100A8C0}
            if proto == 1 and sub in ipv4: return ipv4[sub]
            if proto == 1 and sub in (3, 5, 7, 9):
                log('%s = %d.%d.%d.%d' % (({3: 'IP', 5: 'DNS', 7: 'subnet mask', 9: 'gateway'}[sub],) + tuple(struct.pack('<I', c)))); return 0
            if sub in (0, 1): return proto * 10 + sub
            if proto == 5:
                if sub == 2: return 1
                if sub == 3:
                    if c != 0: return 0xFFFFFFFF
                    self.wr(di, bytes([192, 168, 0, 1, 0x52, 0x54, 0, 0xaa, 0xbb, 0xcc]) + struct.pack('<HH', 1, 300)); return 0
                if sub == 4:
                    e = self.rd(si, 14)
                    log('ARP add %d.%d.%d.%d %s ttl %d' % (tuple(e[:4]) + (':'.join('%02x' % x for x in e[4:10]), struct.unpack_from('<H', e, 12)[0]))); return 0
                if sub == 5: log('ARP remove %d' % s32(c)); return 0
                if sub == 6: log('ARP announce'); return 0
                if sub == 7: return 0
        if a == 77:
            if b == 0: self.futexes[0x100 + len(self.futexes)] = c; log('futex created, value %d' % c); return 0xFF + len(self.futexes)
            if b == 1: return 0 if self.futexes.pop(c, None) is not None else 0xFFFFFFFF
            if b == 2: return 0xFFFFFFFE if self.futexes.get(c) != d else 0xFFFFFFFF
            if b == 3: return 0
            if b == 13: self.wr32(c, 7, 8); return 0
            if b == 11:
                if c != 8: return 0xFFFFFFF7
                self.pipe += self.rd(d, si); return si
            if b == 10:
                if c != 7: return 0xFFFFFFF7
                data, self.pipe = self.pipe[:si], self.pipe[si:]; self.wr(d, data); return len(data)
        return False

    def f70(self, info, enc80=False):
        """File system functions 70 and 80 on host files (see host_path)."""
        uc = self.uc
        sub, off, hi, size, buf = struct.unpack('<5I', self.rd(info, 20))
        if enc80: path = self.kstr(self.rd32(info + 24), self.rd32(info + 20))
        elif self.rd(info + 20, 1) == b'\0': path = self.kstr(self.rd32(info + 21))
        else: path = self.kstr(info + 20)
        host = self.host_path(path)
        def stamp(t):
            y, mo, dd, h, mi, sec = t
            return bytes([sec, mi, h, 0, dd, mo]) + struct.pack('<H', y)
        def bdfe(p, enc, name=None):
            st = os.stat(p)
            attr = self.attrs.get(p, (0x10 if os.path.isdir(p) else 0x20, self.STAMP))
            kind = 0x10 if os.path.isdir(p) else 0
            data = struct.pack('<II', (attr[0] & ~0x18) | kind, enc) + stamp(attr[1]) * 3 + struct.pack('<Q', 0 if kind else st.st_size)
            if name is None: return data
            if enc in (0, 1): return data + name.encode('cp866', 'replace')[:263].ljust(264, b'\0')
            if enc == 2: return data + name.encode('utf-16-le')[:518].ljust(520, b'\0')
            return data + name.encode('utf-8')[:519].ljust(520, b'\0')
        try:
            if sub == 0:
                if os.path.isdir(host): return 10
                with open(host, 'rb') as fh:
                    fh.seek(off); data = fh.read(size)
                self.wr(buf, data)
                return self.regs(0 if len(data) == size else 6, ebx=len(data))
            if sub == 1:
                names = sorted(os.listdir(host))
                if path.rstrip('/') not in ('', self.TMP0): names = ['.', '..'] + names
                part = names[off:off + size]
                blocks = b''.join(bdfe(os.path.join(host, n) if n not in ('.', '..') else host, hi, n) for n in part)
                self.wr(buf, struct.pack('<III', 1, len(part), len(names)) + bytes(20) + blocks)
                return self.regs(0 if len(part) == size else 6, ebx=len(part))
            if sub == 2:
                with open(host, 'wb') as fh: fh.write(self.rd(buf, size))
                return self.regs(0, ebx=size)
            if sub == 3:
                with open(host, 'r+b') as fh:
                    fh.seek(off); fh.write(self.rd(buf, size))
                return self.regs(0, ebx=size)
            if sub == 4: os.truncate(host, off); return 0
            if sub == 5: self.wr(buf, bdfe(host, 0)); return 0
            if sub == 6:
                attr = self.rd(buf, 32)
                t = lambda o: (struct.unpack_from('<H', attr, o + 6)[0], attr[o + 5], attr[o + 4], attr[o + 2], attr[o + 1], attr[o])
                os.stat(host); self.attrs[host] = (struct.unpack_from('<I', attr)[0], t(24))
                self.klog('attributes of %r = %#x' % (path, self.attrs[host][0])); return 0
            if sub == 7:
                if not os.path.isfile(host): return -5
                self.klog('run %r %r%s' % (path, self.cstr(hi) if hi else '', ' (debugged)' if off & 1 else '')); return 5000
            if sub == 8:
                if os.path.isdir(host):
                    if os.listdir(host): return 10
                    os.rmdir(host)
                else: os.remove(host)
                return 0
            if sub == 9:
                if not os.path.isdir(host): os.mkdir(host)
                return 0
            if sub == 10:
                new = self.kstr(buf)
                target = self.host_path(new) if new.startswith('/') else os.path.join(os.path.dirname(host), new)
                os.rename(host, target); return 0
            if sub == 11: os.symlink(self.kstr(buf), host); return 0
            if sub == 12:
                data = os.readlink(host).encode()[:size - 1] + b'\0'
                self.wr(buf, data); return self.regs(0, ebx=len(data))
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
