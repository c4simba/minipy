/* ========================= Typed compiler: i386 listing -> C (target macos) =========================
   The macos target compiles a program exactly as for Linux - one fasm listing
   of i386 code, runtime routines included - and then, instead of running
   fasm, translates that listing into C which the system C compiler builds
   into a native executable (static recompilation).

   The translation keeps the i386 machine model (aot_x2c_rt.c: registers,
   lazy flags, the x87 stack, a 4 GiB guest address space with Linux system
   calls); every instruction becomes a few C statements in one function.
   Direct jumps and calls are gotos; the labels whose addresses are taken
   (return addresses, function values, handlers) get guest addresses
   0x00100000 + 4 * n and are reached through one computed-goto table.

   It understands the fasm subset minipy emits: labels (local .x, anonymous
   @@ / @f / @b), db dw dd dq rb rw rd rq, align, X = expr, `if [~]defined X`
   / else / end if, repeat N / end repeat with %, and the instructions of
   aot_codegen.c and aot_rtlib.asm. Comments ";@import CI<k> <lib> <symbol>"
   and ";@ccall <signature>" (see ccall_marker in aot_codegen.c) describe the
   C functions a ctypes program calls. */

#include "aot.h"
#include "util.h"
#include <stdarg.h>
#include <ctype.h>
#include <setjmp.h>

static const char x2c_prelude[] =
#include "aot_x2c_rt.inc"
;

typedef int64_t i64;
typedef uint32_t u32;

/* ---------------------------------------------------------------- buffers, errors */
typedef struct { char *s; size_t len, cap; } XBuf;
static void xb_put(XBuf *b, const char *s, size_t n){
    if(b->len+n+1>b->cap){ b->cap=(b->len+n+1)*2; b->s=(char*)xrealloc(b->s,b->cap); }
    memcpy(b->s+b->len,s,n); b->len+=n; b->s[b->len]=0;
}
static void xb_printf(XBuf *b, const char *fmt, ...){
    char tmp[1024]; va_list ap; va_start(ap,fmt); int n=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    if(n<(int)sizeof tmp){ xb_put(b,tmp,(size_t)n); return; }
    char *big=(char*)xmalloc((size_t)n+1); va_start(ap,fmt); vsnprintf(big,(size_t)n+1,fmt,ap); va_end(ap);
    xb_put(b,big,(size_t)n); free(big);
}

static jmp_buf x_fail;
static int x_line;
static void xfail(const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    fprintf(stderr,"minipy: C translation, listing line %d: ",x_line); vfprintf(stderr,fmt,ap); fprintf(stderr,"\n");
    va_end(ap);
    longjmp(x_fail,1);
}

/* ---------------------------------------------------------------- symbols */
typedef enum { SY_NONE, SY_CODE, SY_DATA, SY_CONST } SyKind;
typedef struct {
    char *name; SyKind kind;
    i64 val; int known;             /* SY_DATA / SY_CONST: value (known: in this layout pass) */
    int cidx;                       /* SY_CODE: index in the jump table (-1: never jumped to indirectly) */
    int item;                       /* SY_CONST: its item */
    int skipped;                    /* SY_CODE: inside a routine done natively */
} Sym;
static Sym *syms; static int nsyms, capsyms;
static int *symhash; static int hashcap;
static unsigned hstr(const char *s){ unsigned h=2166136261u; while(*s){ h^=(unsigned char)*s++; h*=16777619u; } return h; }
static int sym_find(const char *name){
    if(!hashcap) return -1;
    for(unsigned i=hstr(name)&(unsigned)(hashcap-1);;i=(i+1)&(unsigned)(hashcap-1)){
        int k=symhash[i]; if(k<0) return -1;
        if(!strcmp(syms[k].name,name)) return k;
    }
}
static int sym_get(const char *name){
    int k=sym_find(name); if(k>=0) return k;
    if((nsyms+1)*2>hashcap){
        int nc=hashcap?hashcap*2:1024; int *nh=(int*)xmalloc(sizeof(int)*(size_t)nc);
        for(int i=0;i<nc;i++) nh[i]=-1;
        for(int j=0;j<nsyms;j++){ unsigned i=hstr(syms[j].name)&(unsigned)(nc-1); while(nh[i]>=0) i=(i+1)&(unsigned)(nc-1); nh[i]=j; }
        free(symhash); symhash=nh; hashcap=nc;
    }
    if(nsyms==capsyms){ capsyms=capsyms?capsyms*2:1024; syms=(Sym*)xrealloc(syms,sizeof(Sym)*(size_t)capsyms); }
    Sym *s=&syms[nsyms]; memset(s,0,sizeof *s); s->name=xstrdup2(name); s->cidx=-1; s->item=-1;
    unsigned i=hstr(name)&(unsigned)(hashcap-1); while(symhash[i]>=0) i=(i+1)&(unsigned)(hashcap-1); symhash[i]=nsyms;
    return nsyms++;
}

/* jump-table entries: code symbols (>= 0) and return points (-1 - n) */
static int *ctab; static int nctab, capctab;
static int ctab_add(int v){ if(nctab==capctab){ capctab=capctab?capctab*2:256; ctab=(int*)xrealloc(ctab,sizeof(int)*(size_t)capctab); } ctab[nctab]=v; return nctab++; }
#define CODE_BASE 0x00100000u
#define DATA_BASE 0x08000000u
static u32 code_addr(int sym){ Sym *s=&syms[sym]; if(s->cidx<0) s->cidx=ctab_add(sym); return CODE_BASE+4u*(u32)s->cidx; }

/* ---------------------------------------------------------------- tokens */
typedef enum { TK_ID, TK_NUM, TK_STR, TK_FLT, TK_OP } TkKind;
typedef struct { TkKind k; char *s; i64 v; int op; } Tok2;
typedef struct { Tok2 *v; int n; } Toks;

/* ASCII case-insensitive comparisons (strcasecmp is not C99) */
static int x_ieq(const char *a, const char *b){ while(*a && tolower((unsigned char)*a)==tolower((unsigned char)*b)){ a++; b++; } return tolower((unsigned char)*a)==tolower((unsigned char)*b); }
static int x_ieqn(const char *a, const char *b, size_t n){ for(size_t i=0;i<n;i++){ if(tolower((unsigned char)a[i])!=tolower((unsigned char)b[i])) return 0; if(!a[i]) return 1; } return 1; }
static int is_idc(int c){ return isalnum(c)||c=='_'||c=='.'||c=='@'||c=='?'||c=='$'||c=='#'; }

/* local labels (.x) belong to the last global label; @@ labels are numbered */
static char cur_base[512];
static int anon_count;

static char *resolve_name(const char *w){
    char buf[1100];
    if(w[0]=='.' && w[1] && w[1]!='.'){ snprintf(buf,sizeof buf,"%s%s",cur_base,w); return xstrdup2(buf); }
    if(!strcmp(w,"@f")||!strcmp(w,"@F")){ snprintf(buf,sizeof buf,"@@%d",anon_count+1); return xstrdup2(buf); }
    if(!strcmp(w,"@b")||!strcmp(w,"@B")||!strcmp(w,"@r")||!strcmp(w,"@R")){ snprintf(buf,sizeof buf,"@@%d",anon_count); return xstrdup2(buf); }
    return xstrdup2(w);
}

static Toks tokenize(const char *p){
    Toks t; t.v=NULL; t.n=0; int cap=0;
    while(*p){
        if(isspace((unsigned char)*p)){ p++; continue; }
        if(t.n==cap){ cap=cap?cap*2:8; t.v=(Tok2*)xrealloc(t.v,sizeof(Tok2)*(size_t)cap); }
        Tok2 *k=&t.v[t.n++]; memset(k,0,sizeof *k);
        if(*p=='\'' || *p=='"'){                         /* 'text' ('' is a quote) */
            char q=*p++; XBuf b; memset(&b,0,sizeof b); xb_put(&b,"",0);
            for(;;){
                if(!*p) xfail("unterminated string");
                if(*p==q){ if(p[1]==q){ xb_put(&b,p,1); p+=2; continue; } p++; break; }
                xb_put(&b,p,1); p++;
            }
            k->k=TK_STR; k->s=b.s; k->v=(i64)b.len; continue;
        }
        if(isdigit((unsigned char)*p)){
            const char *s=p; while(isalnum((unsigned char)*p)||*p=='.') p++;
            if((*p=='+'||*p=='-') && (p[-1]=='e'||p[-1]=='E') && !(s[0]=='0'&&(s[1]=='x'||s[1]=='X'))){ p++; while(isdigit((unsigned char)*p)) p++; }
            char w[128]; size_t n=(size_t)(p-s); if(n>=sizeof w) xfail("number too long"); memcpy(w,s,n); w[n]=0;
            if(w[0]=='0'&&(w[1]=='x'||w[1]=='X')){ k->k=TK_NUM; k->v=(i64)strtoull(w+2,NULL,16); continue; }
            if(strchr(w,'.')||((strchr(w,'e')||strchr(w,'E')) && w[n-1]!='h' && w[n-1]!='H')){ k->k=TK_FLT; k->s=xstrdup2(w); continue; }
            char last=(char)tolower((unsigned char)w[n-1]);
            if(last=='h'){ w[n-1]=0; k->k=TK_NUM; k->v=(i64)strtoull(w,NULL,16); continue; }
            if(last=='b' && strspn(w,"01")==n-1){ w[n-1]=0; k->k=TK_NUM; k->v=(i64)strtoull(w,NULL,2); continue; }
            char *end; k->k=TK_NUM; k->v=(i64)strtoull(w,&end,10);
            if(*end) xfail("bad number '%s'",w);
            continue;
        }
        if(is_idc((unsigned char)*p) && *p!='$' ){
            const char *s=p; while(is_idc((unsigned char)*p)) p++;
            char w[1024]; size_t n=(size_t)(p-s); if(n>=sizeof w) xfail("name too long"); memcpy(w,s,n); w[n]=0;
            k->k=TK_ID; k->s=resolve_name(w); continue;
        }
        k->k=TK_OP; k->op=*p++;
    }
    return t;
}

