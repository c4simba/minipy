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
    static const char *mods[]={"sys","thread","asyncio","math","time","random","typing","functools","__future__","collections","collections.abc","ctypes","json","minipy","dataclasses","abc","string",NULL};   /* (thread: above) */
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
static int resolve_import(Units *us, AotUnit *from, const char *dotted, int line){
    char *dir=mpy_fs_dirname(from->path);
    int ok=1;
    for(const char *p=dotted;;){
        const char *dot=strchr(p,'.');
        char *prefix=xstrndup2(dotted,dot?(int)(dot-dotted):(int)strlen(dotted));
        int leaf=(dot==NULL);
        if(!find_unit(us,prefix)){
            if(is_builtin_module(prefix)){
                if(!leaf && strcmp(dotted,"collections.abc")){ fprintf(stderr,"%s:%d: error: built-in module '%s' has no submodules\n",from->path,line,prefix); ok=0; }
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
static int scan_imports_in(Units *us, AotUnit *u, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
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
    { int eg=0; for(int i=0;i<us.n;i++) if(us.v[i]->src && (strstr(us.v[i]->src,"ExceptionGroup") || strstr(us.v[i]->src,"except*") || strstr(us.v[i]->src,"except *"))) eg=1;
      if(eg) add_unit(&us,"__mpy_eg","<exceptiongroup>",xstrdup2(EG_PRELUDE)); }      /* ExceptionGroup, except*: written in Python */
    { int ck=0; for(int i=0;i<us.n;i++) if(us.v[i]->src && strstr(us.v[i]->src,"cmp_to_key")) ck=1;
      if(ck){ add_unit(&us,"__mpy_cmpkey","<cmp_to_key>",xstrdup2(CMPKEY_PRELUDE)); if(!scan_imports(&us,us.v[us.n-1])) return 1; } }

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
        char *args=fasm_args(cc_tmpl,c_path,base);
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
