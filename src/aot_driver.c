/* ========================= AOT driver: minipy --compile =========================
   1. import pass: parse the entry script and every module it imports. Imports
      are resolved here, at compile time, the same way the interpreter resolves
      them at run time (relative to the importing file, `<name>.mpy` or
      `<name>/__init__.mpy`, namespace packages for intermediate components);
      therefore every import must sit at the top of its module.
   2. type check + code generation: all modules -> one fasm listing
      (aot_types.c, aot_codegen.c), runtime routines included.
   3. fasm turns the listing straight into the executable (Linux ELF or
      KolibriOS application): no linker, no runtime library to install. The
      same works inside KolibriOS with its own fasm. */

#include "aot.h"
#include "pystdlib.h"
#include "py_front.h"
#include "fs.h"

/* macos: the C compiler; override with --cc/--cc-args or MPY_CC/MPY_CC_ARGS. */
#ifndef MPY_AOT_CC
#  define MPY_AOT_CC "cc"
#endif
#ifndef MPY_AOT_CC_ARGS            /* {in} {out} {dir} are substituted */
#  define MPY_AOT_CC_ARGS "-O1 -w {in} -o {out} -lm"
#endif

/* Defaults; override with --fasm/--fasm-args or MPY_FASM/MPY_FASM_ARGS. */
#ifndef MPY_AOT_FASM
#  if defined(MPY_KOLIBRI)
#    define MPY_AOT_FASM "/sys/develop/fasm"
#  else
#    define MPY_AOT_FASM "fasm"
#  endif
#endif
#ifndef MPY_AOT_FASM_ARGS          /* {in} {out} {dir} are substituted */
#  if defined(MPY_KOLIBRI)
#    define MPY_AOT_FASM_ARGS "{in},{out},{dir}"
#  else
#    define MPY_AOT_FASM_ARGS "-m 262144 {in} {out}"
#  endif
#endif

static void usage(const char *program){
    fprintf(stderr,
        "usage: %s --compile [options] file.mpy\n"
        "  -o FILE          output executable (default: the script name without .mpy)\n"
        "  --target T       linux (i386 ELF), kolibri (KolibriOS application) or macos\n"
        "                   (native: the listing translated to C, FILE.c, built by cc);\n"
        "                   default: the system minipy runs on\n"
        "  -S               only write the assembly listing (FILE.asm)\n"
        "  --fasm PATH      fasm executable                       [env MPY_FASM]\n"
        "  --fasm-args A    fasm arguments, {in} {out} {dir} replaced  [env MPY_FASM_ARGS]\n"
        "  --cc PATH        macos: C compiler                     [env MPY_CC]\n"
        "  --cc-args A      macos: its arguments, {in} {out} {dir} replaced  [env MPY_CC_ARGS]\n"
        "  --stack BYTES    kolibri: application stack size (default 65536)\n"
        "  -v               print the commands being run\n"
        "  --count-allocs   debugging: the program reports its live heap blocks at exit\n"
        "Compiled programs are statically typed: every variable, parameter, field and\n"
        "container element keeps one type (annotated or inferred), None is the zero\n"
        "value of that type, and imports must be at the top of a module.\n",
        program);
}

/* ---------------------------------------------------------------- import pass */

typedef struct { AotUnit **v; int n, cap; } Units;

static AotUnit *find_unit(Units *us, const char *name){ for(int i=0;i<us->n;i++) if(!strcmp(us->v[i]->name,name)) return us->v[i]; return NULL; }
/* `if __name__ == "__main__":` at the top level of a module */
static int is_main_guard(AotUnit *u, Stmt *s){
    (void)u;
    Expr *e=s->expr;
    if(s->kind!=STMT_IF || !e || e->kind!=EXPR_COMPARE || e->count!=2 || e->items[1]->akind!=CMP_EQ) return 0;
    Expr *a=e->items[0], *b=e->items[1];
    if(a->kind!=EXPR_NAME){ Expr *t=a; a=b; b=t; }
    return a->kind==EXPR_NAME && !strcmp(a->name,"__name__") && b->kind==EXPR_LITERAL && b->tok->kind==T_STRING && !strcmp(b->tok->text,"__main__");
}
static int is_docstring(AotUnit *u, Stmt *s);
/* The guard's body runs in the program's main module and never in an imported
   one: it is put in place of the if (or its else part is). Imports in it move
   up to the module's other imports - a compiled module imports at its top. */
static void apply_main_guards(AotUnit *u, int is_main){
    Ast *m=u->ast; int n=m->body_count, header=0, any=0;
    for(int i=0;i<n;i++) if(is_main_guard(u,m->body[i])) any=1;
    if(!any) return;
    while(header<n && (m->body[header]->kind==STMT_IMPORT || m->body[header]->kind==STMT_FROM_IMPORT || (header==0 && is_docstring(u,m->body[0])))) header++;
    int cap=n; for(int i=0;i<n;i++) if(is_main_guard(u,m->body[i])) cap+=m->body[i]->body_count+m->body[i]->orelse_count;
    Stmt **out=MPY_NEW_ARR(Stmt*,cap), **rest=MPY_NEW_ARR(Stmt*,cap); int k=0, r=0;
    for(int i=0;i<header;i++) out[k++]=m->body[i];
    for(int i=header;i<n;i++){ Stmt *s=m->body[i];
        if(!is_main_guard(u,s)){ rest[r++]=s; continue; }
        Stmt **b=is_main?s->body:s->orelse; int bn=is_main?s->body_count:s->orelse_count;
        for(int j=0;j<bn;j++){
            if(b[j]->kind==STMT_IMPORT || b[j]->kind==STMT_FROM_IMPORT) out[k++]=b[j];
            else rest[r++]=b[j];
        }
    }
    for(int i=0;i<r;i++) out[k++]=rest[i];
    free(rest);
    m->body=out; m->body_count=k; m->body_cap=cap;
}
static AotUnit *add_unit(Units *us, const char *name, const char *path, char *src){
    AotUnit *u=MPY_NEW0(AotUnit);
    u->name=xstrdup2(name); u->path=path?xstrdup2(path):NULL; u->src=src;
    if(src){
        if(!(u->ast=py_front(path,src))) exit(1);
        apply_main_guards(u,!strcmp(name,"__main__"));
    }
    if(us->n==us->cap){ us->cap=us->cap?us->cap*2:8; us->v=(AotUnit**)xrealloc(us->v,sizeof(AotUnit*)*(size_t)us->cap); }
    us->v[us->n++]=u;
    return u;
}
static int is_builtin_module(const char *name){
    static const char *mods[]={"sys","thread","asyncio","math","typing","functools","__future__","collections.abc","ctypes","json","minipy","dataclasses","abc",NULL};   /* (thread: above) */
    for(int i=0;mods[i];i++) if(!strcmp(mods[i],name)) return 1;
    return 0;
}

