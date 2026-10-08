/* ========================= Interpreter modules: asyncio, json, minipy, _ctypes =========================
   What the interpreter provides of the modules compiled programs get from the
   compiler (aot_*.c), so one program runs either way:
     asyncio  - coroutines run when called (await x is x): run / sleep / gather / create_task;
     json     - dumps (separators=, ensure_ascii=) and loads;
     minipy   - endpoint(f): f called with its parameters from a dict of strings, the
                result as JSON (what the compiler writes for a minipy.Endpoint);
     _ctypes  - dlopen / dlsym / call / errno, under lib/ctypes.mpy (host builds). */

#include "vm.h"
#include "containers.h"
#include "platform/platform.h"
#include <math.h>
#if !defined(MPY_PLATFORM_KOLIBRI)
#include <dlfcn.h>
#include <errno.h>
#endif

/* ---- floats as Python's repr writes them: the shortest digits that read back */
void mpy_float_repr(char *out, size_t n, double f){
    if(isnan(f)){ snprintf(out,n,"nan"); return; }
    if(isinf(f)){ snprintf(out,n,f<0?"-inf":"inf"); return; }
    if(f==0){ snprintf(out,n,signbit(f)?"-0.0":"0.0"); return; }
    char buf[64]; int p;
    for(p=0;p<17;p++){ snprintf(buf,sizeof buf,"%.*e",p,f); if(strtod(buf,NULL)==f) break; }
    if(p==17) snprintf(buf,sizeof buf,"%.16e",f);
    char digits[32]; int nd=0, neg=0; const char *q=buf;
    if(*q=='-'){ neg=1; q++; }
    while(*q && *q!='e'){ if(*q>='0'&&*q<='9') digits[nd++]=*q; q++; }
    int e=atoi(q+1);
    while(nd>1 && digits[nd-1]=='0') nd--;
    char tmp[96]; int k=0;
    if(neg) tmp[k++]='-';
    if(e<-4 || e>=16){                                     /* 1e+16, 1.5e-05 */
        tmp[k++]=digits[0];
        if(nd>1){ tmp[k++]='.'; for(int i=1;i<nd;i++) tmp[k++]=digits[i]; }
        k+=snprintf(tmp+k,sizeof tmp-(size_t)k,"e%c%02d",e<0?'-':'+',e<0?-e:e);
    } else if(e>=0){
        for(int i=0;i<=e;i++) tmp[k++]=i<nd?digits[i]:'0';
        tmp[k++]='.';
        if(nd>e+1) for(int i=e+1;i<nd;i++) tmp[k++]=digits[i]; else tmp[k++]='0';
    } else {
        tmp[k++]='0'; tmp[k++]='.';
        for(int i=0;i<-e-1;i++) tmp[k++]='0';
        for(int i=0;i<nd;i++) tmp[k++]=digits[i];
    }
    tmp[k]=0;
    snprintf(out,n,"%s",tmp);
}

/* ---- asyncio: coroutines have already run when they are awaited */
static Value native_asyncio_run(int argc, Value *argv){ if(argc!=1) runtime_error("asyncio.run() takes one coroutine"); return argv[0]; }
static Value native_asyncio_task(int argc, Value *argv){ if(argc!=1) runtime_error("asyncio.create_task() takes one coroutine"); return argv[0]; }
static Value native_asyncio_gather(int argc, Value *argv){ Obj *o=new_list(); for(int i=0;i<argc;i++) list_push(&o->as.list,argv[i]); return objv(o); }
static Value native_asyncio_sleep(int argc, Value *argv){
    if(argc<1) runtime_error("asyncio.sleep() takes the seconds");
    double s=argv[0].type==V_FLOAT?argv[0].as.f:(double)as_int(argv[0]);
    if(s>0) mpy_thread_sleep_ms((int)(s*1000+0.5));
    return argc>1?argv[1]:nonev();
}
Native N_ASYNCIO_RUN={"run",1,native_asyncio_run}, N_ASYNCIO_TASK={"create_task",1,native_asyncio_task},
       N_ASYNCIO_GATHER={"gather",-1,native_asyncio_gather}, N_ASYNCIO_SLEEP={"sleep",-1,native_asyncio_sleep};

