/* ========================= Typed compiler: peephole optimizer =========================
   aot/codegen builds a function out of small templates: a value in eax
   (edx:eax for an int), a binary operator's left operand pushed on the
   machine stack, arguments stored one by one. Where two templates meet the
   code is often longer than it needs to be. This pass rewrites the finished
   text of one function (fasm, as aot/codegen writes it) into shorter code
   that leaves the same values in every register, flag and memory word that
   is used afterwards.

   What is used afterwards comes from a backward liveness pass over the
   function's jumps (the eight registers and the flags; memory is reasoned
   about only within a few neighbouring instructions). It rests on these
   conventions of the generated code and the runtime:
     - a compiled function (called by its dotted label, or a method through
       its object's method table: call dword [eax+n]) takes its arguments on
       the stack - and edx, its closure, when aot/codegen says so - keeps
       ebx, esi, edi and ebp, and leaves eax, ecx, edx and the flags undefined;
     - a runtime routine reads the registers rt_inputs says, any other
       routine called or jumped to may read every register; none reads the
       flags on entry (none of aot/runtime's entry points does);
     - ret hands back eax and edx (an int) and the kept registers.
   A rewrite never makes a register live where it was not, so the liveness
   computed once per pass stays a safe over-estimate while the pass rewrites.
   Labels, comments and anything unrecognised end the stretch of code one
   rewrite may span; an unrecognised instruction reads everything, and a
   function with conditional assembly in it is left alone.

   MPY_NO_PEEP=1 turns the pass off; MPY_PEEP_STATS=1 counts its rewrites. */

#include "aot.h"
#include "util.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- registers */

enum { RA=1, RC=2, RD=4, RB=8, RSP=16, RBP=32, RSI=64, RDI=128, RFL=256 };
#define R_GP  (RA|RC|RD|RB|RSP|RBP|RSI|RDI)
#define R_ALL (R_GP|RFL)

static const struct { const char *name; unsigned bit; int width; } regs[]={
    {"eax",RA,32},{"ax",RA,16},{"al",RA,8},{"ah",RA,8},
    {"ecx",RC,32},{"cx",RC,16},{"cl",RC,8},{"ch",RC,8},
    {"edx",RD,32},{"dx",RD,16},{"dl",RD,8},{"dh",RD,8},
    {"ebx",RB,32},{"bx",RB,16},{"bl",RB,8},{"bh",RB,8},
    {"esp",RSP,32},{"sp",RSP,16},{"ebp",RBP,32},{"bp",RBP,16},
    {"esi",RSI,32},{"si",RSI,16},{"edi",RDI,32},{"di",RDI,16},
};
/* the register an operand is (0 if it is none); *width its size in bits */
static unsigned reg_of(const char *o, int *width){
    for(size_t i=0;i<MPY_ARRAY_SIZE(regs);i++) if(!strcmp(o,regs[i].name)){ if(width) *width=regs[i].width; return regs[i].bit; }
    return 0;
}
static unsigned reg32(const char *o){ int w; unsigned r=reg_of(o,&w); return w==32 ? r : 0; }
static int ident_ch(int c){ return isalnum(c)||c=='_'||c=='.'||c=='@'||c=='$'||c=='?'; }
/* every register an operand mentions (an address's base and index) */
static unsigned regs_in(const char *o){
    unsigned r=0; char tok[64];
    for(const char *s=o;*s;){
        if(!ident_ch((unsigned char)*s)){ s++; continue; }
        const char *b=s; while(*s && ident_ch((unsigned char)*s)) s++;
        size_t n=(size_t)(s-b); if(n<sizeof tok){ memcpy(tok,b,n); tok[n]=0; r|=reg_of(tok,NULL); }
    }
    return r;
}
static int is_mem(const char *o){ return strchr(o,'[')!=NULL; }
static const char *reg_name(unsigned bit){
    switch(bit){ case RA: return "eax"; case RC: return "ecx"; case RD: return "edx"; case RB: return "ebx";
                 case RSI: return "esi"; case RDI: return "edi"; case RBP: return "ebp"; default: return "esp"; }
}

/* ---------------------------------------------------------------- lines */

enum { K_INS, K_LABEL, K_NOTE, K_OTHER };     /* an instruction, a label, a comment, anything else */
enum { F_NEXT, F_COND, F_JMP, F_RET };         /* where control goes after an instruction */

typedef struct {
    char *line;                 /* as written (no newline) */
    int kind;
    char txt[200];              /* K_INS: the instruction without indentation and comment */
    char mn[16];                /* ... its mnemonic */
    int nop; char op[3][160];   /* ... its operands */
    char name[160];             /* K_LABEL */
    unsigned use, def;          /* registers (and flags) read / written */
    int memw;                   /* writes memory (or may: calls) */
    int pure;                   /* nothing but use -> def: may go when def is dead */
    int flow;                   /* F_* */
    int target;                 /* F_COND / F_JMP: index of the local label (-1: elsewhere) */
    unsigned exit_live;         /* what is live where a jump out of the function goes */
    unsigned in, out;           /* live before / after */
} Ins;

typedef struct { Ins *v; int n, cap; } P;

static long stat_count[32]; static const char *stat_name[32];
static void count(int k, const char *name){ stat_count[k]++; stat_name[k]=name; }
static long peep_budget=-2;     /* MPY_PEEP_MAX=n: only the first n rewrites (finding a wrong one by bisection) */
static int budget_left(void){
    if(peep_budget==-2){ const char *e=getenv("MPY_PEEP_MAX"); peep_budget=e&&*e?atol(e):-1; }
    if(peep_budget<0) return 1;
    if(peep_budget==0) return 0;
    peep_budget--; return 1;
}

static void split_ops(Ins *x, const char *s){
    x->nop=0;
    while(*s==' '||*s=='\t') s++;
    if(!*s) return;
    for(;;){
        const char *b=s; int depth=0; char q=0;
        while(*s){
            if(q){ if(*s==q) q=0; }
            else if(*s=='\''||*s=='"') q=*s;
            else if(*s=='[') depth++;
            else if(*s==']') depth--;
            else if(*s==',' && depth==0) break;
            s++;
        }
        const char *e=s; while(e>b && (e[-1]==' '||e[-1]=='\t')) e--;
        while(b<e && (*b==' '||*b=='\t')) b++;
        if(x->nop<3){ size_t n=(size_t)(e-b); if(n>=sizeof x->op[0]) n=sizeof x->op[0]-1; memcpy(x->op[x->nop],b,n); x->op[x->nop][n]=0; }
        x->nop++;
        if(!*s) break;
        s++;
    }
}

static void classify(Ins *x);

/* one line of text -> an entry */
static void parse_line(Ins *x, const char *s, size_t n){
    memset(x,0,sizeof *x);
    x->line=xstrndup2(s,(int)n); x->target=-1;
    const char *e=s+n;
    if(n==0){ x->kind=K_NOTE; return; }
    if(s[0]!=' ' && s[0]!='\t'){
        if(s[0]==';'){ x->kind=K_NOTE; return; }
        const char *p=s; while(p<e && ident_ch((unsigned char)*p)) p++;
        if(p>s && p<e && *p==':'){
            const char *r=p+1; while(r<e && (*r==' '||*r=='\t')) r++;
            if(r==e || *r==';'){ size_t k=(size_t)(p-s); if(k>=sizeof x->name) k=sizeof x->name-1;
                memcpy(x->name,s,k); x->name[k]=0; x->kind=K_LABEL; return; }
        }
        x->kind=K_OTHER; return;
    }
    const char *p=s; while(p<e && (*p==' '||*p=='\t')) p++;
    if(p==e){ x->kind=K_NOTE; return; }
    if(*p==';'){ x->kind=K_NOTE; return; }
    const char *t=p; char q=0;                          /* the instruction ends at a comment */
    while(t<e){ if(q){ if(*t==q) q=0; } else if(*t=='\''||*t=='"') q=*t; else if(*t==';') break; t++; }
    while(t>p && (t[-1]==' '||t[-1]=='\t')) t--;
    size_t k=(size_t)(t-p);
    if(k>=sizeof x->txt){ x->kind=K_OTHER; return; }
    memcpy(x->txt,p,k); x->txt[k]=0;
    const char *m=x->txt; size_t ml=0; while(m[ml] && !isspace((unsigned char)m[ml])) ml++;
    if(ml>=sizeof x->mn){ x->kind=K_OTHER; return; }
    memcpy(x->mn,m,ml); x->mn[ml]=0;
    static const char *const directives[]={"db","dw","dd","dq","du","rb","rw","rd","rq","align","times","if","else","end","repeat","virtual","label","file","macro","struc","format","use32","org","include",NULL};
    for(int i=0;directives[i];i++) if(!strcmp(x->mn,directives[i])){ x->kind=K_OTHER; return; }
    split_ops(x,m+ml);
    if(x->nop>3){ x->kind=K_OTHER; return; }
    x->kind=K_INS;
    classify(x);
}

static int starts(const char *s, const char *p){ return !strncmp(s,p,strlen(p)); }
static int is_jcc(const char *m){ return m[0]=='j' && strcmp(m,"jmp"); }