/* BaseExceptionGroup / ExceptionGroup and the steps of except*: compiled with programs that use them */
static const char EG_PRELUDE[]=
"from typing import Callable\n"
"\n"
"class BaseExceptionGroup(Exception):\n"
"    def __init__(self, message: str, exceptions: list[BaseException]):\n"
"        n = len(exceptions)\n"
"        if n == 0:\n"
"            raise ValueError(\"second argument (exceptions) must be a non-empty sequence\")\n"
"        super().__init__(message + \" (\" + str(n) + \" sub-exception\" + (\"s\" if n > 1 else \"\") + \")\")\n"
"        self.message = message\n"
"        self.exceptions = tuple(exceptions)\n"
"        self.as_tuple = False\n"
"    def derive(self, excs: list[BaseException]) -> \"BaseExceptionGroup\":\n"
"        return BaseExceptionGroup(self.message, excs)\n"
"    def split(self, pred: Callable[[BaseException], bool]) -> tuple[\"BaseExceptionGroup | None\", \"BaseExceptionGroup | None\"]:\n"
"        if pred(self):\n"
"            return (self, None)\n"
"        match: list[BaseException] = []\n"
"        rest: list[BaseException] = []\n"
"        for e in self.exceptions:\n"
"            if isinstance(e, BaseExceptionGroup):\n"
"                m, r = e.split(pred)\n"
"                if m is not None:\n"
"                    match.append(m)\n"
"                if r is not None:\n"
"                    rest.append(r)\n"
"            elif pred(e):\n"
"                match.append(e)\n"
"            else:\n"
"                rest.append(e)\n"
"        mg: BaseExceptionGroup | None = None\n"
"        rg: BaseExceptionGroup | None = None\n"
"        if match:\n"
"            mg = self.derive(match)\n"
"        if rest:\n"
"            rg = self.derive(rest)\n"
"        return (mg, rg)\n"
"    def subgroup(self, pred: Callable[[BaseException], bool]) -> \"BaseExceptionGroup | None\":\n"
"        return self.split(pred)[0]\n"
"    def __repr__(self) -> str:\n"
"        inner = \", \".join([repr(e) for e in self.exceptions])\n"
"        return type(self).__name__ + \"(\" + repr(self.message) + \", \" + (\"(\" + inner + \",)\" if self.as_tuple else \"[\" + inner + \"]\") + \")\"\n"
"\n"
"class ExceptionGroup(BaseExceptionGroup):\n"
"    def __init__(self, message: str, exceptions: list[BaseException]):\n"
"        for e in exceptions:\n"
"            if not isinstance(e, Exception):\n"
"                raise TypeError(\"Cannot nest BaseExceptions in an ExceptionGroup\")\n"
"        super().__init__(message, exceptions)\n"
"    def derive(self, excs: list[BaseException]) -> BaseExceptionGroup:\n"
"        return ExceptionGroup(self.message, excs)\n"
"\n"
"def __mpy_leaves(e: BaseException, out: list[BaseException]) -> None:\n"
"    if isinstance(e, BaseExceptionGroup):\n"
"        for x in e.exceptions:\n"
"            __mpy_leaves(x, out)\n"
"    else:\n"
"        out.append(e)\n"
"\n"
"class __mpy_EGState:\n"
"    def __init__(self, exc: BaseException):\n"
"        self.orig = exc\n"
"        self.naked = not isinstance(exc, BaseExceptionGroup)\n"
"        self.rest: BaseException | None = exc\n"
"        self.raised: list[BaseException] = []\n"
"        self.again: list[bool] = []\n"
"        self.matched: BaseException | None = None\n"
"    def match(self, pred: Callable[[BaseException], bool]) -> BaseExceptionGroup | None:\n"
"        rest = self.rest\n"
"        self.matched = None\n"
"        if rest is None:\n"
"            return None\n"
"        if self.naked:\n"
"            if not pred(rest):\n"
"                return None\n"
"            g = ExceptionGroup(\"\", [rest])\n"
"            g.as_tuple = True\n"
"            self.rest = None\n"
"            self.matched = g\n"
"            return g\n"
"        if not isinstance(rest, BaseExceptionGroup):\n"
"            return None\n"
"        m, r = rest.split(pred)\n"
"        self.rest = r\n"
"        self.matched = m\n"
"        return m\n"
"    def handler_raised(self, x: BaseException) -> None:\n"
"        self.raised.append(x)\n"
"        self.again.append(x is self.matched)\n"
"    def end(self) -> None:\n"
"        keep: list[BaseException] = []\n"
"        rest = self.rest\n"
"        if rest is not None:\n"
"            if self.naked:\n"
"                keep.append(rest)\n"
"            else:\n"
"                __mpy_leaves(rest, keep)\n"
"        new: list[BaseException] = []\n"
"        for i in range(len(self.raised)):\n"
"            if self.again[i]:\n"
"                __mpy_leaves(self.raised[i], keep)\n"
"            else:\n"
"                new.append(self.raised[i])\n"
"        result: BaseException | None = None\n"
"        if keep:\n"
"            orig = self.orig\n"
"            if self.naked:\n"
"                result = orig\n"
"            elif isinstance(orig, BaseExceptionGroup):\n"
"                result = orig.split(lambda e: e in keep)[0]\n"
"        if new:\n"
"            if result is not None:\n"
"                new.append(result)\n"
"            if len(new) == 1 and result is None:\n"
"                raise new[0]\n"
"            raise ExceptionGroup(\"\", new)\n"
"        if result is not None:\n"
"            raise result\n";
/* complex and cmath: written in Python, compiled with programs that use them */
static const char CMATH_PRELUDE[]=
"import math\n"
"\n"
"pi = math.pi\n"
"e = math.e\n"
"tau = math.tau\n"
"inf = math.inf\n"
"nan = math.nan\n"
"\n"
"def _fmt(x: float) -> str:\n"
"    s = repr(x)\n"
"    if s.endswith(\".0\"):\n"
"        s = s[:-2]\n"
"    return s\n"
"\n"
"class complex:\n"
"    def __init__(self, real: float = 0.0, imag: float = 0.0):\n"
"        self.real = real\n"
"        self.imag = imag\n"
"    def __add__(self, other: \"complex\") -> \"complex\":\n"
"        return complex(self.real + other.real, self.imag + other.imag)\n"
"    def __radd__(self, other: \"complex\") -> \"complex\":\n"
"        return complex(other.real + self.real, other.imag + self.imag)\n"
"    def __sub__(self, other: \"complex\") -> \"complex\":\n"
"        return complex(self.real - other.real, self.imag - other.imag)\n"
"    def __rsub__(self, other: \"complex\") -> \"complex\":\n"
"        return complex(other.real - self.real, other.imag - self.imag)\n"
"    def __mul__(self, other: \"complex\") -> \"complex\":\n"
"        return complex(self.real * other.real - self.imag * other.imag, self.real * other.imag + self.imag * other.real)\n"
"    def __rmul__(self, other: \"complex\") -> \"complex\":\n"
"        return complex(other.real * self.real - other.imag * self.imag, other.real * self.imag + other.imag * self.real)\n"
"    def __truediv__(self, other: \"complex\") -> \"complex\":\n"
"        return _div(self.real, self.imag, other.real, other.imag)\n"
"    def __rtruediv__(self, other: \"complex\") -> \"complex\":\n"
"        return _div(other.real, other.imag, self.real, self.imag)\n"
"    def __pow__(self, other: \"complex\") -> \"complex\":\n"
"        return _pow(self.real, self.imag, other.real, other.imag)\n"
"    def __rpow__(self, other: \"complex\") -> \"complex\":\n"
"        return _pow(other.real, other.imag, self.real, self.imag)\n"
"    def __neg__(self) -> \"complex\":\n"
"        return complex(-self.real, -self.imag)\n"
"    def __pos__(self) -> \"complex\":\n"
"        return complex(self.real, self.imag)\n"
"    def __abs__(self) -> float:\n"
"        return math.hypot(self.real, self.imag)\n"
"    def __eq__(self, other: \"complex\") -> bool:\n"
"        return self.real == other.real and self.imag == other.imag\n"
"    def __ne__(self, other: \"complex\") -> bool:\n"
"        return self.real != other.real or self.imag != other.imag\n"
"    def __hash__(self) -> int:\n"
"        if self.imag == 0.0:\n"
"            return hash(self.real)\n"
"        return hash((self.real, self.imag))\n"
"    def __bool__(self) -> bool:\n"
"        return self.real != 0.0 or self.imag != 0.0\n"
"    def conjugate(self) -> \"complex\":\n"
"        return complex(self.real, -self.imag)\n"
"    def __repr__(self) -> str:\n"
"        if self.real == 0.0 and math.copysign(1.0, self.real) == 1.0:\n"
"            return _fmt(self.imag) + \"j\"\n"
"        im = _fmt(self.imag)\n"
"        return \"(\" + _fmt(self.real) + (\"\" if im[0] == \"-\" else \"+\") + im + \"j)\"\n"
"\n"
"def _div(ar: float, ai: float, br: float, bi: float) -> complex:\n"
"    if br == 0.0 and bi == 0.0:\n"
"        raise ZeroDivisionError(\"division by zero\")\n"
"    if abs(br) >= abs(bi):\n"
"        r = bi / br\n"
"        d = br + bi * r\n"
"        return complex((ar + ai * r) / d, (ai - ar * r) / d)\n"
"    r = br / bi\n"
"    d = br * r + bi\n"
"    return complex((ar * r + ai) / d, (ai * r - ar) / d)\n"
"\n"
"def _pow(ar: float, ai: float, br: float, bi: float) -> complex:\n"
"    if br == 0.0 and bi == 0.0:\n"
"        return complex(1.0, 0.0)\n"
"    if ar == 0.0 and ai == 0.0:\n"
"        if bi != 0.0 or br < 0.0:\n"
"            raise ZeroDivisionError(\"zero to a negative or complex power\")\n"
"        return complex(0.0, 0.0)\n"
"    if bi == 0.0 and br == math.floor(br) and abs(br) <= 100.0:\n"
"        n = int(br)\n"
"        u = -n if n < 0 else n\n"
"        rr = 1.0\n"
"        ri = 0.0\n"
"        pr = ar\n"
"        pim = ai\n"
"        mask = 1\n"
"        while mask > 0 and u >= mask:\n"
"            if u & mask:\n"
"                t = rr * pr - ri * pim\n"
"                ri = rr * pim + ri * pr\n"
"                rr = t\n"
"            t = pr * pr - pim * pim\n"
"            pim = pr * pim + pim * pr\n"
"            pr = t\n"
"            mask = mask << 1\n"
"        if n >= 0:\n"
"            return complex(rr, ri)\n"
"        return _div(1.0, 0.0, rr, ri)\n"
"    vabs = math.hypot(ar, ai)\n"
"    ln = math.pow(vabs, br)\n"
"    at = math.atan2(ai, ar)\n"
"    phase = at * br\n"
"    if bi != 0.0:\n"
"        ln = ln / math.exp(at * bi)\n"
"        phase = phase + bi * math.log(vabs)\n"
"    return complex(ln * math.cos(phase), ln * math.sin(phase))\n"
"\n"
"infj = complex(0.0, math.inf)\n"
"nanj = complex(0.0, math.nan)\n"
"\n"
"def phase(z: complex) -> float:\n"
"    return math.atan2(z.imag, z.real)\n"
"\n"
"def polar(z: complex) -> tuple[float, float]:\n"
"    return (math.hypot(z.real, z.imag), math.atan2(z.imag, z.real))\n"
"\n"
"def rect(r: float, phi: float) -> complex:\n"
"    return complex(r * math.cos(phi), r * math.sin(phi))\n"
"\n"
"def sqrt(z: complex) -> complex:\n"
"    if z.real == 0.0 and z.imag == 0.0:\n"
"        return complex(0.0, z.imag)\n"
"    ax = abs(z.real) / 8.0\n"
"    s = 2.0 * math.sqrt(ax + math.hypot(ax, abs(z.imag) / 8.0))\n"
"    d = abs(z.imag) / (2.0 * s)\n"
"    if z.real >= 0.0:\n"
"        return complex(s, math.copysign(d, z.imag))\n"
"    return complex(d, math.copysign(s, z.imag))\n"
"\n"
"def exp(z: complex) -> complex:\n"
"    l = math.exp(z.real)\n"
"    return complex(l * math.cos(z.imag), l * math.sin(z.imag))\n"
"\n"
"def log(z: complex) -> complex:\n"
"    return complex(math.log(math.hypot(z.real, z.imag)), math.atan2(z.imag, z.real))\n"
"\n"
"def log10(z: complex) -> complex:\n"
"    r = log(z)\n"
"    return complex(r.real / math.log(10.0), r.imag / math.log(10.0))\n"
"\n"
"def isfinite(z: complex) -> bool:\n"
"    return math.isfinite(z.real) and math.isfinite(z.imag)\n"
"\n"
"def isinf(z: complex) -> bool:\n"
"    return math.isinf(z.real) or math.isinf(z.imag)\n"
"\n"
"def isnan(z: complex) -> bool:\n"
"    return math.isnan(z.real) or math.isnan(z.imag)\n"
"\n"
"def __mpy_complex_parse(s: str) -> complex:\n"
"    t = s.strip()\n"
"    if t.startswith(\"(\") and t.endswith(\")\"):\n"
"        t = t[1:-1].strip()\n"
"    try:\n"
"        if t and (t[-1] == \"j\" or t[-1] == \"J\"):\n"
"            body = t[:-1]\n"
"            k = -1\n"
"            for i in range(len(body) - 1, 0, -1):\n"
"                if (body[i] == \"+\" or body[i] == \"-\") and body[i - 1] != \"e\" and body[i - 1] != \"E\":\n"
"                    k = i\n"
"                    break\n"
"            if k < 0:\n"
"                im = body\n"
"                if im == \"\" or im == \"+\" or im == \"-\":\n"
"                    im = im + \"1\"\n"
"                return complex(0.0, float(im))\n"
"            re = body[:k]\n"
"            im = body[k:]\n"
"            if im == \"+\" or im == \"-\":\n"
"                im = im + \"1\"\n"
"            return complex(float(re), float(im))\n"
"        return complex(float(t), 0.0)\n"
"    except ValueError:\n"
"        raise ValueError(\"complex() arg is a malformed string\")\n";
/* functools.cmp_to_key(f) as a sort key: lambda x: __mpy_CmpKey(x, f) (aot_types.c), its objects compared by f */
static const char CMPKEY_PRELUDE[]=
"from typing import Callable, Generic, TypeVar\n"
"\n"
"T = TypeVar(\"T\")\n"
"\n"
"class __mpy_CmpKey(Generic[T]):\n"
"    def __init__(self, obj: T, cmp: Callable[[T, T], int]):\n"
"        self.obj = obj\n"
"        self.cmp = cmp\n"
"    def __lt__(self, other: \"__mpy_CmpKey[T]\") -> bool:\n"
"        return self.cmp(self.obj, other.obj) < 0\n"
"    def __gt__(self, other: \"__mpy_CmpKey[T]\") -> bool:\n"
"        return self.cmp(self.obj, other.obj) > 0\n"
"    def __le__(self, other: \"__mpy_CmpKey[T]\") -> bool:\n"
"        return self.cmp(self.obj, other.obj) <= 0\n"
"    def __ge__(self, other: \"__mpy_CmpKey[T]\") -> bool:\n"
"        return self.cmp(self.obj, other.obj) >= 0\n"
"    def __eq__(self, other: \"__mpy_CmpKey[T]\") -> bool:\n"
"        return self.cmp(self.obj, other.obj) == 0\n";
/* the source has complex numbers: complex(), cmath or a literal like 2j */
static int uses_complex(const char *s){
    if(strstr(s,"complex") || strstr(s,"cmath")) return 1;
    for(const char *p=s+1;*p;p++) if((*p=='j'||*p=='J') && ((p[-1]>='0'&&p[-1]<='9')||p[-1]=='.')){
        char n=p[1]; if((n>='a'&&n<='z')||(n>='A'&&n<='Z')||(n>='0'&&n<='9')||n=='_') continue;
        const char *q=p-1; while(q>s && ((*q>='0'&&*q<='9')||*q=='.'||*q=='_'||*q=='e'||*q=='E')) q--;
        if(!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||*q=='_')) return 1; }
    return 0;
}
/* `import name` at the top of the program's main module: a module the compiler adds
   (sys.argv's) runs its top level before the program, as an imported one would */