static Value native_sys_exit(int argc, Value *argv){
    int code=0;
    if(argc>0 && argv[0].type==V_INT) code=(int)argv[0].as.i;
    else if(argc>0 && argv[0].type!=V_NONE){ print_value(builtin_str(argv[0])); printf("\n"); code=1; }
    fflush(stdout); exit(code);
    return nonev();
}
Native N_SYS_EXIT={"exit",-1,native_sys_exit};

/* ---- json */
typedef struct { char *s; size_t n, cap; } JBuf;
static void jb_add(JBuf *b, const char *s, size_t n){
    if(b->n+n+1>b->cap){ b->cap=(b->n+n+1)*2+64; b->s=(char*)xrealloc(b->s,b->cap); }
    memcpy(b->s+b->n,s,n); b->n+=n; b->s[b->n]=0;
}
static void jb_cstr(JBuf *b, const char *s){ jb_add(b,s,strlen(s)); }
static void jb_u4(JBuf *b, unsigned v){ char t[8]; snprintf(t,sizeof t,"\\u%04x",v&0xFFFF); jb_cstr(b,t); }
static void jb_string(JBuf *b, const char *s, size_t len, int ascii){
    jb_add(b,"\"",1);
    for(size_t i=0;i<len;i++){
        unsigned char ch=(unsigned char)s[i];
        if(ch=='"'){ jb_cstr(b,"\\\""); continue; }
        if(ch=='\\'){ jb_cstr(b,"\\\\"); continue; }
        if(ch<0x20){
            const char *esc=ch=='\n'?"\\n":ch=='\r'?"\\r":ch=='\t'?"\\t":ch=='\b'?"\\b":ch=='\f'?"\\f":NULL;
            if(esc) jb_cstr(b,esc); else jb_u4(b,ch);
            continue;
        }
        if(ch<0x80 || !ascii){ jb_add(b,(const char*)&ch,1); continue; }
        unsigned cp; int more;                                  /* UTF-8 -> \uXXXX (surrogate pairs) */
        if(ch>=0xF0){ cp=ch&7; more=3; } else if(ch>=0xE0){ cp=ch&15; more=2; } else { cp=ch&31; more=1; }
        while(more-- && i+1<len){ i++; cp=(cp<<6)|((unsigned char)s[i]&63); }
        if(cp>=0x10000){ cp-=0x10000; jb_u4(b,0xD800+(cp>>10)); jb_u4(b,0xDC00+(cp&0x3FF)); }
        else jb_u4(b,cp);
    }
    jb_add(b,"\"",1);
}
static void json_write(JBuf *b, Value v, const char *isep, const char *ksep, int ascii, int depth){
    if(depth>200) raise_named("ValueError","Circular reference detected");
    if(v.type==V_NONE){ jb_cstr(b,"null"); return; }
    if(v.type==V_BOOL){ jb_cstr(b,v.as.boolean?"true":"false"); return; }
    if(v.type==V_INT){ char t[32]; snprintf(t,sizeof t,"%lld",(long long)v.as.i); jb_cstr(b,t); return; }
    if(v.type==V_FLOAT){
        double f=v.as.f; char t[64];
        if(isnan(f)) snprintf(t,sizeof t,"NaN"); else if(isinf(f)) snprintf(t,sizeof t,f<0?"-Infinity":"Infinity"); else mpy_float_repr(t,sizeof t,f);
        jb_cstr(b,t); return; }
    if(is_obj(v,O_STRING)){ jb_string(b,v.as.obj->as.str.s,(size_t)v.as.obj->as.str.len,ascii); return; }
    if(is_obj(v,O_LIST)||is_obj(v,O_TUPLE)){
        List *l=is_obj(v,O_LIST)?&v.as.obj->as.list:&v.as.obj->as.tuple;
        jb_cstr(b,"[");
        for(int i=0;i<l->count;i++){ if(i) jb_cstr(b,isep); json_write(b,l->items[i],isep,ksep,ascii,depth+1); }
        jb_cstr(b,"]"); return; }
    if(is_obj(v,O_DICT)||is_obj(v,O_INSTANCE)){                /* an object: its fields (as compiled code) */
        Dict *d=is_obj(v,O_DICT)?&v.as.obj->as.dict:v.as.obj->as.inst.fields;
        int first=1;
        jb_cstr(b,"{");
        for(int i=0;i<d->count;i++){
            if(is_obj(v,O_INSTANCE) && d->keys[i][0]=='_') continue;
            if(!first) jb_cstr(b,isep);
            first=0;
            const char *k=d->keys[i];
            if(!strcmp(k,"True")) k="true"; else if(!strcmp(k,"False")) k="false";
            jb_string(b,k,strlen(k),ascii); jb_cstr(b,ksep);
            json_write(b,d->vals[i],isep,ksep,ascii,depth+1);
        }
        jb_cstr(b,"}"); return; }
    char msg[160]; snprintf(msg,sizeof msg,"Object of type %s is not JSON serializable",value_type_name(v));
    raise_named("TypeError",msg);
}
Value mpy_json_dumps(Value v, const char *isep, const char *ksep, int ascii){
    JBuf b={0};
    json_write(&b,v,isep,ksep,ascii,0);
    Value r=stringv_len(b.s?b.s:"",(int)b.n); free(b.s);
    return r;
}
static Value native_json_dumps(int argc, Value *argv){
    if(argc!=1) runtime_error("json.dumps() takes the value (and keyword arguments)");
    return mpy_json_dumps(argv[0],", ",": ",1);
}
/* json.dumps(obj, separators=(item, key), ensure_ascii=, indent=None, sort_keys=False) */
static int json_dumps_kw(List *pos, Dict *kw, Value *out){
    if(pos->count!=1) return 0;
    const char *isep=", ", *ksep=": "; int ascii=1;
    for(int i=0;i<kw->count;i++){ const char *k=kw->keys[i]; Value v=kw->vals[i];
        if(!strcmp(k,"separators")){
            if(!is_obj(v,O_TUPLE)||v.as.obj->as.tuple.count!=2||!is_obj(v.as.obj->as.tuple.items[0],O_STRING)||!is_obj(v.as.obj->as.tuple.items[1],O_STRING)) runtime_error("json.dumps(): separators must be a tuple of two strings");
            isep=v.as.obj->as.tuple.items[0].as.obj->as.str.s; ksep=v.as.obj->as.tuple.items[1].as.obj->as.str.s;
        } else if(!strcmp(k,"ensure_ascii")) ascii=truthy(v);
        else if((!strcmp(k,"indent") && v.type==V_NONE) || (!strcmp(k,"sort_keys") && !truthy(v))) continue;
        else { char m[96]; snprintf(m,sizeof m,"json.dumps(): '%s' is not supported",k); runtime_error(m); }
    }
    *out=mpy_json_dumps(pos->items[0],isep,ksep,ascii);
    return 1;
}
/* json.loads */
typedef struct { const char *s; int i, n; } JIn;
MPY_NORETURN static void json_fail(JIn *in, const char *what){
    char m[160]; snprintf(m,sizeof m,"%s: line 1 column %d (char %d)",what,in->i+1,in->i); raise_named("ValueError",m);
}
static void json_ws(JIn *in){ while(in->i<in->n && (in->s[in->i]==' '||in->s[in->i]=='\t'||in->s[in->i]=='\n'||in->s[in->i]=='\r')) in->i++; }
static int json_hex4(JIn *in){
    if(in->i+4>in->n) json_fail(in,"Invalid \\uXXXX escape");
    int v=0; for(int k=0;k<4;k++){ char c=in->s[in->i++]; v*=16;
        if(c>='0'&&c<='9') v+=c-'0'; else if(c>='a'&&c<='f') v+=c-'a'+10; else if(c>='A'&&c<='F') v+=c-'A'+10; else json_fail(in,"Invalid \\uXXXX escape"); }
    return v;
}
static void jb_utf8(JBuf *b, unsigned cp){
    char t[4]; int n;
    if(cp<0x80){ t[0]=(char)cp; n=1; }
    else if(cp<0x800){ t[0]=(char)(0xC0|(cp>>6)); t[1]=(char)(0x80|(cp&63)); n=2; }
    else if(cp<0x10000){ t[0]=(char)(0xE0|(cp>>12)); t[1]=(char)(0x80|((cp>>6)&63)); t[2]=(char)(0x80|(cp&63)); n=3; }
    else { t[0]=(char)(0xF0|(cp>>18)); t[1]=(char)(0x80|((cp>>12)&63)); t[2]=(char)(0x80|((cp>>6)&63)); t[3]=(char)(0x80|(cp&63)); n=4; }
    jb_add(b,t,(size_t)n);
}
static Value json_value(JIn *in, int depth);
static Value json_string(JIn *in){
    JBuf b={0}; in->i++;
    for(;;){
        if(in->i>=in->n) json_fail(in,"Unterminated string starting at");
        char c=in->s[in->i++];
        if(c=='"') break;
        if(c!='\\'){ jb_add(&b,&c,1); continue; }
        if(in->i>=in->n) json_fail(in,"Unterminated string starting at");
        c=in->s[in->i++];
        switch(c){
            case '"': case '\\': case '/': jb_add(&b,&c,1); break;
            case 'n': jb_cstr(&b,"\n"); break; case 'r': jb_cstr(&b,"\r"); break; case 't': jb_cstr(&b,"\t"); break;
            case 'b': jb_cstr(&b,"\b"); break; case 'f': jb_cstr(&b,"\f"); break;
            case 'u':{ unsigned cp=(unsigned)json_hex4(in);
                if(cp>=0xD800 && cp<0xDC00 && in->i+6<=in->n && in->s[in->i]=='\\' && in->s[in->i+1]=='u'){
                    in->i+=2; unsigned lo=(unsigned)json_hex4(in); cp=0x10000+((cp-0xD800)<<10)+(lo-0xDC00); }
                jb_utf8(&b,cp); break; }
            default: json_fail(in,"Invalid \\escape");
        }
    }
    Value r=stringv_len(b.s?b.s:"",(int)b.n); free(b.s);
    return r;
}
static Value json_value(JIn *in, int depth){
    if(depth>500) json_fail(in,"Too deeply nested");
    json_ws(in);
    if(in->i>=in->n) json_fail(in,"Expecting value");
    char c=in->s[in->i];
    if(c=='"') return json_string(in);
    if(c=='{'){
        in->i++; Obj *o=new_dict_obj(); json_ws(in);
        if(in->i<in->n && in->s[in->i]=='}'){ in->i++; return objv(o); }
        for(;;){
            json_ws(in);
            if(in->i>=in->n || in->s[in->i]!='"') json_fail(in,"Expecting property name enclosed in double quotes");
            Value k=json_string(in); json_ws(in);
            if(in->i>=in->n || in->s[in->i]!=':') json_fail(in,"Expecting ':' delimiter");
            in->i++;
            Value v=json_value(in,depth+1);
            dict_set(&o->as.dict,k.as.obj->as.str.s,v);
            json_ws(in);
            if(in->i<in->n && in->s[in->i]==','){ in->i++; continue; }
            if(in->i<in->n && in->s[in->i]=='}'){ in->i++; return objv(o); }
            json_fail(in,"Expecting ',' delimiter");
        }
    }
    if(c=='['){
        in->i++; Obj *o=new_list(); json_ws(in);
        if(in->i<in->n && in->s[in->i]==']'){ in->i++; return objv(o); }
        for(;;){
            list_push(&o->as.list,json_value(in,depth+1));
            json_ws(in);
            if(in->i<in->n && in->s[in->i]==','){ in->i++; continue; }
            if(in->i<in->n && in->s[in->i]==']'){ in->i++; return objv(o); }
            json_fail(in,"Expecting ',' delimiter");
        }
    }
    static const struct { const char *w; int k; } words[]={{"true",1},{"false",2},{"null",3},{"NaN",4},{"Infinity",5},{"-Infinity",6},{NULL,0}};
    for(int w=0;words[w].w;w++){ int L=(int)strlen(words[w].w);
        if(in->i+L<=in->n && !strncmp(in->s+in->i,words[w].w,(size_t)L)){ in->i+=L;
            switch(words[w].k){ case 1: return boolv(1); case 2: return boolv(0); case 3: return nonev();
                case 4: return floatv(NAN); case 5: return floatv(INFINITY); default: return floatv(-INFINITY); } } }
    if(c=='-'||(c>='0'&&c<='9')){
        int st=in->i, isf=0;
        if(in->s[in->i]=='-') in->i++;
        while(in->i<in->n && in->s[in->i]>='0' && in->s[in->i]<='9') in->i++;
        if(in->i<in->n && in->s[in->i]=='.'){ isf=1; in->i++; while(in->i<in->n && in->s[in->i]>='0' && in->s[in->i]<='9') in->i++; }
        if(in->i<in->n && (in->s[in->i]=='e'||in->s[in->i]=='E')){ isf=1; in->i++; if(in->i<in->n && (in->s[in->i]=='+'||in->s[in->i]=='-')) in->i++; while(in->i<in->n && in->s[in->i]>='0' && in->s[in->i]<='9') in->i++; }
        char t[64]; int L=in->i-st; if(L>63) L=63; memcpy(t,in->s+st,(size_t)L); t[L]=0;
        if(isf) return floatv(strtod(t,NULL));
        return intv((int64_t)strtoll(t,NULL,10));
    }
    json_fail(in,"Expecting value");
}
static Value native_json_loads(int argc, Value *argv){
    if(argc!=1 || !is_obj(argv[0],O_STRING)) runtime_error("json.loads() takes a str");
    JIn in={argv[0].as.obj->as.str.s,0,argv[0].as.obj->as.str.len};
    Value v=json_value(&in,0);
    json_ws(&in);
    if(in.i<in.n) json_fail(&in,"Extra data");
    return v;
}
Native N_JSON_DUMPS={"dumps",-1,native_json_dumps}, N_JSON_LOADS={"loads",1,native_json_loads};

