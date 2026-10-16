/* ========================= Typed compiler, types: ctypes =========================
   ctypes declarations (CDLL, argtypes, restype) taken out of the
   program: C functions the compiled code calls. */

#include "check.h"

/* What a module calls ctypes by: `import ctypes [as x]`, `from ctypes import ...`. */
typedef struct { const char *mod; const char *local[64]; const char *member[64]; int n; } CtNames;
static void ct_names(AotUnit *u, CtNames *ct){
    memset(ct,0,sizeof *ct);
    Stmt **b=u->ast->body; int n=u->ast->body_count;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_IMPORT && !strcmp(s->name2?s->name2:s->name,"ctypes")) ct->mod=s->name;
        if(s->kind==STMT_FROM_IMPORT){
            char *dotted=aot_from_import_module(u,s);
            if(!strcmp(dotted,"ctypes"))
                for(int k=0;k<s->nnames;k++) if(strcmp(s->names[k],"*") && ct->n<64){
                    ct->local[ct->n]=s->asnames[k]?s->asnames[k]:s->names[k]; ct->member[ct->n]=s->names[k]; ct->n++; }
            free(dotted);
        }
    }
}
/* e names a member of ctypes: its name, else NULL */
static const char *ct_member(CtNames *ct, Expr *e){
    if(e->kind==EXPR_ATTRIBUTE && e->a->kind==EXPR_NAME && ct->mod && !strcmp(e->a->name,ct->mod)) return e->name;
    if(e->kind==EXPR_NAME) for(int i=0;i<ct->n;i++) if(!strcmp(ct->local[i],e->name)) return ct->member[i];
    return NULL;
}
static int ct_type(const char *m, CType *out){
    static const struct { const char *name; CType t; } map[]={
        {"c_int",CT_INT},{"c_long",CT_LONG},{"c_int32",CT_INT},{"c_ssize_t",CT_LONG},
        {"c_uint",CT_UINT},{"c_ulong",CT_ULONG},{"c_uint32",CT_UINT},{"c_size_t",CT_ULONG},
        {"c_short",CT_SHORT},{"c_int16",CT_SHORT},{"c_ushort",CT_USHORT},{"c_uint16",CT_USHORT},
        {"c_byte",CT_BYTE},{"c_int8",CT_BYTE},{"c_char",CT_BYTE},{"c_ubyte",CT_UBYTE},{"c_uint8",CT_UBYTE},{"c_bool",CT_BOOL},
        {"c_longlong",CT_LONGLONG},{"c_int64",CT_LONGLONG},{"c_ulonglong",CT_ULONGLONG},{"c_uint64",CT_ULONGLONG},
        {"c_double",CT_DOUBLE},{"c_longdouble",CT_DOUBLE},{"c_float",CT_FLOAT},{"c_char_p",CT_CHARP},{"c_void_p",CT_VOIDP},{NULL,0}};
    for(int i=0;map[i].name;i++) if(!strcmp(map[i].name,m)){ *out=map[i].t; return 1; }
    return 0;
}
ACLib *clib_get(AProg *p, const char *soname){
    for(int i=0;i<p->nclibs;i++) if(!strcmp(p->clibs[i]->soname,soname)) return p->clibs[i];
    ACLib *l=PPY_NEW0(ACLib); l->soname=xstrdup2(soname); l->id=p->nclibs;
    p->clibs=(ACLib**)xrealloc(p->clibs,sizeof(ACLib*)*(size_t)(p->nclibs+1)); p->clibs[p->nclibs++]=l;
    return l;
}
ACFunc *cfunc_get(AProg *p, ACLib *lib, const char *sym){
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->lib==lib && !strcmp(p->cfuncs[i]->sym,sym)) return p->cfuncs[i];
    ACFunc *f=PPY_NEW0(ACFunc); f->lib=lib; f->sym=xstrdup2(sym); f->id=p->ncfuncs; f->nargtypes=-1;
    p->cfuncs=(ACFunc**)xrealloc(p->cfuncs,sizeof(ACFunc*)*(size_t)(p->ncfuncs+1)); p->cfuncs[p->ncfuncs++]=f;
    return f;
}
/* A C function named by e (lib.f or an alias of one) in module m, else NULL */
static ACFunc *ct_func_ref(AProg *p, AModule *m, Expr *e){
    if(e->kind==EXPR_NAME){ ASym *s=module_sym(m,e->name); return s && s->kind==AS_CFUNC ? (ACFunc*)s->p : NULL; }
    if(e->kind==EXPR_ATTRIBUTE && e->a->kind==EXPR_NAME){ ASym *s=module_sym(m,e->a->name);
        if(s && s->kind==AS_CLIB) return cfunc_get(p,(ACLib*)s->p,e->name); }
    return NULL;
}
static CType ct_decl_type(Ck *c, CtNames *ct, Expr *e){
    CType t; const char *m;
    if(e->kind==EXPR_NONE) return CT_VOID;
    if(!(m=ct_member(ct,e)) || !ct_type(m,&t)) ck_err(c,e->line,"expected a ctypes type (ctypes.c_int, c_char_p, c_double, ...) or None");
    return t;
}
/* The module-level ctypes statements, taken out of the program:
     lib = ctypes.CDLL("libc.so.6")       (or ctypes.cdll.LoadLibrary(...), CDLL(None))
     f = lib.name                          (an alias)
     lib.name.restype = ctypes.c_char_p    lib.name.argtypes = [ctypes.c_int, ...] */
