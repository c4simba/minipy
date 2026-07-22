/* ========================= Built-in type methods ========================= */

#include "vm.h"
#include "containers.h"

/* --- small helpers --- */
static void mcollect(Value v, List *out){ Value it=native_iter(1,&v); Value nx; while(iterator_next_value(it,&nx)) list_push(out,nx); }
static int is_ws(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }

/* dynamic char buffer */
typedef struct { char *s; int len, cap; } CBuf;
static void cb_ch(CBuf *b,char c){ if(b->len>=b->cap){ b->cap=b->cap?b->cap*2:32; b->s=(char*)xrealloc(b->s,(size_t)b->cap+1); } b->s[b->len++]=c; }
static void cb_mem(CBuf *b,const char *s,int n){ for(int i=0;i<n;i++) cb_ch(b,s[i]); }
static Value cb_take(CBuf *b){ Value r=stringv_len(b->s?b->s:"",b->len); free(b->s); return r; }

static const char *req_str(Value v,const char *m){ if(!is_obj(v,O_STRING)) runtime_error(m); return v.as.obj->as.str.s; }

/* ---- string methods ---- */
static Value str_method(Value recv,const char *name,int argc,Value *argv){
    String *st=&recv.as.obj->as.str; const char *s=st->s; int n=st->len;
    if(!strcmp(name,"upper")){ CBuf b={0}; for(int i=0;i<n;i++) cb_ch(&b,(char)toupper((unsigned char)s[i])); return cb_take(&b); }
    if(!strcmp(name,"lower")){ CBuf b={0}; for(int i=0;i<n;i++) cb_ch(&b,(char)tolower((unsigned char)s[i])); return cb_take(&b); }
    if(!strcmp(name,"capitalize")){ CBuf b={0}; for(int i=0;i<n;i++){ char c=s[i]; cb_ch(&b,(char)(i==0?toupper((unsigned char)c):tolower((unsigned char)c))); } return cb_take(&b); }
    if(!strcmp(name,"strip")||!strcmp(name,"lstrip")||!strcmp(name,"rstrip")){
        const char *chars=argc>=1?req_str(argv[0],"strip() arg must be str"):NULL;
        int a=0,e=n; int dol=strcmp(name,"rstrip")!=0, dor=strcmp(name,"lstrip")!=0;
        #define IN_SET(ch) (chars? (strchr(chars,(ch))!=NULL) : is_ws((unsigned char)(ch)))
        if(dol) while(a<e && IN_SET(s[a])) a++;
        if(dor) while(e>a && IN_SET(s[e-1])) e--;
        #undef IN_SET
        return stringv_len(s+a,e-a);
    }
    if(!strcmp(name,"startswith")){ const char *pre=req_str(argv[0],"startswith() arg must be str"); int pl=(int)strlen(pre); return boolv(pl<=n && memcmp(s,pre,(size_t)pl)==0); }
    if(!strcmp(name,"endswith")){ const char *suf=req_str(argv[0],"endswith() arg must be str"); int sl=(int)strlen(suf); return boolv(sl<=n && memcmp(s+n-sl,suf,(size_t)sl)==0); }
    if(!strcmp(name,"find")||!strcmp(name,"index")){ const char *sub=req_str(argv[0],"substring must be str"); int sl=(int)strlen(sub); for(int i=0;i+sl<=n;i++) if(memcmp(s+i,sub,(size_t)sl)==0) return intv(i); if(!strcmp(name,"index")) runtime_error("substring not found"); return intv(-1); }
    if(!strcmp(name,"count")){ const char *sub=req_str(argv[0],"substring must be str"); int sl=(int)strlen(sub); if(sl==0) return intv(n+1); int cnt=0; for(int i=0;i+sl<=n;){ if(memcmp(s+i,sub,(size_t)sl)==0){ cnt++; i+=sl; } else i++; } return intv(cnt); }
    if(!strcmp(name,"replace")){ const char *o=req_str(argv[0],"replace() args must be str"); const char *nw=req_str(argv[1],"replace() args must be str"); int ol=(int)strlen(o), nl=(int)strlen(nw); int limit=argc>=3?(int)as_int(argv[2]):-1; CBuf b={0}; int i=0,done=0; while(i<n){ if(ol>0 && i+ol<=n && (limit<0||done<limit) && memcmp(s+i,o,(size_t)ol)==0){ cb_mem(&b,nw,nl); i+=ol; done++; } else cb_ch(&b,s[i++]); } return cb_take(&b); }
    if(!strcmp(name,"split")){
        Obj *out=new_list();
        if(argc==0){ int i=0; while(i<n){ while(i<n && is_ws((unsigned char)s[i])) i++; if(i>=n) break; int j=i; while(j<n && !is_ws((unsigned char)s[j])) j++; list_push(&out->as.list,stringv_len(s+i,j-i)); i=j; } return objv(out); }
        const char *sep=req_str(argv[0],"split() sep must be str"); int sl=(int)strlen(sep); if(sl==0) runtime_error("empty separator");
        int i=0,start=0; while(i+sl<=n){ if(memcmp(s+i,sep,(size_t)sl)==0){ list_push(&out->as.list,stringv_len(s+start,i-start)); i+=sl; start=i; } else i++; } list_push(&out->as.list,stringv_len(s+start,n-start)); return objv(out);
    }
    if(!strcmp(name,"join")){ List parts; memset(&parts,0,sizeof(parts)); mcollect(argv[0],&parts); CBuf b={0}; for(int i=0;i<parts.count;i++){ if(i) cb_mem(&b,s,n); const char *ps=req_str(parts.items[i],"join() items must be str"); cb_mem(&b,ps,(int)strlen(ps)); } return cb_take(&b); }
    if(!strcmp(name,"isdigit")){ if(n==0) return boolv(0); for(int i=0;i<n;i++) if(!(s[i]>='0'&&s[i]<='9')) return boolv(0); return boolv(1); }
    if(!strcmp(name,"isalpha")){ if(n==0) return boolv(0); for(int i=0;i<n;i++) if(!isalpha((unsigned char)s[i])) return boolv(0); return boolv(1); }
    if(!strcmp(name,"isspace")){ if(n==0) return boolv(0); for(int i=0;i<n;i++) if(!is_ws((unsigned char)s[i])) return boolv(0); return boolv(1); }
    runtime_error("unknown string method"); return nonev();
}