/* ---- minipy.endpoint(f): f with its parameters from a dict of strings, its result as
   JSON. Errors: ValueError("#endpoint\nmissing|parsing\n<name>\n<type>\n<text>"), as
   the adapters the compiler writes raise them. */
static Value native_minipy_endpoint(int argc, Value *argv){
    if(argc!=1) runtime_error("minipy.endpoint() takes a function");
    if(is_obj(argv[0],O_BOUND_NATIVE) && !strcmp(argv[0].as.obj->as.bn.name,"__endpoint__")) return argv[0];
    if(!is_obj(argv[0],O_FUNCTION)) raise_named("TypeError","minipy.endpoint() takes a function");
    Obj *b=new_obj(O_BOUND_NATIVE); b->as.bn.receiver=argv[0]; b->as.bn.name=xstrdup2("__endpoint__");
    return objv(b);
}
Native N_MINIPY_ENDPOINT={"endpoint",1,native_minipy_endpoint};

MPY_NORETURN static void endpoint_error(const char *kind, const char *name, const char *type, const char *text){
    size_t n=strlen(name)+strlen(type)+strlen(text)+40; char *m=(char*)xmalloc(n);
    snprintf(m,n,"#endpoint\n%s\n%s\n%s\n%s",kind,name,type,text);
    raise_named("ValueError",m);
}
static int parse_int_text(const char *s, int64_t *out){
    while(*s==' '||*s=='\t') s++;
    const char *p=s; if(*p=='+'||*p=='-') p++;
    if(!(*p>='0'&&*p<='9')) return 0;
    char *end; long long v=strtoll(s,&end,10);
    while(*end==' '||*end=='\t') end++;
    if(*end) return 0;
    *out=v; return 1;
}
static int parse_float_text(const char *s, double *out){
    char *end; double v=strtod(s,&end);
    if(end==s) return 0;
    while(*end==' '||*end=='\t') end++;
    if(*end) return 0;
    *out=v; return 1;
}
Value mpy_endpoint_call(Value fnv, int argc, Value *argv){
    if(argc!=1 || !is_obj(argv[0],O_DICT)) runtime_error("an endpoint takes a dict of strings");
    Function *fn=&fnv.as.obj->as.fn; Dict *values=&argv[0].as.obj->as.dict;
    int nreg=fn->arity-(fn->star_index>=0)-(fn->dstar_index>=0);
    Value *args=MPY_NEW_ARR(Value,nreg>0?nreg:1);
    for(int i=0;i<nreg;i++){
        const char *name=fn->params[i], *type=fn->annots && fn->annots[i] ? fn->annots[i] : "str";
        Value v;
        if(!dict_get(values,name,&v)){
            int first_default=nreg-fn->default_count;
            if(i>=first_default){ args[i]=fn->defaults[i-first_default]; continue; }
            endpoint_error("missing",name,type,"");
        }
        const char *text=is_obj(v,O_STRING)?v.as.obj->as.str.s:"";
        if(!strcmp(type,"int")){ int64_t x; if(!parse_int_text(text,&x)) endpoint_error("parsing",name,type,text); args[i]=intv(x); }
        else if(!strcmp(type,"float")){ double x; if(!parse_float_text(text,&x)) endpoint_error("parsing",name,type,text); args[i]=floatv(x); }
        else if(!strcmp(type,"bool")){
            char low[16]; size_t L=strlen(text); if(L>15) L=15;
            for(size_t k=0;k<L;k++) low[k]=(char)tolower((unsigned char)text[k]); low[L]=0;
            static const char *yes[]={"1","true","t","yes","y","on",NULL}, *no[]={"0","false","f","no","n","off",NULL};
            int r=-1; for(int k=0;yes[k];k++) if(!strcmp(low,yes[k])) r=1; for(int k=0;no[k];k++) if(!strcmp(low,no[k])) r=0;
            if(r<0) endpoint_error("parsing",name,type,text);
            args[i]=boolv(r);
        } else args[i]=v;
    }
    Value result=call_value(fnv,nreg,args);
    free(args);
    return mpy_json_dumps(result,",",":",0);
}