/* compiled functions that read edx on entry (aot_peephole_takes_edx) */
static char **env_names; static int nenv, cenv;
void aot_peephole_reset(void){ for(int i=0;i<nenv;i++) free(env_names[i]); nenv=0; }
void aot_peephole_takes_edx(const char *label){
    if(nenv==cenv){ cenv=cenv?cenv*2:16; env_names=(char**)xrealloc(env_names,sizeof(char*)*(size_t)cenv); }
    env_names[nenv++]=xstrdup2(label);
}
static int takes_edx(const char *label){ for(int i=0;i<nenv;i++) if(!strcmp(env_names[i],label)) return 1; return 0; }

/* The registers some runtime routines read (aot/runtime; the stack and ebp
   aside): the ones the generated code calls most. Any other routine counts
   as reading every register. */
static const struct { const char *name; unsigned use; } rt_inputs[]={
    {"rt_attrerr_text",RSI},{"rt_typeerr_text",RSI},{"rt_nameerr_text",RSI},{"rt_unbound_text",RSI},
    {"rt_panic_none",0},{"rt_int_overflow",0},{"rt_throw",RA},
    {"rt_incref",RA},{"rt_decref",RA},{"rt_alloc",RA},{"rt_obj_new",RA|RC|RD},
    {"rt_list_new",RA},{"rt_list_push",RA},{"rt_list_at",RA|RD},{"rt_tuple_new",RA},
    {"rt_i32sat",RA|RD},{"rt_imul",RA|RD},{"rt_idivmod",RA|RD},{"rt_idivc",RA|RC|RD},{"rt_str_eq",RA|RD},
    {"rt_release",RA},{"rt_list_add32",RA},{"rt_list_add64",RA|RD},{"rt_list_addf",0},
};
static unsigned routine_use(const char *name){
    for(size_t i=0;i<MPY_ARRAY_SIZE(rt_inputs);i++) if(!strcmp(rt_inputs[i].name,name)) return rt_inputs[i].use|RSP|RBP;
    if(name[0]=='N' && name[1]=='A' && isdigit((unsigned char)name[2])) return RSP|RBP;   /* NA<z> (aot/codegen/none.c): raises an AttributeError */
    return R_GP;
}

/* the operand that a call or jump names: a label, or 0 for a register / memory */
static int is_label_opnd(const char *o){ return *o && !is_mem(o) && !reg_of(o,NULL) && (isalpha((unsigned char)*o)||*o=='_'||*o=='.'||*o=='@'); }

static void classify(Ins *x){
    const char *m=x->mn; unsigned use=0, def=0; int memw=0, pure=0, flow=F_NEXT;
    const char *a=x->nop>0?x->op[0]:"", *b=x->nop>1?x->op[1]:"";
    int wa=0; unsigned ra=reg_of(a,&wa);
    /* the destination operand: a full register is written, a part of one read too, memory written */
    #define DST_WRITE() do{ if(ra){ def|=ra; if(wa!=32) use|=ra; } else { use|=regs_in(a); memw=1; } }while(0)
    #define DST_RW()    do{ if(ra){ def|=ra; use|=ra; } else { use|=regs_in(a); memw=1; } }while(0)
    if(!strcmp(m,"mov")||!strcmp(m,"movzx")||!strcmp(m,"movsx")){
        if(x->nop!=2) goto unknown;
        use|=regs_in(b); DST_WRITE(); pure=ra!=0;
    }
    else if(!strcmp(m,"lea")){ if(x->nop!=2||!ra) goto unknown; use|=regs_in(b); def|=ra; pure=1; }
    else if(!strcmp(m,"xchg")){
        if(x->nop!=2) goto unknown;
        unsigned rb=reg_of(b,NULL);
        if(ra){ use|=ra; def|=ra; } else { use|=regs_in(a); memw=1; }
        if(rb){ use|=rb; def|=rb; } else { use|=regs_in(b); memw=1; }
    }
    else if(!strcmp(m,"push")){ if(x->nop!=1) goto unknown; use|=regs_in(a)|RSP; def|=RSP; memw=1; }
    else if(!strcmp(m,"pop")){ if(x->nop!=1) goto unknown; use|=RSP; def|=RSP; if(ra){ if(wa!=32) goto unknown; def|=ra; } else { use|=regs_in(a); memw=1; } }
    else if(!strcmp(m,"add")||!strcmp(m,"sub")||!strcmp(m,"and")||!strcmp(m,"or")||!strcmp(m,"xor")||!strcmp(m,"adc")||!strcmp(m,"sbb")){
        if(x->nop!=2) goto unknown;
        if((!strcmp(m,"xor")||!strcmp(m,"sub")) && ra && !strcmp(a,b)){ def|=ra|RFL; if(wa!=32) use|=ra; pure=1; }   /* zeroing: reads nothing */
        else { use|=regs_in(b); DST_RW(); def|=RFL; if(m[1]=='d'&&m[2]=='c') use|=RFL; if(m[0]=='s'&&m[1]=='b') use|=RFL; pure=ra!=0; }
    }
    else if(!strcmp(m,"cmp")||!strcmp(m,"test")){ if(x->nop!=2) goto unknown; use|=regs_in(a)|regs_in(b); def|=RFL; pure=1; }
    else if(!strcmp(m,"inc")||!strcmp(m,"dec")){ if(x->nop!=1) goto unknown; DST_RW(); use|=RFL; def|=RFL; pure=ra!=0; }
    else if(!strcmp(m,"neg")){ if(x->nop!=1) goto unknown; DST_RW(); def|=RFL; pure=ra!=0; }
    else if(!strcmp(m,"not")){ if(x->nop!=1) goto unknown; DST_RW(); pure=ra!=0; }
    else if(!strcmp(m,"shl")||!strcmp(m,"shr")||!strcmp(m,"sar")||!strcmp(m,"sal")||!strcmp(m,"rol")||!strcmp(m,"ror")||!strcmp(m,"rcl")||!strcmp(m,"rcr")){
        if(x->nop!=2) goto unknown;
        DST_RW(); use|=regs_in(b)|RFL; def|=RFL;             /* (a count of 0 keeps the flags) */
    }
    else if(!strcmp(m,"shld")||!strcmp(m,"shrd")){ if(x->nop!=3) goto unknown; DST_RW(); use|=regs_in(b)|regs_in(x->op[2])|RFL; def|=RFL; }
    else if(!strcmp(m,"imul")){
        if(x->nop==1){ use|=RA|regs_in(a); def|=RA|RD|RFL; }
        else if(x->nop==2){ if(!ra) goto unknown; use|=ra|regs_in(b); def|=ra|RFL; }
        else { if(!ra) goto unknown; use|=regs_in(b); def|=ra|RFL; }
    }
    else if(!strcmp(m,"mul")){ if(x->nop!=1) goto unknown; use|=RA|regs_in(a); def|=RA|RD|RFL; }
    else if(!strcmp(m,"div")||!strcmp(m,"idiv")){ if(x->nop!=1) goto unknown; use|=RA|RD|regs_in(a); def|=RA|RD|RFL; }
    else if(!strcmp(m,"cdq")){ use|=RA; def|=RD; pure=1; }
    else if(starts(m,"set")){ if(x->nop!=1) goto unknown; use|=RFL; DST_WRITE(); if(ra) use|=ra; pure=ra!=0; }
    else if(starts(m,"cmov")){ if(x->nop!=2||!ra) goto unknown; use|=RFL|ra|regs_in(b); def|=ra; }
    else if(!strcmp(m,"jecxz")){ flow=F_COND; use|=RC; }
    else if(is_jcc(m)){ if(x->nop!=1) goto unknown; flow=F_COND; use|=RFL; }
    else if(!strcmp(m,"jmp")){ if(x->nop!=1) goto unknown; flow=F_JMP; if(!is_label_opnd(a)) use|=regs_in(a); }
    else if(!strcmp(m,"call")){
        if(x->nop!=1) goto unknown;
        memw=1;
        int compiled = is_label_opnd(a) && strchr(a,'.') && !starts(a,"rt_") && a[0]!='.' && !strstr(a,".destroy");
        int method = starts(a,"dword [eax+") && a[strlen(a)-1]==']';    /* (through other registers: a type descriptor's routines, eax an argument) */
        if(compiled){ use|=RSP|(takes_edx(a)?RD:0); def|=RA|RC|RD|RFL; }
        else if(method){ use|=RSP|RA; def|=RA|RC|RD|RFL; }
        else { use|=is_label_opnd(a)?routine_use(a):R_GP; def|=RFL; }
    }
    else if(!strcmp(m,"ret")){ if(x->nop) goto unknown; flow=F_RET; use|=RA|RD|RB|RSI|RDI|RBP|RSP; }
    else if(!strcmp(m,"rep")){
        if(x->nop!=1) goto unknown;
        if(starts(a,"stos")){ use|=RC|RDI|RA; def|=RC|RDI; memw=1; }
        else if(starts(a,"movs")){ use|=RC|RSI|RDI; def|=RC|RSI|RDI; memw=1; }
        else goto unknown;
    }
    else if(!strcmp(m,"int")){ if(x->nop!=1) goto unknown; use|=R_GP; def|=RA; memw=1; }   /* a system call: arguments in any register, eax back, the flags kept (iret) */
    else if(!strcmp(m,"sahf")){ use|=RA; def|=RFL; }
    else if(!strcmp(m,"cld")||!strcmp(m,"std")){ use|=RFL; def|=RFL; }
    else if(!strcmp(m,"nop")){ }
    else if(m[0]=='f'){                                    /* x87: its own stack; general registers only in addresses */
        for(int i=0;i<x->nop && i<3;i++) use|=regs_in(x->op[i]);
        if(!strcmp(m,"fnstsw")||!strcmp(m,"fstsw")){ if(x->nop==1 && !strcmp(a,"ax")){ use|=RA; def|=RA; } else memw=1; }
        else if(starts(m,"fcomi")||starts(m,"fucomi")) def|=RFL;
        if(x->nop>=1 && is_mem(a) && (starts(m,"fst")||starts(m,"fist")||starts(m,"fnst")||starts(m,"fbstp")||starts(m,"fsave")||starts(m,"fnsave"))) memw=1;
    }
    else goto unknown;
    #undef DST_WRITE
    #undef DST_RW
    x->use=use; x->def=def; x->memw=memw; x->pure=pure && !memw && !(def&(RSP|RBP)); x->flow=flow;
    return;
unknown:
    x->use=R_ALL; x->def=0; x->memw=1; x->pure=0; x->flow=F_NEXT;
    x->kind=K_OTHER;
}