/* ---- argparse in compiled programs: parse_args() gives a Namespace with a typed field per
   destination of the program's add_argument() calls (and set_defaults() literals); its class is
   written here from those calls and added to argparse.py's source. */
typedef struct { char *s; size_t n, cap; } SBuf;
typedef struct { char *dest, *field, *ty, *init; } NsField;
typedef struct { NsField *f; int n; SBuf *err; const char *path; int line; } NsScan;
static void sbf(SBuf *b, const char *fmt, ...){
    va_list ap; va_start(ap,fmt); int k=vsnprintf(NULL,0,fmt,ap); va_end(ap);
    if(k<0) return;
    if(b->n+(size_t)k+1>b->cap){ b->cap=(b->n+(size_t)k+1)*2; b->s=(char*)xrealloc(b->s,b->cap); }
    va_start(ap,fmt); vsnprintf(b->s+b->n,(size_t)k+1,fmt,ap); va_end(ap); b->n+=(size_t)k;
}
/* a literal as source text (numbers, strings, None, True / False, lists / tuples of them); 0: not one */
static int lit_text(Expr *e, SBuf *o){
    if(!e) return 0;
    if(e->kind==EXPR_NONE){ sbf(o,"None"); return 1; }
    if(e->kind==EXPR_TRUE){ sbf(o,"True"); return 1; }
    if(e->kind==EXPR_FALSE){ sbf(o,"False"); return 1; }
    if(e->kind==EXPR_UNARY && e->op==T_MINUS && e->a->kind==EXPR_LITERAL && e->a->tok->kind==T_NUMBER){ sbf(o,"-%s",e->a->tok->text); return 1; }
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_NUMBER){ sbf(o,"%s",e->tok->text); return 1; }
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && !e->tok->i){
        sbf(o,"\"");
        for(int64_t i=0;i<e->tok->len;i++){ unsigned char ch=(unsigned char)e->tok->text[i];
            if(ch=='"'||ch=='\\') sbf(o,"\\%c",ch); else if(ch<32) sbf(o,"\\x%02x",ch); else sbf(o,"%c",ch); }
        sbf(o,"\""); return 1; }
    if(e->kind==EXPR_LIST || e->kind==EXPR_TUPLE){
        sbf(o,e->kind==EXPR_LIST?"[":"(");
        for(int i=0;i<e->count;i++){ if(i) sbf(o,", "); if(!lit_text(e->items[i],o)) return 0; }
        if(e->kind==EXPR_TUPLE && e->count==1) sbf(o,",");
        sbf(o,e->kind==EXPR_LIST?"]":")"); return 1; }
    return 0;
}
/* the type of a literal: "int", "str", "list[int]" ...; NULL: not known */
static const char *lit_ty(Expr *e){
    if(!e) return NULL;
    if(e->kind==EXPR_TRUE||e->kind==EXPR_FALSE) return "bool";
    if(e->kind==EXPR_UNARY && e->op==T_MINUS) e=e->a;
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_NUMBER) return e->tok->is_float?"float":"int";
    if(e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && !e->tok->i) return "str";
    if(e->kind==EXPR_LIST && e->count){ const char *t=lit_ty(e->items[0]); if(!t) return NULL;
        static char buf[8][64]; static int k; k=(k+1)&7; snprintf(buf[k],sizeof buf[k],"list[%s]",t); return buf[k]; }
    return NULL;
}
static Expr *kwarg(Expr *call, const char *name){
    for(int i=0;i<call->count;i++) if(call->items[i]->akind==3 && !strcmp(call->items[i]->kw,name)) return call->items[i];
    return NULL;
}
static const char *str_lit(Expr *e){ return e && e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && !e->tok->i ? e->tok->text : NULL; }
static void ns_add(NsScan *ns, const char *dest, const char *ty, const char *init, int line){
    for(int i=0;i<ns->n;i++) if(!strcmp(ns->f[i].dest,dest)){
        if(strcmp(ns->f[i].ty,ty)){ sbf(ns->err,"%s:%d: error: argparse: '%s' is %s here and %s elsewhere (one type per destination in compiled code)\n",ns->path,line,dest,ty,ns->f[i].ty); }
        return; }
    ns->f=(NsField*)xrealloc(ns->f,sizeof(NsField)*(size_t)(ns->n+1));
    NsField *f=&ns->f[ns->n++]; f->dest=xstrdup2(dest); f->ty=xstrdup2(ty); f->init=xstrdup2(init);
    char fl[256]; snprintf(fl,sizeof fl,"%s",dest); for(char *q=fl;*q;q++) if(!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9')||*q=='_')) *q='_';
    f->field=xstrdup2(fl);
}
static void ns_call(NsScan *ns, Expr *e){
    int line=e->line; const char *flags[16]; int nf=0;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind) continue; const char *s=str_lit(a); if(!s) return; if(nf<16) flags[nf++]=s; }
    if(!nf && !kwarg(e,"dest")) return;
    Expr *k_action=kwarg(e,"action"), *k_nargs=kwarg(e,"nargs"), *k_const=kwarg(e,"const"), *k_default=kwarg(e,"default"),
         *k_type=kwarg(e,"type"), *k_required=kwarg(e,"required"), *k_dest=kwarg(e,"dest");
    const char *action= k_action ? str_lit(k_action) : "store";
    if(!action){ sbf(ns->err,"%s:%d: error: argparse: action= must be a string in compiled code (custom Action classes are not supported)\n",ns->path,line); return; }
    if(!strcmp(action,"help") || !strcmp(action,"version")) return;
    int positional= nf && flags[0][0]!='-';
    char dest[256]={0};
    if(k_dest && str_lit(k_dest)) snprintf(dest,sizeof dest,"%s",str_lit(k_dest));
    else if(positional) snprintf(dest,sizeof dest,"%s",flags[0]);
    else { for(int i=0;i<nf && !dest[0];i++) if(flags[i][0]=='-' && flags[i][1]=='-' && flags[i][2]) snprintf(dest,sizeof dest,"%s",flags[i]+2);
        if(!dest[0] && nf) snprintf(dest,sizeof dest,"%s",flags[0]+1);
        for(char *q=dest;*q;q++) if(*q=='-') *q='_'; }
    if(!dest[0]) return;
    const char *T="str", *get="str";
    if(k_type){ if(k_type->kind==EXPR_NAME && !strcmp(k_type->name,"int")){ T="int"; get="int"; }
        else if(k_type->kind==EXPR_NAME && !strcmp(k_type->name,"float")){ T="float"; get="float"; }
        else if(k_type->kind==EXPR_NAME && !strcmp(k_type->name,"str")){ T="str"; get="str"; }
        else { sbf(ns->err,"%s:%d: error: argparse: type= must be int, float or str in compiled code\n",ns->path,line); return; } }
    char nargs[32]="";
    if(k_nargs){ const char *s=str_lit(k_nargs); if(s) snprintf(nargs,sizeof nargs,"%s",s);
        else if(k_nargs->kind==EXPR_LITERAL && k_nargs->tok->kind==T_NUMBER && !k_nargs->tok->is_float) snprintf(nargs,sizeof nargs,"%lld",(long long)k_nargs->tok->i);
        else if(k_nargs->kind!=EXPR_NONE){ sbf(ns->err,"%s:%d: error: argparse: nargs= must be a literal in compiled code\n",ns->path,line); return; } }
    int required= k_required && k_required->kind==EXPR_TRUE;
    SBuf dflt={0}; int has_dflt=0, dflt_none=1, dflt_lit=0;
    if(k_default){ has_dflt=1; dflt_none= k_default->kind==EXPR_NONE; dflt_lit=lit_text(k_default,&dflt); }
    SBuf ty={0}, init={0}; char q[300]; snprintf(q,sizeof q,"\"%s\"",dest);
    int list= !strcmp(nargs,"*") || !strcmp(nargs,"+") || !strcmp(nargs,"...") || (nargs[0]>='0'&&nargs[0]<='9');
    const char *dty= k_default ? lit_ty(k_default) : NULL;
    int dfit= dflt_lit && dty && (!strcmp(dty,T) || (list && !strncmp(dty,"list[",5)));
    #define DFLT(ok_none) do{ if(has_dflt && !dflt_none){ if(dfit || (dflt_lit && strcmp(action,"store"))) sbf(&init,"%s",dflt.s); else if(!list) sbf(&init,"p.dflt_%s(%s)",get,q); \
            else { sbf(ns->err,"%s:%d: error: argparse: the default of '%s' must be a literal in compiled code\n",ns->path,line); return; } } else sbf(&init,"None"); }while(0)
    if(!strcmp(action,"store_true") || !strcmp(action,"store_false")){
        int t= action[6]=='t';
        sbf(&ty,"bool"); sbf(&init,"%s if p.seen(%s) else %s",t?"True":"False",q, has_dflt&&dflt_lit&&!dflt_none ? dflt.s : t?"False":"True"); }
    else if(!strcmp(action,"store_const") || !strcmp(action,"append_const")){
        SBuf c={0}; if(!k_const || !lit_text(k_const,&c) || !lit_ty(k_const)){ sbf(ns->err,"%s:%d: error: argparse: const= must be a literal in compiled code\n",ns->path,line); return; }
        const char *ct=lit_ty(k_const);
        if(action[0]=='s'){ int opt= !(has_dflt && !dflt_none); sbf(&ty,"%s%s",ct,opt?" | None":""); sbf(&init,"%s if p.seen(%s) else ",c.s,q); DFLT(1); }
        else { sbf(&ty,"list[%s] | None",ct); sbf(&init,"[%s] * p.count(%s) if p.seen(%s) else ",c.s,q,q); DFLT(1); }
        free(c.s); }
    else if(!strcmp(action,"count")){
        int opt= !(has_dflt && !dflt_none); sbf(&ty,"int%s",opt?" | None":""); sbf(&init,"p.count(%s) + %s if p.seen(%s) else ",q,opt?"0":dflt.s,q); DFLT(1); }
    else if(!strcmp(action,"append") || !strcmp(action,"extend")){
        if(list && !strcmp(action,"append")){ sbf(ns->err,"%s:%d: error: argparse: append with nargs= is not supported in compiled code\n",ns->path,line); return; }
        int opt= !(has_dflt && !dflt_none);
        sbf(&ty,"list[%s]%s",T,opt?" | None":"");
        if(opt) sbf(&init,"p.get_%ss(%s) if p.seen(%s) else None",get,q,q);
        else { if(!dflt_lit){ sbf(ns->err,"%s:%d: error: argparse: the default of '%s' must be a literal in compiled code\n",ns->path,line); return; }
            sbf(&init,"%s + p.get_%ss(%s) if p.seen(%s) else %s",dflt.s,get,q,q,dflt.s); } }
    else if(!strcmp(action,"store")){
        if(list){
            int always= positional && (strcmp(nargs,"*") || !has_dflt);
            if(positional && !strcmp(nargs,"*")){ sbf(&ty,"list[%s]",T); sbf(&init,"p.get_%ss(%s) if p.seen(%s) else ",get,q,q); if(has_dflt && !dflt_none && dflt_lit) sbf(&init,"%s",dflt.s); else sbf(&init,"[]"); }
            else if(always){ sbf(&ty,"list[%s]",T); sbf(&init,"p.get_%ss(%s)",get,q); }
            else { int opt= !(has_dflt && !dflt_none) && !required; sbf(&ty,"list[%s]%s",T,opt?" | None":""); sbf(&init,"p.get_%ss(%s) if p.seen(%s) else ",get,q,q); if(required) sbf(&init,"[]"); else DFLT(1); } }
        else if(!strcmp(nargs,"?")){
            int opt= !(has_dflt && !dflt_none);
            sbf(&ty,"%s%s",T,opt?" | None":"");
            SBuf c={0}; int hc= k_const && lit_text(k_const,&c);
            sbf(&init,"(p.get_%s(%s) if not p.none(%s) else %s) if p.seen(%s) else ",get,q,q,hc?c.s:"None",q); DFLT(1); free(c.s);
            if(hc && !opt){ /* (const and default given: both of T) */ } }
        else {
            if(positional || required){ sbf(&ty,"%s",T); sbf(&init,"p.get_%s(%s)",get,q); }
            else { int opt= !(has_dflt && !dflt_none); sbf(&ty,"%s%s",T,opt?" | None":""); sbf(&init,"p.get_%s(%s) if p.seen(%s) else ",get,q,q); DFLT(1); } } }
    else { sbf(ns->err,"%s:%d: error: argparse: action '%s' is not supported in compiled code\n",ns->path,line); return; }
    #undef DFLT
    ns_add(ns,dest,ty.s,init.s,line);
    free(ty.s); free(init.s); free(dflt.s);
}
static void ns_defaults(NsScan *ns, Expr *e){                      /* set_defaults(name=literal) */
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind!=3) continue;
        SBuf v={0}; const char *t=lit_ty(a);
        if(!t || !lit_text(a,&v)){ sbf(ns->err,"%s:%d: error: argparse: set_defaults(%s=...) must be a literal in compiled code\n",ns->path,e->line,a->kw); free(v.s); continue; }
        int known=0; for(int k=0;k<ns->n;k++) if(!strcmp(ns->f[k].dest,a->kw)) known=1;
        if(!known) ns_add(ns,a->kw,t,v.s,e->line);
        free(v.s); }
}
static void ns_expr(NsScan *ns, Expr *e);
static void ns_stmts(NsScan *ns, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i]; ns->line=s->line;
        ns_expr(ns,s->expr); ns_expr(ns,s->expr2); ns_expr(ns,s->value);
        for(int k=0;k<s->ntargets;k++) ns_expr(ns,s->targets[k]);
        ns_stmts(ns,s->body,s->body_count); ns_stmts(ns,s->orelse,s->orelse_count); }
}
static void ns_expr(NsScan *ns, Expr *e){
    if(!e) return;
    if(e->kind==EXPR_CALL && e->a && e->a->kind==EXPR_ATTRIBUTE){
        if(!strcmp(e->a->name,"add_argument")) ns_call(ns,e);
        else if(!strcmp(e->a->name,"set_defaults")) ns_defaults(ns,e);
        else if(!strcmp(e->a->name,"add_subparsers")){ Expr *d=kwarg(e,"dest");      /* the command chosen */
            if(d && str_lit(d)){ SBuf t={0}; sbf(&t,"p.get_str(\"%s\") if p.seen(\"%s\") else None",str_lit(d),str_lit(d)); ns_add(ns,str_lit(d),"str | None",t.s,e->line); free(t.s); } } }
    ns_expr(ns,e->a); ns_expr(ns,e->b); ns_expr(ns,e->c); ns_expr(ns,e->d);
    for(int i=0;i<e->count;i++) ns_expr(ns,e->items[i]);
    for(int i=0;i<e->vcount;i++) ns_expr(ns,e->vals[i]);
    for(int i=0;i<e->nclause;i++) ns_expr(ns,e->clauses[i].iter);
}
/* argparse.py's source with the program's Namespace class after it */
static char *argparse_with_namespace(Units *us, const char *src, SBuf *err){
    NsScan ns; memset(&ns,0,sizeof ns); ns.err=err;
    for(int i=0;i<us->n;i++){ AotUnit *u=us->v[i]; if(!u->ast || !strcmp(u->name,"argparse")) continue;
        if(!u->src || !strstr(u->src,"add_argument")) continue;
        ns.path=u->path; ns_stmts(&ns,u->ast->body,u->ast->body_count); }
    SBuf b={0}; size_t sl=strlen(src);
    b.cap=sl+4096; b.s=(char*)xmalloc(b.cap); memcpy(b.s,src,sl); b.n=sl; b.s[sl]=0;      /* (the module's text, then the class) */
    sbf(&b,"\n\nif sys._compiled:\n    class Namespace(_NamespaceBase):\n");
    sbf(&b,"        \"\"\"The arguments parse_args() found: a field per destination.\"\"\"\n\n");
    sbf(&b,"        def __init__(self, p: _Parsed | None = None) -> None:\n            if p is None:\n                p = _Parsed()\n            self._dests: list[str] = p.dests\n");
    for(int i=0;i<ns.n;i++){ const char *t=ns.f[i].ty;                 /* (a destination of another parser: nothing) */
        const char *zero= strstr(t,"| None") ? "None" : !strncmp(t,"list",4) ? "[]" : !strcmp(t,"int") ? "0" : !strcmp(t,"float") ? "0.0" : !strcmp(t,"bool") ? "False" : "\"\"";
        sbf(&b,"            self.%s: %s = (%s) if p.has(\"%s\") else %s\n",ns.f[i].field,t,ns.f[i].init,ns.f[i].dest,zero); }
    sbf(&b,"\n        def _repr_of(self, d: str) -> str:\n");
    for(int i=0;i<ns.n;i++) sbf(&b,"            if d == \"%s\":\n                return repr(self.%s)\n",ns.f[i].dest,ns.f[i].field);
    sbf(&b,"            return \"None\"\n\n");
    sbf(&b,"        def __repr__(self) -> str:\n            parts: list[str] = []\n            for d in self._dests:\n");
    sbf(&b,"                parts.append(d + \"=\" + self._repr_of(d) if d.isidentifier() else repr(d) + \": \" + self._repr_of(d))\n");
    sbf(&b,"            return \"Namespace(\" + \", \".join(parts) + \")\"\n\n");
    sbf(&b,"        def __contains__(self, key: str) -> bool:\n            return key in self._dests\n");
    for(int i=0;i<ns.n;i++){ free(ns.f[i].dest); free(ns.f[i].field); free(ns.f[i].ty); free(ns.f[i].init); }
    free(ns.f);
    return b.s;
}
/* ---- optparse in compiled programs: parse_args() gives a Values object with a typed field per
   destination of the program's add_option() / make_option() calls; the parser records events
   (destination, action, option, value strings) that Values replays. Its class is written here
   and added to optparse.py's source. */