/* ---------------------------------------------------------------- items */
typedef enum { IT_LABEL, IT_CONST, IT_INSN, IT_DATA, IT_RES, IT_ALIGN } ItKind;
typedef struct {
    ItKind kind; int line; int sec;  /* sec 0 code, 1 data */
    int sym;                         /* IT_LABEL / IT_CONST */
    char mnem[16];                   /* IT_INSN: mnemonic (with its rep prefix: "rep movsb") */
    int unit;                        /* IT_DATA / IT_RES: 1 2 4 8 */
    Toks toks;
    int rep;                         /* the repeat counter (%) */
    char *ccall;                     /* IT_INSN: ;@ccall signature */
    u32 addr;                        /* data items: address */
} Item;
static Item *items; static int nitems, capitems;
static Item *item_new(ItKind k, int line, int sec){
    if(nitems==capitems){ capitems=capitems?capitems*2:4096; items=(Item*)xrealloc(items,sizeof(Item)*(size_t)capitems); }
    Item *it=&items[nitems++]; memset(it,0,sizeof *it); it->kind=k; it->line=line; it->sec=sec; it->sym=-1; return it;
}

/* imports: ;@import CI<k> lib sym */
typedef struct { char *slot, *lib, *sym; int slotsym; } Import;
static Import *imports; static int nimports;

/* ---------------------------------------------------------------- expressions */
/* A value: c + sum(reg[i] * coefficient) (registers only in addresses). */
typedef struct { i64 c; int coef[8]; int regs; } Lin;
static const char *reg32[8]={"eax","ecx","edx","ebx","esp","ebp","esi","edi"};
static const char *reg16[8]={"ax","cx","dx","bx","sp","bp","si","di"};
static const char *reg8[8]={"al","cl","dl","bl","ah","ch","dh","bh"};
static int find_reg(const char *s, int *size){
    for(int i=0;i<8;i++){
        if(!strcmp(s,reg32[i])){ *size=4; return i; }
        if(!strcmp(s,reg16[i])){ *size=2; return i; }
        if(!strcmp(s,reg8[i])){ *size=1; return i; }
    }
    return -1;
}

typedef struct { Toks *t; int i, end; i64 dollar; int has_dollar; int rep; int pass_final; int unresolved; int allow_regs; } Ev;
static Lin ev_or(Ev *e);
static Tok2 *ev_peek(Ev *e){ return e->i<e->end ? &e->t->v[e->i] : NULL; }
static int ev_isop(Ev *e, int op){ Tok2 *k=ev_peek(e); return k && k->k==TK_OP && k->op==op; }
static int ev_isword(Ev *e, const char *w){ Tok2 *k=ev_peek(e); return k && k->k==TK_ID && x_ieq(k->s,w); }
static Lin lin_c(i64 c){ Lin l; memset(&l,0,sizeof l); l.c=c; return l; }
static void need_const(Lin a){ if(a.regs) xfail("registers in an expression that must be a number"); }
static i64 sym_value(Ev *e, int k);
static i64 str_value(Tok2 *k){                       /* 'abc' as a number: little-endian */
    if(k->v>8) xfail("string constant too long");
    i64 v=0; for(int i=(int)k->v-1;i>=0;i--) v=(v<<8)|(unsigned char)k->s[i];
    return v;
}
static Lin ev_primary(Ev *e){
    Tok2 *k=ev_peek(e);
    if(!k) xfail("expression expected");
    e->i++;
    if(k->k==TK_NUM) return lin_c(k->v);
    if(k->k==TK_STR) return lin_c(str_value(k));
    if(k->k==TK_FLT) xfail("a floating-point number here");
    if(k->k==TK_OP){
        if(k->op=='('){ Lin v=ev_or(e); if(!ev_isop(e,')')) xfail("')' expected"); e->i++; return v; }
        if(k->op=='$'){ if(!e->has_dollar) xfail("$ outside the data segment"); return lin_c(e->dollar); }
        if(k->op=='%'){ if(!e->rep) xfail("%% outside repeat"); return lin_c(e->rep); }
        xfail("unexpected '%c'",k->op);
    }
    if(!strcmp(k->s,"$")){ if(!e->has_dollar) xfail("$ outside the data segment"); return lin_c(e->dollar); }
    int size, r=find_reg(k->s,&size);
    if(r>=0){
        if(!e->allow_regs || size!=4) xfail("register %s in an expression",k->s);
        Lin l=lin_c(0); l.coef[r]=1; l.regs=1; return l;
    }
    int s=sym_find(k->s);
    if(s<0 || syms[s].kind==SY_NONE){
        if(e->pass_final) xfail("undefined symbol '%s'",k->s);
        e->unresolved=1; return lin_c(0);
    }
    return lin_c(sym_value(e,s));
}
static Lin ev_unary(Ev *e){
    if(ev_isop(e,'-')){ e->i++; Lin a=ev_unary(e); a.c=-a.c; for(int i=0;i<8;i++) a.coef[i]=-a.coef[i]; return a; }
    if(ev_isop(e,'+')){ e->i++; return ev_unary(e); }
    if(ev_isop(e,'~') || ev_isword(e,"not")){ e->i++; Lin a=ev_unary(e); need_const(a); a.c=~a.c; return a; }
    return ev_primary(e);
}
static Lin ev_mul(Ev *e){
    Lin a=ev_unary(e);
    for(;;){
        if(ev_isop(e,'*')){ e->i++; Lin b=ev_unary(e);
            if(a.regs && b.regs) xfail("register times register");
            if(b.regs){ Lin t=a; a=b; b=t; }
            for(int i=0;i<8;i++) a.coef[i]*= (int)b.c;
            a.c*=b.c; continue; }
        if(ev_isop(e,'/')){ e->i++; Lin b=ev_unary(e); need_const(a); need_const(b); if(!b.c) xfail("division by zero"); a.c/=b.c; continue; }
        if(ev_isword(e,"mod")){ e->i++; Lin b=ev_unary(e); need_const(a); need_const(b); if(!b.c) xfail("division by zero"); a.c%=b.c; continue; }
        return a;
    }
}
static Lin ev_add(Ev *e){
    Lin a=ev_mul(e);
    for(;;){
        int op= ev_isop(e,'+') ? 1 : ev_isop(e,'-') ? -1 : 0;
        if(!op) return a;
        e->i++; Lin b=ev_mul(e);
        a.c+=op*b.c; for(int i=0;i<8;i++){ a.coef[i]+=op*b.coef[i]; } a.regs=0; for(int i=0;i<8;i++) if(a.coef[i]) a.regs=1;
    }
}
static Lin ev_shift(Ev *e){
    Lin a=ev_add(e);
    for(;;){
        if(ev_isword(e,"shl")){ e->i++; Lin b=ev_add(e); need_const(a); need_const(b); a.c=(i64)((uint64_t)a.c<<b.c); continue; }
        if(ev_isword(e,"shr")){ e->i++; Lin b=ev_add(e); need_const(a); need_const(b); a.c=(i64)((uint64_t)a.c>>b.c); continue; }
        return a;
    }
}
static Lin ev_and(Ev *e){
    Lin a=ev_shift(e);
    while(ev_isword(e,"and")){ e->i++; Lin b=ev_shift(e); need_const(a); need_const(b); a.c&=b.c; }
    return a;
}
static Lin ev_or(Ev *e){
    Lin a=ev_and(e);
    for(;;){
        if(ev_isword(e,"or")){ e->i++; Lin b=ev_and(e); need_const(a); need_const(b); a.c|=b.c; continue; }
        if(ev_isword(e,"xor")){ e->i++; Lin b=ev_and(e); need_const(a); need_const(b); a.c^=b.c; continue; }
        return a;
    }
}
static int layout_final;
static i64 eval_item_const(int item);
static i64 sym_value(Ev *e, int k){
    Sym *s=&syms[k];
    switch(s->kind){
        case SY_CODE: return (i64)code_addr(k);
        case SY_DATA: if(!s->known){ if(e->pass_final) xfail("'%s' has no address",s->name); e->unresolved=1; } return s->val;
        case SY_CONST:
            if(!s->known){
                if(s->item<0) xfail("constant '%s' without a value",s->name);
                if(!e->pass_final){ e->unresolved=1; return s->val; }
                s->val=eval_item_const(s->item); s->known=1;   /* a forward reference */
            }
            return s->val;
        default: xfail("'%s' is not defined",s->name);
    }
    return 0;
}
/* a constant from its tokens; $ is its place in the data */
static i64 eval_item_const(int item){
    Item *it=&items[item];
    Ev e; memset(&e,0,sizeof e); e.t=&it->toks; e.end=it->toks.n; e.rep=it->rep; e.pass_final=layout_final;
    if(it->sec){ e.dollar=it->addr; e.has_dollar=1; }
    Lin v=ev_or(&e); need_const(v);
    if(e.i!=e.end) xfail("junk after an expression");
    return v.c;
}
static i64 eval_range(Toks *t, int a, int b, int rep, i64 dollar, int has_dollar, int final, int *unresolved){
    Ev e; memset(&e,0,sizeof e); e.t=t; e.i=a; e.end=b; e.rep=rep; e.dollar=dollar; e.has_dollar=has_dollar; e.pass_final=final;
    Lin v=ev_or(&e); need_const(v);
    if(e.i!=e.end) xfail("junk after an expression");
    if(unresolved && e.unresolved) *unresolved=1;
    return v.c;
}

/* ---------------------------------------------------------------- reading the listing */
typedef struct { char *text; int line; int rep; char *marker; } Line;
static Line *lines; static int nlines, caplines;
static void line_add(char *text, int line, int rep, char *marker){
    if(nlines==caplines){ caplines=caplines?caplines*2:4096; lines=(Line*)xrealloc(lines,sizeof(Line)*(size_t)caplines); }
    lines[nlines].text=text; lines[nlines].line=line; lines[nlines].rep=rep; lines[nlines].marker=marker; nlines++;
}
/* strip the comment (outside quotes); a ";@..." comment is returned as the marker */
static char *strip_comment(const char *s, size_t n, char **marker){
    char *r=xstrndup2(s,(int)n); char q=0;
    *marker=NULL;
    for(char *p=r;*p;p++){
        if(q){ if(*p==q) q=0; continue; }
        if(*p=='\'' || *p=='"'){ q=*p; continue; }
        if(*p==';'){ if(p[1]=='@') *marker=xstrdup2(p+2); *p=0; break; }
    }
    size_t l=strlen(r); while(l && isspace((unsigned char)r[l-1])) r[--l]=0;
    return r;
}
static const char *skip_ws(const char *p){ while(*p==' '||*p=='\t') p++; return p; }
static int word_is(const char *p, const char *w){ size_t n=strlen(w); return x_ieqn(p,w,n) && !is_idc((unsigned char)p[n]); }
static int is_data_dir(const char *w, int *unit, int *res){
    static const char *d[]={"db","dw","dd","dq","rb","rw","rd","rq"};
    for(int i=0;i<8;i++) if(word_is(w,d[i])){ *unit=1<<(i&3); *res=i>=4; return 1; }
    return 0;
}

