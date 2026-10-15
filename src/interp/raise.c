/* ========================= Interpreter: raising exceptions =========================
   Catch points, raising (with longjmp), exception matching, the
   traceback entries; "Did you mean" suggestions (CPython's
   Python/suggestions.c). */

#include "run.h"

void mp_catch_push(Catch *c){ c->prev=mp_ts->catch; c->frame=mp_ts->frame; c->depth=mp_ts->depth; mp_ts->catch=c; }
void mp_catch_pop(Catch *c){ mp_ts->catch=c->prev; }
Value mp_catch_exc(Catch *c){ (void)c; Value e=mp_ts->exc; mp_ts->exc=v_undef(); return e; }

static int is_exc_class(Value v){ return IS_TYPE(v) && mp_is_subtype(AS_TYPE(v),E_BaseException); }
int mp_isinstance(Value v, Type *t){ return mp_is_subtype(TYPE(v),t); }
int mp_is_subtype(Type *t, Type *base){
    if(t==base) return 1;
    if(base==T_object) return 1;
    for(int i=0;i<t->nmro;i++) if(t->mro[i]==base) return 1;
    return 0;
}
Value mp_exc_args(Type *t, Value args){
    ExcObj *e=(ExcObj*)mp_alloc(t,sizeof(ExcObj));
    e->args=args; e->cause=v_none(); e->context=v_none(); e->tb=v_undef(); e->notes=v_undef(); e->hint=v_none();
    return v_obj(e);
}
static void exc_set(Value e, const char *k, Value v){ ExcObj *x=AS_EXC(e); if(!x->dict) x->dict=AS_DICT(mp_dict()); mp_dict_set_s(x->dict,k,v); }