typedef struct { char *dest, *field, *ty, *dflt; int opt; SBuf apply; } OpField;
typedef struct { OpField *f; int n; SBuf *err; const char *path; } OpScan;
static OpField *op_field(OpScan *os, const char *dest){
    for(int i=0;i<os->n;i++) if(!strcmp(os->f[i].dest,dest)) return &os->f[i];
    os->f=(OpField*)xrealloc(os->f,sizeof(OpField)*(size_t)(os->n+1));
    OpField *f=&os->f[os->n++]; memset(f,0,sizeof *f); f->dest=xstrdup2(dest);
    char fl[256]; snprintf(fl,sizeof fl,"%s",dest); for(char *q=fl;*q;q++) if(!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9')||*q=='_')) *q='_';
    f->field=xstrdup2(fl); f->opt=1;
    return f;
}
static int op_type_of(OpScan *os, OpField *f, const char *ty, int line){
    if(!f->ty){ f->ty=xstrdup2(ty); return 1; }
    if(strcmp(f->ty,ty)){ sbf(os->err,"%s:%d: error: optparse: '%s' is %s here and %s elsewhere (one type per destination in compiled code)\n",os->path,line,f->dest,ty,f->ty); return 0; }
    return 1;
}
static void op_call(OpScan *os, Expr *e){
    int line=e->line; const char *flags[16]; int nf=0;
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind) continue; const char *s=str_lit(a); if(!s) return; if(nf<16) flags[nf++]=s; }
    if(!nf) return;
    Expr *k_action=kwarg(e,"action"), *k_type=kwarg(e,"type"), *k_dest=kwarg(e,"dest"), *k_default=kwarg(e,"default"),
         *k_nargs=kwarg(e,"nargs"), *k_const=kwarg(e,"const"), *k_choices=kwarg(e,"choices");
    if(kwarg(e,"callback")){ sbf(os->err,"%s:%d: error: optparse: callbacks are not supported in compiled code\n",os->path,line); return; }
    const char *action= k_action ? str_lit(k_action) : "store";
    if(!action){ sbf(os->err,"%s:%d: error: optparse: action= must be a string in compiled code\n",os->path,line); return; }
    if(!strcmp(action,"help") || !strcmp(action,"version")) return;
    { static const char *const known[]={"store","store_const","store_true","store_false","append","append_const","count",NULL};
      int ok=0; for(int i=0;known[i];i++) if(!strcmp(action,known[i])) ok=1;
      if(!ok) return; }                                             /* (an unknown one: the OptionError when it runs) */
    for(int i=0;i<nf;i++){ const char *o=flags[i]; size_t l=strlen(o);    /* (bad option strings: the OptionError too) */
        if(l<2 || o[0]!='-' || (l==2 && o[1]=='-') || (l>2 && (o[1]!='-' || o[2]=='-'))) return; }
    const char *type=NULL;
    if(k_type){ if(str_lit(k_type)) type=str_lit(k_type);
        else if(k_type->kind==EXPR_NAME) type=k_type->name;
        else { sbf(os->err,"%s:%d: error: optparse: type= must be a literal in compiled code\n",os->path,line); return; } }
    int store= !strcmp(action,"store") || !strcmp(action,"append");
    if(!type && store) type= k_choices ? "choice" : "string";
    const char *V="str";
    if(type){ if(!strcmp(type,"int") || !strcmp(type,"long")) V="int"; else if(!strcmp(type,"float")) V="float";
        else if(!strcmp(type,"string") || !strcmp(type,"str") || !strcmp(type,"choice")) V="str";
        else { sbf(os->err,"%s:%d: error: optparse: type '%s' is not supported in compiled code\n",os->path,line,type); return; } }
    char shorts[512]="", longs[512]="";
    for(int i=0;i<nf;i++){ char *b= strlen(flags[i])==2 ? shorts : longs; if(b[0]) strncat(b,"/",sizeof shorts-strlen(b)-1); strncat(b,flags[i],sizeof shorts-strlen(b)-1); }
    char id[1024]; snprintf(id,sizeof id,"%s%s%s",shorts,shorts[0]&&longs[0]?"/":"",longs);
    char dest[256]="";
    if(k_dest && str_lit(k_dest)) snprintf(dest,sizeof dest,"%s",str_lit(k_dest));
    else if(k_dest){ sbf(os->err,"%s:%d: error: optparse: dest= must be a string in compiled code\n",os->path,line); return; }
    else { for(int i=0;i<nf && !dest[0];i++) if(strlen(flags[i])>2) snprintf(dest,sizeof dest,"%s",flags[i]+2);
        if(!dest[0]) snprintf(dest,sizeof dest,"%c",flags[0][1]);
        for(char *q=dest;*q;q++) if(*q=='-') *q='_'; }
    int nargs=1;
    if(k_nargs){ if(k_nargs->kind==EXPR_LITERAL && k_nargs->tok->kind==T_NUMBER && !k_nargs->tok->is_float) nargs=(int)k_nargs->tok->i;
        else { sbf(os->err,"%s:%d: error: optparse: nargs= must be a literal in compiled code\n",os->path,line); return; } }
    OpField *f=op_field(os,dest);
    char conv[160], val[1200]; SBuf ty={0};
    #define CONV(k) (snprintf(conv,sizeof conv,!strcmp(V,"int")?"_parse_int(ev.vals[%d])":!strcmp(V,"float")?"float(ev.vals[%d])":"ev.vals[%d]",(k)),conv)
    if(store){
        if(nargs>1){ SBuf t={0}, v={0}; sbf(&t,"tuple["); sbf(&v,"(");
            for(int k=0;k<nargs;k++){ sbf(&t,"%s%s",k?", ":"",V); sbf(&v,"%s%s",k?", ":"",CONV(k)); }
            sbf(&t,"]"); sbf(&v,")"); snprintf(val,sizeof val,"%s",v.s);
            if(!strcmp(action,"append")) sbf(&ty,"list[%s]",t.s); else sbf(&ty,"%s",t.s); free(t.s); free(v.s); }
        else { snprintf(val,sizeof val,"%s",CONV(0)); if(!strcmp(action,"append")) sbf(&ty,"list[%s]",V); else sbf(&ty,"%s",V); } }
    else if(!strcmp(action,"store_true") || !strcmp(action,"store_false")){ sbf(&ty,"bool"); snprintf(val,sizeof val,"%s",action[6]=='t'?"True":"False"); }
    else if(!strcmp(action,"store_const") || !strcmp(action,"append_const")){
        SBuf c={0}; const char *ct=lit_ty(k_const);
        if(!k_const || !ct || !lit_text(k_const,&c)){ sbf(os->err,"%s:%d: error: optparse: const= must be a literal in compiled code\n",os->path,line); free(c.s); return; }
        snprintf(val,sizeof val,"%s",c.s); free(c.s);
        if(action[0]=='a') sbf(&ty,"list[%s]",ct); else sbf(&ty,"%s",ct); }
    else if(!strcmp(action,"count")){ sbf(&ty,"int"); snprintf(val,sizeof val,"1"); }
    else return;                                                    /* (an unknown one: the OptionError when it runs) */
    #undef CONV
    if(!op_type_of(os,f,ty.s,line)){ free(ty.s); return; }
    free(ty.s);
    if(k_default && k_default->kind!=EXPR_NONE){
        SBuf d={0}; const char *dt=lit_ty(k_default);
        if(!dt || !lit_text(k_default,&d)){ sbf(os->err,"%s:%d: error: optparse: the default of '%s' must be a literal in compiled code\n",os->path,line,dest); free(d.s); return; }
        int ok= !strcmp(dt,f->ty) || (!strcmp(dt,"str") && (!strcmp(f->ty,"int") || !strcmp(f->ty,"float"))) || (!strcmp(dt,"int") && !strcmp(f->ty,"float"));
        if(!ok){ sbf(os->err,"%s:%d: error: optparse: the default of '%s' is %s, its values %s (one type per destination in compiled code)\n",os->path,line,dest,dt,f->ty); free(d.s); return; }
        free(f->dflt); f->dflt=d.s; f->opt=0; }
    /* what the option does to the field */
    int list= !strncmp(f->ty,"list[",5);
    sbf(&f->apply,"            %s ev.opt == \"%s\":\n",f->apply.n?"elif":"if",id);
    if(!strcmp(action,"count")) sbf(&f->apply,"                self.%s = _inc(self.%s)\n",f->field,f->field);
    else if(list) sbf(&f->apply,"                self.%s = _app(self.%s, %s)\n",f->field,f->field,val);
    else sbf(&f->apply,"                self.%s = %s\n",f->field,val);
}
static void op_defaults(OpScan *os, Expr *e, int single){        /* set_defaults(name=literal), set_default("name", literal) */
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; const char *name;
        if(single){ if(i || a->akind || !str_lit(a) || e->count<2) return; name=str_lit(a); a=e->items[1]; }
        else { if(a->akind!=3) continue; name=a->kw; }
        if(a->kind==EXPR_NONE){ if(single) return; continue; }
        const char *t=lit_ty(a); SBuf v={0};
        if(!t || !lit_text(a,&v)){ free(v.s); if(single) return; continue; }      /* (the runtime default text still applies to scalars) */
        OpField *f=op_field(os,name);
        if(!f->ty) f->ty=xstrdup2(t);
        free(f->dflt); f->dflt=v.s; f->opt=0;
        if(single) return; }
}
static void op_expr(OpScan *os, Expr *e);
static void op_stmts(OpScan *os, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        op_expr(os,s->expr); op_expr(os,s->expr2); op_expr(os,s->value);
        for(int k=0;k<s->ntargets;k++) op_expr(os,s->targets[k]);
        op_stmts(os,s->body,s->body_count); op_stmts(os,s->orelse,s->orelse_count); }
}
static void op_expr(OpScan *os, Expr *e){
    if(!e) return;
    if(e->kind==EXPR_CALL && e->a){
        const char *nm= e->a->kind==EXPR_ATTRIBUTE ? e->a->name : e->a->kind==EXPR_NAME ? e->a->name : NULL;
        if(nm && (!strcmp(nm,"add_option") || !strcmp(nm,"make_option"))) op_call(os,e);
        else if(nm && e->a->kind==EXPR_ATTRIBUTE && !strcmp(nm,"set_defaults")) op_defaults(os,e,0);
        else if(nm && e->a->kind==EXPR_ATTRIBUTE && !strcmp(nm,"set_default")) op_defaults(os,e,1); }
    op_expr(os,e->a); op_expr(os,e->b); op_expr(os,e->c); op_expr(os,e->d);
    for(int i=0;i<e->count;i++) op_expr(os,e->items[i]);
    for(int i=0;i<e->vcount;i++) op_expr(os,e->vals[i]);
    for(int i=0;i<e->nclause;i++) op_expr(os,e->clauses[i].iter);
}
/* optparse.py's source with the program's Values class after it */
static char *optparse_with_values(Units *us, const char *src, SBuf *err){
    OpScan os; memset(&os,0,sizeof os); os.err=err;
    for(int i=0;i<us->n;i++){ AotUnit *u=us->v[i]; if(!u->ast || !strcmp(u->name,"optparse")) continue;
        if(!u->src || (!strstr(u->src,"add_option") && !strstr(u->src,"make_option"))) continue;
        os.path=u->path; op_stmts(&os,u->ast->body,u->ast->body_count); }
    SBuf b={0}; size_t sl=strlen(src);
    b.cap=sl+4096; b.s=(char*)xmalloc(b.cap); memcpy(b.s,src,sl); b.n=sl; b.s[sl]=0;
    sbf(&b,"\n\nif sys._compiled:\n    def _app(xs: list[_T] | None, v: _T) -> list[_T]:\n        if xs is None:\n            return [v]\n        xs.append(v)\n        return xs\n\n");
    sbf(&b,"    def _inc(n: int | None) -> int:\n        return 1 if n is None else n + 1\n\n");
    sbf(&b,"    class Values(_ValuesBase):\n        \"\"\"The option values parse_args() found: a field per destination.\"\"\"\n\n");
    sbf(&b,"        def __init__(self, p: _Parsed | None = None) -> None:\n            if p is None:\n                p = _Parsed()\n            self._dests: list[str] = p.dests\n");
    for(int i=0;i<os.n;i++){ OpField *f=&os.f[i]; const char *t=f->ty?f->ty:"str"; const char *q=f->dest;
        int list= !strncmp(t,"list[",5) || !strncmp(t,"tuple[",6);
        if(f->opt){
            if(list) sbf(&b,"            self.%s: %s | None = None\n",f->field,t);
            else sbf(&b,"            self.%s: %s | None = p.d_%s(\"%s\")\n",f->field,t,t,q); }
        else {
            const char *zero= list ? "[]" : !strcmp(t,"int") ? "0" : !strcmp(t,"float") ? "0.0" : !strcmp(t,"bool") ? "False" : "\"\"";
            if(list) sbf(&b,"            self.%s: %s = list(%s) if p.has(\"%s\") else %s\n",f->field,t,f->dflt,q,zero);
            else sbf(&b,"            self.%s: %s = %s\n            _%s = p.d_%s(\"%s\")\n            if _%s is not None:\n                self.%s = _%s\n",
                     f->field,t,zero,f->field,t,q,f->field,f->field,f->field); } }
    sbf(&b,"            for ev in p.events:\n                self._apply(ev)\n\n");
    sbf(&b,"        def _apply(self, ev: _Event) -> None:\n            d = ev.dest\n");
    int any=0;
    for(int i=0;i<os.n;i++){ OpField *f=&os.f[i]; if(!f->apply.n) continue;
        sbf(&b,"            %s d == \"%s\":\n",any?"elif":"if",f->dest); any=1;
        for(char *l=f->apply.s;*l;){ char *nl=strchr(l,'\n'); sbf(&b,"    %.*s\n",(int)(nl-l),l); l=nl+1; } }
    if(!any) sbf(&b,"            pass\n");
    sbf(&b,"\n        def _repr_of(self, d: str) -> str:\n");
    for(int i=0;i<os.n;i++) sbf(&b,"            if d == \"%s\":\n                return repr(self.%s)\n",os.f[i].dest,os.f[i].field);
    sbf(&b,"            return \"None\"\n\n");
    sbf(&b,"        def __str__(self) -> str:\n            parts: list[str] = []\n            for d in self._dests:\n                parts.append(repr(d) + \": \" + self._repr_of(d))\n            return \"{\" + \", \".join(parts) + \"}\"\n\n");
    sbf(&b,"        def __repr__(self) -> str:\n            return \"<Values at 0x%%x: %%s>\" %% (id(self), str(self))\n");
    for(int i=0;i<os.n;i++){ free(os.f[i].dest); free(os.f[i].field); free(os.f[i].ty); free(os.f[i].dflt); free(os.f[i].apply.s); }
    free(os.f);
    return b.s;
}
/* ---- types.SimpleNamespace in compiled programs: a class with a field for each keyword name the
   program's SimpleNamespace(...) calls give (in that order), appended to types.py's source. */