/* ---- _ctypes: C functions of shared libraries (lib/ctypes.mpy builds ctypes on it) */
#if !defined(MPY_PLATFORM_KOLIBRI)
static int mpy_last_errno;
static Value native_ct_dlopen(int argc, Value *argv){
    const char *name=argc>0 && is_obj(argv[0],O_STRING) ? argv[0].as.obj->as.str.s : NULL;
    void *h=dlopen(name,RTLD_NOW|RTLD_GLOBAL);
#if defined(__APPLE__)
    if(!h && name && !strncmp(name,"libc.so",7)) h=dlopen("libSystem.B.dylib",RTLD_NOW|RTLD_GLOBAL);   /* the C library */
    if(!h && name && !strncmp(name,"libm.so",7)) h=dlopen("libSystem.B.dylib",RTLD_NOW|RTLD_GLOBAL);
#endif
    if(!h){ const char *e=dlerror(); raise_named("OSError",e?e:"cannot load the library"); }
    return intv((int64_t)(intptr_t)h);
}
static Value native_ct_dlsym(int argc, Value *argv){
    if(argc!=2 || !is_obj(argv[1],O_STRING)) runtime_error("dlsym(handle, name)");
    void *p=dlsym((void*)(intptr_t)as_int(argv[0]),argv[1].as.obj->as.str.s);
    if(!p){ char m[200]; snprintf(m,sizeof m,"function '%s' not found",argv[1].as.obj->as.str.s); raise_named("AttributeError",m); }
    return intv((int64_t)(intptr_t)p);
}
typedef intptr_t (*cfn_i)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t);
typedef intptr_t (*cfn_v1)(intptr_t,...); typedef intptr_t (*cfn_v2)(intptr_t,intptr_t,...);
typedef intptr_t (*cfn_v3)(intptr_t,intptr_t,intptr_t,...); typedef intptr_t (*cfn_v4)(intptr_t,intptr_t,intptr_t,intptr_t,...);
/* One function per call shape: in one function an optimizer may merge the
   indirect calls and lose the variadic convention (Apple arm64 passes the
   variadic arguments on the stack). */