/* ---------------------------------------------------------------- the function */

static void p_free(P *p){ for(int i=0;i<p->n;i++) free(p->v[i].line); free(p->v); }

static void p_parse(P *p, const char *text, size_t len){
    const char *s=text, *e=text+len;
    while(s<e){
        const char *nl=memchr(s,'\n',(size_t)(e-s)); size_t n=nl?(size_t)(nl-s):(size_t)(e-s);
        if(p->n==p->cap){ p->cap=p->cap?p->cap*2:256; p->v=(Ins*)xrealloc(p->v,sizeof(Ins)*(size_t)p->cap); }
        parse_line(&p->v[p->n++],s,n);
        s+=n; if(nl) s++;
    }
}

/* labels by name: open addressing, rebuilt by each analysis */
static int *lab_tab; static int lab_cap;
static unsigned hname(const char *s){ unsigned h=2166136261u; while(*s){ h^=(unsigned char)*s++; h*=16777619u; } return h; }
static void labels_build(P *p){
    int need=16; while(need<p->n*2+16) need*=2;
    if(need>lab_cap){ free(lab_tab); lab_tab=(int*)xmalloc(sizeof(int)*(size_t)need); lab_cap=need; }
    for(int i=0;i<lab_cap;i++) lab_tab[i]=-1;
    for(int i=0;i<p->n;i++) if(p->v[i].kind==K_LABEL){
        unsigned h=hname(p->v[i].name)&(unsigned)(lab_cap-1);
        while(lab_tab[h]>=0) h=(h+1)&(unsigned)(lab_cap-1);
        lab_tab[h]=i;
    }
}
static int label_index(P *p, const char *name){
    for(unsigned h=hname(name)&(unsigned)(lab_cap-1);;h=(h+1)&(unsigned)(lab_cap-1)){
        int k=lab_tab[h]; if(k<0) return -1;
        if(!strcmp(p->v[k].name,name)) return k;
    }
}

/* what is live where a jump out of the function lands */
static unsigned exit_live(const char *target){ return routine_use(target); }

/* liveness of the registers and the flags before and after every entry */
static void analyze(P *p){
    labels_build(p);
    for(int i=0;i<p->n;i++){ Ins *x=&p->v[i];
        x->in=0; x->out=0;
        if(x->kind==K_INS && (x->flow==F_COND||x->flow==F_JMP)){
            x->target=is_label_opnd(x->op[0]) ? label_index(p,x->op[0]) : -1;
            x->exit_live=is_label_opnd(x->op[0]) ? exit_live(x->op[0]) : R_GP;
        }
    }
    for(int changed=1;changed;){
        changed=0;
        for(int i=p->n-1;i>=0;i--){ Ins *x=&p->v[i];
            unsigned next = i+1<p->n ? p->v[i+1].in : R_ALL, out;
            if(x->kind!=K_INS){ out=next; if(x->in!=out || x->out!=out){ x->in=x->out=out; changed=1; } continue; }
            switch(x->flow){
                case F_COND: out=next|(x->target>=0?p->v[x->target].in:x->exit_live); break;
                case F_JMP:  out=x->target>=0?p->v[x->target].in:x->exit_live; break;
                case F_RET:  out=0; break;
                default:     out=next;
            }
            unsigned in=x->use|(out&~x->def);
            if(in!=x->in || out!=x->out){ x->in=in; x->out=out; changed=1; }
        }
    }
}

/* replace entries [i, i+nold) by new instruction lines */
static long trace_n=0; static int trace=-1;    /* MPY_PEEP_TRACE=1: every rewrite on stderr */
static void replace(P *p, int i, int nold, const char *const *lines, int nnew){
    if(trace<0){ const char *e=getenv("MPY_PEEP_TRACE"); trace=e&&*e&&strcmp(e,"0"); }
    if(trace){
        fprintf(stderr,"--- rewrite %ld\n",++trace_n);
        for(int k=0;k<nold;k++) fprintf(stderr,"-  %s\n",p->v[i+k].txt[0]?p->v[i+k].txt:p->v[i+k].line);
        for(int k=0;k<nnew;k++) fprintf(stderr,"+  %s\n",lines[k]);
    }
    for(int k=0;k<nold;k++) free(p->v[i+k].line);
    int grow=nnew-nold;
    if(p->n+grow>p->cap){ while(p->n+grow>p->cap) p->cap=p->cap?p->cap*2:256; p->v=(Ins*)xrealloc(p->v,sizeof(Ins)*(size_t)p->cap); }
    memmove(&p->v[i+nnew],&p->v[i+nold],sizeof(Ins)*(size_t)(p->n-i-nold));
    p->n+=grow;
    for(int k=0;k<nnew;k++){
        char buf[256]; snprintf(buf,sizeof buf,"        %s",lines[k]);
        parse_line(&p->v[i+k],buf,strlen(buf));
        p->v[i+k].in=p->v[i+k].out=R_ALL;                 /* not analysed yet: everything counts as live */
    }
}
static void delete(P *p, int i, int n){ replace(p,i,n,NULL,0); }

/* ---------------------------------------------------------------- matching */

static Ins *at(P *p, int i){ return i>=0 && i<p->n && p->v[i].kind==K_INS ? &p->v[i] : NULL; }
static int is(P *p, int i, const char *txt){ Ins *x=at(p,i); return x && !strcmp(x->txt,txt); }
static int mn_is(Ins *x, const char *m){ return x && !strcmp(x->mn,m); }

/* a memory operand without "dword " and a "+0" displacement: [esp+0] is [esp] */
static void mem_norm(const char *o, char *out, size_t n){
    if(starts(o,"dword ")) o+=6;
    snprintf(out,n,"%s",o);
    size_t l=strlen(out);
    if(l>=4 && !strcmp(out+l-3,"+0]")){ out[l-3]=']'; out[l-2]=0; }
}
static int same_mem(const char *a, const char *b){ char x[200], y[200]; mem_norm(a,x,sizeof x); mem_norm(b,y,sizeof y); return !strcmp(x,y); }
/* a plain dword memory operand (no byte/word/qword size) */
static int dword_mem(const char *o){ return is_mem(o) && (o[0]=='[' || starts(o,"dword [")); }
/* [x] + k (an operand of mem_norm's form) */
static int mem_add(const char *o, int k, char *out, size_t n){
    char m[200]; mem_norm(o,m,sizeof m);
    size_t l=strlen(m); if(l<3 || m[0]!='[' || m[l-1]!=']') return 0;
    m[l-1]=0;
    char *p=m+l-2; while(p>m && isdigit((unsigned char)*p)) p--;
    long d=0; char base[200];
    if((*p=='+'||*p=='-') && isdigit((unsigned char)p[1])){ d=strtol(p,NULL,10); *p=0; }
    snprintf(base,sizeof base,"%s",m+1);
    d+=k;
    if(d==0) snprintf(out,n,"[%s]",base);
    else snprintf(out,n,"[%s%+ld]",base,d);
    return 1;
}
/* an integer constant operand */
static int imm_of(const char *o, int64_t *v){
    if(!*o) return 0;
    char *end; long long x;
    if(o[0]=='0' && (o[1]=='x'||o[1]=='X')) x=strtoll(o,&end,16);
    else if(isdigit((unsigned char)o[0]) || (o[0]=='-' && isdigit((unsigned char)o[1]))) x=strtoll(o,&end,10);
    else return 0;
    if(*end) return 0;
    *v=x; return 1;
}
static void imm32(char *out, size_t n, uint32_t v){ snprintf(out,n,"%d",(int)(int32_t)v); }

/* ---------------------------------------------------------------- rewrites */