typedef struct { char **v; int n; } SnNames;
static void sn_expr(SnNames *sn, Expr *e);
static void sn_stmts(SnNames *sn, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        sn_expr(sn,s->expr); sn_expr(sn,s->expr2); sn_expr(sn,s->value);
        for(int k=0;k<s->ntargets;k++) sn_expr(sn,s->targets[k]);
        sn_stmts(sn,s->body,s->body_count); sn_stmts(sn,s->orelse,s->orelse_count); }
}
static void sn_expr(SnNames *sn, Expr *e){
    if(!e) return;
    if(e->kind==EXPR_CALL && e->a && ((e->a->kind==EXPR_NAME && !strcmp(e->a->name,"SimpleNamespace")) ||
                                      (e->a->kind==EXPR_ATTRIBUTE && !strcmp(e->a->name,"SimpleNamespace"))))
        for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; if(a->akind!=3 || !a->kw) continue;
            int have=0; for(int k=0;k<sn->n;k++) if(!strcmp(sn->v[k],a->kw)) have=1;
            if(!have){ sn->v=(char**)xrealloc(sn->v,sizeof(char*)*(size_t)(sn->n+1)); sn->v[sn->n++]=xstrdup2(a->kw); } }
    sn_expr(sn,e->a); sn_expr(sn,e->b); sn_expr(sn,e->c); sn_expr(sn,e->d);
    for(int i=0;i<e->count;i++) sn_expr(sn,e->items[i]);
    for(int i=0;i<e->vcount;i++) sn_expr(sn,e->vals[i]);
    for(int i=0;i<e->nclause;i++) sn_expr(sn,e->clauses[i].iter);
}
static char *types_with_namespace(Units *us, const char *src){
    SnNames sn={0};
    for(int i=0;i<us->n;i++){ AotUnit *u=us->v[i]; if(!u->ast || !strcmp(u->name,"types")) continue;
        if(!u->src || !strstr(u->src,"SimpleNamespace")) continue;
        sn_stmts(&sn,u->ast->body,u->ast->body_count); }
    SBuf b={0}; size_t sl=strlen(src);
    b.cap=sl+4096; b.s=(char*)xmalloc(b.cap); memcpy(b.s,src,sl); b.n=sl; b.s[sl]=0;
    sbf(&b,"\n\nif sys._compiled:\n    class SimpleNamespace:\n");
    sbf(&b,"        \"\"\"An object whose attributes are the keyword arguments (those the program gives anywhere).\"\"\"\n\n");
    sbf(&b,"        def __init__(self");
    if(sn.n) sbf(&b,", *");
    for(int i=0;i<sn.n;i++) sbf(&b,", %s=None",sn.v[i]);
    sbf(&b,") -> None:\n            self._given: list[str] = []\n");
    for(int i=0;i<sn.n;i++) sbf(&b,"            self.%s = %s\n            if %s is not None:\n                self._given.append(\"%s\")\n",sn.v[i],sn.v[i],sn.v[i],sn.v[i]);
    sbf(&b,"\n        def _repr_of(self, k: str) -> str:\n");
    for(int i=0;i<sn.n;i++) sbf(&b,"            if k == \"%s\":\n                return repr(self.%s)\n",sn.v[i],sn.v[i]);
    sbf(&b,"            return \"None\"\n\n");
    sbf(&b,"        def __repr__(self) -> str:\n            return \"namespace(\" + \", \".join([k + \"=\" + self._repr_of(k) for k in self._given]) + \")\"\n");
    for(int i=0;i<sn.n;i++) free(sn.v[i]);
    free(sn.v);
    return b.s;
}
/* sys.stderr used as an object (print(..., file=sys.stderr) needs none) */
static int has_stderr_value(const char *src){
    for(const char *p=strstr(src,".stderr");p;p=strstr(p+1,".stderr")){
        const char *q=p; if(q-src>=3 && !strncmp(q-3,"sys",3)) q-=3; else return 1;
        while(q>src && (q[-1]==' ')) q--;
        if(q>src && q[-1]=='='){ q--; while(q>src && q[-1]==' ') q--; if(q-src>=4 && !strncmp(q-4,"file",4)) continue; }
        return 1; }
    return 0;
}
static void main_imports(AotUnit *main, const char *name){
    char src[160]; snprintf(src,sizeof src,"import %s\n",name);
    Stmt *blk=py_front_stmts(main->path,src,1);
    if(!blk || !blk->body_count) return;
    Ast *m=main->ast; Stmt **nb=MPY_NEW_ARR(Stmt*,m->body_count+1);
    nb[0]=blk->body[0]; for(int i=0;i<m->body_count;i++) nb[i+1]=m->body[i];
    m->body=nb; m->body_count++; m->body_cap=m->body_count;
}
/* code run when the main module's code is done (as CPython's interpreter ends: threading._shutdown(), atexit) */
static void main_epilogue(AotUnit *main, const char *code){
    Stmt *blk=py_front_stmts(main->path,code,main->ast->body_count?main->ast->body[main->ast->body_count-1]->line:1);
    if(!blk || !blk->body_count) return;
    Ast *m=main->ast; Stmt **nb=MPY_NEW_ARR(Stmt*,m->body_count+blk->body_count);
    for(int i=0;i<m->body_count;i++) nb[i]=m->body[i];
    for(int i=0;i<blk->body_count;i++) nb[m->body_count+i]=blk->body[i];
    m->body=nb; m->body_count+=blk->body_count; m->body_cap=m->body_count;
}
/* a source naming an encoding other than utf-8 / ascii / latin-1 (or saying "encoding": one
   from elsewhere): such a program has the code pages and utf-16/32 of _codecs_more.py */