/* names a line defines (for `if defined`): labels and constants, not local ones */
static char **defset; static int ndefset, capdefset;
static void def_add(const char *p, size_t n){
    if(!n || p[0]=='.' || p[0]=='@') return;
    if(ndefset==capdefset){ capdefset=capdefset?capdefset*2:1024; defset=(char**)xrealloc(defset,sizeof(char*)*(size_t)capdefset); }
    defset[ndefset++]=xstrndup2(p,(int)n);
}
static int def_has(const char *s){ for(int i=0;i<ndefset;i++) if(!strcmp(defset[i],s)) return 1; return 0; }
static void collect_defs(const char *text){
    const char *p=skip_ws(text), *s=p;
    while(is_idc((unsigned char)*p)) p++;
    size_t n=(size_t)(p-s);
    if(!n) return;
    const char *q=skip_ws(p); int unit, res;
    if(*q==':' ) def_add(s,n);
    else if(*q=='=' || is_data_dir(q,&unit,&res)) def_add(s,n);
}

/* if / else / end if; repeat / end repeat */
static void preprocess(char **raw, int *rawline, char **rawmarker, int nraw){
    for(int i=0;i<nraw;i++) collect_defs(raw[i]);
    int active[64], depth=0; active[0]=1;
    int taken[64];
    for(int i=0;i<nraw;i++){
        x_line=rawline[i];
        const char *p=skip_ws(raw[i]);
        if(word_is(p,"if")){
            p=skip_ws(p+2); int neg=0;
            if(*p=='~'){ neg=1; p=skip_ws(p+1); }
            if(!word_is(p,"defined")) xfail("only `if defined X` is supported");
            p=skip_ws(p+7); const char *s=p; while(is_idc((unsigned char)*p)) p++;
            char name[512]; size_t n=(size_t)(p-s); if(!n||n>=sizeof name) xfail("bad if"); memcpy(name,s,n); name[n]=0;
            int c=def_has(name)^neg;
            if(depth==63) xfail("if nested too deep");
            depth++; taken[depth]=c; active[depth]=active[depth-1]&&c; continue;
        }
        if(word_is(p,"else")){ if(!depth) xfail("else without if"); active[depth]=active[depth-1]&&!taken[depth]; taken[depth]=1; continue; }
        if(word_is(p,"end") && word_is(skip_ws(p+3),"if")){ if(!depth) xfail("end if without if"); depth--; continue; }
        if(!active[depth]) continue;
        if(word_is(p,"repeat")){
            Toks t=tokenize(p+6);
            i64 n=eval_range(&t,0,t.n,0,0,0,1,NULL);
            int j=i+1, nest=1;
            for(;j<nraw;j++){ const char *q=skip_ws(raw[j]);
                if(word_is(q,"repeat")) nest++;
                else if(word_is(q,"end") && word_is(skip_ws(q+3),"repeat") && !--nest) break; }
            if(j>=nraw) xfail("repeat without end repeat");
            for(i64 r=1;r<=n;r++) for(int k=i+1;k<j;k++) line_add(raw[k],rawline[k],(int)r,rawmarker[k]);
            i=j; continue;
        }
        line_add(raw[i],rawline[i],0,rawmarker[i]);
    }
    if(depth) xfail("if without end if");
}

static char entry_name[512];
static char *pending_ccall;

static void parse_lines(void){
    int sec=0;
    cur_base[0]=0; anon_count=0;
    for(int li=0;li<nlines;li++){
        Line *L=&lines[li]; x_line=L->line;
        if(L->marker){
            if(!strncmp(L->marker,"ccall ",6)) pending_ccall=xstrdup2(skip_ws(L->marker+6));
            else if(!strncmp(L->marker,"import ",7)){
                char a[256], b[256], c[256];
                if(sscanf(L->marker+7,"%255s %255s %255s",a,b,c)!=3) xfail("bad ;@import");
                imports=(Import*)xrealloc(imports,sizeof(Import)*(size_t)(nimports+1));
                imports[nimports].slot=xstrdup2(a); imports[nimports].lib=xstrdup2(b); imports[nimports].sym=xstrdup2(c); imports[nimports].slotsym=-1; nimports++;
            }
        }
        const char *p=skip_ws(L->text);
        if(!*p) continue;
        /* label: */
        const char *s=p; while(is_idc((unsigned char)*p)) p++;
        if(p>s && *p==':' && p[1]!='='){
            char name[1024]; size_t n=(size_t)(p-s); if(n>=sizeof name) xfail("label too long");
            memcpy(name,s,n); name[n]=0;
            char *full;
            if(!strcmp(name,"@@")){ anon_count++; char b[32]; snprintf(b,sizeof b,"@@%d",anon_count); full=xstrdup2(b); }
            else { full=resolve_name(name); if(name[0]!='.') snprintf(cur_base,sizeof cur_base,"%s",name); }
            int k=sym_get(full); free(full);
            if(syms[k].kind!=SY_NONE) xfail("'%s' is defined twice",syms[k].name);
            syms[k].kind=sec?SY_DATA:SY_CODE;
            Item *it=item_new(IT_LABEL,L->line,sec); it->sym=k;
            p=skip_ws(p+1);
            if(!*p) continue;
            s=p; while(is_idc((unsigned char)*p)) p++;
        }
        size_t wn=(size_t)(p-s);
        char w[1024]; if(wn>=sizeof w) xfail("unknown statement"); memcpy(w,s,wn); w[wn]=0;
        const char *rest=skip_ws(p);
        int unit, res;
        if(x_ieq(w,"format") || x_ieq(w,"use32") || x_ieq(w,"org")) continue;
        if(x_ieq(w,"entry")){ snprintf(entry_name,sizeof entry_name,"%s",rest); continue; }
        if(x_ieq(w,"segment")){ sec=strstr(rest,"writeable")||strstr(rest,"writable") ? 1 : 0; continue; }
        if(x_ieq(w,"align")){ Item *it=item_new(IT_ALIGN,L->line,sec); it->toks=tokenize(rest); it->rep=L->rep; continue; }
        if(is_data_dir(s,&unit,&res)){
            if(!sec) xfail("data in the code segment");
            Item *it=item_new(res?IT_RES:IT_DATA,L->line,sec); it->unit=unit; it->toks=tokenize(rest); it->rep=L->rep; continue;
        }
        if(*rest=='='){                                  /* X = expr */
            char *full=resolve_name(w); int k=sym_get(full); free(full);
            if(syms[k].kind!=SY_NONE && syms[k].kind!=SY_CONST) xfail("'%s' is defined twice",w);
            syms[k].kind=SY_CONST;
            Item *it=item_new(IT_CONST,L->line,sec); it->sym=k; it->toks=tokenize(rest+1); it->rep=L->rep;
            syms[k].item=(int)(it-items);
            continue;
        }
        if(is_data_dir(rest,&unit,&res)){                /* name db ... */
            if(!sec) xfail("data in the code segment");
            char *full=resolve_name(w); int k=sym_get(full); free(full);
            if(w[0]!='.') snprintf(cur_base,sizeof cur_base,"%s",w);
            if(syms[k].kind!=SY_NONE) xfail("'%s' is defined twice",syms[k].name);
            syms[k].kind=SY_DATA;
            Item *lb=item_new(IT_LABEL,L->line,sec); lb->sym=k;
            Item *it=item_new(res?IT_RES:IT_DATA,L->line,sec); it->unit=unit; it->toks=tokenize(skip_ws(rest+2)); it->rep=L->rep;
            continue;
        }
        /* an instruction */
        if(sec) xfail("instruction '%s' in the data segment",w);
        Item *it=item_new(IT_INSN,L->line,sec);
        for(char *c=w;*c;c++) *c=(char)tolower((unsigned char)*c);
        if(!strcmp(w,"rep")||!strcmp(w,"repe")||!strcmp(w,"repz")||!strcmp(w,"repne")||!strcmp(w,"repnz")){
            const char *q=rest; while(is_idc((unsigned char)*q)) q++;
            char w2[32]; size_t n2=(size_t)(q-rest); if(!n2||n2>=sizeof w2) xfail("rep without an instruction");
            memcpy(w2,rest,n2); w2[n2]=0;
            const char *pre=!strcmp(w,"repz")?"repe":!strcmp(w,"repnz")?"repne":w;
            snprintf(it->mnem,sizeof it->mnem,"%s %s",pre,w2);
            rest=skip_ws(q);
        } else snprintf(it->mnem,sizeof it->mnem,"%s",w);
        it->toks=tokenize(rest);
        it->ccall=pending_ccall; pending_ccall=NULL;
    }
}