/* code after jmp / ret up to the next label: never runs */
static int rule_unreachable(P *p, int i){
    Ins *x=at(p,i); if(!x || (x->flow!=F_JMP && x->flow!=F_RET)) return 0;
    int k=i+1; while(at(p,k)) k++;
    if(k==i+1) return 0;
    count(0,"unreachable code"); delete(p,i+1,k-i-1); return 1;
}
/* a jump to the label right after it */
static int rule_jump_next(P *p, int i){
    Ins *x=at(p,i); if(!x || (x->flow!=F_JMP && x->flow!=F_COND) || !is_label_opnd(x->op[0])) return 0;
    for(int k=i+1;k<p->n && p->v[k].kind!=K_INS && p->v[k].kind!=K_OTHER;k++)
        if(p->v[k].kind==K_LABEL && !strcmp(p->v[k].name,x->op[0])){ count(1,"jump to the next line"); delete(p,i,1); return 1; }
    return 0;
}
static const char *const ccpairs[][2]={{"e","ne"},{"z","nz"},{"l","ge"},{"le","g"},{"b","ae"},{"be","a"},{"s","ns"},{"o","no"},{"p","np"},{"c","nc"},{"nge","nl"},{"nle","ng"},{"nae","nb"},{"nbe","na"},{"pe","po"}};
static const char *cc_neg(const char *cc){
    for(size_t i=0;i<MPY_ARRAY_SIZE(ccpairs);i++){ if(!strcmp(cc,ccpairs[i][0])) return ccpairs[i][1]; if(!strcmp(cc,ccpairs[i][1])) return ccpairs[i][0]; }
    return NULL;
}
/* jcc L1 / jmp L2 / L1:  ->  j!cc L2 / L1: */
static int rule_jcc_over_jmp(P *p, int i){
    Ins *x=at(p,i), *y=at(p,i+1);
    if(!x || !y || x->flow!=F_COND || !strcmp(x->mn,"jecxz") || y->flow!=F_JMP || !is_label_opnd(y->op[0]) || !is_label_opnd(x->op[0])) return 0;
    const char *nc=cc_neg(x->mn+1); if(!nc) return 0;
    int k=i+2, found=0;
    for(;k<p->n && p->v[k].kind==K_LABEL;k++) if(!strcmp(p->v[k].name,x->op[0])) found=1;
    if(!found) return 0;
    char l[200]; snprintf(l,sizeof l,"j%s %s",nc,y->op[0]);
    const char *nl[]={l}; count(2,"jcc over jmp"); replace(p,i,2,nl,1); return 1;
}
/* a jump to a jmp: straight to where that one goes */
static int rule_thread(P *p, int i){
    Ins *x=at(p,i); if(!x || (x->flow!=F_JMP && x->flow!=F_COND) || !is_label_opnd(x->op[0])) return 0;
    char dest[160]; snprintf(dest,sizeof dest,"%s",x->op[0]);
    int moved=0;
    for(int hop=0;hop<8;hop++){
        int t=label_index(p,dest); if(t<0) break;
        int k=t; while(k<p->n && p->v[k].kind==K_LABEL) k++;
        Ins *y=at(p,k); if(!y || y->flow!=F_JMP || !is_label_opnd(y->op[0]) || !strcmp(y->op[0],dest) || !strcmp(y->op[0],x->op[0])) break;
        snprintf(dest,sizeof dest,"%s",y->op[0]); moved=1;
    }
    if(!moved) return 0;
    char l[200]; snprintf(l,sizeof l,"%s %s",x->mn,dest);
    const char *nl[]={l}; count(3,"jump to a jump"); replace(p,i,1,nl,1); return 1;
}
/* mov [m],r ... mov r2,[m]: the value is still in r */
static int rule_reload(P *p, int i){
    Ins *x=at(p,i); if(!mn_is(x,"mov") || x->nop!=2) return 0;
    unsigned r=reg32(x->op[0]); if(!r || !dword_mem(x->op[1])) return 0;
    unsigned addr=regs_in(x->op[1]);
    for(int j=i-1;j>=0 && j>=i-8;j--){
        Ins *y=at(p,j); if(!y) return 0;
        if(mn_is(y,"mov") && y->nop==2 && dword_mem(y->op[0]) && same_mem(y->op[0],x->op[1])){
            unsigned s=reg32(y->op[1]); if(!s) return 0;
            for(int k=j+1;k<i;k++) if(p->v[k].memw || (p->v[k].def&(s|addr))) return 0;
            if(s==r){ count(4,"reload of a stored value"); delete(p,i,1); return 1; }
            char l[64]; snprintf(l,sizeof l,"mov %s,%s",reg_name(r),reg_name(s));
            const char *nl[]={l}; count(4,"reload of a stored value"); replace(p,i,1,nl,1); return 1;
        }
        if(y->memw || (y->def&addr)) return 0;
    }
    return 0;
}
/* an esp-based operand as seen 4 bytes higher on the stack (one push less); 0 if it is the pushed word itself */
static int esp_less4(const char *o, char *out, size_t n){
    if(!(regs_in(o)&RSP)) { snprintf(out,n,"%s",o); return 1; }
    char m[200]; mem_norm(o,m,sizeof m);
    if(!starts(m,"[esp")) return 0;
    if(!strcmp(m,"[esp]")) return 0;
    if(m[4]!='+' ) return 0;
    char *end; long d=strtol(m+5,&end,10); if(strcmp(end,"]") || d<4) return 0;
    return mem_add(o,-4,out,n);
}
/* push / pop pairs */
static int rule_pushpop(P *p, int i){
    Ins *x=at(p,i), *y=at(p,i+1); if(!x || !y || x->nop!=1) return 0;
    if(mn_is(x,"push") && mn_is(y,"pop") && y->nop==1){
        unsigned a=reg32(x->op[0]), b=reg32(y->op[0]);
        if(!a || !b || a==RSP || b==RSP) return 0;
        if(a==b){ count(5,"push r / pop r"); delete(p,i,2); return 1; }
        char l[64]; snprintf(l,sizeof l,"mov %s,%s",y->op[0],x->op[0]);
        const char *nl[]={l}; count(5,"push r / pop r"); replace(p,i,2,nl,1); return 1;
    }
    if(mn_is(x,"pop") && mn_is(y,"push") && y->nop==1){       /* pop r / push r: r = the top word */
        unsigned a=reg32(x->op[0]); if(!a || a==RSP || strcmp(x->op[0],y->op[0])) return 0;
        char l[64]; snprintf(l,sizeof l,"mov %s,[esp]",x->op[0]);
        const char *nl[]={l}; count(6,"pop r / push r"); replace(p,i,2,nl,1); return 1;
    }
    /* push r1 / mov r1,x / mov r2,r1 / pop r1  ->  mov r2,x */
    Ins *z=at(p,i+2), *w=at(p,i+3);
    if(mn_is(x,"push") && z && w && mn_is(w,"pop") && mn_is(z,"mov") && w->nop==1 && z->nop==2){
        unsigned r1=reg32(x->op[0]), r2=reg32(z->op[0]);
        if(!r1 || !r2 || r1==r2 || r1==RSP || r2==RSP || r1==RBP || r2==RBP) return 0;
        if(strcmp(w->op[0],x->op[0]) || strcmp(z->op[1],x->op[0])) return 0;
        if(y->nop!=2 || reg32(y->op[0])!=r1) return 0;
        char l[220], src[200];
        if(mn_is(y,"xor") && !strcmp(y->op[1],y->op[0])) snprintf(l,sizeof l,"xor %s,%s",reg_name(r2),reg_name(r2));
        else if(mn_is(y,"mov")||mn_is(y,"movzx")||mn_is(y,"movsx")||mn_is(y,"lea")){
            if(!esp_less4(y->op[1],src,sizeof src)){
                if(!mn_is(y,"mov")) return 0;
                char m[200]; mem_norm(y->op[1],m,sizeof m); if(strcmp(m,"[esp]")) return 0;
                snprintf(src,sizeof src,"%s",reg_name(r1));      /* the pushed word: r1 itself */
            }
            snprintf(l,sizeof l,"%s %s,%s",y->mn,reg_name(r2),src);
        }
        else return 0;
        const char *nl[]={l}; count(7,"push / mov / mov / pop"); replace(p,i,4,nl,1); return 1;
    }
    return 0;
}
/* an instruction whose results nobody reads */
static int rule_dead(P *p, int i){
    Ins *x=at(p,i); if(!x || !x->pure || !x->def) return 0;
    if(x->def & x->out) return 0;
    count(8,"result never read"); delete(p,i,1); return 1;
}
/* add esp,a / sub esp,b ... : one adjustment */
static int rule_esp(P *p, int i){
    Ins *x=at(p,i), *y=at(p,i+1);
    if(!x || !y || x->nop!=2 || y->nop!=2 || strcmp(x->op[0],"esp") || strcmp(y->op[0],"esp")) return 0;
    if(!(mn_is(x,"add")||mn_is(x,"sub")) || !(mn_is(y,"add")||mn_is(y,"sub"))) return 0;
    int64_t a, b; if(!imm_of(x->op[1],&a) || !imm_of(y->op[1],&b)) return 0;
    if(y->out & RFL) return 0;
    int64_t d=(mn_is(x,"add")?a:-a)+(mn_is(y,"add")?b:-b);
    count(9,"esp adjustments merged");
    if(d==0){ delete(p,i,2); return 1; }
    char l[64]; if(d>0) snprintf(l,sizeof l,"add esp,%lld",(long long)d); else snprintf(l,sizeof l,"sub esp,%lld",(long long)-d);
    const char *nl[]={l}; replace(p,i,2,nl,1); return 1;
}
/* a word pushed instead of stored into a slot just made: sub esp,4 / mov eax,x / mov [esp],eax */
static int push_src(Ins *y, char *out, size_t n){   /* what `mov r,x` / `xor r,r` loads, as a push operand (not esp-based) */
    if(mn_is(y,"xor") && !strcmp(y->op[0],y->op[1])){ snprintf(out,n,"0"); return 1; }
    if(mn_is(y,"or") && !strcmp(y->op[1],"-1")){ snprintf(out,n,"-1"); return 1; }
    if(!mn_is(y,"mov") || y->nop!=2) return 0;
    const char *s=y->op[1];
    if(regs_in(s)&RSP) return 0;
    if(is_mem(s)){ if(!dword_mem(s)) return 0; char m[200]; mem_norm(s,m,sizeof m); snprintf(out,n,"dword %s",m); return 1; }
    if(reg_of(s,NULL)){ if(!reg32(s)) return 0; snprintf(out,n,"%s",s); return 1; }
    int64_t v; if(imm_of(s,&v)){ imm32(out,n,(uint32_t)v); return 1; }
    snprintf(out,n,"%s",s); return 1;                     /* a label's address */
}
static int rule_argpush(P *p, int i){
    Ins *x=at(p,i); if(!x || !is(p,i,"sub esp,4") ) {
        if(!x || !is(p,i,"sub esp,8")) return 0;
        /* sub esp,8 / mov eax,x / mov edx,y / mov [esp],eax / mov [esp+4],edx  ->  push y / push x */
        Ins *a=at(p,i+1), *b=at(p,i+2), *c=at(p,i+3), *d=at(p,i+4);
        if(!a||!b||!c||!d) return 0;
        if(reg32(a->op[0])!=RA || reg32(b->op[0])!=RD) return 0;
        if(!(is(p,i+3,"mov [esp],eax")||is(p,i+3,"mov [esp+0],eax")) || !is(p,i+4,"mov [esp+4],edx")) return 0;
        if(d->out & (RA|RD|RFL)) return 0;
        char sx[200], sy[200];
        if(!push_src(a,sx,sizeof sx) || !push_src(b,sy,sizeof sy)) return 0;
        if(regs_in(b->op[1])&RA) return 0;                 /* y read eax as x set it */
        if(!strcmp(sx,"eax")||!strcmp(sy,"edx")||!strcmp(sy,"eax")) return 0;
        char l1[220], l2[220]; snprintf(l1,sizeof l1,"push %s",sy); snprintf(l2,sizeof l2,"push %s",sx);
        const char *nl[]={l1,l2}; count(10,"arguments pushed"); replace(p,i,5,nl,2); return 1;
    }
    Ins *a=at(p,i+1); if(!a || a->nop!=2 || reg32(a->op[0])!=RA) return 0;
    if(!(is(p,i+2,"mov [esp],eax")||is(p,i+2,"mov [esp+0],eax"))) return 0;
    if(at(p,i+2)->out & (RA|RFL)) return 0;
    char s[200]; if(!push_src(a,s,sizeof s) || !strcmp(s,"eax")) return 0;
    char l[220]; snprintf(l,sizeof l,"push %s",s);
    const char *nl[]={l}; count(10,"arguments pushed"); replace(p,i,3,nl,1); return 1;
}