static int names_codec(const char *t){
    static const char *const names[]={"encoding","cp4","cp8","cp12","cp1250","koi8","8859-","8859_","iso8859","latin2","latin9","mac","utf-16","utf_16","utf16",
        "utf-32","utf_32","utf32","windows-","ibm","cyrillic","l2","l9","unicode-escape","unicode_escape",NULL};
    for(int k=0;names[k];k++){ const char *p=t; size_t n=strlen(names[k]);
        while((p=strstr(p,names[k]))){
            if(k==0) return 1;
            char q=p>t?p[-1]:' ';                                             /* inside a string literal: 'cp866' ... */
            if(q=='"'||q=='\''||q=='-'||q=='_') return 1;
            p+=n; } }
    return 0;
}
static int is_package_path(const char *path){ const char *b=strrchr(path,'/'); b=b?b+1:path; return !strncmp(b,"__init__.",9); }
static char *concat3(const char *a, const char *b, const char *c){ size_t la=strlen(a), lb=strlen(b), lc=strlen(c); char *r=(char*)xmalloc(la+lb+lc+1); memcpy(r,a,la); memcpy(r+la,b,lb); memcpy(r+la+lb,c,lc+1); return r; }
/* the file of module a.b.c when package a.b is loaded: in its folder */
static char *package_member(Units *us, const char *dotted){
    const char *dot=strrchr(dotted,'.'); if(!dot) return NULL;
    char *parent=xstrndup2(dotted,(int)(dot-dotted)); AotUnit *t=find_unit(us,parent); free(parent);
    if(!t || !t->path || !is_package_path(t->path)) return NULL;
    char *dir=mpy_fs_dirname(t->path);
    char *r=mpy_fs_find_module(dir,dot+1);
    if(r && strncmp(r,dir,strlen(dir))){ free(r); r=NULL; }            /* (only the package's own) */
    free(dir);
    return r;
}
/* Make `dotted` and each of its prefixes available, as vm_import_dotted would. */
/* standard library modules written for the interpreter only (metaclasses, introspection ...) */
static int interp_only_module(const char *name){
    static const char *const mods[]={"_py_abc","_colorize",NULL};   /* (classes and functions as values of any kind: the interpreter's) */
    for(int i=0;mods[i];i++) if(!strcmp(mods[i],name)) return 1;
    return 0;
}
static int resolve_import(Units *us, AotUnit *from, const char *dotted, int line){
    char *dir=mpy_fs_dirname(from->path);
    int ok=1;
    for(const char *p=dotted;;){
        const char *dot=strchr(p,'.');
        char *prefix=xstrndup2(dotted,dot?(int)(dot-dotted):(int)strlen(dotted));
        int leaf=(dot==NULL);
        if(!find_unit(us,prefix)){
            int pkg=0; const char *ss; const char *al=mpy_stdlib_alias(prefix);
            if(al){ if(!find_unit(us,al) && (ss=mpy_stdlib_source(al,&pkg))){ char path[300]; snprintf(path,sizeof path,"<stdlib>/%s.py",al); add_unit(us,al,path,xstrdup2(ss)); } }   /* os.path: posixpath */
            else if(is_builtin_module(prefix)){
                if(!leaf && strcmp(dotted,"collections.abc")){ fprintf(stderr,"%s:%d: error: built-in module '%s' has no submodules\n",from->path,line,prefix); ok=0; }
            } else if(interp_only_module(prefix)){
                fprintf(stderr,"%s:%d: error: module '%s' is only available in the interpreter (not in compiled programs)\n",from->path,line,prefix); ok=0;
            } else if((ss=mpy_stdlib_source(prefix,&pkg))){            /* the standard library written in Python */
                char path[300]; char *slashed=xstrdup2(prefix); for(char *q=slashed;*q;q++) if(*q=='.') *q='/';
                snprintf(path,sizeof path,pkg?"<stdlib>/%s/__init__.py":"<stdlib>/%s.py",slashed); free(slashed);
                add_unit(us,prefix,path,xstrdup2(ss));                 /* (its imports: scanned with every unit's) */
            } else {
                char *path=strchr(prefix,'.') ? package_member(us,prefix) : NULL, *err=NULL;    /* pkg.sub: next to the package */
                if(!path) path=mpy_fs_find_module(dir,prefix);       /* the importer's folder, MINIPYPATH, <minipy>/lib */
                char *src=path?mpy_fs_try_read_file(path,&err):NULL;
                free(err);
                if(src) add_unit(us,prefix,path,src);
                else if(!leaf) add_unit(us,prefix,NULL,NULL);          /* namespace package */
                else { fprintf(stderr,"%s:%d: error: No module named '%s'\n",from->path,line,prefix); ok=0; }
                free(path);
            }
        }
        free(prefix);
        if(leaf || !ok) break;
        p=dot+1;
    }
    free(dir);
    return ok;
}