/* ---------------------------------------------------------------- data layout */
static int data_item_size(Item *it){
    Toks *t=&it->toks; int n=0, a=0;
    for(int i=0;i<=t->n;i++){
        if(i<t->n && !(t->v[i].k==TK_OP && t->v[i].op==',')) continue;
        if(i==a) xfail("empty data item");
        if(it->unit==1 && i-a==1 && t->v[a].k==TK_STR) n+=(int)t->v[a].v;
        else {
            for(int k=a;k<i;k++) if(t->v[k].k==TK_ID && x_ieq(t->v[k].s,"dup")) xfail("dup is not supported");
            n+=it->unit;
        }
        a=i+1;
    }
    return n;
}
static u32 data_end, scratch_addr;
/* addresses of the data; returns 1 while some are not known or still moving */
static int layout(int final){
    u32 addr=DATA_BASE; int unresolved=0, moved=0;
    layout_final=final;
    for(int i=0;i<nitems;i++){ Item *it=&items[i];
        x_line=it->line;
        if(it->kind==IT_CONST){                      /* in either segment ($: only in the data) */
            Sym *s=&syms[it->sym];
            it->addr=addr;
            Ev e; memset(&e,0,sizeof e); e.t=&it->toks; e.end=it->toks.n; e.rep=it->rep; e.dollar=addr; e.has_dollar=it->sec; e.pass_final=final;
            Lin v=ev_or(&e); need_const(v); if(e.i!=e.end) xfail("junk after an expression");
            if(e.unresolved) unresolved=1;
            if(!s->known || s->val!=v.c) moved=1;
            s->val=v.c; s->known|=!e.unresolved;
            continue;
        }
        if(!it->sec) continue;
        it->addr=addr;
        switch(it->kind){
            case IT_LABEL: if(syms[it->sym].val!=(i64)addr) moved=1; syms[it->sym].val=addr; syms[it->sym].known=1; break;
            case IT_ALIGN:{ i64 a=eval_range(&it->toks,0,it->toks.n,it->rep,addr,1,final,&unresolved);
                if(a<=0 || (a&(a-1))) xfail("align needs a power of two");
                addr=(addr+(u32)a-1)&~((u32)a-1); break; }
            case IT_DATA: addr+=(u32)data_item_size(it); break;
            case IT_RES:{ i64 n=eval_range(&it->toks,0,it->toks.n,it->rep,addr,1,final,&unresolved);
                if(n<0) xfail("negative reservation"); addr+=(u32)(n*it->unit); break; }
            default: break;
        }
    }
    addr=(addr+15u)&~15u; scratch_addr=addr; addr+=1024;   /* the native routines' text */
    data_end=addr;
    return unresolved||moved;
}

/* the initialized bytes from DATA_BASE (the rest is zero) */
static unsigned char *image; static u32 image_len;
static void image_put(u32 addr, i64 v, int unit){
    u32 off=addr-DATA_BASE;
    if(off+(u32)unit>image_len) image_len=off+(u32)unit;
    for(int i=0;i<unit;i++) image[off+(u32)i]=(unsigned char)((uint64_t)v>>(8*i));
}
static void build_image(void){
    image=(unsigned char*)xmalloc(data_end-DATA_BASE+16); memset(image,0,data_end-DATA_BASE+16); image_len=0;
    for(int i=0;i<nitems;i++){ Item *it=&items[i];
        if(it->kind!=IT_DATA) continue;
        x_line=it->line;
        Toks *t=&it->toks; u32 addr=it->addr; int a=0;
        for(int k=0;k<=t->n;k++){
            if(k<t->n && !(t->v[k].k==TK_OP && t->v[k].op==',')) continue;
            if(it->unit==1 && k-a==1 && t->v[a].k==TK_STR){
                for(i64 j=0;j<t->v[a].v;j++) image_put(addr++,(unsigned char)t->v[a].s[j],1);
            } else if(k-a==1 && t->v[a].k==TK_FLT){
                if(it->unit==8){ double d=strtod(t->v[a].s,NULL); i64 bits; memcpy(&bits,&d,8); image_put(addr,bits,8); }
                else if(it->unit==4){ float f=(float)strtod(t->v[a].s,NULL); int32_t bits; memcpy(&bits,&f,4); image_put(addr,(i64)(uint32_t)bits,4); }
                else xfail("a floating-point number in db/dw");
                addr+=(u32)it->unit;
            } else if(k-a==2 && t->v[a].k==TK_OP && t->v[a].op=='-' && t->v[a+1].k==TK_FLT && it->unit==8){
                double d=-strtod(t->v[a+1].s,NULL); i64 bits; memcpy(&bits,&d,8); image_put(addr,bits,8); addr+=8;
            } else {
                i64 v=eval_range(t,a,k,it->rep,addr,1,1,NULL);
                image_put(addr,v,it->unit); addr+=(u32)it->unit;
            }
            a=k+1;
        }
    }
}

/* ---------------------------------------------------------------- operands */
typedef enum { O_NONE, O_REG, O_MEM, O_IMM, O_ST } OKind;
typedef struct { OKind k; int size; int reg; Lin addr; i64 imm; int st; int code_sym; } Opnd;

static int size_word(const char *s){
    if(x_ieq(s,"byte")) return 1; if(x_ieq(s,"word")) return 2; if(x_ieq(s,"dword")) return 4;
    if(x_ieq(s,"qword")) return 8; if(x_ieq(s,"tword")) return 10; return 0;
}
static Opnd parse_opnd(Toks *t, int a, int b){
    Opnd o; memset(&o,0,sizeof o); o.code_sym=-1;
    if(a>=b) xfail("operand expected");
    if(t->v[a].k==TK_ID && size_word(t->v[a].s)){ o.size=size_word(t->v[a].s); a++; }
    if(a>=b) xfail("operand expected");
    Tok2 *k=&t->v[a];
    if(k->k==TK_OP && k->op=='['){
        if(!(t->v[b-1].k==TK_OP && t->v[b-1].op==']')) xfail("']' expected");
        Ev e; memset(&e,0,sizeof e); e.t=t; e.i=a+1; e.end=b-1; e.pass_final=1; e.allow_regs=1;
        o.addr=ev_or(&e); if(e.i!=e.end) xfail("junk in an address");
        int nr=0; for(int i=0;i<8;i++) if(o.addr.coef[i]){ nr++; int c=o.addr.coef[i]; if(c!=1&&c!=2&&c!=4&&c!=8&&c!=3&&c!=5&&c!=9) xfail("bad scale"); }
        if(nr>2) xfail("more than two registers in an address");
        o.k=O_MEM; return o;
    }
    if(b-a==1 && k->k==TK_ID){
        int size, r=find_reg(k->s,&size);
        if(r>=0){ o.k=O_REG; o.reg=r; if(o.size && o.size!=size) xfail("operand size mismatch"); o.size=size; return o; }
        if(x_ieq(k->s,"st")){ o.k=O_ST; o.st=0; return o; }
        if((k->s[0]=='s'||k->s[0]=='S') && (k->s[1]=='t'||k->s[1]=='T') && k->s[2]>='0' && k->s[2]<='7' && !k->s[3]){ o.k=O_ST; o.st=k->s[2]-'0'; return o; }
        int s=sym_find(k->s);
        if(s>=0 && syms[s].kind==SY_CODE) o.code_sym=s;     /* a jump/call target */
    }
    o.k=O_IMM; o.imm=eval_range(t,a,b,0,0,0,1,NULL);
    return o;
}
static int split_opnds(Toks *t, Opnd *o, int max){
    int n=0, a=0, depth=0;
    for(int i=0;i<=t->n;i++){
        if(i<t->n){ Tok2 *k=&t->v[i];
            if(k->k==TK_OP && (k->op=='['||k->op=='(')) depth++;
            if(k->k==TK_OP && (k->op==']'||k->op==')')) depth--;
            if(!(k->k==TK_OP && k->op==',' && depth==0)) continue; }
        if(i==a && i==t->n && n==0) break;
        if(n==max) xfail("too many operands");
        o[n++]=parse_opnd(t,a,i); a=i+1;
    }
    return n;
}

/* C text of operands */
static XBuf out;
static char cbuf[8][512]; static int cbi;
static char *cstr(void){ cbi=(cbi+1)&7; return cbuf[cbi]; }
static const char *addr_c(Lin *l){
    char *s=cstr(); int n=0; s[0]=0;
    for(int i=0;i<8;i++) if(l->coef[i]){
        if(l->coef[i]==1) n+=snprintf(s+n,512-(size_t)n,"%s%s",n?"+":"",reg32[i]);
        else n+=snprintf(s+n,512-(size_t)n,"%s%s*%du",n?"+":"",reg32[i],l->coef[i]);
    }
    if(l->c || !n) n+=snprintf(s+n,512-(size_t)n,"%s0x%Xu",n?"+":"",(u32)l->c);
    return s;
}
/* memory operands are read and written through a_ (set by mem_setup) */
static int cur_has_mem;
static void mem_setup(Opnd *o, int n){
    cur_has_mem=0;
    for(int i=0;i<n;i++) if(o[i].k==O_MEM){
        if(cur_has_mem) xfail("two memory operands");
        cur_has_mem=1; xb_printf(&out,"u32 a_=%s; ",addr_c(&o[i].addr));
    }
}
static const char *mem_acc(int size){
    switch(size){ case 1: return "B(a_)"; case 2: return "W(a_)"; case 4: return "D(a_)"; case 8: return "Q(a_)"; }
    xfail("unsupported operand size %d",size); return "";
}
static const char *rd(Opnd *o, int size){
    char *s=cstr();
    switch(o->k){
        case O_REG:
            if(size==4) return reg32[o->reg];
            if(size==2){ snprintf(s,512,"(u16)%s",reg32[o->reg]); return s; }
            if(o->reg<4) snprintf(s,512,"(u8)%s",reg32[o->reg]); else snprintf(s,512,"(u8)(%s>>8)",reg32[o->reg-4]);
            return s;
        case O_MEM: return mem_acc(size);
        case O_IMM:
            if(size==1) snprintf(s,512,"0x%Xu",(u32)o->imm&0xFFu);
            else if(size==2) snprintf(s,512,"0x%Xu",(u32)o->imm&0xFFFFu);
            else snprintf(s,512,"0x%Xu",(u32)o->imm);
            return s;
        default: xfail("bad operand"); return "";
    }
}
static void wr(Opnd *o, int size, const char *v){
    switch(o->k){
        case O_REG:
            if(size==4) xb_printf(&out,"%s=%s; ",reg32[o->reg],v);
            else if(size==2) xb_printf(&out,"SET16(%s,%s); ",reg32[o->reg],v);
            else if(o->reg<4) xb_printf(&out,"SET8L(%s,%s); ",reg32[o->reg],v);
            else xb_printf(&out,"SET8H(%s,%s); ",reg32[o->reg-4],v);
            return;
        case O_MEM:
            if(size==1) xb_printf(&out,"B(a_)=(u8)(%s); ",v);
            else if(size==2) xb_printf(&out,"W(a_)=(u16)(%s); ",v);
            else xb_printf(&out,"D(a_)=%s; ",v);
            return;
        default: xfail("cannot write this operand");
    }
}
static int opsize(Opnd *o, int n){
    int s=0;
    for(int i=0;i<n;i++) if(o[i].k==O_REG || (o[i].k==O_MEM && o[i].size)){ if(s && o[i].size && o[i].size!=s) xfail("operand size mismatch"); if(o[i].size) s=o[i].size; }
    if(!s) xfail("operand size unknown");
    return s;
}
static int shift_of(int size){ return size==1?24:size==2?16:0; }