typedef struct { int mem; int64_t v; char lo[200], hi[200]; } B64;
static int load64(P *p, int i, B64 *b);
/* the push at j is one of an int's push edx / push eax that rule_int_operand may fold */
static int int_fold_ahead(P *p, int j){
    B64 b;
    if(is(p,j,"push edx") && is(p,j+1,"push eax")) return load64(p,j+2,&b);
    if(is(p,j,"push eax") && is(p,j-1,"push edx")) return load64(p,j+1,&b);
    return 0;
}
/* mov r,x ... push r  ->  push x  (r read by nothing else, dead after the push) */
static int rule_push_fold(P *p, int i){
    Ins *x=at(p,i); if(!x || x->nop!=2) return 0;
    unsigned r=reg32(x->op[0]); if(!r || r==RSP || r==RBP) return 0;
    char src[200];
    if(mn_is(x,"xor") && !strcmp(x->op[1],x->op[0])) snprintf(src,sizeof src,"0");
    else if(mn_is(x,"or") && !strcmp(x->op[1],"-1")) snprintf(src,sizeof src,"-1");
    else if(mn_is(x,"mov")){
        const char *o=x->op[1]; int64_t v;
        if(regs_in(o)&(RSP|r)) return 0;
        if(is_mem(o)){ if(!dword_mem(o)) return 0; char m[200]; mem_norm(o,m,sizeof m); snprintf(src,sizeof src,"dword %s",m); }
        else if(reg_of(o,NULL)) return 0;
        else if(imm_of(o,&v)) imm32(src,sizeof src,(uint32_t)v);
        else snprintf(src,sizeof src,"%s",o);
    }
    else return 0;
    unsigned addr=regs_in(x->op[1]);
    for(int j=i+1;j<p->n && j<=i+6;j++){
        Ins *y=at(p,j); if(!y) return 0;
        if(mn_is(y,"push") && y->nop==1 && reg32(y->op[0])==r){
            if(y->out & r) return 0;
            if(int_fold_ahead(p,j)) return 0;            /* (rule_int_operand does better there) */
            char l[220]; snprintf(l,sizeof l,"push %s",src);
            const char *nl[]={l}; count(12,"mov r,x / push r"); replace(p,j,1,nl,1); delete(p,i,1); return 1;
        }
        if((y->use|y->def)&r) return 0;
        if(y->def&addr) return 0;
        if(is_mem(x->op[1]) && y->memw && !mn_is(y,"push")) return 0;   /* (a push writes only the stack below) */
        if(y->flow!=F_NEXT) return 0;
    }
    return 0;
}

/* sub esp,n / <each argument: computed, stored into its slot> ... : the
   arguments pushed, last first - when computing them reads nothing another
   one wrote (only eax, ecx, edx, no stores, no calls) and those registers
   and the flags are dead after the last store */
static int store_slot(Ins *y, int n, int *off, unsigned *reg, char *imm, size_t immn){
    if(!mn_is(y,"mov") || y->nop!=2) return 0;
    char m[200]; mem_norm(y->op[0],m,sizeof m);
    if(!dword_mem(y->op[0]) || !starts(m,"[esp")) return 0;
    long k=0;
    if(strcmp(m,"[esp]")){ if(m[4]!='+') return 0; char *end; k=strtol(m+5,&end,10); if(strcmp(end,"]")) return 0; }
    if(k<0 || k>=n || k%4) return 0;
    *off=(int)k; *reg=reg32(y->op[1]); imm[0]=0;
    if(*reg){ if(!(*reg&(RA|RC|RD))) return 0; return 1; }
    int64_t v; if(!imm_of(y->op[1],&v)) return 0;
    imm32(imm,immn,(uint32_t)v); return 1;
}
static int rule_args_push(P *p, int i){
    Ins *x=at(p,i); if(!mn_is(x,"sub") || x->nop!=2 || strcmp(x->op[0],"esp")) return 0;
    int64_t n; if(!imm_of(x->op[1],&n) || n<4 || n>64 || n%4) return 0;
    int nslot=(int)n/4, filled=0;
    struct { int cs, ce, ss, se; } g[16]; int ng=0;
    int slot_of[16]; for(int k=0;k<16;k++) slot_of[k]=-1;
    int j=i+1;
    while(filled<nslot){
        if(ng==16) return 0;
        int cs=j; unsigned defd=0;
        for(;;){                                            /* computing it */
            Ins *y=at(p,j); if(!y) return 0;
            int off; unsigned reg; char imm[32];
            if(store_slot(y,(int)n,&off,&reg,imm,sizeof imm)) break;
            if(!y->pure || y->flow!=F_NEXT || (y->def&~(RA|RC|RD|RFL))) return 0;
            for(int k=0;k<y->nop;k++) if(regs_in(y->op[k])&RSP) return 0;
            if(y->use&(RA|RC|RD)&~defd) return 0;           /* (would read another argument's register) */
            defd|=y->def; j++;
            if(j-cs>12) return 0;
        }
        int ce=j, ss=j;
        for(;;){                                            /* ... stored */
            Ins *y=at(p,j); int off; unsigned reg; char imm[32];
            if(!y || !store_slot(y,(int)n,&off,&reg,imm,sizeof imm)) break;
            if(reg && !(reg&defd)) return 0;
            if(slot_of[off/4]>=0) return 0;
            slot_of[off/4]=ng; filled++; j++;
        }
        g[ng].cs=cs; g[ng].ce=ce; g[ng].ss=ss; g[ng].se=j; ng++;
    }
    Ins *last=&p->v[j-1];
    if(last->out & (RA|RC|RD|RFL)) return 0;
    for(int s=0,prev=-1;s<nslot;s++){ if(slot_of[s]<prev) return 0; prev=slot_of[s]; }   /* each argument's slots after the one before's */
    const char *nl[64]; char L[64][220]; int nn=0;
    for(int k=ng-1;k>=0;k--){
        for(int q=g[k].cs;q<g[k].ce;q++){ if(nn==64) return 0; snprintf(L[nn],sizeof L[nn],"%s",p->v[q].txt); nl[nn]=L[nn]; nn++; }
        for(int s=nslot-1;s>=0;s--){ if(slot_of[s]!=k) continue;
            for(int q=g[k].ss;q<g[k].se;q++){ int off; unsigned reg; char imm[32];
                store_slot(&p->v[q],(int)n,&off,&reg,imm,sizeof imm);
                if(off!=s*4) continue;
                if(nn==64) return 0;
                if(reg) snprintf(L[nn],sizeof L[nn],"push %s",reg_name(reg)); else snprintf(L[nn],sizeof L[nn],"push %s",imm);
                nl[nn]=L[nn]; nn++; } }
    }
    count(13,"arguments pushed, last first"); replace(p,i,j-i,nl,nn); return 1;
}