/* ---- list methods ---- */
static Value list_method(Value recv,const char *name,int argc,Value *argv){
    List *l=&recv.as.obj->as.list;
    if(!strcmp(name,"append")){ list_push(l,argv[0]); return nonev(); }
    if(!strcmp(name,"extend")){ mcollect(argv[0],l); return nonev(); }
    if(!strcmp(name,"pop")){ if(l->count==0) runtime_error("pop from empty list"); int i=argc>=1?(int)as_int(argv[0]):l->count-1; if(i<0) i+=l->count; if(i<0||i>=l->count) runtime_error("pop index out of range"); Value v=l->items[i]; for(int j=i;j<l->count-1;j++) l->items[j]=l->items[j+1]; l->count--; return v; }
    if(!strcmp(name,"insert")){ int i=(int)as_int(argv[0]); Value v=argv[1]; if(i<0) i+=l->count; if(i<0) i=0; if(i>l->count) i=l->count; list_push(l,nonev()); for(int j=l->count-1;j>i;j--) l->items[j]=l->items[j-1]; l->items[i]=v; return nonev(); }
    if(!strcmp(name,"remove")){ for(int i=0;i<l->count;i++) if(val_equal(l->items[i],argv[0])){ for(int j=i;j<l->count-1;j++) l->items[j]=l->items[j+1]; l->count--; return nonev(); } runtime_error("list.remove(x): x not in list"); }
    if(!strcmp(name,"index")){ for(int i=0;i<l->count;i++) if(val_equal(l->items[i],argv[0])) return intv(i); runtime_error("value not in list"); }
    if(!strcmp(name,"count")){ int c=0; for(int i=0;i<l->count;i++) if(val_equal(l->items[i],argv[0])) c++; return intv(c); }
    if(!strcmp(name,"reverse")){ for(int i=0,j=l->count-1;i<j;i++,j--){ Value t=l->items[i]; l->items[i]=l->items[j]; l->items[j]=t; } return nonev(); }
    if(!strcmp(name,"sort")){ for(int i=1;i<l->count;i++){ Value key=l->items[i]; int j=i-1; while(j>=0 && truthy(compare(key,l->items[j],OP_LT))){ l->items[j+1]=l->items[j]; j--; } l->items[j+1]=key; } return nonev(); }
    if(!strcmp(name,"clear")){ l->count=0; return nonev(); }
    if(!strcmp(name,"copy")){ Obj *o=new_list(); for(int i=0;i<l->count;i++) list_push(&o->as.list,l->items[i]); return objv(o); }
    runtime_error("unknown list method"); return nonev();
}