/* ---------------------------------------------------------------- code */
static int nret;
static int cond_code(const char *c){
    static const struct { const char *s; int cc; } t[]={
        {"o",0},{"no",1},{"b",2},{"c",2},{"nae",2},{"ae",3},{"nb",3},{"nc",3},{"e",4},{"z",4},{"ne",5},{"nz",5},
        {"be",6},{"na",6},{"a",7},{"nbe",7},{"s",8},{"ns",9},{"p",10},{"pe",10},{"np",11},{"po",11},
        {"l",12},{"nge",12},{"ge",13},{"nl",13},{"le",14},{"ng",14},{"g",15},{"nle",15},{NULL,0}};
    for(int i=0;t[i].s;i++) if(!strcmp(t[i].s,c)) return t[i].cc;
    return -1;
}
static const char *cc_name[16]={"CC_O","CC_NO","CC_B","CC_AE","CC_E","CC_NE","CC_BE","CC_A","CC_S","CC_NS","CC_P","CC_NP","CC_L","CC_GE","CC_LE","CC_G"};

static void jump_to(Opnd *o){
    if(o->k==O_IMM && o->code_sym>=0){ xb_printf(&out,"goto L%d;",o->code_sym); return; }
    if(o->k==O_IMM) xfail("jump to a non-code address");
    xb_printf(&out,"pc=%s; goto dispatch;",rd(o,4));
}
static void flags(const char *kind, int size, const char *a, const char *b, const char *r){
    int sh=shift_of(size);
    xb_printf(&out,"fk=%s|%d; ",kind,sh<<8);
    if(a) xb_printf(&out,"fa=(u32)(%s)<<%d; ",a,sh);
    if(b) xb_printf(&out,"fb=(u32)(%s)<<%d; ",b,sh);
    if(r) xb_printf(&out,"fr=(u32)(%s)<<%d; ",r,sh);
}

/* a C function through ctypes: `call [CI<k>]` with its ;@ccall signature;
   the arguments are at [esp] (cdecl, the i386 layout) */
static void ccall(Item *it, Opnd *o){
    if(o->k!=O_MEM || o->addr.regs) xfail("C call through a register");
    int imp=-1;
    for(int i=0;i<nimports;i++) if(syms[imports[i].slotsym].val==o->addr.c) imp=i;
    if(imp<0) xfail("C call to an unknown import slot");
    const char *sig=it->ccall, *p=sig;
    char ret=*p++;
    if(*p++!=':') xfail("bad ;@ccall signature");
    XBuf proto, args; memset(&proto,0,sizeof proto); memset(&args,0,sizeof args); xb_put(&proto,"",0); xb_put(&args,"",0);
    int off=0, nfixed=0, var=0;
    for(;*p;p++){
        if(*p=='|'){ var=1; continue; }
        const char *ct, *val; char v[128];
        switch(*p){
            case 'i': ct="int"; snprintf(v,sizeof v,"(int)(i32)D(s_+%d)",off); off+=4; break;
            case 'u': ct="unsigned"; snprintf(v,sizeof v,"D(s_+%d)",off); off+=4; break;
            case 'h': ct="short"; snprintf(v,sizeof v,"(short)D(s_+%d)",off); off+=4; break;
            case 'H': ct="unsigned short"; snprintf(v,sizeof v,"(unsigned short)D(s_+%d)",off); off+=4; break;
            case 'b': ct="signed char"; snprintf(v,sizeof v,"(signed char)D(s_+%d)",off); off+=4; break;
            case 'B': ct="unsigned char"; snprintf(v,sizeof v,"(unsigned char)D(s_+%d)",off); off+=4; break;
            case 'o': ct="_Bool"; snprintf(v,sizeof v,"D(s_+%d)!=0",off); off+=4; break;
            case 'l': ct="long"; snprintf(v,sizeof v,"(long)(i32)D(s_+%d)",off); off+=4; break;
            case 'L': ct="unsigned long"; snprintf(v,sizeof v,"(unsigned long)D(s_+%d)",off); off+=4; break;
            case 'q': ct="long long"; snprintf(v,sizeof v,"(long long)Q(s_+%d)",off); off+=8; break;
            case 'Q': ct="unsigned long long"; snprintf(v,sizeof v,"(unsigned long long)Q(s_+%d)",off); off+=8; break;
            case 'd': ct="double"; snprintf(v,sizeof v,"FD(s_+%d)",off); off+=8; break;
            case 'f': ct="float"; snprintf(v,sizeof v,"FS(s_+%d)",off); off+=4; break;
            case 's': ct="char*"; snprintf(v,sizeof v,"(char*)x2c_ptr(D(s_+%d))",off); off+=4; break;
            case 'p': ct="void*"; snprintf(v,sizeof v,"x2c_ptr(D(s_+%d))",off); off+=4; break;
            default: xfail("bad ;@ccall type '%c'",*p); return;
        }
        val=v;
        if(!var){ xb_printf(&proto,"%s%s",nfixed?",":"",ct); nfixed++; }
        xb_printf(&args,"%s%s",args.len?",":"",val);
    }
    if(var) xb_printf(&proto,"%s...",nfixed?",":"");
    else if(!nfixed) xb_printf(&proto,"void");
    const char *rt;
    switch(ret){
        case 'v': rt="void"; break; case 'i': rt="int"; break; case 'u': rt="unsigned"; break; case 'h': rt="short"; break;
        case 'H': rt="unsigned short"; break; case 'b': rt="signed char"; break; case 'B': rt="unsigned char"; break; case 'o': rt="_Bool"; break;
        case 'l': rt="long"; break; case 'L': rt="unsigned long"; break; case 'q': rt="long long"; break; case 'Q': rt="unsigned long long"; break;
        case 'd': rt="double"; break; case 'f': rt="float"; break; case 's': rt="char*"; break; case 'p': rt="void*"; break;
        default: xfail("bad ;@ccall result '%c'",ret); return;
    }
    int errno_fn=!strcmp(imports[imp].sym,"__errno_location");
    xb_printf(&out,"{ u32 s_=esp; ");
    if(ret!='v') xb_printf(&out,"%s r_=",rt);
    xb_printf(&out,"((%s(*)(%s))CFN(%d))(%s); ",rt,proto.s,imp,args.s);
    if(!errno_fn) xb_printf(&out,"x2c_errno=errno; ");
    switch(ret){
        case 'v': break;
        case 'q': case 'Q': xb_printf(&out,"eax=(u32)r_; edx=(u32)((unsigned long long)r_>>32); "); break;
        case 'd': case 'f': xb_printf(&out,"FPUSH((double)r_); "); break;
        case 's': xb_printf(&out,"eax=x2c_unstr(r_); "); break;
        case 'p': xb_printf(&out,"eax=x2c_unptr(r_); "); break;
        default: xb_printf(&out,"eax=(u32)r_; "); break;
    }
    xb_printf(&out,"}");
    free(proto.s); free(args.s);
}

/* x87 memory operands */
static const char *fmem(Opnd *o, int integer){
    char *s=cstr();
    if(o->k!=O_MEM) xfail("x87 memory operand expected");
    if(integer){
        if(o->size==2) return "(double)(i16)W(a_)";
        if(o->size==4) return "(double)(i32)D(a_)";
        if(o->size==8) return "(double)(i64)Q(a_)";
    } else {
        if(o->size==4) return "(double)FS(a_)";
        if(o->size==8) return "FD(a_)";
    }
    xfail("x87 operand size"); (void)s; return "";
}
/* fadd fsub fsubr fmul fdiv fdivr [p] and fiadd ... */
static void farith(const char *m, Opnd *o, int n){
    int pop=0, integer=0; char base[16]; snprintf(base,sizeof base,"%s",m+1);   /* add, subrp, ... */
    if(m[1]=='i'){ integer=1; memmove(base,base+1,strlen(base)); }
    size_t bl=strlen(base);
    if(bl && base[bl-1]=='p' && strcmp(base,"p")){ pop=1; base[--bl]=0; }
    int rev=0;
    if(bl==4 && base[3]=='r'){ rev=1; base[3]=0; }           /* subr divr */
    char op= !strcmp(base,"add")?'+' : !strcmp(base,"sub")?'-' : !strcmp(base,"mul")?'*' : !strcmp(base,"div")?'/' : 0;
    if(!op) xfail("unknown x87 instruction %s",m);
    const char *dst, *src; char d[32], s[64];
    if(n==0){ if(!pop) xfail("%s needs operands",m); snprintf(d,sizeof d,"ST(1)"); snprintf(s,sizeof s,"ST(0)"); }
    else if(n==1){ if(pop) xfail("%s with one operand",m); snprintf(d,sizeof d,"ST(0)"); snprintf(s,sizeof s,"%s",fmem(&o[0],integer)); }
    else { if(o[0].k!=O_ST||o[1].k!=O_ST) xfail("%s operands",m); snprintf(d,sizeof d,"ST(%d)",o[0].st); snprintf(s,sizeof s,"ST(%d)",o[1].st); }
    dst=d; src=s;
    if(rev) xb_printf(&out,"%s=%s%c%s; ",dst,src,op,dst);
    else xb_printf(&out,"%s=%s%c%s; ",dst,dst,op,src);
    if(pop) xb_printf(&out,"FPOP(); ");
}

static int native_skip;
static int in_native(const char *name);