/* ---- ints: push edx / push eax / <load b> / <op with the left operand on the stack> ---- */
/* edx:eax = an int constant, or a variable in memory not addressed through eax, ecx, edx or esp */
static int load64(P *p, int i, B64 *b){
    Ins *x=at(p,i), *y=at(p,i+1); if(!x || !y || x->nop!=2 || y->nop!=2) return 0;
    memset(b,0,sizeof *b);
    int64_t lo, hi;
    if(reg32(x->op[0])==RA && reg32(y->op[0])==RD){
        int have_lo = mn_is(x,"xor")&&!strcmp(x->op[1],"eax") ? (lo=0,1) : mn_is(x,"mov") ? imm_of(x->op[1],&lo) : 0;
        int have_hi = mn_is(y,"xor")&&!strcmp(y->op[1],"edx") ? (hi=0,1) : mn_is(y,"or")&&!strcmp(y->op[1],"-1") ? (hi=-1,1) : mn_is(y,"mov") ? imm_of(y->op[1],&hi) : 0;
        if(have_lo && have_hi){
            b->v=(int64_t)(((uint64_t)(uint32_t)hi<<32)|(uint32_t)lo);
            imm32(b->lo,sizeof b->lo,(uint32_t)lo); imm32(b->hi,sizeof b->hi,(uint32_t)hi); return 1; }
    }
    if(!mn_is(x,"mov") || !mn_is(y,"mov")) return 0;
    const char *ml, *mh;
    if(reg32(x->op[0])==RA && reg32(y->op[0])==RD){ ml=x->op[1]; mh=y->op[1]; }
    else if(reg32(x->op[0])==RD && reg32(y->op[0])==RA){ ml=y->op[1]; mh=x->op[1]; }
    else return 0;
    if(!dword_mem(ml) || !dword_mem(mh) || (regs_in(ml)&(RA|RC|RD|RSP))) return 0;
    char want[200]; if(!mem_add(ml,4,want,sizeof want) || !same_mem(want,mh)) return 0;
    mem_norm(ml,b->lo,sizeof b->lo); mem_norm(mh,b->hi,sizeof b->hi); b->mem=1;
    return 1;
}
/* does [i, i+n) read exactly these lines? (a NULL entry: any setCC al / jCC *, see cc) */
static int match_seq(P *p, int i, const char *const *t, int n, char *cc, size_t ccn, char *jl, size_t jln){
    for(int k=0;k<n;k++){
        Ins *x=at(p,i+k); if(!x) return 0;
        if(t[k]) { if(strcmp(x->txt,t[k])) return 0; continue; }
        if(starts(x->mn,"set") && x->nop==1 && !strcmp(x->op[0],"al")){ snprintf(cc,ccn,"%s",x->mn+3); continue; }
        if(is_jcc(x->mn) && x->nop==1 && is_label_opnd(x->op[0]) && strcmp(x->mn,"jecxz")){ snprintf(cc,ccn,"%s",x->mn+1); snprintf(jl,jln,"%s",x->op[0]); continue; }
        return 0;
    }
    return 1;
}
#define SEQ(...) ((const char *const[]){__VA_ARGS__}), (int)(sizeof((const char *const[]){__VA_ARGS__})/sizeof(const char*))

static int rule_int_operand(P *p, int i){
    if(!is(p,i,"push edx") || !is(p,i+1,"push eax")) return 0;
    B64 b; if(!load64(p,i+2,&b)) return 0;
    int o=i+4; char cc[16]="", jl[160]="";
    char L[8][240]; const char *nl[8]; int n=0;
    #define NL(...) do{ snprintf(L[n],sizeof L[n],__VA_ARGS__); nl[n]=L[n]; n++; }while(0)
    #define LAST(k) (&p->v[o+(k)-1])
    int imm=!b.mem, lo0=imm && (uint32_t)b.v==0, hi0=imm && (uint64_t)b.v>>32==0;
    /* a + b */
    if(match_seq(p,o,SEQ("add eax,[esp]","adc edx,[esp+4]","jo rt_int_overflow","add esp,8"),cc,16,jl,160)){
        if(LAST(4)->out & RFL) return 0;
        NL("add eax,%s",b.lo); NL("adc edx,%s",b.hi); NL("jo rt_int_overflow");
        count(11,"int operand folded"); replace(p,i,8,nl,n); return 1;
    }
    /* a - b */
    if(match_seq(p,o,SEQ("mov ecx,eax","mov eax,[esp]","sub eax,ecx","mov ecx,edx","mov edx,[esp+4]","sbb edx,ecx","jo rt_int_overflow","add esp,8"),cc,16,jl,160)){
        Ins *l=LAST(8); if(l->out & RFL) return 0;
        if(!(l->out & RC)){ NL("sub eax,%s",b.lo); NL("sbb edx,%s",b.hi); }
        else {                                            /* ecx = b's high word, as the template leaves it */
            if(b.mem) NL("mov ecx,%s",b.hi); else if(hi0) NL("xor ecx,ecx"); else NL("mov ecx,%s",b.hi);
            NL("sub eax,%s",b.lo); NL("sbb edx,ecx"); }
        NL("jo rt_int_overflow");
        count(11,"int operand folded"); replace(p,i+0,12,nl,n); return 1;
    }
    /* a == b, a != b: a value */
    if(match_seq(p,o,SEQ("xor eax,[esp]","xor edx,[esp+4]","or eax,edx",NULL,"movzx eax,al","add esp,8"),cc,16,jl,160) && !jl[0]){
        if(LAST(6)->out & RFL) return 0;
        if(!lo0) NL("xor eax,%s",b.lo); if(!hi0) NL("xor edx,%s",b.hi); NL("or eax,edx"); NL("set%s al",cc); NL("movzx eax,al");
        count(11,"int operand folded"); replace(p,i,10,nl,n); return 1;
    }
    /* ... a jump */
    if(match_seq(p,o,SEQ("xor eax,[esp]","xor edx,[esp+4]","or eax,edx","lea esp,[esp+8]",NULL),cc,16,jl,160) && jl[0]){
        if(!lo0) NL("xor eax,%s",b.lo); if(!hi0) NL("xor edx,%s",b.hi); NL("or eax,edx"); NL("j%s %s",cc,jl);
        count(11,"int operand folded"); replace(p,i,9,nl,n); return 1;
    }
    /* a // c, a % c with a constant 0 < c < 2**31: a shift or a mask for a power of two, else rt_idivc
       (rt_idivmod's code includes it) */
    int div=match_seq(p,o,SEQ("call rt_idivmod","add esp,8"),cc,16,jl,160);
    int mod=!div && match_seq(p,o,SEQ("call rt_idivmod","mov eax,[rt_scratch]","mov edx,[rt_scratch+4]","add esp,8"),cc,16,jl,160);
    if((div||mod) && imm && b.v>0 && b.v<=0x7FFFFFFF){
        if(LAST(div?2:4)->out & RFL) return 0;
        int k=0; while(k<31 && ((int64_t)1<<k)<b.v) k++;
        if(((int64_t)1<<k)==b.v){
            if(div && k){ NL("shrd eax,edx,%d",k); NL("sar edx,%d",k); }
            if(mod){ if(k) NL("and eax,%d",(int)(b.v-1)); else NL("xor eax,eax"); NL("xor edx,edx"); }
        } else {
            NL("mov ecx,%d",(int)b.v); NL("call rt_idivc");
            if(mod){ NL("mov eax,ecx"); NL("xor edx,edx"); }
        }
        count(15,"int division by a constant"); replace(p,i,div?6:8,nl,n); return 1;   /* (a // 1: nothing) */
    }
    /* a & b, a | b, a ^ b */
    static const char *const bitops[]={"and","or","xor"};
    for(int k=0;k<3;k++){
        char t1[32], t2[32]; snprintf(t1,sizeof t1,"%s eax,[esp]",bitops[k]); snprintf(t2,sizeof t2,"%s edx,[esp+4]",bitops[k]);
        if(!match_seq(p,o,SEQ(t1,t2,"add esp,8"),cc,16,jl,160)) continue;
        if(LAST(3)->out & RFL) return 0;
        int and=k==0;
        if(!(imm && (and ? (uint32_t)b.v==0xFFFFFFFFu : lo0))) NL("%s eax,%s",bitops[k],b.lo);
        if(imm && and && hi0) NL("xor edx,edx");
        else if(!(imm && (and ? (uint64_t)b.v>>32==0xFFFFFFFFu : hi0))) NL("%s edx,%s",bitops[k],b.hi);
        count(11,"int operand folded"); replace(p,i,7,nl,n); return 1;
    }
    /* a < b, a >= b: a - b */
    int val=match_seq(p,o,SEQ("mov ecx,[esp]","sub ecx,eax","mov ecx,[esp+4]","sbb ecx,edx",NULL,"movzx eax,al","add esp,8"),cc,16,jl,160) && !jl[0];
    int jmp=!val && match_seq(p,o,SEQ("mov ecx,[esp]","sub ecx,eax","mov ecx,[esp+4]","sbb ecx,edx","lea esp,[esp+8]",NULL),cc,16,jl,160) && jl[0];
    if(val||jmp){
        Ins *l=val?LAST(7):LAST(6);
        if(val && (l->out & RFL)) return 0;
        if(lo0) NL("test eax,eax"); else NL("cmp eax,%s",b.lo);   /* (the same flags) */
        NL("mov ecx,edx"); NL("sbb ecx,%s",b.hi);           /* ecx = a - b's high word, as the template leaves it */
        if(val){ NL("set%s al",cc); NL("movzx eax,al"); }
        else if(l->out & RA) NL("mov eax,%s",b.lo);
        if(l->out & RD){                                     /* edx:eax = b, as the template leaves them */
            if(val && hi0) NL("xor edx,edx"); else NL("mov edx,%s",b.hi); }
        if(jmp) NL("j%s %s",cc,jl);
        count(11,"int operand folded"); replace(p,i,4+(val?7:6),nl,n); return 1;
    }
    /* a > b, a <= b: b - a */
    val=match_seq(p,o,SEQ("sub eax,[esp]","sbb edx,[esp+4]",NULL,"movzx eax,al","add esp,8"),cc,16,jl,160) && !jl[0];
    jmp=!val && match_seq(p,o,SEQ("sub eax,[esp]","sbb edx,[esp+4]","lea esp,[esp+8]",NULL),cc,16,jl,160) && jl[0];
    if(val||jmp){
        Ins *l=val?LAST(5):LAST(4);
        if(val && (l->out & RFL)) return 0;
        if(l->out & RC) return 0;                           /* (the template keeps ecx) */
        if(jmp && (l->out & RA)) return 0;                  /* (and leaves b - a in edx:eax) */
        if(strcmp(cc,"l") && strcmp(cc,"ge")) return 0;
        if(imm){                                            /* a > b: a >= b+1; a <= b: a < b+1 */
            if(b.v==INT64_MAX || (l->out & RD)) return 0;
            int64_t v1=b.v+1; char lo[32], hi[32];
            imm32(lo,sizeof lo,(uint32_t)v1); imm32(hi,sizeof hi,(uint32_t)((uint64_t)v1>>32));
            const char *nc=!strcmp(cc,"l")?"ge":"l";
            NL("cmp eax,%s",lo); NL("mov ecx,edx"); NL("sbb ecx,%s",hi);
            if(val){ NL("set%s al",nc); NL("movzx eax,al"); } else NL("j%s %s",nc,jl);
        } else {
            NL("mov ecx,%s",b.lo); NL("sub ecx,eax"); NL("mov ecx,%s",b.hi); NL("sbb ecx,edx");
            if(val){ NL("set%s al",cc); NL("movzx eax,al"); }
            if(l->out & RD) NL("mov edx,ecx");              /* b - a's high word, as the template leaves it */
            if(jmp) NL("j%s %s",cc,jl);
        }
        count(11,"int operand folded"); replace(p,i,4+(val?5:4),nl,n); return 1;
    }
    #undef NL
    #undef LAST
    return 0;
}

