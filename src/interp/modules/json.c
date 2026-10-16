/* ========================= Interpreter, modules: json =========================
   json.dumps / json.loads in C. */

#include "modules.h"

typedef struct { const char *isep, *ksep; int ascii, sort; Value indent; int depth; } JOpt;
static void j_string(SBuf *b, const char *s, int64_t len, int ascii){
    sb_putc(b,'"');
    int64_t i=0;
    while(i<len){
        unsigned char ch=(unsigned char)s[i];
        if(ch=='"'){ sb_puts(b,"\\\""); i++; continue; }
        if(ch=='\\'){ sb_puts(b,"\\\\"); i++; continue; }
        if(ch<0x20){ const char *e= ch=='\n'?"\\n":ch=='\r'?"\\r":ch=='\t'?"\\t":ch=='\b'?"\\b":ch=='\f'?"\\f":NULL; if(e) sb_puts(b,e); else sb_printf(b,"\\u%04x",ch); i++; continue; }
        if(ch<0x7f || !ascii){ sb_putc(b,(char)ch); i++; continue; }      /* (ensure_ascii: DEL too, as \u007f) */
        int64_t p=i; uint32_t cp=(uint32_t)mp_utf8_decode(s,len,&p); i=p;
        if(cp>=0x10000){ cp-=0x10000; sb_printf(b,"\\u%04x\\u%04x",0xD800+(cp>>10),0xDC00+(cp&0x3FF)); }
        else sb_printf(b,"\\u%04x",cp);
    }
    sb_putc(b,'"');
}
static void j_newline(SBuf *b, JOpt *o, int depth){
    if(IS_NONE(o->indent)) return;
    sb_putc(b,'\n');
    for(int i=0;i<depth;i++){ if(IS_STR(o->indent)) sb_puts(b,mp_cstr(o->indent)); else for(int64_t k=0;k<o->indent.u.i;k++) sb_putc(b,' '); }
}
static void j_key(SBuf *b, Value k, JOpt *o){
    k=mp_unbox(k);
    if(IS_STR(k)){ j_string(b,AS_STR(k)->s,AS_STR(k)->len,o->ascii); return; }
    if(k.k==V_BOOL){ sb_puts(b,k.u.i?"\"true\"":"\"false\""); return; }
    if(k.k==V_NONE){ sb_puts(b,"\"null\""); return; }
    if(k.k==V_INT){ sb_printf(b,"\"%lld\"",(long long)k.u.i); return; }
    if(k.k==V_FLOAT){ char t[64]; mp_float_repr(t,sizeof t,k.u.f); sb_printf(b,"\"%s\"",t); return; }
    mp_raise_t(E_TypeError,"keys must be str, int, float, bool or None, not %s",mp_type_name(k));
}
static void j_write(SBuf *b, Value v, JOpt *o, int depth){
    if(depth>400) mp_raise_t(E_ValueError,"Circular reference detected");
    v=mp_unbox(v);                                         /* (an int/float subclass: its value) */
    switch(v.k){
        case V_NONE: sb_puts(b,"null"); return;
        case V_BOOL: sb_puts(b,v.u.i?"true":"false"); return;
        case V_INT: sb_printf(b,"%lld",(long long)v.u.i); return;
        case V_FLOAT:{ double f=v.u.f; char t[64]; if(isnan(f)) strcpy(t,"NaN"); else if(isinf(f)) strcpy(t,f<0?"-Infinity":"Infinity"); else mp_float_repr(t,sizeof t,f); sb_puts(b,t); return; }
        default: break;
    }
    if(IS_STR(v)){ j_string(b,AS_STR(v)->s,AS_STR(v)->len,o->ascii); return; }
    if(IS_LIST(v)||IS_TUPLE(v)){
        int64_t n= IS_LIST(v)?AS_LIST(v)->len:AS_TUPLE(v)->len;
        sb_putc(b,'[');
        if(n){
            for(int64_t i=0;i<n;i++){ if(i) sb_puts(b,o->isep); j_newline(b,o,depth+1); j_write(b,IS_LIST(v)?AS_LIST(v)->items[i]:AS_TUPLE(v)->items[i],o,depth+1); }
            j_newline(b,o,depth);
        }
        sb_putc(b,']'); return;
    }
    DictObj *d=NULL; int inst=0;
    if(v.k==V_OBJ && v.u.o->type->layout==LY_DICT) d=AS_DICT(v);
    else if(v.k==V_OBJ && v.u.o->type->layout==LY_INSTANCE && !(mp_type_lookup_s(TYPE(v),"__json__").k!=V_UNDEF)){ d=AS_INST(v)->dict; inst=1; }   /* an object: its fields (as compiled programs) */
    if(d){
        Value keys=mp_list(0,NULL); int64_t pos=0; Value k, val;
        while(mp_dict_next(d,&pos,&k,&val)){ if(inst && IS_STR(k) && mp_cstr(k)[0]=='_') continue; mp_list_append(keys,k); }
        if(o->sort) mp_sort(keys,v_none(),0);
        sb_putc(b,'{');
        ListObj *kl=AS_LIST(keys);
        for(int64_t i=0;i<kl->len;i++){
            if(i) sb_puts(b,o->isep);
            j_newline(b,o,depth+1);
            j_key(b,kl->items[i],o); sb_puts(b,o->ksep);
            Value x; mp_dict_get(d,kl->items[i],&x);
            j_write(b,x,o,depth+1);
        }
        if(kl->len) j_newline(b,o,depth);
        sb_putc(b,'}'); return;
    }
    mp_raise_t(E_TypeError,"Object of type %s is not JSON serializable",mp_type_name(v));
}
static Value js_dumps(int argc, Value *argv, TupleObj *kw){
    int np=mod_npos(argc,kw);
    if(np!=1) mp_raise_t(E_TypeError,"dumps() takes 1 positional argument but %d were given",np);
    JOpt o; o.indent=mod_kwarg(argc,argv,kw,"indent",v_none()); o.ascii=mp_truth(mod_kwarg(argc,argv,kw,"ensure_ascii",v_bool(1)));
    o.sort=mp_truth(mod_kwarg(argc,argv,kw,"sort_keys",v_bool(0)));
    Value seps=mod_kwarg(argc,argv,kw,"separators",v_none());
    o.isep= IS_NONE(o.indent) ? ", " : ","; o.ksep=": ";
    if(IS_INTLIKE(o.indent) && o.indent.u.i<0) o.indent=v_int(0);
    if(!IS_NONE(seps)){
        if(!IS_TUPLE(seps) || AS_TUPLE(seps)->len!=2 || !IS_STR(AS_TUPLE(seps)->items[0]) || !IS_STR(AS_TUPLE(seps)->items[1])) mp_raise_t(E_TypeError,"separators must be a tuple of two strings");
        o.isep=mp_cstr(AS_TUPLE(seps)->items[0]); o.ksep=mp_cstr(AS_TUPLE(seps)->items[1]);
    }
    SBuf b={0};
    Catch c;
    if(!CATCH_BEGIN(c)){ j_write(&b,argv[0],&o,0); CATCH_END(c); }
    else { free(b.s); mp_raise(mp_catch_exc(&c)); }
    return sb_value(&b);
}
typedef struct { const char *s; int64_t i, n; } JIn;
static PPY_NORETURN void j_fail(JIn *in, const char *what){
    int64_t line=1, col=1; for(int64_t k=0;k<in->i && k<in->n;k++){ if(in->s[k]=='\n'){ line++; col=1; } else col++; }
    mp_raise_t(E_ValueError,"%s: line %lld column %lld (char %lld)",what,(long long)line,(long long)col,(long long)in->i);
}
static void j_ws(JIn *in){ while(in->i<in->n && (in->s[in->i]==' '||in->s[in->i]=='\t'||in->s[in->i]=='\n'||in->s[in->i]=='\r')) in->i++; }
static int j_hex4(JIn *in){
    if(in->i+4>in->n) j_fail(in,"Invalid \\uXXXX escape");
    int v=0; for(int k=0;k<4;k++){ char c=in->s[in->i++]; v*=16; if(c>='0'&&c<='9') v+=c-'0'; else if(c>='a'&&c<='f') v+=c-'a'+10; else if(c>='A'&&c<='F') v+=c-'A'+10; else j_fail(in,"Invalid \\uXXXX escape"); }
    return v;
}
static Value j_value(JIn *in, int depth);
static Value j_str(JIn *in){
    SBuf b={0}; int64_t start=in->i; in->i++;
    for(;;){
        if(in->i>=in->n){ in->i=start; free(b.s); j_fail(in,"Unterminated string starting at"); }
        char c=in->s[in->i++];
        if(c=='"') break;
        if((unsigned char)c<0x20){ in->i--; free(b.s); j_fail(in,"Invalid control character at"); }
        if(c!='\\'){ sb_putc(&b,c); continue; }
        if(in->i>=in->n){ free(b.s); j_fail(in,"Unterminated string starting at"); }
        c=in->s[in->i++];
        switch(c){
            case '"': case '\\': case '/': sb_putc(&b,c); break;
            case 'n': sb_putc(&b,'\n'); break; case 'r': sb_putc(&b,'\r'); break; case 't': sb_putc(&b,'\t'); break;
            case 'b': sb_putc(&b,'\b'); break; case 'f': sb_putc(&b,'\f'); break;
            case 'u':{ uint32_t cp=(uint32_t)j_hex4(in);
                if(cp>=0xD800 && cp<0xDC00 && in->i+6<=in->n && in->s[in->i]=='\\' && in->s[in->i+1]=='u'){ int64_t save=in->i; in->i+=2; uint32_t lo=(uint32_t)j_hex4(in);
                    if(lo>=0xDC00 && lo<0xE000) cp=0x10000+((cp-0xD800)<<10)+(lo-0xDC00); else in->i=save; }
                char t[4]; int m=mp_utf8_encode(t,cp); sb_put(&b,t,m); break; }
            default: in->i-=2; free(b.s); j_fail(in,"Invalid \\escape");
        }
    }
    return sb_value(&b);
}
static Value j_value(JIn *in, int depth){
    if(depth>500) j_fail(in,"Too deeply nested");
    j_ws(in);
    if(in->i>=in->n) j_fail(in,"Expecting value");
    char c=in->s[in->i];
    if(c=='"') return j_str(in);
    if(c=='{'){
        in->i++; Value d=mp_dict(); j_ws(in);
        if(in->i<in->n && in->s[in->i]=='}'){ in->i++; return d; }
        for(;;){
            j_ws(in);
            if(in->i>=in->n || in->s[in->i]!='"') j_fail(in,"Expecting property name enclosed in double quotes");
            Value k=j_str(in); j_ws(in);
            if(in->i>=in->n || in->s[in->i]!=':') j_fail(in,"Expecting ':' delimiter");
            in->i++;
            mp_dict_set(AS_DICT(d),k,j_value(in,depth+1));
            j_ws(in);
            if(in->i<in->n && in->s[in->i]==','){ in->i++; continue; }
            if(in->i<in->n && in->s[in->i]=='}'){ in->i++; return d; }
            j_fail(in,"Expecting ',' delimiter");
        }
    }
    if(c=='['){
        in->i++; Value l=mp_list(0,NULL); j_ws(in);
        if(in->i<in->n && in->s[in->i]==']'){ in->i++; return l; }
        for(;;){
            mp_list_append(l,j_value(in,depth+1));
            j_ws(in);
            if(in->i<in->n && in->s[in->i]==','){ in->i++; continue; }
            if(in->i<in->n && in->s[in->i]==']'){ in->i++; return l; }
            j_fail(in,"Expecting ',' delimiter");
        }
    }
    static const struct { const char *w; int k; } words[]={{"true",1},{"false",2},{"null",3},{"NaN",4},{"Infinity",5},{"-Infinity",6},{NULL,0}};
    for(int w=0;words[w].w;w++){ int64_t L=(int64_t)strlen(words[w].w);
        if(in->i+L<=in->n && !strncmp(in->s+in->i,words[w].w,(size_t)L)){ in->i+=L;
            switch(words[w].k){ case 1: return v_bool(1); case 2: return v_bool(0); case 3: return v_none(); case 4: return v_float(NAN); case 5: return v_float(INFINITY); default: return v_float(-INFINITY); } } }
    if(c=='-'||(c>='0'&&c<='9')){
        int64_t st=in->i; int isf=0;
        if(in->s[in->i]=='-') in->i++;
        if(in->i>=in->n || !(in->s[in->i]>='0'&&in->s[in->i]<='9')){ in->i=st; j_fail(in,"Expecting value"); }
        if(in->s[in->i]=='0') in->i++; else while(in->i<in->n && in->s[in->i]>='0' && in->s[in->i]<='9') in->i++;
        if(in->i+1<in->n && in->s[in->i]=='.' && in->s[in->i+1]>='0' && in->s[in->i+1]<='9'){ isf=1; in->i++; while(in->i<in->n && in->s[in->i]>='0' && in->s[in->i]<='9') in->i++; }
        if(in->i<in->n && (in->s[in->i]=='e'||in->s[in->i]=='E')){ int64_t save=in->i; in->i++; if(in->i<in->n && (in->s[in->i]=='+'||in->s[in->i]=='-')) in->i++;
            if(in->i<in->n && in->s[in->i]>='0' && in->s[in->i]<='9'){ isf=1; while(in->i<in->n && in->s[in->i]>='0' && in->s[in->i]<='9') in->i++; } else in->i=save; }
        char t[128]; int64_t L=in->i-st; if(L>127) L=127; memcpy(t,in->s+st,(size_t)L); t[L]=0;
        if(isf) return v_float(strtod(t,NULL));
        int64_t v; if(mp_parse_int(t,L,10,&v)!=1) mp_raise_t(E_OverflowError,"int too large (ints are 64-bit)");
        return v_int(v);
    }
    j_fail(in,"Expecting value");
}
static Value js_loads(int argc, Value *argv, TupleObj *kw){
    (void)kw; mod_nargs("loads",mod_npos(argc,kw),1,1);
    Value s=argv[0];
    if(IS_BYTES(s)) s=mp_strn((const char*)AS_BYTES(s)->s,AS_BYTES(s)->len);
    if(!IS_STR(s)) mp_raise_t(E_TypeError,"the JSON object must be str, bytes or bytearray, not %s",mp_type_name(s));
    JIn in={AS_STR(s)->s,0,AS_STR(s)->len};
    Value v=j_value(&in,0);
    j_ws(&in);
    if(in.i<in.n) j_fail(&in,"Extra data");
    return v;
}
Value make_json(void){
    Value m=register_module("_ppy_json");
    set_fn(m,"dumps",js_dumps); set_fn(m,"loads",js_loads);
    Value err=mp_dict_get_s(mp_builtins,"ValueError",NULL) ? v_obj(E_ValueError) : v_none();
    mp_dict_set_s(mdict(m),"JSONDecodeError",err);
    return m;
}