static void insn(Item *it){
    Opnd o[3]; int n;
    const char *m=it->mnem;
    x_line=it->line;
    /* push / pop of several registers */
    if((!strcmp(m,"push")||!strcmp(m,"pop")) && it->toks.n>1){
        int all=1; for(int i=0;i<it->toks.n;i++){ int sz; if(it->toks.v[i].k!=TK_ID || find_reg(it->toks.v[i].s,&sz)<0 || sz!=4) all=0; }
        if(all){
            for(int i=0;i<it->toks.n;i++){ int sz, r=find_reg(it->toks.v[i].s,&sz);
                if(m[1]=='u') xb_printf(&out,"{ u32 v_=%s; esp-=4; D(esp)=v_; } ",reg32[r]);
                else xb_printf(&out,"%s=D(esp); esp+=4; ",reg32[r]); }
            return;
        }
    }
    n=split_opnds(&it->toks,o,3);
    xb_printf(&out,"{ ");
    mem_setup(o,n);
    #define IS(x) (!strcmp(m,x))
    if(IS("mov")){
        if(n!=2) xfail("mov needs two operands");
        int s=opsize(o,2); wr(&o[0],s,rd(&o[1],s));
    }
    else if(IS("movzx")||IS("movsx")){
        if(n!=2 || o[0].k!=O_REG) xfail("%s operands",m);
        int ss=o[1].k==O_REG?o[1].size:o[1].size; if(!ss) xfail("%s: source size unknown",m);
        char v[256];
        if(m[3]=='z') snprintf(v,sizeof v,"(u32)%s",rd(&o[1],ss));
        else snprintf(v,sizeof v,"(u32)(i32)(%s)%s",ss==1?"i8":"i16",rd(&o[1],ss));
        wr(&o[0],o[0].size,v);
    }
    else if(IS("lea")){ if(n!=2||o[1].k!=O_MEM||o[0].k!=O_REG) xfail("lea operands"); wr(&o[0],4,"a_"); }
    else if(IS("xchg")){
        if(n!=2) xfail("xchg operands");
        int s=opsize(o,2); xb_printf(&out,"u32 t_=%s; ",rd(&o[0],s)); wr(&o[0],s,rd(&o[1],s)); wr(&o[1],s,"t_");
    }
    else if(IS("push")){
        if(n!=1) xfail("push operands");
        int s=o[0].k==O_IMM?4:opsize(o,1); if(s!=4) xfail("push of %d bytes",s);
        xb_printf(&out,"u32 v_=%s; esp-=4; D(esp)=v_; ",rd(&o[0],4));
    }
    else if(IS("pop")){
        if(n!=1) xfail("pop operands");
        xb_printf(&out,"u32 v_=D(esp); esp+=4; ");
        if(o[0].k==O_MEM) xb_printf(&out,"a_=%s; ",addr_c(&o[0].addr));     /* an esp-based address counts after the pop */
        wr(&o[0],4,"v_");
    }
    else if(IS("add")||IS("sub")||IS("and")||IS("or")||IS("xor")||IS("cmp")||IS("test")){
        if(n!=2) xfail("%s needs two operands",m);
        int s=opsize(o,2);
        char op= IS("add")?'+' : IS("sub")||IS("cmp")?'-' : IS("and")||IS("test")?'&' : IS("or")?'|' : '^';
        xb_printf(&out,"u32 x_=%s, y_=%s, r_=x_%cy_; ",rd(&o[0],s),rd(&o[1],s),op);
        if(!IS("cmp")&&!IS("test")) wr(&o[0],s,"r_");
        if(op=='+') flags("FK_ADD",s,"x_","y_","r_");
        else if(op=='-') flags("FK_SUB",s,"x_","y_","r_");
        else flags("FK_LOGIC",s,NULL,NULL,"r_");
    }
    else if(IS("adc")||IS("sbb")){
        if(n!=2) xfail("%s needs two operands",m);
        int s=opsize(o,2), sh=shift_of(s);
        if(sh) xfail("%s of %d bytes",m,s);
        xb_printf(&out,"u32 x_=%s, y_=%s, c_=CF_NOW, r_; ",rd(&o[0],s),rd(&o[1],s));
        if(IS("adc")) xb_printf(&out,"r_=x_+y_+c_; fc=c_?r_<=x_:r_<x_; ");
        else xb_printf(&out,"r_=x_-y_-c_; fc=c_?x_<=y_:x_<y_; ");
        wr(&o[0],s,"r_"); flags(IS("adc")?"FK_ADC":"FK_SBB",s,"x_","y_","r_");
    }
    else if(IS("shld")||IS("shrd")){
        if(n!=3) xfail("%s needs three operands",m);
        if(opsize(o,2)!=4) xfail("%s of a non-dword",m);
        if(o[2].k==O_IMM) xb_printf(&out,"u32 c_=%uu; ",(u32)o[2].imm&31u);
        else if(o[2].k==O_REG && o[2].size==1 && o[2].reg==1) xb_printf(&out,"u32 c_=ecx&31u; ");
        else xfail("shift count must be a number or cl");
        xb_printf(&out,"if(c_){ u32 x_=%s, y_=%s, r_; ",rd(&o[0],4),rd(&o[1],4));
        if(IS("shld")) xb_printf(&out,"r_=(x_<<c_)|(y_>>(32-c_)); fc=(x_>>(32-c_))&1u; ");
        else xb_printf(&out,"r_=(x_>>c_)|(y_<<(32-c_)); fc=(x_>>(c_-1))&1u; ");
        wr(&o[0],4,"r_"); flags("FK_SHIFT",4,NULL,NULL,"r_"); xb_printf(&out,"} ");
    }
    else if(IS("inc")||IS("dec")){
        if(n!=1) xfail("%s operands",m);
        int s=opsize(o,1);
        xb_printf(&out,"u32 x_=%s, r_=x_%c1u; ",rd(&o[0],s),m[0]=='i'?'+':'-'); wr(&o[0],s,"r_");
        xb_printf(&out,"fc=CF_NOW; "); flags(m[0]=='i'?"FK_INC":"FK_DEC",s,"x_","1u","r_");
    }
    else if(IS("neg")){
        if(n!=1) xfail("neg operands");
        int s=opsize(o,1);
        xb_printf(&out,"u32 x_=%s, r_=0u-x_; ",rd(&o[0],s)); wr(&o[0],s,"r_"); flags("FK_SUB",s,"0u","x_","r_");
    }
    else if(IS("not")){ if(n!=1) xfail("not operands"); int s=opsize(o,1); char v[300]; snprintf(v,sizeof v,"~%s",rd(&o[0],s)); wr(&o[0],s,v); }
    else if(IS("shl")||IS("sal")||IS("shr")||IS("sar")){
        if(n!=2) xfail("%s operands",m);
        int s=opsize(o,1), bits=8*s;
        if(o[1].k==O_IMM) xb_printf(&out,"u32 c_=%uu; ",(u32)o[1].imm&31u);
        else if(o[1].k==O_REG && o[1].size==1 && o[1].reg==1) xb_printf(&out,"u32 c_=ecx&31u; ");
        else xfail("shift count must be a number or cl");
        xb_printf(&out,"if(c_){ u32 x_=%s, r_; ",rd(&o[0],s));
        if(!strcmp(m,"shl")||!strcmp(m,"sal")) xb_printf(&out,"r_=x_<<c_; fc=c_<=%d?(x_>>(%d-c_))&1u:0u; ",bits,bits);
        else if(!strcmp(m,"shr")) xb_printf(&out,"r_=x_>>c_; fc=(x_>>(c_-1))&1u; ");
        else xb_printf(&out,"i32 sx_=(i32)(%s)x_; r_=(u32)(sx_>>c_); fc=(u32)(sx_>>(c_-1))&1u; ",s==1?"i8":s==2?"i16":"i32");
        wr(&o[0],s,"r_"); flags("FK_SHIFT",s,NULL,NULL,"r_"); xb_printf(&out,"} ");
    }
    else if(IS("imul")){
        if(n==1){ int s=opsize(o,1); if(s!=4) xfail("imul size");
            xb_printf(&out,"i64 p_=(i64)(i32)eax*(i64)(i32)%s; eax=(u32)p_; edx=(u32)((uint64_t)p_>>32); fc=p_!=(i64)(i32)p_; fk=FK_MUL; fr=eax; ",rd(&o[0],4)); }
        else { Opnd *d=&o[0], *a=n==3?&o[1]:&o[0], *b=n==3?&o[2]:&o[1];
            if(d->k!=O_REG || d->size!=4) xfail("imul operands");
            xb_printf(&out,"i64 p_=(i64)(i32)%s*(i64)(i32)%s; ",rd(a,4),rd(b,4)); wr(d,4,"(u32)p_");
            xb_printf(&out,"fc=p_!=(i64)(i32)p_; fk=FK_MUL; fr=(u32)p_; "); }
    }
    else if(IS("mul")){
        if(n!=1) xfail("mul operands"); int s=opsize(o,1); if(s!=4) xfail("mul size");
        xb_printf(&out,"uint64_t p_=(uint64_t)eax*%s; eax=(u32)p_; edx=(u32)(p_>>32); fc=edx!=0; fk=FK_MUL; fr=eax; ",rd(&o[0],4));
    }
    else if(IS("div")||IS("idiv")){
        if(n!=1) xfail("%s operands",m); int s=opsize(o,1);
        if(s==4 && m[0]=='d') xb_printf(&out,"u32 d_=%s; uint64_t n_=((uint64_t)edx<<32)|eax; if(!d_||(n_/d_)>>32) x2c_fatal(\"division error\"); eax=(u32)(n_/d_); edx=(u32)(n_%%d_); ",rd(&o[0],4));
        else if(s==4) xb_printf(&out,"i32 d_=(i32)%s; i64 n_=(i64)(((uint64_t)edx<<32)|eax); if(!d_||(d_==-1&&n_==INT64_MIN)||n_/d_!=(i32)(n_/d_)) x2c_fatal(\"division error\"); eax=(u32)(i32)(n_/d_); edx=(u32)(i32)(n_%%d_); ",rd(&o[0],4));
        else if(s==1 && m[0]=='d') xb_printf(&out,"u32 d_=%s, n_=(u16)eax; if(!d_||n_/d_>255) x2c_fatal(\"division error\"); SET8L(eax,n_/d_); SET8H(eax,n_%%d_); ",rd(&o[0],1));
        else xfail("%s of this size",m);
    }
    else if(IS("cdq")) xb_printf(&out,"edx=(u32)((i32)eax>>31); ");
    else if(IS("cwde")) xb_printf(&out,"eax=(u32)(i32)(i16)eax; ");
    else if(m[0]=='s' && m[1]=='e' && m[2]=='t' && cond_code(m+3)>=0){
        if(n!=1) xfail("set operands");
        char v[64]; snprintf(v,sizeof v,"(u32)CC(%s)",cc_name[cond_code(m+3)]); wr(&o[0],1,v);
    }
    else if(IS("jmp")){ if(n!=1) xfail("jmp operands"); jump_to(&o[0]); }
    else if(m[0]=='j' && cond_code(m+1)>=0){
        if(n!=1 || o[0].code_sym<0) xfail("conditional jump to a non-label");
        xb_printf(&out,"if(CC(%s)) goto L%d; ",cc_name[cond_code(m+1)],o[0].code_sym);
    }
    else if(IS("jecxz")){ if(n!=1 || o[0].code_sym<0) xfail("jecxz target"); xb_printf(&out,"if(!ecx) goto L%d; ",o[0].code_sym); }
    else if(IS("call")){
        if(n!=1) xfail("call operands");
        if(it->ccall){ xb_printf(&out,"} "); ccall(it,&o[0]); xb_printf(&out,"\n"); return; }
        int r=nret++; int ci=ctab_add(-1-r);
        if(o[0].k==O_IMM && o[0].code_sym>=0) xb_printf(&out,"esp-=4; D(esp)=0x%Xu; goto L%d; } R%d: ;\n",CODE_BASE+4u*(u32)ci,o[0].code_sym,r);
        else { if(o[0].k==O_IMM) xfail("call to a non-code address");
            xb_printf(&out,"u32 t_=%s; esp-=4; D(esp)=0x%Xu; pc=t_; goto dispatch; } R%d: ;\n",rd(&o[0],4),CODE_BASE+4u*(u32)ci,r); }
        return;
    }
    else if(IS("ret")||IS("retn")){
        i64 k=n?o[0].imm:0;
        xb_printf(&out,"pc=D(esp); esp+=%u; goto dispatch; ",(u32)(4+k));
    }
    else if(IS("leave")) xb_printf(&out,"esp=ebp; ebp=D(esp); esp+=4; ");
    else if(IS("int")){
        if(n!=1 || o[0].k!=O_IMM || o[0].imm!=0x80) xfail("only int 0x80 is supported");
        xb_printf(&out,"X2cRegs r_={eax,ebx,ecx,edx,esi,edi,ebp}; eax=x2c_syscall(m,&r_); ");
    }
    else if(IS("sahf")) xb_printf(&out,"fk=FK_SAHF; fr=(eax>>8)&0xFFu; ");
    else if(IS("cld")) xb_printf(&out,"df=0; ");
    else if(IS("std")) xb_printf(&out,"df=1; ");
    else if(IS("nop")||IS("fwait")||IS("wait")||IS("fnclex")||IS("fclex")) ;
    /* strings */
    else if(IS("rep movsb")||IS("rep movsd")||IS("movsb")||IS("movsd")){
        int u=m[strlen(m)-1]=='b'?1:4, rep=m[0]=='r';
        if(rep){
            xb_printf(&out,"u32 n_=ecx*%du; if(n_){ if(!df && (edi<=esi || edi-esi>=n_)){ memmove(m+edi,m+esi,n_); esi+=n_; edi+=n_; ecx=0; } "
                           "else while(ecx){ %s(edi)=%s(esi); if(df){ esi-=%d; edi-=%d; } else { esi+=%d; edi+=%d; } ecx--; } } ",u,u==1?"B":"D",u==1?"B":"D",u,u,u,u);
        } else xb_printf(&out,"%s(edi)=%s(esi); if(df){ esi-=%d; edi-=%d; } else { esi+=%d; edi+=%d; } ",u==1?"B":"D",u==1?"B":"D",u,u,u,u);
    }
    else if(IS("rep stosb")||IS("rep stosd")||IS("stosb")||IS("stosd")){
        int u=m[strlen(m)-1]=='b'?1:4, rep=m[0]=='r';
        if(rep && u==1) xb_printf(&out,"if(ecx){ if(!df){ memset(m+edi,(u8)eax,ecx); edi+=ecx; ecx=0; } else while(ecx){ B(edi)=(u8)eax; edi--; ecx--; } } ");
        else if(rep) xb_printf(&out,"while(ecx){ D(edi)=eax; edi+=df?-4:4; ecx--; } ");
        else xb_printf(&out,"%s(edi)=%s; edi+=df?-%d:%d; ",u==1?"B":"D",u==1?"(u8)eax":"eax",u,u);
    }
    else if(IS("repe cmpsb")||IS("cmpsb")){
        if(m[0]=='r') xb_printf(&out,"while(ecx){ u32 x_=B(esi), y_=B(edi); esi+=df?-1:1; edi+=df?-1:1; ecx--; fk=FK_SUB|(24<<8); fa=x_<<24; fb=y_<<24; fr=(x_-y_)<<24; if(x_!=y_) break; } ");
        else xb_printf(&out,"u32 x_=B(esi), y_=B(edi); esi+=df?-1:1; edi+=df?-1:1; fk=FK_SUB|(24<<8); fa=x_<<24; fb=y_<<24; fr=(x_-y_)<<24; ");
    }
    else if(IS("repne scasb")||IS("repe scasb")||IS("scasb")){
        const char *stop=m[3]=='n'?"x_==y_":m[0]=='r'?"x_!=y_":NULL;
        const char *body="u32 x_=(u8)eax, y_=B(edi); edi+=df?-1:1; fk=FK_SUB|(24<<8); fa=x_<<24; fb=y_<<24; fr=(x_-y_)<<24; ";
        if(stop) xb_printf(&out,"while(ecx){ %s ecx--; if(%s) break; } ",body,stop);
        else xb_printf(&out,"%s",body);
    }
    else if(IS("lodsd")) xb_printf(&out,"eax=D(esi); esi+=df?-4:4; ");
    else if(IS("lodsb")) xb_printf(&out,"SET8L(eax,B(esi)); esi+=df?-1:1; ");
    /* x87 */
    else if(m[0]=='f'){
        if(IS("fninit")||IS("finit")) xb_printf(&out,"ft=0; fcw=0x37Fu; fsw=0; ");
        else if(IS("fldcw")) xb_printf(&out,"fcw=W(a_); ");
        else if(IS("fnstcw")||IS("fstcw")) xb_printf(&out,"W(a_)=(u16)fcw; ");
        else if(IS("fnstsw")||IS("fstsw")){
            if(n==1 && o[0].k==O_REG && o[0].size==2 && o[0].reg==0) xb_printf(&out,"SET16(eax,(fsw&~0x3800u)|((ft&7u)<<11)); ");
            else xb_printf(&out,"W(a_)=(u16)((fsw&~0x3800u)|((ft&7u)<<11)); ");
        }
        else if(IS("fld")){
            if(n!=1) xfail("fld operands");
            if(o[0].k==O_ST) xb_printf(&out,"double v_=ST(%d); FPUSH(v_); ",o[0].st);
            else xb_printf(&out,"FPUSH(%s); ",fmem(&o[0],0));
        }
        else if(IS("fild")) xb_printf(&out,"FPUSH(%s); ",fmem(&o[0],1));
        else if(IS("fst")||IS("fstp")){
            if(n!=1) xfail("%s operands",m);
            if(o[0].k==O_ST) xb_printf(&out,"ST(%d)=ST(0); ",o[0].st);
            else if(o[0].size==8) xb_printf(&out,"FD(a_)=ST(0); ");
            else if(o[0].size==4) xb_printf(&out,"FS(a_)=(float)ST(0); ");
            else xfail("%s size",m);
            if(m[3]=='p') xb_printf(&out,"FPOP(); ");
        }
        else if(IS("fist")||IS("fistp")){
            if(n!=1||o[0].k!=O_MEM) xfail("%s operands",m);
            if(o[0].size==4) xb_printf(&out,"D(a_)=x2c_fist32(ST(0),fcw); ");
            else if(o[0].size==8) xb_printf(&out,"Q(a_)=x2c_fist64(ST(0),fcw); ");
            else if(o[0].size==2) xb_printf(&out,"W(a_)=x2c_fist16(ST(0),fcw); ");
            else xfail("%s size",m);
            if(m[4]=='p') xb_printf(&out,"FPOP(); ");
        }
        else if(IS("fld1")) xb_printf(&out,"FPUSH(1.0); ");
        else if(IS("fldz")) xb_printf(&out,"FPUSH(0.0); ");
        else if(IS("fldpi")) xb_printf(&out,"FPUSH(3.141592653589793); ");
        else if(IS("fldl2e")) xb_printf(&out,"FPUSH(1.4426950408889634); ");
        else if(IS("fldl2t")) xb_printf(&out,"FPUSH(3.321928094887362); ");
        else if(IS("fldln2")) xb_printf(&out,"FPUSH(0.6931471805599453); ");
        else if(IS("fldlg2")) xb_printf(&out,"FPUSH(0.30102999566398120); ");
        else if(IS("fchs")) xb_printf(&out,"ST(0)=-ST(0); ");
        else if(IS("fabs")) xb_printf(&out,"ST(0)=fabs(ST(0)); ");
        else if(IS("fsqrt")) xb_printf(&out,"ST(0)=sqrt(ST(0)); ");
        else if(IS("fsin")) xb_printf(&out,"ST(0)=sin(ST(0)); fsw&=~0x400u; ");
        else if(IS("fcos")) xb_printf(&out,"ST(0)=cos(ST(0)); fsw&=~0x400u; ");
        else if(IS("fptan")) xb_printf(&out,"ST(0)=tan(ST(0)); fsw&=~0x400u; FPUSH(1.0); ");
        else if(IS("fpatan")) xb_printf(&out,"ST(1)=atan2(ST(1),ST(0)); FPOP(); ");
        else if(IS("fyl2x")) xb_printf(&out,"ST(1)=x2c_fyl2x(ST(1),ST(0)); FPOP(); ");
        else if(IS("f2xm1")) xb_printf(&out,"ST(0)=exp2(ST(0))-1.0; ");
        else if(IS("fscale")) xb_printf(&out,"{ double s_=trunc(ST(1)); ST(0)=ldexp(ST(0),s_>100000?100000:s_<-100000?-100000:(int)s_); } ");
        else if(IS("frndint")) xb_printf(&out,"ST(0)=x2c_round(ST(0),fcw); ");
        else if(IS("fprem")) xb_printf(&out,"ST(0)=fmod(ST(0),ST(1)); fsw&=~0x400u; ");
        else if(IS("fxch")){ int i=n?o[0].st:1; if(n && o[0].k!=O_ST) xfail("fxch operand"); xb_printf(&out,"double v_=ST(0); ST(0)=ST(%d); ST(%d)=v_; ",i,i); }
        else if(IS("ftst")) xb_printf(&out,"FCOM(ST(0),0.0); ");
        else if(IS("fcom")||IS("fcomp")||IS("fucom")||IS("fucomp")){
            const char *src;
            if(!n) src="ST(1)";
            else if(o[0].k==O_ST){ char *s=cstr(); snprintf(s,512,"ST(%d)",o[0].st); src=s; }
            else src=fmem(&o[0],0);
            xb_printf(&out,"FCOM(ST(0),%s); ",src);
            if(m[strlen(m)-1]=='p') xb_printf(&out,"FPOP(); ");
        }
        else if(IS("fcompp")||IS("fucompp")) xb_printf(&out,"FCOM(ST(0),ST(1)); FPOP(); FPOP(); ");
        else if(IS("fincstp")) xb_printf(&out,"ft=(ft+1)&7; ");
        else if(IS("fdecstp")) xb_printf(&out,"ft=(ft-1)&7; ");
        else if(IS("ffree")) ;
        else farith(m,o,n);
    }
    else xfail("unsupported instruction '%s'",m);
    #undef IS
    xb_printf(&out,"}\n");
}