/* What an import nested in statement `s` would be inside of. */
static const char *construct_name(Stmt *s, const char *outer){
    switch(s->kind){
        case STMT_FUNCTION_DEF: return "a function";
        case STMT_CLASS_DEF: return "a class";
        case STMT_IF: return "an if statement";
        case STMT_WHILE: case STMT_FOR: return "a loop";
        case STMT_TRY: return "a try statement";
        case STMT_WITH: return "a with statement";
        default: return outer;
    }
}
static int nested_import_error(AotUnit *u, Stmt **b, int n, const char *where){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_IMPORT || s->kind==STMT_FROM_IMPORT){
            fprintf(stderr,"%s:%d: error: import inside %s; in compiled programs every import must be at the top of the module\n",u->path,s->line,where);
            return 1; }
        const char *w=construct_name(s,where);
        if(nested_import_error(u,s->body,s->body_count,w) || nested_import_error(u,s->orelse,s->orelse_count,w)) return 1;
    }
    return 0;
}
static int is_docstring(AotUnit *u, Stmt *s){
    (void)u;
    return s->kind==STMT_EXPR && !s->ann_only && s->expr && s->expr->kind==EXPR_LITERAL && s->expr->tok->kind==T_STRING;
}
/* The imported modules: from import statements anywhere (one inside a
   function or a block runs the module's top level where it is, as in Python). */
/* `sys._compiled` (1) / `not sys._compiled` (0) as an if's test, else -1: the interpreter's branches import nothing here */
static int compiled_test(Expr *e){
    if(!e) return -1;
    if(e->kind==EXPR_UNARY && e->op==T_NOT){ int r=compiled_test(e->a); return r<0?-1:!r; }
    if(e->kind==EXPR_ATTRIBUTE && !strcmp(e->name,"_compiled") && e->a && e->a->kind==EXPR_NAME && !strcmp(e->a->name,"sys")) return 1;
    return -1;
}
static int scan_imports_in(Units *us, AotUnit *u, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_IF){ int r=compiled_test(s->expr);
            if(r==1){ if(!scan_imports_in(us,u,s->body,s->body_count)) return 0; continue; }
            if(r==0){ if(!scan_imports_in(us,u,s->orelse,s->orelse_count)) return 0; continue; } }
        if(s->kind==STMT_IMPORT || s->kind==STMT_FROM_IMPORT){
            char *mod=s->kind==STMT_IMPORT ? xstrdup2(s->name2?s->name2:s->name) : aot_from_import_module(u,s);
            if(!mod) return 0;
            int ok=resolve_import(us,u,mod,s->line);
            if(ok && s->kind==STMT_FROM_IMPORT && !is_builtin_module(mod))    /* from pkg import sub: a submodule, when there is one */
                for(int k=0;k<s->nnames;k++){ if(!strcmp(s->names[k],"*")) continue;
                    char *sub=concat3(mod,".",s->names[k]); AotUnit *pk=find_unit(us,mod);
                    if(pk && pk->path && is_package_path(pk->path) && !find_unit(us,sub)){ char *path=package_member(us,sub);
                        if(path){ char *err=NULL, *src=mpy_fs_try_read_file(path,&err); free(err); if(src) add_unit(us,sub,path,src); free(path); } }
                    free(sub); }
            free(mod);
            if(!ok) return 0;
            continue;
        }
        if(!scan_imports_in(us,u,s->body,s->body_count) || !scan_imports_in(us,u,s->orelse,s->orelse_count)) return 0;
    }
    return 1;
}
static int scan_imports(Units *us, AotUnit *u){ (void)nested_import_error; return scan_imports_in(us,u,u->ast->body,u->ast->body_count); }

/* ---------------------------------------------------------------- running tools */

static char *strip_ext(const char *path){
    size_t n=strlen(path); const char *slash=strrchr(path,'/'), *dot=strrchr(path,'.');
    if(dot && (!slash || dot>slash) && (strcmp(dot,".mpy")==0 || strcmp(dot,".py")==0)) n=(size_t)(dot-path);
    return xstrndup2(path,(int)n);
}
static char *concat(const char *a, const char *b){ size_t la=strlen(a), lb=strlen(b); char *r=(char*)xmalloc(la+lb+1); memcpy(r,a,la); memcpy(r+la,b,lb+1); return r; }

/* Quote one argument for the platform's command line. */
static char *quote_arg(const char *s){
#if defined(MPY_KOLIBRI)
    return xstrdup2(s);                                   /* KolibriOS passes the parameter string verbatim */
#else
    size_t n=strlen(s), k=2; for(size_t i=0;i<n;i++) k+= s[i]=='\'' ? 4 : 1;
    char *r=(char*)xmalloc(k+1), *o=r; *o++='\'';
    for(size_t i=0;i<n;i++){ if(s[i]=='\''){ memcpy(o,"'\\''",4); o+=4; } else *o++=s[i]; }
    *o++='\''; *o=0; return r;
#endif
}
/* Expand {in} {out} {dir} in a fasm argument template. */
static char *fasm_args(const char *tmpl, const char *in, const char *out){
    char *dir=mpy_fs_dirname(in), *qi=quote_arg(in), *qo=quote_arg(out), *qd=quote_arg(dir);
    size_t cap=strlen(tmpl)+strlen(qi)+strlen(qo)+strlen(qd)*4+16; char *r=(char*)xmalloc(cap); size_t k=0;
    for(const char *p=tmpl;*p;){
        const char *rep=NULL; size_t skip=0;
        if(!strncmp(p,"{in}",4)){ rep=qi; skip=4; } else if(!strncmp(p,"{out}",5)){ rep=qo; skip=5; } else if(!strncmp(p,"{dir}",5)){ rep=qd; skip=5; }
        if(rep){ size_t l=strlen(rep); if(k+l+1>cap){ cap=(k+l+1)*2; r=(char*)xrealloc(r,cap); } memcpy(r+k,rep,l); k+=l; p+=skip; }
        else { if(k+2>cap){ cap*=2; r=(char*)xrealloc(r,cap); } r[k++]=*p++; }
    }
    r[k]=0; free(dir); free(qi); free(qo); free(qd);
    return r;
}
static int run_tool(const char *program, const char *args, const char *option, int verbose){
    if(verbose) printf("%s %s\n",program,args);
    int rc=mpy_platform_run(program,args);
    if(rc==127) fprintf(stderr,"minipy: cannot run `%s`; is it installed? (choose another with %s)\n",program,option);
    else if(rc!=0) fprintf(stderr,"minipy: `%s` failed (exit status %d)\n",program,rc);
    return rc;
}

/* ---------------------------------------------------------------- entry */