typedef int (*Rule)(P*, int);
static const Rule rules[]={ rule_unreachable, rule_jump_next, rule_jcc_over_jmp, rule_thread, rule_int_operand,
                            rule_args_push, rule_pushpop, rule_reload, rule_argpush, rule_push_fold, rule_esp, rule_dead };

static int peep_off=-1, peep_stats=-1;

char *aot_peephole(const char *text, size_t len, size_t *outlen){
    if(peep_off<0){ const char *e=getenv("MPY_NO_PEEP"); peep_off=e && *e && strcmp(e,"0"); }
    P p={0};
    if(!peep_off){
        p_parse(&p,text,len);
        for(int i=0;i<p.n;i++) if(p.v[i].kind==K_OTHER){ p_free(&p); memset(&p,0,sizeof p); break; }   /* conditional assembly, data: left alone */
    }
    if(!p.n){
        char *r=(char*)xmalloc(len+1); memcpy(r,text,len); r[len]=0; *outlen=len; return r;
    }
    for(int pass=0;pass<8;pass++){
        analyze(&p);
        int changed=0;
        for(int i=0;i<p.n;i++)
            for(int tries=0;tries<4;tries++){
                int hit=0;
                for(size_t r=0;r<MPY_ARRAY_SIZE(rules) && budget_left();r++){
                    if(rules[r](&p,i)){ hit=1; break; }
                    if(peep_budget>=0) peep_budget++;     /* (no rewrite: nothing spent) */
                }
                if(!hit) break;
                changed=1;
                if(i>=p.n) break;
            }
        if(!changed) break;
    }
    size_t total=0; for(int i=0;i<p.n;i++) total+=strlen(p.v[i].line)+1;
    char *r=(char*)xmalloc(total+1), *o=r;
    for(int i=0;i<p.n;i++){ size_t l=strlen(p.v[i].line); memcpy(o,p.v[i].line,l); o+=l; *o++='\n'; }
    *o=0;
    if(len && text[len-1]!='\n' && o>r){ o--; *o=0; }
    *outlen=(size_t)(o-r);
    p_free(&p);
    return r;
}

/* ---------------------------------------------------------------- reuse: outlining

   The same few instructions before the same call - an object made, an
   exception raised, a tuple of two - come back all over a program. Each
   such stretch that occurs often enough becomes a routine of its own,
   OL<k>: the instructions, then a jump (not a call) to where the call went;
   every place it was says `call OL<k>`. The callee returns straight to that
   place, its arguments and return address where they were: one jump more
   per call. The stretch touches no esp (no push, pop, call before its end)
   and no label of the function it came from. */

/* bytes of an instruction, roughly (fasm picks the short forms) */
static int mem_bytes(const char *o){
    const char *b=strchr(o,'['), *e=b?strchr(b,']'):NULL; if(!b||!e) return 4;
    char in[200]; size_t n=(size_t)(e-b-1); if(n>=sizeof in) return 6; memcpy(in,b+1,n); in[n]=0;
    unsigned r=regs_in(in); int has_label=0, idx=strchr(in,'*')!=NULL; long d=0;
    for(const char *q=in;*q;){
        if(isalpha((unsigned char)*q)||*q=='_'){ const char *t=q; while(*q && ident_ch((unsigned char)*q)) q++;
            char tok[64]; size_t k=(size_t)(q-t); if(k<sizeof tok){ memcpy(tok,t,k); tok[k]=0; if(!reg_of(tok,NULL)) has_label=1; } else has_label=1; continue; }
        if(isdigit((unsigned char)*q)){ char *end; long v=strtol(q,&end,0); d+= (q>in && q[-1]=='-') ? -v : v; q=end; continue; }
        q++;
    }
    int sib=(r&RSP) || idx || (r & (r-1));                 /* (two registers: base and index) */
    if(has_label || !r) return 1+(sib?1:0)+4;
    int disp = d==0 && !(r&RBP) ? 0 : (d>=-128 && d<=127) ? 1 : 4;
    return 1+(sib?1:0)+disp;
}
static int imm_bytes(const char *o, int wide){ int64_t v; if(imm_of(o,&v) && v>=-128 && v<=127) return 1; return wide?4:1; }
static int est_size(Ins *x){
    const char *m=x->mn, *a=x->nop>0?x->op[0]:"", *b=x->nop>1?x->op[1]:"";
    int ra=reg_of(a,NULL)!=0, rb=reg_of(b,NULL)!=0, ma=is_mem(a), mb=is_mem(b);
    if(!strcmp(m,"mov")){
        if(ra && rb) return 2;
        if(ra && mb) return reg32(a)==RA && !regs_in(b) ? 5 : 1+mem_bytes(b);
        if(ma && rb) return reg32(b)==RA && !regs_in(a) ? 5 : 1+mem_bytes(a);
        if(ra) return 5;
        if(ma) return 1+mem_bytes(a)+(starts(a,"byte")?1:starts(a,"word")?3:4);
    }
    if(!strcmp(m,"movzx")||!strcmp(m,"movsx")) return mb?2+mem_bytes(b):3;
    if(!strcmp(m,"lea")) return 1+mem_bytes(b);
    if(!strcmp(m,"add")||!strcmp(m,"sub")||!strcmp(m,"and")||!strcmp(m,"or")||!strcmp(m,"xor")||!strcmp(m,"adc")||!strcmp(m,"sbb")||!strcmp(m,"cmp")){
        if(ra && rb) return 2;
        if(ra && mb) return 1+mem_bytes(b);
        if(ma && rb) return 1+mem_bytes(a);
        if(ra){ int k=imm_bytes(b,1); return k==1?3:(reg32(a)==RA?5:6); }
        if(ma) return 1+mem_bytes(a)+imm_bytes(b,!starts(a,"byte"));
    }
    if(!strcmp(m,"test")) return ra&&rb?2:ra?(reg32(a)==RA?5:6):1+mem_bytes(a)+4;
    if(!strcmp(m,"push")||!strcmp(m,"pop")) return ra?1:ma?1+mem_bytes(a):imm_bytes(a,1)==1?2:5;
    if(!strcmp(m,"inc")||!strcmp(m,"dec")) return ra?1:1+mem_bytes(a);
    if(!strcmp(m,"call")||!strcmp(m,"jmp")) return ma?1+mem_bytes(a):ra?2:5;
    if(is_jcc(m)) return 6;
    if(starts(m,"set")) return ma?2+mem_bytes(a):3;
    if(!strcmp(m,"xchg")) return ma||mb?1+mem_bytes(ma?a:b):(reg32(a)==RA||reg32(b)==RA?1:2);
    if(!strcmp(m,"cdq")||!strcmp(m,"sahf")) return 1;
    if(m[0]=='f') return x->nop && ma ? 1+mem_bytes(a) : 2;
    return 4;
}