#if defined(__GNUC__) || defined(__clang__)
#define CT_NOINLINE __attribute__((noinline))
#else
#define CT_NOINLINE
#endif
static CT_NOINLINE intptr_t ct_call_n(void *f, intptr_t *a){ return ((cfn_i)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v1(void *f, intptr_t *a){ return ((cfn_v1)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v2(void *f, intptr_t *a){ return ((cfn_v2)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v3(void *f, intptr_t *a){ return ((cfn_v3)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v4(void *f, intptr_t *a){ return ((cfn_v4)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
typedef double (*cfn_d0)(void); typedef double (*cfn_d1)(double); typedef double (*cfn_d2)(double,double); typedef double (*cfn_d3)(double,double,double);
/* call(address, restype kind, fixed, arg...): ints, str, buffers and None as
   integer arguments (up to 8); a double result takes up to 3 double arguments.
   fixed >= 0: a variadic function with that many fixed arguments (1 to 4). */
static Value native_ct_call(int argc, Value *argv){
    if(argc<3 || !is_obj(argv[1],O_STRING)) runtime_error("call(address, restype, fixed, *args)");
    void *fp=(void*)(intptr_t)as_int(argv[0]); const char *rk=argv[1].as.obj->as.str.s;
    int fixed=(int)as_int(argv[2]);
    argv++; argc--;
    int n=argc-2, floats=0;
    for(int i=0;i<n;i++) if(argv[2+i].type==V_FLOAT) floats++;
    errno=0;
    if(!strcmp(rk,"f64")||!strcmp(rk,"f32")){
        if(floats!=n || n>3) runtime_error("the interpreter calls a C function returning a double with 0 to 3 double arguments");
        double a[3]={0,0,0}; for(int i=0;i<n;i++) a[i]=argv[2+i].as.f;
        double r=n==0?((cfn_d0)fp)():n==1?((cfn_d1)fp)(a[0]):n==2?((cfn_d2)fp)(a[0],a[1]):((cfn_d3)fp)(a[0],a[1],a[2]);
        mpy_last_errno=errno; return floatv(r);
    }
    if(floats || n>8) runtime_error("the interpreter calls C functions with up to 8 integer, pointer or str arguments");
    intptr_t a[8]={0};
    for(int i=0;i<n;i++){ Value v=argv[2+i];
        if(v.type==V_INT) a[i]=(intptr_t)v.as.i;
        else if(v.type==V_BOOL) a[i]=v.as.boolean;
        else if(v.type==V_NONE) a[i]=0;
        else if(is_obj(v,O_STRING)) a[i]=(intptr_t)v.as.obj->as.str.s;
        else if(is_obj(v,O_BUFFER)) a[i]=(intptr_t)v.as.obj->as.buf.data;
        else raise_named("TypeError","a C function takes int, str, bytes, buffers or None");
    }
    if(fixed>=0 && fixed<n && (fixed<1 || fixed>4)) runtime_error("a variadic C function takes 1 to 4 fixed arguments in the interpreter");
    intptr_t r= fixed<0 || fixed>=n ? ct_call_n(fp,a) : fixed==1 ? ct_call_v1(fp,a) : fixed==2 ? ct_call_v2(fp,a) : fixed==3 ? ct_call_v3(fp,a) : ct_call_v4(fp,a);
    mpy_last_errno=errno;
    if(!strcmp(rk,"void")) return nonev();
    if(!strcmp(rk,"i32")) return intv((int32_t)r);
    if(!strcmp(rk,"u32")) return intv((uint32_t)r);
    if(!strcmp(rk,"i16")) return intv((int16_t)r);
    if(!strcmp(rk,"u16")) return intv((uint16_t)r);
    if(!strcmp(rk,"i8")) return intv((int8_t)r);
    if(!strcmp(rk,"u8")) return intv((uint8_t)r);
    if(!strcmp(rk,"bool")) return boolv((r&0xFF)!=0);
    if(!strcmp(rk,"str")) return r ? stringv((const char*)r) : nonev();
    if(!strcmp(rk,"up")) return intv((int64_t)(uintptr_t)r);
    return intv((int64_t)r);                                       /* ip, i64, ptr */
}
static Value native_ct_errno(int argc, Value *argv){ (void)argc; (void)argv; return intv(mpy_last_errno); }
#else
static Value ct_unsupported(int argc, Value *argv){ (void)argc; (void)argv; runtime_error("ctypes is not available on this platform"); return nonev(); }
#define native_ct_dlopen ct_unsupported
#define native_ct_dlsym ct_unsupported
#define native_ct_call ct_unsupported
#define native_ct_errno ct_unsupported
#endif
Native N_CT_DLOPEN={"dlopen",-1,native_ct_dlopen}, N_CT_DLSYM={"dlsym",2,native_ct_dlsym},
       N_CT_CALL={"call",-1,native_ct_call}, N_CT_ERRNO={"errno",0,native_ct_errno};

/* keyword arguments of these built-ins (native_call_kw) */
int stdlib_call_kw(Native *n, List *pos, Dict *kw, Value *out){
    if(n==&N_JSON_DUMPS) return json_dumps_kw(pos,kw,out);
    return 0;
}

static Dict *module(const char *name){
    Dict *d=dict_new(); dict_set(d,"__name__",stringv(name));
    dict_set(vm.modules,name,objv(new_module(name,d)));
    return d;
}
void mpy_stdlib_register(Dict *sysd){
    dict_set(sysd,"exit",nativev(&N_SYS_EXIT));
    Dict *a=module("asyncio");
    dict_set(a,"run",nativev(&N_ASYNCIO_RUN)); dict_set(a,"create_task",nativev(&N_ASYNCIO_TASK)); dict_set(a,"ensure_future",nativev(&N_ASYNCIO_TASK));
    dict_set(a,"gather",nativev(&N_ASYNCIO_GATHER)); dict_set(a,"sleep",nativev(&N_ASYNCIO_SLEEP));
    Dict *j=module("json");
    dict_set(j,"dumps",nativev(&N_JSON_DUMPS)); dict_set(j,"loads",nativev(&N_JSON_LOADS));
    Dict *m=module("minipy");
    dict_set(m,"endpoint",nativev(&N_MINIPY_ENDPOINT)); dict_set(m,"Endpoint",stringv("minipy.Endpoint"));
    Dict *c=module("_ctypes");
    dict_set(c,"dlopen",nativev(&N_CT_DLOPEN)); dict_set(c,"dlsym",nativev(&N_CT_DLSYM)); dict_set(c,"call",nativev(&N_CT_CALL)); dict_set(c,"errno",nativev(&N_CT_ERRNO));
}