/* ---- dict methods ---- */
static Value dict_method(Value recv,const char *name,int argc,Value *argv){
    Dict *d=&recv.as.obj->as.dict;
    if(!strcmp(name,"get")){ char *k=value_to_cstr(argv[0]); Value v; int ok=dict_get(d,k,&v); free(k); if(ok) return v; return argc>=2?argv[1]:nonev(); }
    if(!strcmp(name,"setdefault")){ char *k=value_to_cstr(argv[0]); Value v; if(dict_get(d,k,&v)){ free(k); return v; } Value def=argc>=2?argv[1]:nonev(); dict_set(d,k,def); free(k); return def; }
    if(!strcmp(name,"pop")){ char *k=value_to_cstr(argv[0]); int idx=dict_find(d,k); free(k); if(idx<0){ if(argc>=2) return argv[1]; runtime_error("KeyError"); } Value v=d->vals[idx]; for(int j=idx;j<d->count-1;j++){ d->keys[j]=d->keys[j+1]; d->vals[j]=d->vals[j+1]; } d->count--; return v; }
    if(!strcmp(name,"keys")){ Obj *o=new_list(); for(int i=0;i<d->count;i++) list_push(&o->as.list,stringv(d->keys[i])); return objv(o); }
    if(!strcmp(name,"values")){ Obj *o=new_list(); for(int i=0;i<d->count;i++) list_push(&o->as.list,d->vals[i]); return objv(o); }
    if(!strcmp(name,"items")){ Obj *o=new_list(); for(int i=0;i<d->count;i++){ Obj *pr=new_tuple(); list_push(&pr->as.tuple,stringv(d->keys[i])); list_push(&pr->as.tuple,d->vals[i]); list_push(&o->as.list,objv(pr)); } return objv(o); }
    if(!strcmp(name,"update")){ if(argc>=1 && is_obj(argv[0],O_DICT)){ Dict *sd=&argv[0].as.obj->as.dict; for(int i=0;i<sd->count;i++) dict_set(d,sd->keys[i],sd->vals[i]); } return nonev(); }
    if(!strcmp(name,"clear")){ d->count=0; return nonev(); }
    if(!strcmp(name,"copy")){ Obj *o=new_dict_obj(); for(int i=0;i<d->count;i++) dict_set(&o->as.dict,d->keys[i],d->vals[i]); return objv(o); }
    runtime_error("unknown dict method"); return nonev();
}

/* ---- set methods ---- */
static int set_has(List *l,Value v){ for(int i=0;i<l->count;i++) if(val_equal(l->items[i],v)) return 1; return 0; }
static void set_del(List *l,Value v){ for(int i=0;i<l->count;i++) if(val_equal(l->items[i],v)){ for(int j=i;j<l->count-1;j++) l->items[j]=l->items[j+1]; l->count--; return; } }
static Value set_method(Value recv,const char *name,int argc,Value *argv){
    List *l=&recv.as.obj->as.set; (void)argc;
    if(!strcmp(name,"add")){ set_add(l,argv[0]); return nonev(); }
    if(!strcmp(name,"discard")){ set_del(l,argv[0]); return nonev(); }
    if(!strcmp(name,"remove")){ if(!set_has(l,argv[0])) runtime_error("KeyError"); set_del(l,argv[0]); return nonev(); }
    if(!strcmp(name,"clear")){ l->count=0; return nonev(); }
    if(!strcmp(name,"copy")){ Obj *o=new_set(); for(int i=0;i<l->count;i++) set_add(&o->as.set,l->items[i]); return objv(o); }
    if(!strcmp(name,"union")){ Obj *o=new_set(); for(int i=0;i<l->count;i++) set_add(&o->as.set,l->items[i]); List other; memset(&other,0,sizeof(other)); mcollect(argv[0],&other); for(int i=0;i<other.count;i++) set_add(&o->as.set,other.items[i]); return objv(o); }
    if(!strcmp(name,"intersection")){ List other; memset(&other,0,sizeof(other)); mcollect(argv[0],&other); Obj *o=new_set(); for(int i=0;i<l->count;i++) if(set_has(&other,l->items[i])) set_add(&o->as.set,l->items[i]); return objv(o); }
    if(!strcmp(name,"difference")){ List other; memset(&other,0,sizeof(other)); mcollect(argv[0],&other); Obj *o=new_set(); for(int i=0;i<l->count;i++) if(!set_has(&other,l->items[i])) set_add(&o->as.set,l->items[i]); return objv(o); }
    runtime_error("unknown set method"); return nonev();
}

Value call_builtin_method(Value recv,const char *name,int argc,Value *argv){
    if(is_obj(recv,O_STRING)) return str_method(recv,name,argc,argv);
    if(is_obj(recv,O_LIST)) return list_method(recv,name,argc,argv);
    if(is_obj(recv,O_DICT)) return dict_method(recv,name,argc,argv);
    if(is_obj(recv,O_SET)) return set_method(recv,name,argc,argv);
    runtime_error("object has no methods"); return nonev();
}