/* ---------------------------------------------------------------- "Did you mean" (CPython's Python/suggestions.c) */
#define SG_MAX_ITEMS 750
#define SG_MAX_SIZE 40
#define SG_MOVE 2
#define SG_CASE 1
static int sg_cost(char a, char b){
    if((a&31)!=(b&31)) return SG_MOVE;
    if(a==b) return 0;
    if(a>='A' && a<='Z') a+='a'-'A';
    if(b>='A' && b<='Z') b+='a'-'A';
    return a==b ? SG_CASE : SG_MOVE;
}
static int64_t sg_distance(const char *a, int64_t an, const char *b, int64_t bn, int64_t maxc, int64_t *buf){
    if(an==bn && !memcmp(a,b,(size_t)an)) return 0;
    while(an && bn && a[0]==b[0]){ a++; an--; b++; bn--; }
    while(an && bn && a[an-1]==b[bn-1]){ an--; bn--; }
    if(!an || !bn) return (an+bn)*SG_MOVE;
    if(an>SG_MAX_SIZE || bn>SG_MAX_SIZE) return maxc+1;
    if(bn<an){ const char *t=a; a=b; b=t; int64_t n=an; an=bn; bn=n; }
    if((bn-an)*SG_MOVE>maxc) return maxc+1;
    int64_t tmp=SG_MOVE;
    for(int64_t i=0;i<an;i++){ buf[i]=tmp; tmp+=SG_MOVE; }
    int64_t result=0;
    for(int64_t bi=0;bi<bn;bi++){
        char code=b[bi];
        int64_t distance=result=bi*SG_MOVE, minimum=INT64_MAX;
        for(int64_t i=0;i<an;i++){
            int64_t substitute=distance+sg_cost(code,a[i]);
            distance=buf[i];
            int64_t insdel=(result<distance?result:distance)+SG_MOVE;
            result= insdel<substitute ? insdel : substitute;
            buf[i]=result;
            if(result<minimum) minimum=result; }
        if(minimum>maxc) return maxc+1;
    }
    return result;
}
/* the str of list d closest to name (None: no good one) */
static Value sg_best(Value d, Value name){
    ListObj *l=AS_LIST(d);
    if(l->len>=SG_MAX_ITEMS) return v_none();
    StrObj *w=AS_STR(name); int64_t best=INT64_MAX, buf[SG_MAX_SIZE+1]; Value sugg=v_none();
    for(int64_t i=0;i<l->len;i++){ Value it=l->items[i];
        if(!IS_STR(it)) continue;
        StrObj *c=AS_STR(it);
        if(c->len==w->len && !memcmp(c->s,w->s,(size_t)w->len)) continue;
        int64_t maxd=(w->len+c->len+3)*SG_MOVE/6;
        if(best-1<maxd) maxd=best-1;
        int64_t dist=sg_distance(w->s,w->len,c->s,c->len,maxd,buf);
        if(dist>maxd) continue;
        if(IS_NONE(sugg) || dist<best){ sugg=it; best=dist; } }
    return sugg;
}
static const char *const stdlib_names[]={
    "__future__","_abc","_aix_support","_android_support","_apple_support","_ast","_ast_unparse","_asyncio","_bisect","_blake2","_bz2",
    "_codecs","_codecs_cn","_codecs_hk","_codecs_iso2022","_codecs_jp","_codecs_kr","_codecs_tw","_collections","_collections_abc",
    "_colorize","_compat_pickle","_contextvars","_csv","_ctypes","_curses","_curses_panel","_datetime","_dbm","_decimal","_elementtree",
    "_frozen_importlib","_frozen_importlib_external","_functools","_gdbm","_hashlib","_heapq","_hmac","_imp","_interpchannels",
    "_interpqueues","_interpreters","_io","_ios_support","_json","_locale","_lsprof","_lzma","_markupbase","_md5","_multibytecodec",
    "_multiprocessing","_opcode","_opcode_metadata","_operator","_osx_support","_overlapped","_pickle","_posixshmem","_posixsubprocess",
    "_py_abc","_py_warnings","_pydatetime","_pydecimal","_pyio","_pylong","_pyrepl","_queue","_random","_remote_debugging","_scproxy",
    "_sha1","_sha2","_sha3","_signal","_sitebuiltins","_socket","_sqlite3","_sre","_ssl","_stat","_statistics","_string","_strptime",
    "_struct","_suggestions","_symtable","_sysconfig","_thread","_threading_local","_tkinter","_tokenize","_tracemalloc","_types","_typing",
    "_uuid","_warnings","_weakref","_weakrefset","_winapi","_wmi","_zoneinfo","_zstd","abc","annotationlib","antigravity","argparse",
    "array","ast","asyncio","atexit","base64","bdb","binascii","bisect","builtins","bz2","cProfile","calendar","cmath","cmd","code",
    "codecs","codeop","collections","colorsys","compileall","compression","concurrent","configparser","contextlib","contextvars","copy",
    "copyreg","csv","ctypes","curses","dataclasses","datetime","dbm","decimal","difflib","dis","doctest","email","encodings","ensurepip",
    "enum","errno","faulthandler","fcntl","filecmp","fileinput","fnmatch","fractions","ftplib","functools","gc","genericpath","getopt",
    "getpass","gettext","glob","graphlib","grp","gzip","hashlib","heapq","hmac","html","http","idlelib","imaplib","importlib","inspect",
    "io","ipaddress","itertools","json","keyword","linecache","locale","logging","lzma","mailbox","marshal","math","mimetypes","mmap",
    "modulefinder","msvcrt","multiprocessing","netrc","nt","ntpath","nturl2path","numbers","opcode","operator","optparse","os","pathlib",
    "pdb","pickle","pickletools","pkgutil","platform","plistlib","poplib","posix","posixpath","pprint","profile","pstats","pty","pwd",
    "py_compile","pyclbr","pydoc","pydoc_data","pyexpat","queue","quopri","random","re","readline","reprlib","resource","rlcompleter",
    "runpy","sched","secrets","select","selectors","shelve","shlex","shutil","signal","site","smtplib","socket","socketserver","sqlite3",
    "sre_compile","sre_constants","sre_parse","ssl","stat","statistics","string","stringprep","struct","subprocess","symtable","sys",
    "sysconfig","syslog","tabnanny","tarfile","tempfile","termios","textwrap","this","threading","time","timeit","tkinter","token",
    "tokenize","tomllib","trace","traceback","tracemalloc","tty","turtle","turtledemo","types","typing","unicodedata","unittest","urllib",
    "uuid","venv","warnings","wave","weakref","webbrowser","winreg","winsound","wsgiref","xml","xmlrpc","zipapp","zipfile","zipimport",
    "zlib","zoneinfo",NULL
};
static int is_stdlib_name(const char *n){ for(int i=0;stdlib_names[i];i++) if(!strcmp(stdlib_names[i],n)) return 1; return 0; }
/* the closest attribute name of obj (dir(obj): underscored ones only for _names, or on self) */
static Value sg_attr(Value obj, Value name, int show_private){
    Value dirf; if(!mp_dict_get_s(mp_builtins,"dir",&dirf)) return v_none();
    Value d=mp_call1(dirf,obj);
    if(!IS_LIST(d)) d=mp_list_of(d);
    int hide= mp_cstr(name)[0]!='_' && !show_private;
    Value l=mp_list(0,NULL); ListObj *x=AS_LIST(d);
    for(int64_t i=0;i<x->len;i++) if(IS_STR(x->items[i]) && !(hide && mp_cstr(x->items[i])[0]=='_')) mp_list_append(l,x->items[i]);
    return sg_best(l,name);
}
/* what CPython adds to the last line: ". Did you mean: 'x'?" (with_frames: NameError's too) */
Value mp_exc_suggestion(Value e, int with_frames){
    SBuf b={0}; Catch c;
    if(!CATCH_BEGIN(c)){
        ExcObj *x=AS_EXC(e); Value name=v_none(), v;
        if(mp_isinstance(e,E_NameError) || mp_isinstance(e,E_AttributeError)){
            if(x->dict && mp_dict_get_s(x->dict,"name",&v)) name=v;
            if(IS_STR(name)){
                Value sg=v_none();
                if(mp_isinstance(e,E_AttributeError)){ Value obj=v_none(); if(x->dict) mp_dict_get_s(x->dict,"obj",&obj); sg=sg_attr(obj,name,x->show_private); }
                else if(with_frames) sg=x->hint;
                if(IS_STR(sg)) sb_printf(&b,". Did you mean: '%s'?",mp_cstr(sg));
                if(mp_isinstance(e,E_NameError) && is_stdlib_name(mp_cstr(name)))
                    sb_printf(&b,"%s you forget to import '%s'?",IS_STR(sg)?" Or did":". Did",mp_cstr(name)); } }
        else if(mp_isinstance(e,E_ImportError) && x->dict && mp_dict_get_s(x->dict,"name_from",&v) && IS_STR(v)){
            Value mn; if(mp_dict_get_s(x->dict,"name",&mn) && IS_STR(mn)){
                Value mod; if(mp_dict_get(mp_modules,mn,&mod)){
                    Value sg=sg_attr(mod,v,0); if(IS_STR(sg)) sb_printf(&b,". Did you mean: '%s'?",mp_cstr(sg)); } } }
        CATCH_END(c);
    } else { mp_catch_exc(&c); b.n=0; }
    return sb_value(&b);
}
/* a NameError (or UnboundLocalError) for name in frame f: its name, and the closest name f sees */
MPY_NORETURN void raise_name(Type *t, Frame *f, const char *name, const char *fmt, ...){
    char buf[1024]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    Value m=mp_str(buf), e=mp_exc_args(t,mp_tuple(1,&m)), nm=mp_str(name);
    exc_set(e,"name",nm);
    if(f){ Catch c;
        if(!CATCH_BEGIN(c)){
            Value loc=mp_frame_locals(f), self, l=mp_list(0,NULL), k; int64_t pos=0;
            if(IS_DICT(loc) && mp_dict_get_s(AS_DICT(loc),"self",&self) && mp_getattr_opt(self,mp_intern(name),&k)){
                char h[300]; snprintf(h,sizeof h,"self.%s",name); AS_EXC(e)->hint=mp_str(h); }
            else {
                if(IS_DICT(loc)) while(mp_dict_next(AS_DICT(loc),&pos,&k,NULL)) mp_list_append(l,k);
                pos=0; while(mp_dict_next(f->globals,&pos,&k,NULL)) mp_list_append(l,k);
                pos=0; while(mp_dict_next(f->builtins?f->builtins:mp_builtins,&pos,&k,NULL)) mp_list_append(l,k);
                AS_EXC(e)->hint=sg_best(l,nm); }
            CATCH_END(c);
        } else mp_catch_exc(&c); }
    mp_raise(e);
}
/* AttributeError "...'name'" with its name and obj (and whether self raised it) */
MPY_NORETURN void mp_raise_attr(Value obj, Value name, const char *fmt, ...){
    char buf[1024]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    Value m=mp_str(buf), e=mp_exc_args(E_AttributeError,mp_tuple(1,&m));
    exc_set(e,"name",name); exc_set(e,"obj",obj);
    Frame *f=mp_ts->frame;
    if(f && f->code->nlocals>0 && f->fast && obj.k==V_OBJ){ Value s0=f->fast[0];
        if(s0.k==V_OBJ && s0.u.o==obj.u.o && !strcmp(f->code->varnames[0]->s,"self")) AS_EXC(e)->show_private=1; }
    mp_raise(e);
}
Value mp_exc(Type *t, const char *fmt, ...){
    char buf[1024]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    Value m=mp_str(buf);
    return mp_exc_args(t,mp_tuple(1,&m));
}
static MPY_NORETURN void do_raise(Value e){
    Thread *ts=mp_ts;
    ts->exc=e;
    Catch *c=ts->catch;
    if(!c){ mp_print_exception(e); mp_flush_stdout(); exit(1); }
    ts->catch=c->prev; ts->frame=c->frame; ts->depth=c->depth;
    MP_LONGJMP(c->jb,1);
}
Value instantiate_exc(Value e){
    if(is_exc_class(e)) e=mp_call0(e);
    if(!mp_isinstance(e,E_BaseException)) e=mp_exc(E_TypeError,"exceptions must derive from BaseException");
    return e;
}
MPY_NORETURN void mp_raise(Value e){
    e=instantiate_exc(e);
    Thread *ts=mp_ts;
    if(ts->nhandled>0){                            /* raised while handling one: its __context__ */
        Value cur=ts->handled[ts->nhandled-1];
        ExcObj *x=AS_EXC(e);
        if(cur.k==V_OBJ && cur.u.o!=e.u.o && IS_NONE(x->context)){
            /* no cycles: cut the chain where it reaches e */
            Value o=cur;
            while(o.k==V_OBJ && mp_isinstance(o,E_BaseException)){ ExcObj *y=AS_EXC(o); if(y->context.k==V_OBJ && y->context.u.o==e.u.o){ y->context=v_none(); break; } o=y->context; }
            x->context=cur;
        }
    }
    do_raise(e);
}
MPY_NORETURN void reraise(Value e){ do_raise(e); }
MPY_NORETURN void mp_raise_t(Type *t, const char *fmt, ...){
    char buf[1024]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    Value m=mp_str(buf);
    mp_raise(mp_exc_args(t,mp_tuple(1,&m)));
}
int mp_exc_matches(Value e, Value spec){
    if(IS_TUPLE(spec)){ TupleObj *t=AS_TUPLE(spec); for(int64_t i=0;i<t->len;i++) if(mp_exc_matches(e,t->items[i])) return 1; return 0; }
    if(!is_exc_class(spec)) mp_raise_t(E_TypeError,"catching classes that do not inherit from BaseException is not allowed");
    return mp_isinstance(e,AS_TYPE(spec));
}
void tb_add(Value e, Frame *f){
    if(!mp_isinstance(e,E_BaseException)) return;
    ExcObj *x=AS_EXC(e);
    if(x->tb_frame==(void*)f) return;
    x->tb_frame=f;
    if(x->tb.k==V_UNDEF) x->tb=mp_list(0,NULL);
    int ip= f->ip>0 ? f->ip-1 : 0;
    Value ent[5]={mp_str(f->code->file),v_int(f->code->ncode?f->code->lines[ip<f->code->ncode?ip:f->code->ncode-1]:f->code->firstline),v_obj(f->code->name),
                  v_obj(f->code),v_int(ip<f->code->ncode?ip:-1)};
    ListObj *l=AS_LIST(x->tb);
    if(l->len){                                         /* a list / set / dict comprehension's entry: CPython runs it in this frame (PEP 709) */
        TupleObj *last=AS_TUPLE(l->items[l->len-1]);
        if(last->len>=5 && IS(last->items[3],T_code)){ CodeObj *cc=(CodeObj*)last->items[3].u.o;
            const char *cn=cc->name->s;
            if((cc->flags&CO_COMP) && (!strcmp(cn,"<listcomp>")||!strcmp(cn,"<setcomp>")||!strcmp(cn,"<dictcomp>")) && !strcmp(cc->file,f->code->file)
               && IS_STR(last->items[2]) && !strcmp(mp_cstr(last->items[2]),cn)){
                ent[1]=last->items[1]; ent[3]=last->items[3]; ent[4]=last->items[4];
                l->items[l->len-1]=mp_tuple(5,ent); return; } } }
    mp_list_append(x->tb,mp_tuple(5,ent));
}