/* ---------------------------------------------------------------- routines done natively */
static int sym_or_fail(const char *name){ int k=sym_find(name); if(k<0 || syms[k].kind==SY_NONE) xfail("the native %s needs '%s'",name,name); return k; }
static const char *natives[]={"rt_sb_gen_x","rt_sb_fixed_x","rt_fexp_x","rt_fpow_x","rt_float_parse_x","rt_flog_x","rt_flog10_x","rt_flog2_x",
    "rt_fatan_x","rt_fasin_x","rt_facos_x","rt_fsin_x","rt_fcos_x","rt_ftan_x","rt_fatan2_x",NULL};
static int in_native(const char *name){ for(int i=0;natives[i];i++) if(!strcmp(natives[i],name)) return 1; return 0; }
static void native(const char *name){
    const char *ret="pc=D(esp); esp+=4; goto dispatch;";
    if(!strcmp(name,"rt_sb_gen_x"))
        xb_printf(&out,"{ double v_=ST(0); FPOP(); edx=x2c_fmt_gen((char*)m+0x%Xu,v_,edx,ecx); eax=0x%Xu; goto L%d; }\n",scratch_addr,scratch_addr,sym_or_fail("rt_sb_bytes"));
    else if(!strcmp(name,"rt_sb_fixed_x"))
        xb_printf(&out,"{ double v_=ST(0); FPOP(); edx=x2c_fmt_fixed((char*)m+0x%Xu,v_,edx); eax=0x%Xu; goto L%d; }\n",scratch_addr,scratch_addr,sym_or_fail("rt_sb_bytes"));
    else if(!strcmp(name,"rt_fexp_x")) xb_printf(&out,"{ ST(0)=exp(ST(0)); %s }\n",ret);
    else if(!strcmp(name,"rt_fpow_x")) xb_printf(&out,"{ double a_=ST(0), b_=ST(1); FPOP(); ST(0)=pow(a_,b_); %s }\n",ret);
    else if(!strcmp(name,"rt_fatan2_x")) xb_printf(&out,"{ double x_=ST(0), y_=ST(1); FPOP(); ST(0)=atan2(y_,x_); %s }\n",ret);
    else if(!strncmp(name,"rt_f",4) && strcmp(name,"rt_float_parse_x")){   /* one-argument math: the C library's */
        char fn[16]; snprintf(fn,sizeof fn,"%.*s",(int)(strlen(name)-6),name+4);
        xb_printf(&out,"{ ST(0)=%s(ST(0)); %s }\n",fn,ret); }
    else if(!strcmp(name,"rt_float_parse_x")){
        int msg=sym_or_fail("rt_msg_float"), panic=sym_or_fail("rt_panic_value");
        xb_printf(&out,"{ double v_; if(!x2c_parse_float(m,eax,&v_)){ esi=0x%Xu; goto L%d; } FPUSH(v_); %s }\n",(u32)syms[msg].val,panic,ret);
    }
}