typedef struct { int ins; int size; int ok; int is_call; unsigned h; } OLine;
typedef struct { int site; int start; } OSite;              /* the call's line, the stretch's first line */
typedef struct { unsigned h; int len, size; OSite *v; int n, cap; int id; } OGroup;

static int same_stretch(char **txt, OSite a, OSite b){
    if(a.site-a.start!=b.site-b.start) return 0;
    for(int k=0;k<=a.site-a.start;k++) if(strcmp(txt[a.start+k],txt[b.start+k])) return 0;
    return 1;
}
static int og_cmp(const void *x, const void *y){
    const OGroup *a=*(OGroup*const*)x, *b=*(OGroup*const*)y;
    long ga=(long)a->n*(a->size-5)-a->size, gb=(long)b->n*(b->size-5)-b->size;
    return ga<gb?1:ga>gb?-1:0;
}

static int outline_off=-1;
char *aot_outline(const char *text, size_t len, size_t *outlen){
    if(outline_off<0){ const char *e=getenv("MPY_NO_OUTLINE"); outline_off=(e && *e && strcmp(e,"0")) || peep_off>0; }
    if(outline_off){ char *r=(char*)xmalloc(len+1); memcpy(r,text,len); r[len]=0; *outlen=len; return r; }
    /* the lines */
    int nl=0, cl=1024; const char **ls=(const char**)xmalloc(sizeof(char*)*(size_t)cl); int *ll=(int*)xmalloc(sizeof(int)*(size_t)cl);
    for(const char *q=text, *e=text+len; q<e;){
        const char *n=memchr(q,'\n',(size_t)(e-q)); size_t k=n?(size_t)(n-q):(size_t)(e-q);
        if(nl==cl){ cl*=2; ls=(const char**)xrealloc(ls,sizeof(char*)*(size_t)cl); ll=(int*)xrealloc(ll,sizeof(int)*(size_t)cl); }
        ls[nl]=q; ll[nl]=(int)k; nl++; q+=k; if(n) q++;
    }
    OLine *li=(OLine*)xmalloc(sizeof(OLine)*(size_t)(nl+1)); char **txt=(char**)xmalloc(sizeof(char*)*(size_t)(nl+1));
    Ins *t=(Ins*)xmalloc(sizeof(Ins)); int cond=0;
    for(int i=0;i<nl;i++){
        memset(&li[i],0,sizeof li[i]); txt[i]=NULL;
        if(ll[i]>=3 && !strncmp(ls[i],"if ",3)) cond++;                    /* (none of conditional assembly) */
        else if(ll[i]>=6 && !strncmp(ls[i],"end if",6) && cond) cond--;
        if(cond || !ll[i] || (ls[i][0]!=' ' && ls[i][0]!='\t')) continue;   /* labels, comments, directives */
        parse_line(t,ls[i],(size_t)ll[i]); free(t->line);
        if(t->kind!=K_INS) continue;
        li[i].ins=1; li[i].size=est_size(t); txt[i]=xstrdup2(t->txt);
        unsigned h=2166136261u; for(const char *c=t->txt;*c;c++){ h^=(unsigned char)*c; h*=16777619u; } li[i].h=h;
        li[i].is_call=!strcmp(t->mn,"call");
        int ok=!strstr(t->txt,"esp") && strcmp(t->mn,"push") && strcmp(t->mn,"pop") && strcmp(t->mn,"ret") && strcmp(t->mn,"leave") && strcmp(t->mn,"enter");
        if(t->flow==F_JMP || t->flow==F_RET) ok=0;
        if(t->flow==F_COND && (!is_label_opnd(t->op[0]) || t->op[0][0]=='L' || t->op[0][0]=='.' || t->op[0][0]=='@')) ok=0;   /* (a label of the function) */
        if(t->use==R_ALL && !li[i].is_call) ok=0;                                /* (unknown) */
        li[i].ok=ok;
    }
    free(t);
    /* the stretches ending in a call, grouped */
    int ncap=4096; OGroup **tab=(OGroup**)xmalloc(sizeof(OGroup*)*(size_t)ncap); memset(tab,0,sizeof(OGroup*)*(size_t)ncap);
    int ng=0;
    for(int c=0;c<nl;c++){
        if(!li[c].ins || !li[c].is_call || !li[c].ok) continue;
        unsigned h=li[c].h; int size=li[c].size;
        for(int s=c-1;s>=0 && c-s<16;s--){
            if(!li[s].ins || !li[s].ok || li[s].is_call) break;
            h=(h^li[s].h)*16777619u+(unsigned)(c-s); size+=li[s].size;
            if(size<8) continue;
            if(ng*2>=ncap){                                   /* grow the table */
                int nc=ncap*2; OGroup **nt=(OGroup**)xmalloc(sizeof(OGroup*)*(size_t)nc); memset(nt,0,sizeof(OGroup*)*(size_t)nc);
                for(int k=0;k<ncap;k++) if(tab[k]){ unsigned q=tab[k]->h&(unsigned)(nc-1); while(nt[q]) q=(q+1)&(unsigned)(nc-1); nt[q]=tab[k]; }
                free(tab); tab=nt; ncap=nc; }
            OSite site={c,s};
            unsigned q=h&(unsigned)(ncap-1); OGroup *g=NULL;
            for(;tab[q];q=(q+1)&(unsigned)(ncap-1)) if(tab[q]->h==h && tab[q]->len==c-s && same_stretch(txt,tab[q]->v[0],site)){ g=tab[q]; break; }
            if(!g){ g=MPY_NEW0(OGroup); g->h=h; g->len=c-s; g->size=size; g->id=-1; tab[q]=g; ng++; }
            if(g->n==g->cap){ g->cap=g->cap?g->cap*2:4; g->v=(OSite*)xrealloc(g->v,sizeof(OSite)*(size_t)g->cap); }
            g->v[g->n++]=site;
        }
    }
    OGroup **gs=(OGroup**)xmalloc(sizeof(OGroup*)*(size_t)(ng+1)); int k=0;
    for(int i=0;i<ncap;i++) if(tab[i]) gs[k++]=tab[i];
    qsort(gs,(size_t)ng,sizeof(OGroup*),og_cmp);
    /* the most profitable first; a call is outlined once */
    int *taken=(int*)xmalloc(sizeof(int)*(size_t)(nl+1)); for(int i=0;i<=nl;i++) taken[i]=-1;   /* per call line: the stretch's start */
    int *owner=(int*)xmalloc(sizeof(int)*(size_t)(nl+1));
    int nid=0;
    for(int i=0;i<ng;i++){ OGroup *g=gs[i]; int free_=0;
        for(int j=0;j<g->n;j++) if(taken[g->v[j].site]<0) free_++;
        if(free_<2 || (long)free_*(g->size-5)-g->size<=0) continue;
        g->id=nid++;
        for(int j=0;j<g->n;j++) if(taken[g->v[j].site]<0){ taken[g->v[j].site]=g->v[j].start; owner[g->v[j].site]=g->id; }
        count(14,"calls through a shared routine (OL)");
    }
    /* the text again */
    size_t cap=len+1024; char *r=(char*)xmalloc(cap), *o=r;
    #define PUT(s_,n_) do{ size_t n__=(size_t)(n_); while((size_t)(o-r)+n__+2>cap){ size_t off=(size_t)(o-r); cap*=2; r=(char*)xrealloc(r,cap); o=r+off; } memcpy(o,s_,n__); o+=n__; }while(0)
    int *skip_to=(int*)xmalloc(sizeof(int)*(size_t)(nl+1)); for(int i=0;i<=nl;i++) skip_to[i]=-1;
    for(int c=0;c<nl;c++) if(taken[c]>=0) skip_to[taken[c]]=c;
    for(int i=0;i<nl;i++){
        if(skip_to[i]>=0){ char b[32]; int n=snprintf(b,sizeof b,"        call OL%d\n",owner[skip_to[i]]); PUT(b,n); i=skip_to[i]; continue; }
        PUT(ls[i],ll[i]); PUT("\n",1);
    }
    for(int i=0;i<ng;i++){ OGroup *g=gs[i]; if(g->id<0) continue;
        char b[48]; int n=snprintf(b,sizeof b,"\nOL%d:                           ; shared\n",g->id); PUT(b,n);
        OSite st=g->v[0];
        for(int q=st.start;q<st.site;q++){ PUT("        ",8); PUT(txt[q],strlen(txt[q])); PUT("\n",1); }
        PUT("        jmp",11); PUT(txt[st.site]+4,strlen(txt[st.site]+4)); PUT("\n",1);
    }
    #undef PUT
    *o=0; *outlen=(size_t)(o-r);
    for(int i=0;i<ng;i++){ free(gs[i]->v); free(gs[i]); }
    for(int i=0;i<nl;i++) free(txt[i]);
    free(gs); free(tab); free(taken); free(owner); free(skip_to); free(li); free(txt); free(ls); free(ll);
    return r;
}

void aot_peephole_report(void){
    if(peep_stats<0){ const char *e=getenv("MPY_PEEP_STATS"); peep_stats=e && *e && strcmp(e,"0"); }
    if(!peep_stats) return;
    for(int k=0;k<32;k++) if(stat_count[k]) fprintf(stderr,"peephole: %6ld  %s\n",stat_count[k],stat_name[k]);
}