void collect_ctypes(Ck *c, AModule *m){
    AotUnit *u=m->unit; CtNames ct; ct_names(u,&ct);
    if(!ct.mod && !ct.n) return;
    Stmt **b=u->ast->body; int n=u->ast->body_count;
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind!=STMT_ASSIGN) continue;
        AAssign *a=aot_assign(u,s);
        if(a->ntarget!=1 || !a->value || a->aug) continue;
        Expr *t=a->target[0], *v=a->value;
        if(t->kind==EXPR_NAME && v->kind==EXPR_CALL){                 /* lib = CDLL(...) */
            const char *mm=ct_member(&ct,v->a);
            int load=v->a->kind==EXPR_ATTRIBUTE && !strcmp(v->a->name,"LoadLibrary") && (mm=ct_member(&ct,v->a->a)) && !strcmp(mm,"cdll");
            if(!load && !(mm && !strcmp(mm,"CDLL"))) continue;
            if(v->count<1 || v->items[0]->akind) ck_err(c,s->line,"CDLL() takes the library's file name");
            Expr *nm=v->items[0]; const char *so="libc.so.6";
            if(nm->kind==EXPR_LITERAL && nm->tok->kind==T_STRING) so=nm->tok->text;
            else if(nm->kind!=EXPR_NONE) ck_err(c,s->line,"the library name must be a string literal (or None: the C library) in compiled code");
            if(c->p->target==AOT_TARGET_KOLIBRI) ck_err(c,s->line,"ctypes libraries need the linux or macos target");
            if(module_sym(m,t->name)) ck_err(c,s->line,"'%s' is defined twice",t->name);
            symtab_add(&m->syms,t->name,AS_CLIB,clib_get(c->p,so));
            s->kind=STMT_PASS; continue;
        }
        if(t->kind==EXPR_NAME && v->kind==EXPR_ATTRIBUTE){            /* f = lib.name */
            ACFunc *f=ct_func_ref(c->p,m,v);
            if(!f) continue;
            if(module_sym(m,t->name)) ck_err(c,s->line,"'%s' is defined twice",t->name);
            symtab_add(&m->syms,t->name,AS_CFUNC,f);
            s->kind=STMT_PASS; continue;
        }
        if(t->kind==EXPR_ATTRIBUTE && (!strcmp(t->name,"restype") || !strcmp(t->name,"argtypes"))){
            ACFunc *f=ct_func_ref(c->p,m,t->a);
            if(!f) continue;
            if(t->name[0]=='r') f->restype=ct_decl_type(c,&ct,v);
            else {
                if(v->kind==EXPR_NONE){ f->nargtypes=-1; s->kind=STMT_PASS; continue; }
                if(v->kind!=EXPR_LIST && v->kind!=EXPR_TUPLE) ck_err(c,s->line,"argtypes must be a list of ctypes types");
                if(v->count>16) ck_err(c,s->line,"at most 16 argtypes");
                f->nargtypes=v->count;
                for(int k=0;k<v->count;k++){ f->argtypes[k]=ct_decl_type(c,&ct,v->items[k]);
                    if(f->argtypes[k]==CT_VOID) ck_err(c,s->line,"None is not an argument type"); }
            }
            s->kind=STMT_PASS; continue;
        }
    }
}