/* ---------------------------------------------------------------- entry */
static void reset(void){
    for(int i=0;i<nsyms;i++) free(syms[i].name);
    free(syms); syms=NULL; nsyms=capsyms=0; free(symhash); symhash=NULL; hashcap=0;
    free(ctab); ctab=NULL; nctab=capctab=0;
    free(items); items=NULL; nitems=capitems=0;
    free(lines); lines=NULL; nlines=caplines=0;
    free(imports); imports=NULL; nimports=0;
    free(defset); defset=NULL; ndefset=capdefset=0;
    free(image); image=NULL; image_len=0;
    nret=0; native_skip=0; pending_ccall=NULL; entry_name[0]=0; layout_final=0;
    memset(&out,0,sizeof out);
}

int aot_x2c(const char *listing, size_t len, char **res, size_t *reslen){
    reset();
    x_line=0;
    if(setjmp(x_fail)) return 1;
    /* lines, comments, markers */
    int cap=1024, nraw=0; char **raw=(char**)xmalloc(sizeof(char*)*(size_t)cap); int *rawline=(int*)xmalloc(sizeof(int)*(size_t)cap); char **rawmarker=(char**)xmalloc(sizeof(char*)*(size_t)cap);
    for(size_t i=0, ln=1;i<len;ln++){
        size_t j=i; while(j<len && listing[j]!='\n') j++;
        if(nraw==cap){ cap*=2; raw=(char**)xrealloc(raw,sizeof(char*)*(size_t)cap); rawline=(int*)xrealloc(rawline,sizeof(int)*(size_t)cap); rawmarker=(char**)xrealloc(rawmarker,sizeof(char*)*(size_t)cap); }
        raw[nraw]=strip_comment(listing+i,j-i,&rawmarker[nraw]); rawline[nraw]=(int)ln; nraw++;
        i=j+1;
    }
    preprocess(raw,rawline,rawmarker,nraw);
    parse_lines();
    for(int i=0;i<nimports;i++){ int k=sym_find(imports[i].slot); x_line=0; if(k<0||syms[k].kind!=SY_DATA) xfail("import slot %s is not defined",imports[i].slot); imports[i].slotsym=k; }
    /* layout until every address is known */
    for(int pass=0;layout(0);pass++) if(pass>8) break;
    layout(1);
    build_image();

    xb_put(&out,x2c_prelude,strlen(x2c_prelude));
    xb_printf(&out,"\n/* ---------------- the program (translated from the listing) */\n");
    xb_printf(&out,"#define X2C_DATA_END 0x%Xu\n",data_end);
    xb_printf(&out,"static const unsigned char x2c_image[%u] =\n\"",image_len+1);
    for(u32 i=0,col=0;i<image_len;i++){
        unsigned char c=image[i]; char e[8];
        if(c=='\\'||c=='"'){ e[0]='\\'; e[1]=(char)c; xb_put(&out,e,2); col+=2; }
        else if(c>=32 && c<127 && c!='?'){ e[0]=(char)c; xb_put(&out,e,1); col++; }
        else { snprintf(e,sizeof e,"\\%03o",c); xb_put(&out,e,4); col+=4; }
        if(col>=100 && i+1<image_len){ xb_put(&out,"\"\n\"",3); col=0; }
    }
    xb_printf(&out,"\";\n");
    xb_printf(&out,"static X2cImport x2c_imports[%d] = {",nimports+1);
    for(int i=0;i<nimports;i++) xb_printf(&out,"{\"%s\",\"%s\",0},",imports[i].lib,imports[i].sym);
    xb_printf(&out,"{0,0,0}};\n\n");

    int start=sym_find(entry_name[0]?entry_name:"start");
    if(start<0 || syms[start].kind!=SY_CODE) xfail("no entry point");
    xb_printf(&out,"static void x2c_run(u8 *const m, u32 esp){\n"
                   "    u32 eax=0,ecx=0,edx=0,ebx=0,ebp=0,esi=0,edi=0,pc=0;\n"
                   "    u32 fk=FK_LOGIC,fa=0,fb=0,fr=0,fc=0; int df=0;\n"
                   "    double fs[8]={0}; u32 ft=0,fcw=0x37Fu,fsw=0;\n"
                   "    goto L%d;\n",start);
    for(int i=0;i<nitems;i++){ Item *it=&items[i];
        if(it->sec) continue;
        x_line=it->line;
        if(it->kind==IT_LABEL){
            const char *name=syms[it->sym].name;
            xb_printf(&out,"L%d: ;  /* %s */\n",it->sym,name);
            if(in_native(name)){ native(name); native_skip=1; continue; }
            if(native_skip){
                /* a label inside a native routine is the next routine unless it is local to it */
                char *dot=NULL;
                for(int k=0;natives[k];k++){ size_t nl=strlen(natives[k]); if(!strncmp(name,natives[k],nl) && name[nl]=='.') dot=(char*)name; }
                if(dot || name[0]=='@') { xb_printf(&out,"x2c_bad_jump(pc);\n"); continue; }
                native_skip=0;
            }
            continue;
        }
        if(native_skip) continue;
        if(it->kind==IT_INSN) insn(it);
        else if(it->kind==IT_CONST || it->kind==IT_ALIGN) continue;
    }
    xb_printf(&out,"    x2c_fatal(\"fell off the end of the code\");\n");
    xb_printf(&out,"dispatch: {\n        static void *const ctab[%d]={",nctab+1);
    for(int i=0;i<nctab;i++){ if(i%8==0) xb_printf(&out,"\n            ");
        if(ctab[i]>=0) xb_printf(&out,"&&L%d,",ctab[i]); else xb_printf(&out,"&&R%d,",-1-ctab[i]); }
    xb_printf(&out,"0};\n        u32 i_=(pc-0x%Xu)>>2;\n        if(i_>=%du || (pc&3u)) x2c_bad_jump(pc);\n        goto *ctab[i_];\n    }\n}\n\n",CODE_BASE,nctab);
    xb_printf(&out,"int main(int argc, char **argv){\n"
                   "    x2c_argc=argc; x2c_argv=argv;\n"
                   "    signal(SIGPIPE,SIG_IGN);\n"
                   "    x2c_map();\n"
                   "    memcpy(M+X2C_DATA_BASE,x2c_image,sizeof x2c_image-1);\n"
                   "    x2c_brk_start=x2c_brk=(X2C_DATA_END+4095u)&~4095u;\n"
                   "    x2c_run(M,x2c_stack());\n"
                   "    return 0;\n}\n");
    for(int i=0;i<nraw;i++) free(raw[i]);
    free(raw); free(rawline); free(rawmarker);
    *res=out.s; *reslen=out.len; out.s=NULL;
    return 0;
}
