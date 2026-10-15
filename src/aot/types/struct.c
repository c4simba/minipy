/* ========================= Typed compiler, types: struct, tempfile =========================
   struct.pack / unpack with a literal format: the code for its layout;
   tempfile's files. */

#include "check.h"

/* ---- struct.pack / unpack / unpack_from / iter_unpack / calcsize with a literal format: the
   items laid out now (as on i386: the native sizes and alignment of a compiled program), each
   value packed or unpacked by its own helper of struct.py, so each has its type */
typedef struct { char code; int off, size; } SItem;
typedef struct { char *s; size_t n, cap; } SBuf_;
static void tsb_printf(SBuf_ *b, const char *fmt, ...){
    va_list ap; va_start(ap,fmt); int k=vsnprintf(NULL,0,fmt,ap); va_end(ap);
    if(b->n+(size_t)k+1>b->cap){ b->cap=(b->n+(size_t)k+1)*2; b->s=(char*)xrealloc(b->s,b->cap); }
    va_start(ap,fmt); vsnprintf(b->s+b->n,(size_t)k+1,fmt,ap); va_end(ap); b->n+=(size_t)k;
}
static const char *struct_layout(const char *fmt, SItem **out, int *nout, int *total, int *little){
    static const char codes[]="xcbB?hHiIlLqQnNefdspP";
    static const int nsize[]={1,1,1,1,1,2,2,4,4,4,4,8,8,4,4,2,4,8,1,1,4};
    static const int nalign[]={0,0,0,0,1,2,2,4,4,4,4,4,4,4,4,2,4,4,0,0,4};
    static const int ssize[]={1,1,1,1,1,2,2,4,4,4,4,8,8,0,0,2,4,8,1,1,0};
    const char *p=fmt; int native=1; *little=1; *nout=0; *total=0;
    if(*p=='@'||*p=='='||*p=='<'||*p=='>'||*p=='!'){ native= *p=='@'; *little= *p=='@'||*p=='='||*p=='<'; p++; }
    SItem *v=NULL; int n=0, cap=0; long long size=0;
    while(*p){
        if(isspace((unsigned char)*p)){ p++; continue; }
        long long num=1;
        if(*p>='0' && *p<='9'){ num=0; while(*p>='0' && *p<='9'){ num=num*10+(*p-'0'); if(num>100000000) return "total struct size too long"; p++; }
            if(!*p) return "repeat count given without format specifier"; }
        const char *k=strchr(codes,*p); int ci= k ? (int)(k-codes) : -1; char code=*p++;
        if(ci<0 || !code || (!native && !ssize[ci])) return "bad char in struct format";
        int sz= native ? nsize[ci] : ssize[ci];
        if(native && nalign[ci]>1) size=(size+nalign[ci]-1)/nalign[ci]*nalign[ci];
        if(code=='x'){ size+=num; continue; }
        long long reps= code=='s'||code=='p' ? 1 : num;
        for(long long r=0;r<reps;r++){
            if(n==cap){ cap=cap?cap*2:8; v=(SItem*)xrealloc(v,sizeof(SItem)*(size_t)cap); }
            v[n].code=code; v[n].off=(int)size; v[n].size= code=='s'||code=='p' ? (int)num : sz; n++;
            size+= code=='s'||code=='p' ? num : sz; }
        if(size>100000000) return "total struct size too long";
    }
    *out=v; *nout=n; *total=(int)size; return NULL;
}
/* the text unpacking item it from the bytes _mpy_sb at offset base + its offset */
static void struct_unpack_text(char *t, size_t n, SItem *it, const char *base, int little){
    const char *L= little ? "True" : "False";
    switch(it->code){
        case 'c': snprintf(t,n,"__mpy_struct__u_char(_mpy_sb, %s%d)",base,it->off); break;
        case '?': snprintf(t,n,"__mpy_struct__u_bool(_mpy_sb, %s%d)",base,it->off); break;
        case 's': snprintf(t,n,"__mpy_struct__u_str(_mpy_sb, %s%d, %d)",base,it->off,it->size); break;
        case 'p': snprintf(t,n,"__mpy_struct__u_pascal(_mpy_sb, %s%d, %d)",base,it->off,it->size); break;
        case 'e': case 'f': case 'd': snprintf(t,n,"__mpy_struct__u_float(_mpy_sb, %s%d, %d, %s)",base,it->off,it->size,L); break;
        default: snprintf(t,n,"__mpy_struct__u_int(_mpy_sb, %s%d, %d, %s, %s)",base,it->off,it->size,L,
                     islower((unsigned char)it->code)&&it->code!='p' ? "True" : "False");
    }
}
/* the text packing the values <arg>0, <arg>1, ... (pad bytes between them) */
static void struct_pack_text(SBuf_ *t, SItem *it, int n, int size, int little, const char *arg){
    char one[160]; int pos=0; const char *L= little ? "True" : "False";
    tsb_printf(t,"__mpy_struct__join([");
    for(int i=0;i<n;i++){ SItem *x=&it[i];
        if(x->off>pos) tsb_printf(t,"__mpy_struct__pad(%d), ",x->off-pos);
        switch(x->code){
            case 'c': snprintf(one,sizeof one,"__mpy_struct__p_char(%s%d)",arg,i); break;
            case '?': snprintf(one,sizeof one,"__mpy_struct__p_bool(%s%d)",arg,i); break;
            case 's': snprintf(one,sizeof one,"__mpy_struct__p_str(%s%d, %d)",arg,i,x->size); break;
            case 'p': snprintf(one,sizeof one,"__mpy_struct__p_pascal(%s%d, %d)",arg,i,x->size); break;
            case 'e': case 'f': case 'd': snprintf(one,sizeof one,"__mpy_struct__p_float(%s%d, %d, %s, '%c')",arg,i,x->size,L,x->code); break;
            default: snprintf(one,sizeof one,"__mpy_struct__p_int(%s%d, %d, %s, %s, '%c')",arg,i,x->size,L,islower((unsigned char)x->code)?"True":"False",x->code);
        }
        tsb_printf(t,"%s, ",one); pos=x->off+x->size; }
    if(size>pos) tsb_printf(t,"__mpy_struct__pad(%d), ",size-pos);
    tsb_printf(t,"])");
}
static void subst_args(Expr **pe, Expr **args, int nargs){
    Expr *e=*pe; if(!e) return;
    if(e->kind==EXPR_NAME && !strncmp(e->name,"__mpy_a",7)){ int i=atoi(e->name+7);
        if(i>=0 && i<nargs){ Expr *x=MPY_NEW0(Expr); *x=*args[i]; x->akind=e->akind; x->kw=e->kw; *pe=x; } return; }
    subst_args(&e->a,args,nargs); subst_args(&e->b,args,nargs); subst_args(&e->c,args,nargs); subst_args(&e->d,args,nargs);
    for(int i=0;i<e->count;i++) subst_args(&e->items[i],args,nargs);
    for(int i=0;i<e->vcount;i++) subst_args(&e->vals[i],args,nargs);
}
/* f(...) calling struct's pack, unpack, unpack_from, iter_unpack or calcsize -> its name */
const char *struct_func(Ck *c, Expr *f){
    static const char *const names[]={"pack","unpack","unpack_from","iter_unpack","calcsize","Struct",NULL};
    const char *nm=NULL;
    if(!strcmp(c->mod->name,"struct")) return NULL;
    if(f->kind==EXPR_NAME){ if(!strncmp(f->name,"__mpy_",6)) return NULL;          /* (what the calls become) */
        ASym *s=lookup(c,f->name);
        if(s && s->kind==AS_CLASS && !strcmp(((AClass*)s->p)->mod->name,"struct") && !strcmp(((AClass*)s->p)->name,"_CStruct")) return "Struct";
        if(!s || s->kind!=AS_FUNC || strcmp(((AFunc*)s->p)->mod->name,"struct")) return NULL;
        nm=((AFunc*)s->p)->name; }
    else if(f->kind==EXPR_ATTRIBUTE){ AModule *m=module_expr(c,f->a); if(!m || strcmp(m->name,"struct")) return NULL; nm=f->name; }
    else return NULL;
    for(int i=0;names[i];i++) if(!strcmp(names[i],nm)) return names[i];
    return NULL;
}
/* f(...) calling tempfile's NamedTemporaryFile or TemporaryFile -> its name */
const char *tempfile_func(Ck *c, Expr *f){
    if(!strcmp(c->mod->name,"tempfile")) return NULL;
    if(f->kind==EXPR_NAME){ if(!strncmp(f->name,"__mpy_",6)) return NULL;
        ASym *s=lookup(c,f->name);
        if(!s || s->kind!=AS_FUNC || strcmp(((AFunc*)s->p)->mod->name,"tempfile")) return NULL;
        const char *n=((AFunc*)s->p)->name;
        if(!strcmp(n,"_NamedTemporaryFileB")) return "NamedTemporaryFile";
        if(!strcmp(n,"_TemporaryFileB")) return "TemporaryFile";
        return NULL; }
    if(f->kind==EXPR_ATTRIBUTE){ AModule *m=module_expr(c,f->a); if(!m || strcmp(m->name,"tempfile")) return NULL;
        if(!strcmp(f->name,"NamedTemporaryFile") || !strcmp(f->name,"TemporaryFile")) return f->name; }
    return NULL;
}
Ty *ck_struct(Ck *c, Expr *e, const char *fn){
    int line=e->line;
    if(!e->count || e->items[0]->akind) ck_err(c,line,"struct.%s() takes the format first",fn);
    Expr *fe=e->items[0];
    if(!(fe->kind==EXPR_LITERAL && fe->tok->kind==T_STRING && fe->tok->i!=2)){
        if(!strcmp(fn,"calcsize")) return NULL;                      /* (done when running) */
        ck_err(c,line,"struct.%s(): the format must be a literal in compiled code",fn); }
    SItem *it=NULL; int n=0, size=0, little=1;
    const char *bad=struct_layout(fe->tok->text,&it,&n,&size,&little);
    if(bad) ck_err(c,line,"struct.error: %s",bad);
    Expr *args[3]={NULL,NULL,NULL}; int nargs=0;
    SBuf_ t; memset(&t,0,sizeof t);
    char one[160];
    if(!strcmp(fn,"calcsize")){
        if(e->count!=1) ck_err(c,line,"calcsize() takes exactly one argument");
        tsb_printf(&t,"%d",size); }
    else if(!strcmp(fn,"Struct")){                                     /* a _CStruct: its unpacking and packing functions */
        if(e->count!=1) ck_err(c,line,"Struct() takes exactly one argument");
        if(!n) ck_err(c,line,"struct.Struct(): a format without values is not supported in compiled code");
        args[0]=fe; nargs=1;
        tsb_printf(&t,"__mpy_struct__CStruct(__mpy_a0, %d, lambda _mpy_sb, _mpy_so: (",size);
        for(int i=0;i<n;i++){ struct_unpack_text(one,sizeof one,&it[i],"_mpy_so + ",little); tsb_printf(&t,"%s, ",one); }
        tsb_printf(&t,"), lambda ");
        for(int i=0;i<n;i++) tsb_printf(&t,"%s_mpy_v%d",i?", ":"",i);
        tsb_printf(&t,": ");
        struct_pack_text(&t,it,n,size,little,"_mpy_v");
        tsb_printf(&t,")"); }
    else if(!strcmp(fn,"pack")){
        for(int i=1;i<e->count;i++) if(e->items[i]->akind) ck_err(c,line,"pack() takes positional arguments");
        if(e->count-1!=n) ck_err(c,line,"struct.error: pack expected %d items for packing (got %d)",n,e->count-1);
        Expr **vals=e->items+1;
        struct_pack_text(&t,it,n,size,little,"__mpy_a");
        Expr *res=py_front_expr(c->mod->unit->path,t.s,line); free(t.s); free(it);
        if(!res) ck_err(c,line,"internal error: struct.pack");
        subst_args(&res,vals,n);
        { void *keep=e->ty; *e=*res; e->ty=keep; }
        return ck_expr_inner(c,e); }
    else {
        int from= !strcmp(fn,"unpack_from"), iter= !strcmp(fn,"iter_unpack");
        if(!n) ck_err(c,line,"struct.%s(): a format without values is not supported in compiled code",fn);
        static const char *const kws[]={"buffer","offset"};
        for(int i=1;i<e->count;i++){ Expr *a=e->items[i]; int k=i-1;
            if(a->akind==3){ k=-1; for(int q=0;q<2;q++) if(!strcmp(a->kw,kws[q])) k=q;
                if(k<0 || !from) ck_err(c,line,"%s() got an unexpected keyword argument '%s'",fn,a->kw); }
            else if(a->akind) ck_err(c,line,"%s() takes positional arguments",fn);
            if(k>(from?1:0)) ck_err(c,line,"%s() takes at most %d arguments",fn,from?3:2);
            if(args[k]) ck_err(c,line,"%s() got multiple values for argument '%s'",fn,kws[k]);
            args[k]=a; }
        if(!args[0]) ck_err(c,line,"%s() missing required argument 'buffer'",fn);
        nargs= args[1] ? 2 : 1;
        if(from) tsb_printf(&t,"__mpy_struct__unpack_from1(__mpy_a0, %d, %s, lambda _mpy_sb, _mpy_so: (",size,args[1]?"__mpy_a1":"0");
        else if(iter) tsb_printf(&t,"__mpy_struct__iter_unpack1(__mpy_a0, %d, lambda _mpy_sb, _mpy_so: (",size);
        else tsb_printf(&t,"__mpy_struct__unpack1(__mpy_a0, %d, lambda _mpy_sb: (",size);
        for(int i=0;i<n;i++){ struct_unpack_text(one,sizeof one,&it[i],from||iter?"_mpy_so + ":"",little); tsb_printf(&t,"%s, ",one); }
        tsb_printf(&t,"))"); }
    Expr *res=py_front_expr(c->mod->unit->path,t.s,line); free(t.s); free(it);
    if(!res) ck_err(c,line,"internal error: struct.%s",fn);
    subst_args(&res,args,nargs);
    { void *keep=e->ty; *e=*res; e->ty=keep; }
    return ck_expr_inner(c,e);
}