int aot_main(int argc, char **argv, const char *program){
    const char *src_path=NULL, *out=NULL, *fasm=getenv("MPY_FASM"), *fasm_tmpl=getenv("MPY_FASM_ARGS");
    const char *cc=getenv("MPY_CC"), *cc_tmpl=getenv("MPY_CC_ARGS");
    int only_asm=0, verbose=0; unsigned stack=0;
    int count_allocs=0;
#if defined(MPY_KOLIBRI)
    AotTarget target=AOT_TARGET_KOLIBRI;
#elif defined(__APPLE__)
    AotTarget target=AOT_TARGET_MACOS;
#else
    AotTarget target=AOT_TARGET_LINUX;
#endif
    for(int i=0;i<argc;i++){
        const char *a=argv[i];
        #define NEXT() (i+1<argc ? argv[++i] : (usage(program), (char*)NULL))
        if(!strcmp(a,"-o")){ if(!(out=NEXT())) return 2; }
        else if(!strcmp(a,"--target")){ const char *t=NEXT(); if(!t) return 2;
            if(!strcmp(t,"linux")) target=AOT_TARGET_LINUX; else if(!strcmp(t,"kolibri")||!strcmp(t,"kolibrios")) target=AOT_TARGET_KOLIBRI;
            else if(!strcmp(t,"macos")||!strcmp(t,"darwin")) target=AOT_TARGET_MACOS;
            else { fprintf(stderr,"minipy: unknown target '%s' (linux, kolibri, macos)\n",t); return 2; } }
        else if(!strcmp(a,"-S")) only_asm=1;
        else if(!strcmp(a,"--fasm")){ if(!(fasm=NEXT())) return 2; }
        else if(!strcmp(a,"--fasm-args")){ if(!(fasm_tmpl=NEXT())) return 2; }
        else if(!strcmp(a,"--cc")){ if(!(cc=NEXT())) return 2; }
        else if(!strcmp(a,"--cc-args")){ if(!(cc_tmpl=NEXT())) return 2; }
        else if(!strcmp(a,"--stack")){ const char *v=NEXT(); if(!v) return 2; stack=(unsigned)strtoul(v,NULL,0); }
        else if(!strcmp(a,"-v")) verbose=1;
        else if(!strcmp(a,"--count-allocs")) count_allocs=1;
        else if(!strcmp(a,"-h")||!strcmp(a,"--help")){ usage(program); return 0; }
        else if(a[0]=='-'){ fprintf(stderr,"minipy: unknown option '%s'\n",a); usage(program); return 2; }
        else if(!src_path) src_path=a;
        else { fprintf(stderr,"minipy: more than one input file\n"); return 2; }
        #undef NEXT
    }
    if(!src_path){ usage(program); return 2; }
    if(!fasm) fasm=MPY_AOT_FASM;
    if(!fasm_tmpl) fasm_tmpl=MPY_AOT_FASM_ARGS;
    if(!cc) cc=MPY_AOT_CC;
    if(!cc_tmpl) cc_tmpl=MPY_AOT_CC_ARGS;

    /* 1. the entry script and, transitively, its imports */
    Units us; memset(&us,0,sizeof us);
    char *err=NULL, *src=mpy_fs_try_read_file(src_path,&err);
    if(!src){ fprintf(stderr,"minipy: %s\n",err?err:"cannot read input"); free(err); return 1; }
    add_unit(&us,"__main__",src_path,src);
    if(uses_complex(src)) add_unit(&us,"cmath","<cmath>",xstrdup2(CMATH_PRELUDE));   /* (complex) */
    for(int i=0;i<us.n;i++) if(us.v[i]->ast && !scan_imports(&us,us.v[i])) return 1;
    if(!find_unit(&us,"cmath")) for(int i=0;i<us.n;i++) if(us.v[i]->src && uses_complex(us.v[i]->src)){
        add_unit(&us,"cmath","<cmath>",xstrdup2(CMATH_PRELUDE)); if(!scan_imports(&us,us.v[us.n-1])) return 1; break; }
    { int any=0; for(int i=0;i<us.n && !any;i++) if(us.v[i]->src && strstr(us.v[i]->src,"open(")) any=1;       /* open(): io.py (first: what it uses comes next) */
      if(any){ if(!find_unit(&us,"io")){ int pkg; add_unit(&us,"io","<stdlib>/io.py",xstrdup2(mpy_stdlib_source("io",&pkg)));
                   for(int i=0;i<us.n;i++) if(us.v[i]->ast && !scan_imports(&us,us.v[i])) return 1; }
               main_imports(us.v[0],"io"); } }
    { int any=0, more=0;                                                    /* encode() / decode(): codecs.py; other encodings than */
      for(int i=0;i<us.n;i++) if(us.v[i]->src){                            /* utf-8 / ascii / latin-1 named: _codecs_more.py too */
          const char *t=us.v[i]->src;
          if(strstr(t,"encode(") || strstr(t,"decode(") || strstr(t,"str(") || strstr(t,"bytes(") || strstr(t,"ascii(") || strstr(t,"!a}") || strstr(t,"!a:")) any=1;   /* (ascii(x): repr(x).encode("ascii", "backslashreplace")) */
          if(us.v[i]->path && !strncmp(us.v[i]->path,"<stdlib>",8)) continue;
          if(names_codec(t)) more=1; }
      if(any || more){ int pkg;
          if(!find_unit(&us,"codecs")) add_unit(&us,"codecs","<stdlib>/codecs.py",xstrdup2(mpy_stdlib_source("codecs",&pkg)));
          main_imports(us.v[0],"codecs");
          if(more && !find_unit(&us,"_codecs_more")){ add_unit(&us,"_codecs_more","<stdlib>/_codecs_more.py",xstrdup2(mpy_stdlib_source("_codecs_more",&pkg))); main_imports(us.v[0],"_codecs_more"); } } }
    { int any=0; for(int i=0;i<us.n && !any;i++) if(us.v[i]->src && strcmp(us.v[i]->name,"_sysio") && (strstr(us.v[i]->src,".stdout") || strstr(us.v[i]->src,".stdin") ||
                                                                strstr(us.v[i]->src,".__std") || has_stderr_value(us.v[i]->src))) any=1;     /* sys.stdout ...: _sysio.py */
      if(any && !find_unit(&us,"_sysio")){ int pkg; add_unit(&us,"_sysio","<stdlib>/_sysio.py",xstrdup2(mpy_stdlib_source("_sysio",&pkg)));
          if(!scan_imports(&us,us.v[us.n-1])) return 1; main_imports(us.v[0],"_sysio"); } }
    { int eg=0; for(int i=0;i<us.n;i++) if(us.v[i]->src && (strstr(us.v[i]->src,"ExceptionGroup") || strstr(us.v[i]->src,"except*") || strstr(us.v[i]->src,"except *"))) eg=1;
      if(eg) add_unit(&us,"__mpy_eg","<exceptiongroup>",xstrdup2(EG_PRELUDE)); }      /* ExceptionGroup, except*: written in Python */
    { static const char *const oserr[]={"OSError","IOError","EnvironmentError","FileNotFoundError","FileExistsError","PermissionError","IsADirectoryError",
          "NotADirectoryError","ConnectionError","ConnectionRefusedError","ConnectionResetError","ConnectionAbortedError","BrokenPipeError","BlockingIOError",
          "TimeoutError","InterruptedError","ProcessLookupError","ChildProcessError","UnicodeDecodeError","UnicodeEncodeError",NULL};
      int any=0; for(int i=0;i<us.n && !any;i++) if(us.v[i]->src) for(int k=0;oserr[k];k++) if(strstr(us.v[i]->src,oserr[k])){ any=1; break; }
      if(any && !find_unit(&us,"_oserror")){ int pkg; add_unit(&us,"_oserror","<stdlib>/_oserror.py",xstrdup2(mpy_stdlib_source("_oserror",&pkg))); } }   /* OSError(errno, strerror) */
    { int any=0; for(int i=0;i<us.n && !any;i++) if(us.v[i]->src && strstr(us.v[i]->src,"math") && strcmp(us.v[i]->name,"_mathx")) any=1;   /* math's functions in Python: _mathx.py */
      if(any && !find_unit(&us,"_mathx")){ int pkg; add_unit(&us,"_mathx","<stdlib>/_mathx.py",xstrdup2(mpy_stdlib_source("_mathx",&pkg))); main_imports(us.v[0],"_mathx"); } }
    { int any=0; for(int i=0;i<us.n && !any;i++) if(us.v[i]->src && strcmp(us.v[i]->name,"_mpy_exit") && (strstr(us.v[i]->src,"SystemExit") || strstr(us.v[i]->src,"exit("))) any=1;   /* SystemExit's code: _mpy_exit.py */
      if(any && !find_unit(&us,"_mpy_exit")){ int pkg; add_unit(&us,"_mpy_exit","<stdlib>/_mpy_exit.py",xstrdup2(mpy_stdlib_source("_mpy_exit",&pkg))); if(!scan_imports(&us,us.v[us.n-1])) return 1; } }
    { int any=0; for(int i=0;i<us.n && !any;i++) if(us.v[i]->src && strstr(us.v[i]->src,"json") && (strstr(us.v[i]->src,"load") || strstr(us.v[i]->src,"JSONDecodeError"))) any=1;   /* json.loads: _mpy_jsonr.py */
      if(any && !find_unit(&us,"_mpy_jsonr")){ int pkg; add_unit(&us,"_mpy_jsonr","<stdlib>/_mpy_jsonr.py",xstrdup2(mpy_stdlib_source("_mpy_jsonr",&pkg))); if(!scan_imports(&us,us.v[us.n-1])) return 1;
          main_imports(us.v[0],"_mpy_jsonr"); } }
    { int any=0; for(int i=0;i<us.n && !any;i++) if(us.v[i]->src && strstr(us.v[i]->src,"__next__")) any=1;   /* iterator objects: _mpy_iter.py */
      if(any && !find_unit(&us,"_mpy_iter")){ int pkg; add_unit(&us,"_mpy_iter","<stdlib>/_mpy_iter.py",xstrdup2(mpy_stdlib_source("_mpy_iter",&pkg))); } }
    if(find_unit(&us,"threading")) main_epilogue(us.v[0],"import threading as __mpy_threading\n__mpy_threading._shutdown()\n");   /* (non-daemon threads waited for) */
    if(find_unit(&us,"atexit")) main_epilogue(us.v[0],"import atexit as __mpy_atexit\n__mpy_atexit._run_exitfuncs()\n");
    { int any=0; for(int i=0;i<us.n && !any;i++) if(us.v[i]->src && strstr(us.v[i]->src,"argv")) any=1;     /* sys.argv: _sysargs.py */
      if(any && !find_unit(&us,"_sysargs")){ int pkg; add_unit(&us,"_sysargs","<stdlib>/_sysargs.py",xstrdup2(mpy_stdlib_source("_sysargs",&pkg)));
          main_imports(us.v[0],"_sysargs"); } }
    { int ck=0; for(int i=0;i<us.n;i++) if(us.v[i]->src && strstr(us.v[i]->src,"cmp_to_key")) ck=1;
      if(ck){ add_unit(&us,"__mpy_cmpkey","<cmp_to_key>",xstrdup2(CMPKEY_PRELUDE)); if(!scan_imports(&us,us.v[us.n-1])) return 1; } }

    { AotUnit *ty=find_unit(&us,"types");                          /* types: the program's SimpleNamespace class */
      if(ty && ty->src && !strstr(ty->src,"class SimpleNamespace:\n        \"\"\"An object whose attributes are the keyword arguments (those")){
          char *t=types_with_namespace(&us,ty->src); ty->src=t; ty->ast=py_front(ty->path,t); if(!ty->ast) return 1; } }
    { AotUnit *op=find_unit(&us,"optparse");                       /* optparse: the program's Values class */
      if(op && op->src){ SBuf err={0}; char *vs=optparse_with_values(&us,op->src,&err);
          if(err.s){ fputs(err.s,stderr); return 1; }
          if(getenv("MPY_OPDEBUG")) fputs(vs+strlen(op->src),stderr);
          op->src=vs; op->ast=py_front(op->path,vs); if(!op->ast) return 1; } }
    { AotUnit *ap=find_unit(&us,"argparse");                       /* argparse: the program's Namespace class */
      if(ap && ap->src){ SBuf err={0}; char *ns=argparse_with_namespace(&us,ap->src,&err);
          if(err.s){ fputs(err.s,stderr); return 1; }
          ap->src=ns; ap->ast=py_front(ap->path,ns); if(!ap->ast) return 1; } }
    /* 2. types, then one listing for the whole program */
    AotCodegenOptions co; co.target=target; co.stack_size=stack; co.count_allocs=count_allocs;
    char *listing=NULL; size_t len=0;
    if(aot_compile(&co,us.v,us.n,&listing,&len)) return 1;
    char *base=out?xstrdup2(out):strip_ext(src_path);
    char *asm_path=concat(base,".asm");
    char *werr=NULL;
    if(mpy_fs_write_file(asm_path,listing,len,&werr)){ fprintf(stderr,"minipy: %s\n",werr?werr:"cannot write listing"); return 1; }
    if(verbose) printf("wrote %s (%d module%s)\n",asm_path,us.n,us.n==1?"":"s");
    if(only_asm) return 0;

    if(target==AOT_TARGET_MACOS){                          /* 3. C, then the C compiler */
        char *csrc=NULL; size_t clen=0;
        if(aot_x2c(listing,len,&csrc,&clen)) return 1;
        char *c_path=concat(base,".c");
        if(mpy_fs_write_file(c_path,csrc,clen,&werr)){ fprintf(stderr,"minipy: %s\n",werr?werr:"cannot write the C file"); return 1; }
        free(csrc);
        if(verbose) printf("wrote %s\n",c_path);
        mpy_fs_remove(base);
        const char *tmpl=cc_tmpl; char big[256];
        #if defined(__APPLE__)
        if(cc_tmpl==(const char*)MPY_AOT_CC_ARGS && clen>400000){     /* a big program: one huge C function, whose register */
            snprintf(big,sizeof big,"%s -mllvm -join-liveintervals=false -mllvm -enable-misched=false",MPY_AOT_CC_ARGS); tmpl=big; }   /* coalescing and instruction scheduling take clang most of the time */
        #else
        (void)big;
        #endif
        char *args=fasm_args(tmpl,c_path,base);
        int rc=run_tool(cc,args,"--cc",verbose); free(args);
        if(rc) return 1;
        if(!mpy_fs_exists(base)){ fprintf(stderr,"minipy: %s did not produce %s\n",cc,base); return 1; }
        return 0;
    }

    /* 3. fasm writes the executable */
    mpy_fs_remove(base);                                   /* fasm's exit status is not visible on KolibriOS */
    char *args=fasm_args(fasm_tmpl,asm_path,base);
    int rc=run_tool(fasm,args,"--fasm",verbose); free(args);
    if(rc) return 1;
    if(!mpy_fs_exists(base)){ fprintf(stderr,"minipy: fasm did not produce %s (see its output)\n",base); return 1; }
    return 0;
}
