/* ========================= Interpreter: methods of the built-in types =========================
   str (code points over UTF-8; upper / lower / isalpha ... know Latin-1, Latin
   Extended-A, Greek and Cyrillic), bytes, list, tuple, dict, set, int, float,
   files, generators and coroutines, object, BaseException, property. */

#include "interp.h"
#include <math.h>

Value mp_set_copy(SetObj *s, Type *t);
void  mp_set_merge(SetObj *s, SetObj *o);
int   mp_set_next(SetObj *s, int64_t *pos, Value *key);
Value mp_set_pop(SetObj *s);
void  mp_str_repr_into(SBuf *b, StrObj *s);
int   mp_parse_int(const char *s, int64_t n, int base, int64_t *out);
Value mp_iter_kind(int kind, Value src, Value aux, Value aux2);

#define SELF argv[0]
static int npos_of(int argc, TupleObj *kw){ return argc-(kw?(int)kw->len:0); }
static Value kwarg(int argc, Value *argv, TupleObj *kw, const char *name, Value dflt){
    if(!kw) return dflt;
    int np=npos_of(argc,kw);
    for(int64_t i=0;i<kw->len;i++) if(!strcmp(mp_cstr(kw->items[i]),name)) return argv[np+i];
    return dflt;
}
static void nargs(const char *name, int argc, int lo, int hi){
    int n=argc-1;
    if(n<lo || n>hi){
        if(lo==hi && lo==0) mp_raise_t(E_TypeError,"%s() takes no arguments (%d given)",name,n);
        if(lo==hi) mp_raise_t(E_TypeError,"%s() takes exactly one argument (%d given)",name,n);
        if(n<lo) mp_raise_t(E_TypeError,"%s expected at least %d argument%s, got %d",name,lo,lo==1?"":"s",n);
        mp_raise_t(E_TypeError,"%s expected at most %d argument%s, got %d",name,hi,hi==1?"":"s",n);
    }
}
static StrObj *S(Value v){ return AS_STR(v); }
static StrObj *str_arg(Value v, const char *what){
    if(!IS_STR(v)) mp_raise_t(E_TypeError,"%s must be str, not %s",what,mp_type_name(v));
    return AS_STR(v);
}

/* ---------------------------------------------------------------- unicode */
#include "i_unicode.h"
/* the class bits, upper - c, lower - c of a code point */
static const uint16_t *uc_props(uint32_t c){
    static const uint16_t none[3]={0,0,0};
    int lo=0, hi=(int)(sizeof uc_ranges/sizeof uc_ranges[0])-1;
    while(lo<=hi){ int m=(lo+hi)/2; const UcRange *r=&uc_ranges[m];
        if(c<r->lo) hi=m-1; else if(c>r->hi) lo=m+1; else return r->p+((c-r->lo)&1)*3; }
    return none;
}
static int uc_bits(uint32_t c){ return uc_props(c)[0]; }
uint32_t mp_cp_upper(uint32_t c){ const uint16_t *p=uc_props(c); return p[1] ? (c+p[1])&0xFFFF : c; }
uint32_t mp_cp_lower(uint32_t c){ const uint16_t *p=uc_props(c); return p[2] ? (c+p[2])&0xFFFF : c; }
int mp_cp_isspace(uint32_t c){
    return c==' '||(c>=9&&c<=13)||(c>=0x1c&&c<=0x1f)||c==0x85||c==0xA0||c==0x1680||(c>=0x2000&&c<=0x200A)||c==0x2028||c==0x2029||c==0x202F||c==0x205F||c==0x3000;
}
int mp_cp_isalpha(uint32_t c){ return (uc_bits(c)&UC_ALPHA)!=0; }
int mp_cp_isdigit(uint32_t c){ return (uc_bits(c)&UC_DIGIT)!=0; }
int mp_cp_isdecimal(uint32_t c){ return (uc_bits(c)&UC_DECIMAL)!=0; }
int mp_cp_isnumeric(uint32_t c){ return (uc_bits(c)&UC_NUMERIC)!=0; }
/* str.isprintable, and what repr() leaves unescaped */
int mp_cp_printable(uint32_t c){
    if(c<0x80) return c>=0x20 && c!=0x7f;
    int lo=0, hi=(int)(sizeof uc_noprint/sizeof uc_noprint[0])-1;
    while(lo<=hi){ int m=(lo+hi)/2; if(c<uc_noprint[m][0]) hi=m-1; else if(c>uc_noprint[m][1]) lo=m+1; else return 0; }
    return 1;
}
/* the full mapping of c (kind U upper, L lower, T title, F casefold) */
static void put_utf8(SBuf *b, uint32_t c);
static void put_mapped(SBuf *b, uint32_t c, char kind){
    if(c>=0xB5) for(size_t i=0;i<sizeof uc_special/sizeof uc_special[0];i++)
        if(uc_special[i].cp==c && uc_special[i].kind==kind){ sb_puts(b,uc_special[i].to); return; }
    if(kind=='T') kind='U';                 /* title is upper, casefold lower, unless special */
    else if(kind=='F') kind='L';
    if(c>=0xB5) for(size_t i=0;i<sizeof uc_special/sizeof uc_special[0];i++)
        if(uc_special[i].cp==c && uc_special[i].kind==kind){ sb_puts(b,uc_special[i].to); return; }
    put_utf8(b, kind=='U' ? mp_cp_upper(c) : mp_cp_lower(c));
}
/* U+03A3 at [at, after) lowers to final sigma when a cased letter is before
   it and none after (case-ignorable ones in between skipped): CPython's rule */
static int final_sigma(const char *s, int64_t n, int64_t at, int64_t after){
    int64_t j=at; uint32_t c=0; int found=0;
    while(j>0){
        int64_t k=j-1; while(k>0 && ((unsigned char)s[k]&0xC0)==0x80) k--;
        int64_t t=k; c=(uint32_t)mp_utf8_decode(s,n,&t); j=k;
        if(!(uc_bits(c)&UC_IGNORABLE)){ found=1; break; }
    }
    if(!found || !(uc_bits(c)&UC_CASED)) return 0;
    int64_t p=after;
    while(p<n){ c=(uint32_t)mp_utf8_decode(s,n,&p); if(!(uc_bits(c)&UC_IGNORABLE)) return !(uc_bits(c)&UC_CASED); }
    return 1;
}
typedef struct { const char *s; int64_t n, p; } U;
static int unext(U *u, uint32_t *c){ if(u->p>=u->n) return 0; *c=(uint32_t)mp_utf8_decode(u->s,u->n,&u->p); return 1; }
static void put_utf8(SBuf *b, uint32_t c){ char t[4]; int m=mp_utf8_encode(t,c); sb_put(b,t,m); }
static int64_t cp_index(StrObj *s, int64_t off){
    if(s->ascii) return off;
    int64_t c=0, p=0; while(p<off){ mp_utf8_decode(s->s,s->len,&p); c++; } return c;
}
/* start / end arguments (code points) -> byte range */
static void range_args(StrObj *s, int argc, Value *argv, int first, int64_t *bs, int64_t *be){
    int64_t a=0, e=s->cplen;
    if(argc>first && !IS_NONE(argv[first])) a=mp_index(argv[first],"slice");
    if(argc>first+1 && !IS_NONE(argv[first+1])) e=mp_index(argv[first+1],"slice");
    if(a<0){ a+=s->cplen; if(a<0) a=0; } if(a>s->cplen) a=s->cplen+1;
    if(e<0){ e+=s->cplen; if(e<0) e=0; } if(e>s->cplen) e=s->cplen;
    *bs= a>s->cplen ? s->len+1 : mp_str_byteoff(s,a); *be=mp_str_byteoff(s,e);
}
static int64_t find_bytes(const char *h, int64_t hs, int64_t he, const char *n, int64_t nl, int rev){
    if(hs>he) return -1;
    if(!rev){ for(int64_t i=hs;i+nl<=he;i++) if(!memcmp(h+i,n,(size_t)nl)) return i; }
    else { for(int64_t i=he-nl;i>=hs;i--) if(!memcmp(h+i,n,(size_t)nl)) return i; }
    return -1;
}

/* ---------------------------------------------------------------- str */
static Value str_case(Value self, int mode){   /* 0 upper, 1 lower, 2 swapcase, 3 title, 4 capitalize, 5 casefold */
    StrObj *s=S(self); SBuf b={0}; int64_t p=0, n=s->len; int prev_cased=0, first=1;
    while(p<n){
        int64_t at=p; uint32_t c=(uint32_t)mp_utf8_decode(s->s,n,&p); int bits=uc_bits(c); char k;
        switch(mode){
            case 0: k='U'; break;
            case 1: k='L'; break;
            case 2: k= bits&UC_UPPER ? 'L' : bits&UC_LOWER ? 'U' : 0; break;
            case 3: k= prev_cased ? 'L' : 'T'; prev_cased=(bits&UC_CASED)!=0; break;
            case 4: k= first ? 'T' : 'L'; first=0; break;
            default: k='F';
        }
        if(!k) put_utf8(&b,c);
        else if(k=='L' && c==0x3A3) put_utf8(&b,final_sigma(s->s,n,at,p)?0x3C2:0x3C3);
        else put_mapped(&b,c,k);
    }
    return sb_value(&b);
}
static Value m_upper(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("upper",argc,0,0); return str_case(SELF,0); }
static Value m_lower(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("lower",argc,0,0); return str_case(SELF,1); }
static Value m_swapcase(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("swapcase",argc,0,0); return str_case(SELF,2); }
static Value m_title(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("title",argc,0,0); return str_case(SELF,3); }
static Value m_capitalize(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("capitalize",argc,0,0); return str_case(SELF,4); }
static Value m_casefold(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("casefold",argc,0,0); return str_case(SELF,5); }
static Value str_is(Value self, int mode){
    StrObj *s=S(self); U u={s->s,s->len,0}; uint32_t c; int any=0, cased=0, prev_cased=0;
    if(s->len==0) return v_bool(mode==7 || mode==10);
    while(unext(&u,&c)){
        switch(mode){
            case 0: if(!mp_cp_isalpha(c)) return v_bool(0); break;
            case 1: if(!mp_cp_isdigit(c)) return v_bool(0); break;
            case 2: if(!mp_cp_isalpha(c) && !mp_cp_isnumeric(c)) return v_bool(0); break;
            case 3: if(!mp_cp_isspace(c)) return v_bool(0); break;
            case 4: if(uc_bits(c)&(UC_UPPER|UC_TITLE)) return v_bool(0); if(uc_bits(c)&UC_LOWER) any=1; break;
            case 5: if(uc_bits(c)&(UC_LOWER|UC_TITLE)) return v_bool(0); if(uc_bits(c)&UC_UPPER) any=1; break;
            case 6: if(!mp_cp_isdecimal(c)) return v_bool(0); break;
            case 7: if(c>=0x80) return v_bool(0); break;
            case 8: if(!mp_cp_isnumeric(c)) return v_bool(0); break;
            case 9: if(uc_bits(c)&(UC_UPPER|UC_TITLE)){ if(prev_cased) return v_bool(0); prev_cased=1; cased=1; } else if(uc_bits(c)&UC_LOWER){ if(!prev_cased) return v_bool(0); prev_cased=1; cased=1; } else prev_cased=0; break;
            case 10: if(!mp_cp_printable(c)) return v_bool(0); break;
        }
    }
    if(mode==4 || mode==5) return v_bool(any);
    if(mode==9) return v_bool(cased);
    return v_bool(1);
}
#define ISFN(name,mode) static Value m_##name(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs(#name,argc,0,0); return str_is(SELF,mode); }
ISFN(isalpha,0) ISFN(isdigit,1) ISFN(isalnum,2) ISFN(isspace,3) ISFN(islower,4) ISFN(isupper,5) ISFN(isdecimal,6) ISFN(isascii,7) ISFN(isnumeric,8) ISFN(istitle,9) ISFN(isprintable,10)
static Value m_isidentifier(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("isidentifier",argc,0,0);
    StrObj *s=S(SELF); U u={s->s,s->len,0}; uint32_t c; int first=1;
    if(!s->len) return v_bool(0);
    while(unext(&u,&c)){ if(!(c=='_'||mp_cp_isalpha(c)||(!first && mp_cp_isdecimal(c)))) return v_bool(0); first=0; }
    return v_bool(1); }
/* the set of code points of a strip argument */
static int in_chars(StrObj *chars, uint32_t c){
    if(!chars) return mp_cp_isspace(c);
    U u={chars->s,chars->len,0}; uint32_t x; while(unext(&u,&x)) if(x==c) return 1; return 0;
}
static Value strip(int argc, Value *argv, int left, int right, const char *name){
    nargs(name,argc,0,1);
    StrObj *s=S(SELF), *chars= argc>1 && !IS_NONE(argv[1]) ? str_arg(argv[1],"strip arg") : NULL;
    int64_t a=0, e=s->len;
    if(left){ while(a<e){ int64_t p=a; uint32_t c=(uint32_t)mp_utf8_decode(s->s,s->len,&p); if(!in_chars(chars,c)) break; a=p; } }
    if(right){ while(e>a){ int64_t q=e-1; while(q>a && ((unsigned char)s->s[q]&0xC0)==0x80) q--; int64_t p=q; uint32_t c=(uint32_t)mp_utf8_decode(s->s,s->len,&p); if(!in_chars(chars,c)) break; e=q; } }
    if(a==0 && e==s->len && IS(SELF,T_str)) return SELF;
    return mp_strn(s->s+a,e-a);
}
static Value m_strip(int argc, Value *argv, TupleObj *kw){ (void)kw; return strip(argc,argv,1,1,"strip"); }
static Value m_lstrip(int argc, Value *argv, TupleObj *kw){ (void)kw; return strip(argc,argv,1,0,"lstrip"); }
static Value m_rstrip(int argc, Value *argv, TupleObj *kw){ (void)kw; return strip(argc,argv,0,1,"rstrip"); }
static Value split_ws(StrObj *s, int64_t maxsplit, int rev){
    Value l=mp_list(0,NULL);
    if(!rev){
        int64_t p=0, n=s->len;
        while(p<n){
            int64_t q=p; uint32_t c=(uint32_t)mp_utf8_decode(s->s,n,&q);
            if(mp_cp_isspace(c)){ p=q; continue; }
            if(maxsplit==0){ int64_t e=n; while(e>p){ int64_t k=e-1; while(k>p && ((unsigned char)s->s[k]&0xC0)==0x80) k--; int64_t t=k; uint32_t cc=(uint32_t)mp_utf8_decode(s->s,n,&t); if(!mp_cp_isspace(cc)) break; e=k; } mp_list_append(l,mp_strn(s->s+p,e-p)); break; }
            int64_t st=p;
            while(p<n){ q=p; c=(uint32_t)mp_utf8_decode(s->s,n,&q); if(mp_cp_isspace(c)) break; p=q; }
            mp_list_append(l,mp_strn(s->s+st,p-st)); maxsplit--;
        }
        return l;
    }
    /* from the right */
    Value parts=mp_list(0,NULL); int64_t e=s->len;
    while(e>0){
        int64_t k=e-1; while(k>0 && ((unsigned char)s->s[k]&0xC0)==0x80) k--;
        int64_t t=k; uint32_t c=(uint32_t)mp_utf8_decode(s->s,s->len,&t);
        if(mp_cp_isspace(c)){ e=k; continue; }
        if(maxsplit==0){ int64_t a=0; while(a<e){ int64_t q=a; uint32_t cc=(uint32_t)mp_utf8_decode(s->s,s->len,&q); if(!mp_cp_isspace(cc)) break; a=q; } mp_list_append(parts,mp_strn(s->s+a,e-a)); break; }
        int64_t st=e;
        while(e>0){ k=e-1; while(k>0 && ((unsigned char)s->s[k]&0xC0)==0x80) k--; t=k; c=(uint32_t)mp_utf8_decode(s->s,s->len,&t); if(mp_cp_isspace(c)) break; e=k; }
        mp_list_append(parts,mp_strn(s->s+e,st-e)); maxsplit--;
    }
    ListObj *pl=AS_LIST(parts); for(int64_t i=pl->len-1;i>=0;i--) mp_list_append(l,pl->items[i]);
    return l;
}
static Value split_impl(int argc, Value *argv, TupleObj *kw, int rev){
    int np=npos_of(argc,kw);
    Value sepv= np>1 ? argv[1] : kwarg(argc,argv,kw,"sep",v_none());
    Value maxv= np>2 ? argv[2] : kwarg(argc,argv,kw,"maxsplit",v_int(-1));
    StrObj *s=S(SELF); int64_t maxsplit=mp_index(maxv,"maxsplit");
    if(IS_NONE(sepv)) return split_ws(s,maxsplit,rev);
    StrObj *sep=str_arg(sepv,"sep");
    if(!sep->len) mp_raise_t(E_ValueError,"empty separator");
    Value l=mp_list(0,NULL);
    if(!rev){
        int64_t p=0;
        while(maxsplit!=0){ int64_t f=find_bytes(s->s,p,s->len,sep->s,sep->len,0); if(f<0) break; mp_list_append(l,mp_strn(s->s+p,f-p)); p=f+sep->len; maxsplit--; }
        mp_list_append(l,mp_strn(s->s+p,s->len-p));
        return l;
    }
    Value parts=mp_list(0,NULL); int64_t e=s->len;
    while(maxsplit!=0){ int64_t f=find_bytes(s->s,0,e,sep->s,sep->len,1); if(f<0) break; mp_list_append(parts,mp_strn(s->s+f+sep->len,e-f-sep->len)); e=f; maxsplit--; }
    mp_list_append(parts,mp_strn(s->s,e));
    ListObj *pl=AS_LIST(parts); for(int64_t i=pl->len-1;i>=0;i--) mp_list_append(l,pl->items[i]);
    return l;
}
static Value m_split(int argc, Value *argv, TupleObj *kw){ return split_impl(argc,argv,kw,0); }
static Value m_rsplit(int argc, Value *argv, TupleObj *kw){ return split_impl(argc,argv,kw,1); }
static Value m_splitlines(int argc, Value *argv, TupleObj *kw){
    int keep= npos_of(argc,kw)>1 ? mp_truth(argv[1]) : mp_truth(kwarg(argc,argv,kw,"keepends",v_bool(0)));
    StrObj *s=S(SELF); Value l=mp_list(0,NULL); int64_t st=0, p=0;
    while(p<s->len){
        int64_t q=p; uint32_t c=(uint32_t)mp_utf8_decode(s->s,s->len,&q);
        if(c=='\n'||c=='\r'||c==0x0b||c==0x0c||(c>=0x1c&&c<=0x1e)||c==0x85||c==0x2028||c==0x2029){
            int64_t e=q; if(c=='\r' && q<s->len && s->s[q]=='\n') e=q+1;
            mp_list_append(l,mp_strn(s->s+st,(keep?e:p)-st)); st=p=e; continue;
        }
        p=q;
    }
    if(st<s->len) mp_list_append(l,mp_strn(s->s+st,s->len-st));
    return l;
}
static Value m_join(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("join",argc,1,1);
    StrObj *sep=S(SELF); Value items=mp_list_of(argv[1]); ListObj *l=AS_LIST(items);
    SBuf b={0};
    for(int64_t i=0;i<l->len;i++){
        if(!IS_STR(l->items[i])){ free(b.s); mp_raise_t(E_TypeError,"sequence item %lld: expected str instance, %s found",(long long)i,mp_type_name(l->items[i])); }
        if(i) sb_put(&b,sep->s,sep->len);
        sb_put(&b,S(l->items[i])->s,S(l->items[i])->len);
    }
    return sb_value(&b);
}
static Value find_impl(int argc, Value *argv, int rev, int raise, const char *name){
    nargs(name,argc,1,3);
    StrObj *s=S(SELF), *sub=str_arg(argv[1],"must be str, not");
    int64_t bs, be; range_args(s,argc,argv,2,&bs,&be);
    int64_t f= bs>s->len ? -1 : find_bytes(s->s,bs,be,sub->s,sub->len,rev);
    if(f<0){ if(raise) mp_raise_t(E_ValueError,"substring not found"); return v_int(-1); }
    return v_int(cp_index(s,f));
}
static Value m_find(int argc, Value *argv, TupleObj *kw){ (void)kw; return find_impl(argc,argv,0,0,"find"); }
static Value m_rfind(int argc, Value *argv, TupleObj *kw){ (void)kw; return find_impl(argc,argv,1,0,"rfind"); }
static Value m_sindex(int argc, Value *argv, TupleObj *kw){ (void)kw; return find_impl(argc,argv,0,1,"index"); }
static Value m_srindex(int argc, Value *argv, TupleObj *kw){ (void)kw; return find_impl(argc,argv,1,1,"rindex"); }
static Value m_scount(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("count",argc,1,3);
    StrObj *s=S(SELF), *sub=str_arg(argv[1],"must be str, not");
    int64_t bs, be; range_args(s,argc,argv,2,&bs,&be);
    if(bs>s->len) return v_int(0);
    if(!sub->len) return v_int(cp_index(s,be)-cp_index(s,bs)+1);
    int64_t n=0, p=bs; while(1){ int64_t f=find_bytes(s->s,p,be,sub->s,sub->len,0); if(f<0) break; n++; p=f+sub->len; }
    return v_int(n);
}
static Value m_replace(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw);
    if(np<3) mp_raise_t(E_TypeError,"replace expected at least 2 arguments, got %d",np-1);
    StrObj *s=S(SELF), *old=str_arg(argv[1],"replace() argument 1"), *nw=str_arg(argv[2],"replace() argument 2");
    int64_t count= np>3 ? mp_index(argv[3],"count") : mp_index(kwarg(argc,argv,kw,"count",v_int(-1)),"count");
    SBuf b={0};
    if(!old->len){
        U u={s->s,s->len,0}; uint32_t c; int64_t n=0;
        if(count!=0){ sb_put(&b,nw->s,nw->len); n++; }
        while(unext(&u,&c)){ put_utf8(&b,c); if(count<0 || n<count){ sb_put(&b,nw->s,nw->len); n++; } }
        return sb_value(&b);
    }
    int64_t p=0, n=0;
    while(count<0 || n<count){ int64_t f=find_bytes(s->s,p,s->len,old->s,old->len,0); if(f<0) break; sb_put(&b,s->s+p,f-p); sb_put(&b,nw->s,nw->len); p=f+old->len; n++; }
    sb_put(&b,s->s+p,s->len-p);
    return sb_value(&b);
}
static Value starts_ends(int argc, Value *argv, int ends, const char *name){
    nargs(name,argc,1,3);
    StrObj *s=S(SELF); int64_t bs, be; range_args(s,argc,argv,2,&bs,&be);
    Value pre=argv[1];
    if(IS_TUPLE(pre)){
        for(int64_t i=0;i<AS_TUPLE(pre)->len;i++){ Value a[4]={SELF,AS_TUPLE(pre)->items[i],argc>2?argv[2]:v_none(),argc>3?argv[3]:v_none()}; if(mp_truth(starts_ends(argc,a,ends,name))) return v_bool(1); }
        return v_bool(0);
    }
    if(!IS_STR(pre)) mp_raise_t(E_TypeError,"%s first arg must be str or a tuple of str, not %s",name,mp_type_name(pre));
    StrObj *p=S(pre);
    if(bs>s->len || be-bs<p->len) return v_bool(bs<=s->len && p->len==0 && be>=bs);
    return v_bool(!memcmp(s->s+(ends?be-p->len:bs),p->s,(size_t)p->len));
}
static Value m_startswith(int argc, Value *argv, TupleObj *kw){ (void)kw; return starts_ends(argc,argv,0,"startswith"); }
static Value m_endswith(int argc, Value *argv, TupleObj *kw){ (void)kw; return starts_ends(argc,argv,1,"endswith"); }
static Value justify(int argc, Value *argv, int mode, const char *name){
    nargs(name,argc,1,2);
    StrObj *s=S(SELF); int64_t w=mp_index(argv[1],"width");
    uint32_t fill=' ';
    if(argc>2){ StrObj *f=str_arg(argv[2],"The fill character"); if(f->cplen!=1) mp_raise_t(E_TypeError,"The fill character must be exactly one character long"); int64_t p=0; fill=(uint32_t)mp_utf8_decode(f->s,f->len,&p); }
    if(w<=s->cplen) return IS(SELF,T_str)?SELF:mp_strn(s->s,s->len);
    int64_t padn=w-s->cplen, left= mode==0 ? 0 : mode==1 ? padn : padn/2 + (padn&w&1);
    SBuf b={0}; char t[4]; int m=mp_utf8_encode(t,fill);
    for(int64_t i=0;i<left;i++) sb_put(&b,t,m);
    sb_put(&b,s->s,s->len);
    for(int64_t i=0;i<padn-left;i++) sb_put(&b,t,m);
    return sb_value(&b);
}
static Value m_ljust(int argc, Value *argv, TupleObj *kw){ (void)kw; return justify(argc,argv,0,"ljust"); }
static Value m_rjust(int argc, Value *argv, TupleObj *kw){ (void)kw; return justify(argc,argv,1,"rjust"); }
static Value m_center(int argc, Value *argv, TupleObj *kw){ (void)kw; return justify(argc,argv,2,"center"); }
static Value m_zfill(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("zfill",argc,1,1);
    StrObj *s=S(SELF); int64_t w=mp_index(argv[1],"width");
    if(w<=s->cplen) return mp_strn(s->s,s->len);
    SBuf b={0}; int64_t st=0;
    if(s->len && (s->s[0]=='+'||s->s[0]=='-')){ sb_putc(&b,s->s[0]); st=1; }
    for(int64_t i=0;i<w-s->cplen;i++) sb_putc(&b,'0');
    sb_put(&b,s->s+st,s->len-st);
    return sb_value(&b);
}
static Value m_partition_impl(int argc, Value *argv, int rev){
    nargs(rev?"rpartition":"partition",argc,1,1);
    StrObj *s=S(SELF), *sep=str_arg(argv[1],"must be str, not");
    if(!sep->len) mp_raise_t(E_ValueError,"empty separator");
    int64_t f=find_bytes(s->s,0,s->len,sep->s,sep->len,rev);
    Value r[3];
    if(f<0){ if(rev){ r[0]=mp_str(""); r[1]=mp_str(""); r[2]=SELF; } else { r[0]=SELF; r[1]=mp_str(""); r[2]=mp_str(""); } }
    else { r[0]=mp_strn(s->s,f); r[1]=argv[1]; r[2]=mp_strn(s->s+f+sep->len,s->len-f-sep->len); }
    return mp_tuple(3,r);
}
static Value m_partition(int argc, Value *argv, TupleObj *kw){ (void)kw; return m_partition_impl(argc,argv,0); }
static Value m_rpartition(int argc, Value *argv, TupleObj *kw){ (void)kw; return m_partition_impl(argc,argv,1); }
static Value m_removeprefix(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("removeprefix",argc,1,1); StrObj *s=S(SELF), *p=str_arg(argv[1],"removeprefix() argument");
    if(p->len<=s->len && !memcmp(s->s,p->s,(size_t)p->len)) return mp_strn(s->s+p->len,s->len-p->len); return SELF; }
static Value m_removesuffix(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("removesuffix",argc,1,1); StrObj *s=S(SELF), *p=str_arg(argv[1],"removesuffix() argument");
    if(p->len && p->len<=s->len && !memcmp(s->s+s->len-p->len,p->s,(size_t)p->len)) return mp_strn(s->s,s->len-p->len); return SELF; }
static Value m_expandtabs(int argc, Value *argv, TupleObj *kw){
    int64_t ts= npos_of(argc,kw)>1 ? mp_index(argv[1],"tabsize") : mp_index(kwarg(argc,argv,kw,"tabsize",v_int(8)),"tabsize");
    StrObj *s=S(SELF); SBuf b={0}; U u={s->s,s->len,0}; uint32_t c; int64_t col=0;
    while(unext(&u,&c)){
        if(c=='\t'){ if(ts>0){ int64_t n=ts-col%ts; for(int64_t i=0;i<n;i++) sb_putc(&b,' '); col+=n; } }
        else if(c=='\n'||c=='\r'){ put_utf8(&b,c); col=0; }
        else { put_utf8(&b,c); col++; }
    }
    return sb_value(&b);
}
Value mp_builtin_import(int argc, Value *argv, TupleObj *kw);
static int codec_known(Value enc);
/* an encoding the interpreter does not have itself: _codecs_more.py's (code pages, utf-16, utf-32), through codecs.py */
static Value py_codec(const char *fn, Value obj, Value enc, Value errs){
    Value more=mp_str("_codecs_more"); mp_builtin_import(1,&more,NULL);
    Value nm=mp_str("codecs"), mod=mp_builtin_import(1,&nm,NULL);
    Value a[3]={obj,enc,errs.k==V_UNDEF?mp_str("strict"):errs};
    return mp_call(mp_getattr_s(mod,fn),3,a,NULL);
}
static Value m_encode(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw);
    Value enc= np>1 ? argv[1] : kwarg(argc,argv,kw,"encoding",v_undef());
    Value errs= np>2 ? argv[2] : kwarg(argc,argv,kw,"errors",v_undef());
    StrObj *s=S(SELF);
    if(enc.k!=V_UNDEF && !codec_known(enc)) return py_codec("encode",SELF,enc,errs);
    return mp_encode(s->s,s->len,enc.k==V_UNDEF?0:mp_codec(enc),errs);
}
static Value m_sformat(int argc, Value *argv, TupleObj *kw){ return mp_str_format(SELF,argc-1,argv+1,kw); }
static Value m_format_map(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("format_map",argc,1,1);
    DictObj *d=AS_DICT(argv[1]); int n=(int)d->used; Value vals=mp_tuple(n,NULL), names=mp_tuple(n,NULL);
    int64_t pos=0; Value k, v; int i=0; while(mp_dict_next(d,&pos,&k,&v)){ AS_TUPLE(names)->items[i]=k; AS_TUPLE(vals)->items[i]=v; i++; }
    return mp_str_format(SELF,n,AS_TUPLE(vals)->items,AS_TUPLE(names));
}
static Value m_maketrans(int argc, Value *argv, TupleObj *kw){
    (void)kw; Value d=mp_dict();
    if(argc==1 || (argc>=1 && IS_DICT(argv[argc>1?1:0]) && argc<=2)){
        Value src=argv[argc-1]; int64_t pos=0; Value k, v;
        while(mp_dict_next(AS_DICT(src),&pos,&k,&v)){ if(IS_STR(k)){ int64_t p=0; k=v_int(mp_utf8_decode(S(k)->s,S(k)->len,&p)); } mp_dict_set(AS_DICT(d),k,v); }
        return d;
    }
    int base= IS_TYPE(argv[0]) ? 1 : (argc==4 ? 1 : 0);
    StrObj *a=str_arg(argv[base],"maketrans"), *b=str_arg(argv[base+1],"maketrans");
    if(a->cplen!=b->cplen) mp_raise_t(E_ValueError,"the first two maketrans arguments must have equal length");
    U ua={a->s,a->len,0}, ub={b->s,b->len,0}; uint32_t x, y;
    while(unext(&ua,&x) && unext(&ub,&y)) mp_dict_set(AS_DICT(d),v_int(x),v_int(y));
    if(argc>base+2){ StrObj *z=str_arg(argv[base+2],"maketrans"); U uz={z->s,z->len,0}; while(unext(&uz,&x)) mp_dict_set(AS_DICT(d),v_int(x),v_none()); }
    return d;
}
static Value m_translate(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("translate",argc,1,1);
    StrObj *s=S(SELF); SBuf b={0}; U u={s->s,s->len,0}; uint32_t c;
    while(unext(&u,&c)){
        Value r; Catch ct; int found=0;
        if(!CATCH_BEGIN(ct)){ r=mp_getitem(argv[1],v_int(c)); found=1; CATCH_END(ct); }
        else { Value e=mp_catch_exc(&ct); if(!mp_isinstance(e,E_LookupError)) mp_raise(e); }
        if(!found){ put_utf8(&b,c); continue; }
        if(IS_NONE(r)) continue;
        if(IS_INTLIKE(r)) put_utf8(&b,(uint32_t)r.u.i);
        else if(IS_STR(r)) sb_put(&b,S(r)->s,S(r)->len);
        else mp_raise_t(E_TypeError,"character mapping must return integer, None or str");
    }
    return sb_value(&b);
}
static Value m_str_getnewargs(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)kw; return mp_tuple(1,argv); }

/* ---------------------------------------------------------------- codecs */
/* an encoding's name -> 0 utf-8, 1 ascii, 2 latin-1 (LookupError for others) */
int mp_codec(Value enc){
    if(!IS_STR(enc)) mp_raise_t(E_TypeError,"encode() argument 'encoding' must be str, not %s",mp_type_name(enc));
    char e[40]; snprintf(e,sizeof e,"%s",mp_cstr(enc));
    for(char *q=e;*q;q++){ *q=(char)tolower((unsigned char)*q); if(*q=='_'||*q==' ') *q='-'; }
    if(!strcmp(e,"utf-8")||!strcmp(e,"utf8")||!strcmp(e,"u8")) return 0;
    if(!strcmp(e,"ascii")||!strcmp(e,"us-ascii")) return 1;
    if(!strcmp(e,"latin-1")||!strcmp(e,"latin1")||!strcmp(e,"iso-8859-1")||!strcmp(e,"iso8859-1")||!strcmp(e,"l1")) return 2;
    mp_raise_t(E_LookupError,"unknown encoding: %s",mp_cstr(enc));
}
static const char *codec_name(int k){ return k==0 ? "utf-8" : k==1 ? "ascii" : "latin-1"; }
static int codec_known(Value enc){
    if(!IS_STR(enc)) return 1;                       /* (mp_codec's TypeError) */
    char e[40]; snprintf(e,sizeof e,"%s",mp_cstr(enc));
    for(char *q=e;*q;q++){ *q=(char)tolower((unsigned char)*q); if(*q=='_'||*q==' ') *q='-'; }
    return !strcmp(e,"utf-8")||!strcmp(e,"utf8")||!strcmp(e,"u8")||!strcmp(e,"ascii")||!strcmp(e,"us-ascii")||!strcmp(e,"latin-1")||!strcmp(e,"latin1")
           ||!strcmp(e,"iso-8859-1")||!strcmp(e,"iso8859-1")||!strcmp(e,"l1");
}
/* an error handler, looked up only when an error happens: 0 strict, 1 ignore, 2 replace,
   3 backslashreplace, 4 xmlcharrefreplace (encoding only), 5 surrogateescape, 6 surrogatepass */
static int errors_of(Value errs){
    if(errs.k==V_UNDEF) return 0;
    const char *e=mp_cstr(errs);
    if(!strcmp(e,"strict")) return 0;
    if(!strcmp(e,"ignore")) return 1;
    if(!strcmp(e,"replace")) return 2;
    if(!strcmp(e,"backslashreplace")) return 3;
    if(!strcmp(e,"xmlcharrefreplace")) return 4;
    if(!strcmp(e,"surrogateescape")) return 5;
    if(!strcmp(e,"surrogatepass")) return 6;
    mp_raise_t(E_LookupError,"unknown error handler name '%s'",e);
}
/* UnicodeDecodeError / UnicodeEncodeError(encoding, object, start, end, reason), as the codecs raise them */
static MPY_NORETURN void unicode_error(Type *t, const char *codec, Value obj, int64_t start, int64_t end, const char *reason){
    Value a[5]={mp_str(codec),obj,v_int(start),v_int(end),mp_str(reason)};
    mp_raise(mp_call(v_obj(t),5,a,NULL));
}
/* a valid UTF-8 sequence at s[i] -> its length; else 0, *end = where the
   invalid part ends, *why = 1 invalid start byte, 2 invalid continuation
   byte, 3 unexpected end of data (CPython's decoder) */
static int utf8_seq(const unsigned char *s, int64_t n, int64_t i, int64_t *end, int *why){
    unsigned c=s[i], lo=0x80, hi=0xBF; int need;
    if(c<0x80) return 1;
    if(c<0xC2 || c>=0xF5){ *end=i+1; *why=1; return 0; }
    if(c<0xE0) need=1;
    else if(c<0xF0){ need=2; if(c==0xE0) lo=0xA0; else if(c==0xED) hi=0x9F; }
    else { need=3; if(c==0xF0) lo=0x90; else if(c==0xF4) hi=0x8F; }
    for(int k=1;k<=need;k++){
        if(i+k>=n){ *end=n; *why=3; return 0; }
        unsigned d=s[i+k];
        if(d<(k==1?lo:0x80) || d>(k==1?hi:0xBF)){ *end=i+k; *why=2; return 0; }
    }
    return need+1;
}
Value mp_decode(const unsigned char *s, int64_t n, int codec, Value errs){
    SBuf o={0}; int64_t i=0;
    while(i<n){
        int64_t end=i+1; int why=0, len;
        if(codec==0) len=utf8_seq(s,n,i,&end,&why);
        else if(s[i]<0x80 || codec==2){ put_utf8(&o,s[i]); i++; continue; }
        else len=0;
        if(len){ sb_put(&o,(const char*)s+i,len); i+=len; continue; }
        int h=errors_of(errs);
        if(h==6 && codec==0 && s[i]==0xED && i+2<n && s[i+1]>=0xA0 && s[i+1]<=0xBF && (s[i+2]&0xC0)==0x80){   /* a surrogate, passed */
            sb_put(&o,(const char*)s+i,3); i+=3; continue; }
        int esc_ok= h==5; for(int64_t k=i;k<end && esc_ok;k++) if(s[k]<0x80) esc_ok=0;
        if(!h || h==4 || h==6 || (h==5 && !esc_ok)){
            free(o.s);
            if(h==4) mp_raise_t(E_TypeError,"don't know how to handle UnicodeDecodeError in error callback");
            const char *reason= codec ? "ordinal not in range(128)" : why==1 ? "invalid start byte" : why==2 ? "invalid continuation byte" : "unexpected end of data";
            unicode_error(E_UnicodeDecodeError,codec_name(codec),mp_bytes((const char*)s,n),i,end,reason);
        }
        if(h==2) sb_puts(&o,"\xEF\xBF\xBD");
        if(h==3) for(int64_t k=i;k<end;k++) sb_printf(&o,"\\x%02x",s[k]);
        if(h==5) for(int64_t k=i;k<end;k++) put_utf8(&o,0xDC00u+s[k]);   /* (bytes 0x80-0xFF as U+DC80-U+DCFF) */
        i=end;
    }
    return sb_value(&o);
}
static void cp_escape(char *out, uint32_t c){ snprintf(out,16, c<0x100 ? "\\x%02x" : c<0x10000 ? "\\u%04x" : "\\U%08x", c); }
/* a surrogate's UTF-8 form at s[i] (ED A0-BF ..) */
static int is_surrogate_at(const char *s, int64_t n, int64_t i){
    return i+2<n && (unsigned char)s[i]==0xED && (unsigned char)s[i+1]>=0xA0;
}
/* UTF-8 of a str with surrogates (which UTF-8 does not allow): as errors says */
static Value encode_utf8_surrogates(const char *s, int64_t n, Value errs){
    SBuf o={0}; int64_t p=0, pos=0;
    while(p<n){
        if(!is_surrogate_at(s,n,p)){ int64_t at=p; mp_utf8_decode(s,n,&p); sb_put(&o,s+at,p-at); pos++; continue; }
        int64_t start=pos, q=p, endpos=pos;                         /* the run of surrogates */
        while(q<n && is_surrogate_at(s,n,q)){ q+=3; endpos++; }
        int h=errors_of(errs);
        if(h==5){ for(int64_t r=p;r<q;r+=3){ int64_t rr=r; uint32_t d=(uint32_t)mp_utf8_decode(s,n,&rr); if(d<0xDC80||d>0xDCFF){ h=0; break; } } }
        if(!h){ free(o.s); unicode_error(E_UnicodeEncodeError,"utf-8",mp_strn(s,n),start,endpos,"surrogates not allowed"); }
        for(int64_t r=p;r<q;r+=3){ int64_t rr=r; uint32_t d=(uint32_t)mp_utf8_decode(s,n,&rr);
            if(h==2) sb_putc(&o,'?');
            else if(h==3){ char esc[16]; cp_escape(esc,d); sb_puts(&o,esc); }
            else if(h==4) sb_printf(&o,"&#%u;",d);
            else if(h==5) sb_putc(&o,(char)(d-0xDC00));
            else if(h==6) sb_put(&o,s+r,3); }
        p=q; pos=endpos;
    }
    Value r=mp_bytes(o.s?o.s:"",o.n); free(o.s); return r;
}
Value mp_encode(const char *s, int64_t n, int codec, Value errs){
    if(codec==0){ for(int64_t i=0;i+2<n;i++) if(is_surrogate_at(s,n,i)) return encode_utf8_surrogates(s,n,errs);
        return mp_bytes(s,n); }
    uint32_t lim= codec==1 ? 0x80 : 0x100;
    SBuf o={0}; int64_t p=0, pos=0;
    while(p<n){
        int64_t at=p; uint32_t c=(uint32_t)mp_utf8_decode(s,n,&p);
        if(c<lim){ sb_putc(&o,(char)c); pos++; continue; }
        int64_t start=pos, q=p, endpos=pos+1;                       /* the run of characters it cannot encode */
        while(q<n){ int64_t r=q; uint32_t d=(uint32_t)mp_utf8_decode(s,n,&r); if(d<lim) break; q=r; endpos++; }
        int h=errors_of(errs);
        if(h==5){ int64_t r=at; while(r<q){ uint32_t d=(uint32_t)mp_utf8_decode(s,n,&r); if(d<0xDC80||d>0xDCFF){ h=0; break; } } }
        if(!h || h==6){
            free(o.s);
            char reason[48]; snprintf(reason,sizeof reason,"ordinal not in range(%u)",lim);
            unicode_error(E_UnicodeEncodeError,codec_name(codec),mp_strn(s,n),start,endpos,reason);
        }
        if(h==5){ int64_t r=at; while(r<q){ uint32_t d=(uint32_t)mp_utf8_decode(s,n,&r); sb_putc(&o,(char)(d-0xDC00)); } }
        if(h==2) for(int64_t k=start;k<endpos;k++) sb_putc(&o,'?');
        if(h==3||h==4){ int64_t r=at; while(r<q){ uint32_t d=(uint32_t)mp_utf8_decode(s,n,&r);
            if(h==4) sb_printf(&o,"&#%u;",d); else { char esc[16]; cp_escape(esc,d); sb_puts(&o,esc); } } }
        (void)at; p=q; pos=endpos;
    }
    Value r=mp_bytes(o.s?o.s:"",o.n); free(o.s); return r;
}

/* ---------------------------------------------------------------- bytes */
static BytesObj *B(Value v){ return AS_BYTES(v); }
static Value bytes_of(const void *s, int64_t n){ return mp_bytes(s,n); }
static Value m_decode(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw);
    Value enc= np>1 ? argv[1] : kwarg(argc,argv,kw,"encoding",v_undef());
    Value errs= np>2 ? argv[2] : kwarg(argc,argv,kw,"errors",v_undef());
    if(enc.k!=V_UNDEF && !IS_STR(enc)) mp_raise_t(E_TypeError,"decode() argument 'encoding' must be str, not %s",mp_type_name(enc));
    if(enc.k!=V_UNDEF && !codec_known(enc)) return py_codec("decode",SELF,enc,errs);
    return mp_decode(B(SELF)->s,B(SELF)->len,enc.k==V_UNDEF?0:mp_codec(enc),errs);
}
/* bytes.hex([sep[, bytes_per_sep]]): sep (one ASCII character) between groups of bytes_per_sep
   bytes, counted from the right (from the left when negative) */
static Value m_hex(int argc, Value *argv, TupleObj *kw){
    BytesObj *b=B(SELF); int np=npos_of(argc,kw);
    Value sepv= np>1 ? argv[1] : kwarg(argc,argv,kw,"sep",v_none());
    int64_t per= np>2 ? mp_index(argv[2],"bytes_per_sep") : mp_index(kwarg(argc,argv,kw,"bytes_per_sep",v_int(1)),"bytes_per_sep");
    int sep=-1;
    if(sepv.k!=V_NONE){ const unsigned char *t; int64_t n;
        if(IS_STR(sepv)){ t=(const unsigned char*)S(sepv)->s; n=S(sepv)->len; }
        else if(IS_BYTES(sepv)){ t=B(sepv)->s; n=B(sepv)->len; }
        else mp_raise_t(E_TypeError,"object of type '%s' has no len()",mp_type_name(sepv));
        for(int64_t i=0;i<n;i++) if(t[i]>=0x80) mp_raise_t(E_ValueError,"sep must be ASCII.");
        if(n!=1) mp_raise_t(E_ValueError,"sep must be length 1.");
        sep=t[0]; }
    int64_t k= per<0 ? -per : per;
    SBuf o={0}; sb_puts(&o,"");
    for(int64_t i=0;i<b->len;i++){
        if(sep>=0 && k && i && (per>0 ? (b->len-i)%k==0 : i%k==0)) sb_putc(&o,(char)sep);
        sb_printf(&o,"%02x",b->s[i]); }
    return sb_value(&o);
}
static Value m_fromhex(int argc, Value *argv, TupleObj *kw){
    (void)kw; Value x=argv[argc-1];
    if(!IS_STR(x)) mp_raise_t(E_TypeError,"fromhex() argument must be str, not %s",mp_type_name(x));
    StrObj *s=S(x); SBuf o={0}; int64_t i=0;
    while(i<s->len){
        unsigned char c=(unsigned char)s->s[i];
        if(c==' '||(c>=9&&c<=13)){ i++; continue; }
        int hi= isxdigit(c) ? (c<='9'?c-'0':(tolower(c)-'a'+10)) : -1;
        if(hi<0){ free(o.s); mp_raise_t(E_ValueError,"non-hexadecimal number found in fromhex() arg at position %lld",(long long)cp_index(s,i)); }
        if(i+1>=s->len){ free(o.s); mp_raise_t(E_ValueError,"fromhex() arg must contain an even number of hexadecimal digits"); }
        unsigned char d=(unsigned char)s->s[i+1]; int lo= isxdigit(d) ? (d<='9'?d-'0':(tolower(d)-'a'+10)) : -1;
        if(lo<0){ free(o.s); if(d==' '||(d>=9&&d<=13)||!d) mp_raise_t(E_ValueError,"fromhex() arg must contain an even number of hexadecimal digits"); mp_raise_t(E_ValueError,"non-hexadecimal number found in fromhex() arg at position %lld",(long long)cp_index(s,i+1)); }
        sb_putc(&o,(char)(hi*16+lo)); i+=2;
    }
    Value r=mp_bytes(o.s?o.s:"",o.n); free(o.s); return r;
}
/* a bytes argument (or one byte given as an int, when byte_ok) */
static void bytes_arg(Value v, int byte_ok, const char **s, int64_t *n, char *one){
    if(IS_BYTES(v)){ *s=(const char*)B(v)->s; *n=B(v)->len; return; }
    if(IS(v,T_buffer)){ *s=(const char*)((BufferObj*)v.u.o)->data; *n=((BufferObj*)v.u.o)->len; return; }
    if(IS_BA(v)){ *s=(const char*)((ByteArrayObj*)v.u.o)->data; *n=((ByteArrayObj*)v.u.o)->len; return; }
    if(byte_ok && IS_INTLIKE(v)){ if(v.u.i<0||v.u.i>255) mp_raise_t(E_ValueError,"byte must be in range(0, 256)"); *one=(char)v.u.i; *s=one; *n=1; return; }
    if(byte_ok) mp_raise_t(E_TypeError,"argument should be integer or bytes-like object, not '%s'",mp_type_name(v));
    mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(v));
}
/* start / end arguments -> [*a, *e) (a > length: nothing fits) */
static void brange(BytesObj *b, int argc, Value *argv, int first, int64_t *a, int64_t *e){
    *a=0; *e=b->len;
    if(argc>first && !IS_NONE(argv[first])) *a=mp_index(argv[first],"slice");
    if(argc>first+1 && !IS_NONE(argv[first+1])) *e=mp_index(argv[first+1],"slice");
    if(*a<0){ *a+=b->len; if(*a<0) *a=0; } if(*a>b->len) *a=b->len+1;
    if(*e<0){ *e+=b->len; if(*e<0) *e=0; } if(*e>b->len) *e=b->len;
}
static Value bfind(int argc, Value *argv, int rev, int raise, const char *name){
    nargs(name,argc,1,3);
    BytesObj *b=B(SELF); const char *s; int64_t n; char one; bytes_arg(argv[1],1,&s,&n,&one);
    int64_t a, e; brange(b,argc,argv,2,&a,&e);
    int64_t f= a>b->len ? -1 : find_bytes((const char*)b->s,a,e,s,n,rev);
    if(f<0 && raise) mp_raise_t(E_ValueError,"subsection not found");
    return v_int(f);
}
static Value m_bfind(int argc, Value *argv, TupleObj *kw){ (void)kw; return bfind(argc,argv,0,0,"find"); }
static Value m_brfind(int argc, Value *argv, TupleObj *kw){ (void)kw; return bfind(argc,argv,1,0,"rfind"); }
static Value m_bindex(int argc, Value *argv, TupleObj *kw){ (void)kw; return bfind(argc,argv,0,1,"index"); }
static Value m_brindex(int argc, Value *argv, TupleObj *kw){ (void)kw; return bfind(argc,argv,1,1,"rindex"); }
static Value m_bcount(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("count",argc,1,3);
    BytesObj *b=B(SELF); const char *s; int64_t n; char one; bytes_arg(argv[1],1,&s,&n,&one);
    int64_t a, e; brange(b,argc,argv,2,&a,&e);
    if(a>b->len || a>e) return v_int(0);
    if(!n) return v_int(e-a+1);
    int64_t c=0; while(1){ int64_t f=find_bytes((const char*)b->s,a,e,s,n,0); if(f<0) break; c++; a=f+n; }
    return v_int(c);
}
static Value bstarts(int argc, Value *argv, int ends, const char *name){
    nargs(name,argc,1,3);
    BytesObj *b=B(SELF); int64_t a, e; brange(b,argc,argv,2,&a,&e);
    Value pre=argv[1];
    if(IS_TUPLE(pre)){
        for(int64_t i=0;i<AS_TUPLE(pre)->len;i++){ Value x[4]={SELF,AS_TUPLE(pre)->items[i],argc>2?argv[2]:v_none(),argc>3?argv[3]:v_none()}; if(mp_truth(bstarts(argc<2?2:argc,x,ends,name))) return v_bool(1); }
        return v_bool(0);
    }
    if(!IS_BYTES(pre) && !IS(pre,T_buffer) && !IS_BA(pre)) mp_raise_t(E_TypeError,"%s first arg must be bytes or a tuple of bytes, not %s",name,mp_type_name(pre));
    const char *s; int64_t n; char one; bytes_arg(pre,0,&s,&n,&one);
    if(a>b->len || e-a<n) return v_bool(a<=b->len && n==0 && e>=a);
    return v_bool(!memcmp(b->s+(ends?e-n:a),s,(size_t)n));
}
static Value m_bstartswith(int argc, Value *argv, TupleObj *kw){ (void)kw; return bstarts(argc,argv,0,"startswith"); }
static Value m_bendswith(int argc, Value *argv, TupleObj *kw){ (void)kw; return bstarts(argc,argv,1,"endswith"); }
static int bspace(unsigned char c){ return c==' '||(c>=9&&c<=13); }
static Value bsplit(int argc, Value *argv, TupleObj *kw, int rev){
    int np=npos_of(argc,kw);
    Value sepv= np>1 ? argv[1] : kwarg(argc,argv,kw,"sep",v_none());
    Value maxv= np>2 ? argv[2] : kwarg(argc,argv,kw,"maxsplit",v_int(-1));
    BytesObj *b=B(SELF); int64_t maxsplit=mp_index(maxv,"maxsplit"), n=b->len; const unsigned char *s=b->s;
    Value l=mp_list(0,NULL), parts=rev?mp_list(0,NULL):l;
    if(IS_NONE(sepv)){
        if(!rev){
            int64_t p=0;
            while(p<n){
                if(bspace(s[p])){ p++; continue; }
                if(maxsplit==0){ int64_t e=n; while(e>p && bspace(s[e-1])) e--; mp_list_append(l,bytes_of(s+p,e-p)); break; }
                int64_t st=p; while(p<n && !bspace(s[p])) p++;
                mp_list_append(l,bytes_of(s+st,p-st)); maxsplit--;
            }
            return l;
        }
        int64_t e=n;
        while(e>0){
            if(bspace(s[e-1])){ e--; continue; }
            if(maxsplit==0){ int64_t a=0; while(a<e && bspace(s[a])) a++; mp_list_append(parts,bytes_of(s+a,e-a)); break; }
            int64_t st=e; while(e>0 && !bspace(s[e-1])) e--;
            mp_list_append(parts,bytes_of(s+e,st-e)); maxsplit--;
        }
    } else {
        const char *sep; int64_t sn; char one; bytes_arg(sepv,0,&sep,&sn,&one);
        if(!sn) mp_raise_t(E_ValueError,"empty separator");
        if(!rev){
            int64_t p=0;
            while(maxsplit!=0){ int64_t f=find_bytes((const char*)s,p,n,sep,sn,0); if(f<0) break; mp_list_append(l,bytes_of(s+p,f-p)); p=f+sn; maxsplit--; }
            mp_list_append(l,bytes_of(s+p,n-p));
            return l;
        }
        int64_t e=n;
        while(maxsplit!=0){ int64_t f=find_bytes((const char*)s,0,e,sep,sn,1); if(f<0) break; mp_list_append(parts,bytes_of(s+f+sn,e-f-sn)); e=f; maxsplit--; }
        mp_list_append(parts,bytes_of(s,e));
    }
    ListObj *pl=AS_LIST(parts); for(int64_t i=pl->len-1;i>=0;i--) mp_list_append(l,pl->items[i]);
    return l;
}
static Value m_bsplit(int argc, Value *argv, TupleObj *kw){ return bsplit(argc,argv,kw,0); }
static Value m_brsplit(int argc, Value *argv, TupleObj *kw){ return bsplit(argc,argv,kw,1); }
static Value bstrip(int argc, Value *argv, int left, int right, const char *name){
    nargs(name,argc,0,1);
    BytesObj *b=B(SELF); const char *cs=NULL; int64_t cn=0; char one;
    if(argc>1 && !IS_NONE(argv[1])) bytes_arg(argv[1],0,&cs,&cn,&one);
    int64_t a=0, e=b->len;
    #define GOES(c) (cs ? memchr(cs,(c),(size_t)cn)!=NULL : bspace(c))
    if(left) while(a<e && GOES(b->s[a])) a++;
    if(right) while(e>a && GOES(b->s[e-1])) e--;
    #undef GOES
    return bytes_of(b->s+a,e-a);
}
static Value m_bstrip(int argc, Value *argv, TupleObj *kw){ (void)kw; return bstrip(argc,argv,1,1,"strip"); }
static Value m_blstrip(int argc, Value *argv, TupleObj *kw){ (void)kw; return bstrip(argc,argv,1,0,"lstrip"); }
static Value m_brstrip(int argc, Value *argv, TupleObj *kw){ (void)kw; return bstrip(argc,argv,0,1,"rstrip"); }
static Value m_bjoin(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("join",argc,1,1); BytesObj *sep=B(SELF); Value l=mp_list_of(argv[1]); SBuf o={0};
    for(int64_t i=0;i<AS_LIST(l)->len;i++){
        Value x=AS_LIST(l)->items[i];
        if(!IS_BYTES(x) && !IS(x,T_buffer) && !IS_BA(x)){ free(o.s); mp_raise_t(E_TypeError,"sequence item %lld: expected a bytes-like object, %s found",(long long)i,mp_type_name(x)); }
        const char *s; int64_t n; char one; bytes_arg(x,0,&s,&n,&one);
        if(i) sb_put(&o,(const char*)sep->s,sep->len);
        sb_put(&o,s,n);
    }
    Value r=mp_bytes(o.s?o.s:"",o.n); free(o.s); return r;
}
static Value m_breplace(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw);
    if(np<3) mp_raise_t(E_TypeError,"replace expected at least 2 arguments, got %d",np-1);
    BytesObj *b=B(SELF); const char *o, *n; int64_t on, nn; char one1, one2;
    bytes_arg(argv[1],0,&o,&on,&one1); bytes_arg(argv[2],0,&n,&nn,&one2);
    int64_t count= np>3 ? mp_index(argv[3],"count") : mp_index(kwarg(argc,argv,kw,"count",v_int(-1)),"count");
    SBuf out={0}; int64_t p=0, k=0;
    if(!on){
        if(count!=0){ sb_put(&out,n,nn); k++; }
        for(int64_t i=0;i<b->len;i++){ sb_putc(&out,(char)b->s[i]); if(count<0||k<count){ sb_put(&out,n,nn); k++; } }
    } else {
        while(count<0||k<count){ int64_t f=find_bytes((const char*)b->s,p,b->len,o,on,0); if(f<0) break; sb_put(&out,(const char*)b->s+p,f-p); sb_put(&out,n,nn); p=f+on; k++; }
        sb_put(&out,(const char*)b->s+p,b->len-p);
    }
    Value r=mp_bytes(out.s?out.s:"",out.n); free(out.s); return r;
}
static Value bcase(Value self, int mode){        /* 0 upper, 1 lower, 2 swapcase, 3 title, 4 capitalize: ASCII letters only */
    BytesObj *b=B(self); Value r=mp_bytes(b->s,b->len); unsigned char *q=B(r)->s; int prev=0;
    for(int64_t i=0;i<b->len;i++){
        unsigned char c=q[i]; int up=c>='A'&&c<='Z', lo=c>='a'&&c<='z';
        switch(mode){
            case 0: if(lo) c-=32; break;
            case 1: if(up) c+=32; break;
            case 2: if(lo) c-=32; else if(up) c+=32; break;
            case 3: if(prev){ if(up) c+=32; } else if(lo) c-=32; prev=up||lo; break;
            case 4: if(i==0){ if(lo) c-=32; } else if(up) c+=32; break;
        }
        q[i]=c;
    }
    return r;
}
static Value m_bupper(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("upper",argc,0,0); return bcase(SELF,0); }
static Value m_blower(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("lower",argc,0,0); return bcase(SELF,1); }
static Value m_bswapcase(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("swapcase",argc,0,0); return bcase(SELF,2); }
static Value m_btitle(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("title",argc,0,0); return bcase(SELF,3); }
static Value m_bcapitalize(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("capitalize",argc,0,0); return bcase(SELF,4); }
static Value bis(Value self, int mode){          /* 0 isalpha 1 isdigit 2 isalnum 3 isspace 4 islower 5 isupper 6 istitle 7 isascii */
    BytesObj *b=B(self); int any=0, prev=0;
    if(!b->len) return v_bool(mode==7);
    for(int64_t i=0;i<b->len;i++){
        unsigned char c=b->s[i]; int up=c>='A'&&c<='Z', lo=c>='a'&&c<='z', dg=c>='0'&&c<='9';
        switch(mode){
            case 0: if(!up&&!lo) return v_bool(0); break;
            case 1: if(!dg) return v_bool(0); break;
            case 2: if(!up&&!lo&&!dg) return v_bool(0); break;
            case 3: if(!bspace(c)) return v_bool(0); break;
            case 4: if(up) return v_bool(0); if(lo) any=1; break;
            case 5: if(lo) return v_bool(0); if(up) any=1; break;
            case 6: if(up){ if(prev) return v_bool(0); prev=any=1; } else if(lo){ if(!prev) return v_bool(0); prev=any=1; } else prev=0; break;
            case 7: if(c>=0x80) return v_bool(0); break;
        }
    }
    return v_bool(mode>=4 && mode<=6 ? any : 1);
}
#define BISFN(name,mode) static Value m_b##name(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs(#name,argc,0,0); return bis(SELF,mode); }
BISFN(isalpha,0) BISFN(isdigit,1) BISFN(isalnum,2) BISFN(isspace,3) BISFN(islower,4) BISFN(isupper,5) BISFN(istitle,6) BISFN(isascii,7)
static Value bpartition(int argc, Value *argv, int rev){
    nargs(rev?"rpartition":"partition",argc,1,1);
    BytesObj *b=B(SELF); const char *s; int64_t n; char one; bytes_arg(argv[1],0,&s,&n,&one);
    if(!n) mp_raise_t(E_ValueError,"empty separator");
    int64_t f=find_bytes((const char*)b->s,0,b->len,s,n,rev); Value r[3];
    if(f<0){ if(rev){ r[0]=bytes_of("",0); r[1]=bytes_of("",0); r[2]=SELF; } else { r[0]=SELF; r[1]=bytes_of("",0); r[2]=bytes_of("",0); } }
    else { r[0]=bytes_of(b->s,f); r[1]=bytes_of(s,n); r[2]=bytes_of(b->s+f+n,b->len-f-n); }
    return mp_tuple(3,r);
}
static Value m_bpartition(int argc, Value *argv, TupleObj *kw){ (void)kw; return bpartition(argc,argv,0); }
static Value m_brpartition(int argc, Value *argv, TupleObj *kw){ (void)kw; return bpartition(argc,argv,1); }
static Value m_bremoveprefix(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("removeprefix",argc,1,1); BytesObj *b=B(SELF); const char *s; int64_t n; char one; bytes_arg(argv[1],0,&s,&n,&one);
    if(n<=b->len && !memcmp(b->s,s,(size_t)n)) return bytes_of(b->s+n,b->len-n); return SELF; }
static Value m_bremovesuffix(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("removesuffix",argc,1,1); BytesObj *b=B(SELF); const char *s; int64_t n; char one; bytes_arg(argv[1],0,&s,&n,&one);
    if(n && n<=b->len && !memcmp(b->s+b->len-n,s,(size_t)n)) return bytes_of(b->s,b->len-n); return SELF; }
static Value m_bsplitlines(int argc, Value *argv, TupleObj *kw){
    int keep= npos_of(argc,kw)>1 ? mp_truth(argv[1]) : mp_truth(kwarg(argc,argv,kw,"keepends",v_bool(0)));
    BytesObj *b=B(SELF); Value l=mp_list(0,NULL); int64_t st=0, p=0;
    while(p<b->len){
        unsigned char c=b->s[p];
        if(c=='\n'||c=='\r'){ int64_t e=p+1; if(c=='\r' && e<b->len && b->s[e]=='\n') e++; mp_list_append(l,bytes_of(b->s+st,(keep?e:p)-st)); st=p=e; continue; }
        p++;
    }
    if(st<b->len) mp_list_append(l,bytes_of(b->s+st,b->len-st));
    return l;
}
/* bytes.center / ljust / rjust (fill: one byte), zfill, expandtabs, translate, maketrans */
static Value bjustify(int argc, Value *argv, int mode, const char *name){
    nargs(name,argc,1,2);
    BytesObj *s=B(SELF); int64_t w=mp_index(argv[1],"width"); unsigned char fill=' ';
    if(argc>2){ const unsigned char *f; int64_t fn;
        if(!mp_byteslike(argv[2],&f,&fn)) mp_raise_t(E_TypeError,"%s() argument 2 must be a byte string of length 1, not %s",name,mp_type_name(argv[2]));
        if(fn!=1) mp_raise_t(E_TypeError,"%s(): argument 2 must be a byte string of length 1, not a %s object of length %lld",name,mp_type_name(argv[2]),(long long)fn);
        fill=f[0]; }
    if(w<=s->len) return mp_bytes(s->s,s->len);
    int64_t padn=w-s->len, left= mode==0 ? 0 : mode==1 ? padn : padn/2 + (padn&w&1);
    SBuf b={0};
    for(int64_t i=0;i<left;i++) sb_putc(&b,(char)fill);
    sb_put(&b,(const char*)s->s,s->len);
    for(int64_t i=0;i<padn-left;i++) sb_putc(&b,(char)fill);
    Value r=mp_bytes(b.s,b.n); free(b.s); return r;
}
static Value m_bljust(int argc, Value *argv, TupleObj *kw){ (void)kw; return bjustify(argc,argv,0,"ljust"); }
static Value m_brjust(int argc, Value *argv, TupleObj *kw){ (void)kw; return bjustify(argc,argv,1,"rjust"); }
static Value m_bcenter(int argc, Value *argv, TupleObj *kw){ (void)kw; return bjustify(argc,argv,2,"center"); }
static Value m_bzfill(int argc, Value *argv, TupleObj *kw){
    (void)kw; nargs("zfill",argc,1,1);
    BytesObj *s=B(SELF); int64_t w=mp_index(argv[1],"width");
    if(w<=s->len) return mp_bytes(s->s,s->len);
    SBuf b={0}; int64_t st=0;
    if(s->len && (s->s[0]=='+'||s->s[0]=='-')){ sb_putc(&b,(char)s->s[0]); st=1; }
    for(int64_t i=0;i<w-s->len;i++) sb_putc(&b,'0');
    sb_put(&b,(const char*)s->s+st,s->len-st);
    Value r=mp_bytes(b.s,b.n); free(b.s); return r;
}
static Value m_bexpandtabs(int argc, Value *argv, TupleObj *kw){
    int64_t ts= npos_of(argc,kw)>1 ? mp_index(argv[1],"tabsize") : mp_index(kwarg(argc,argv,kw,"tabsize",v_int(8)),"tabsize");
    BytesObj *s=B(SELF); SBuf b={0}; int64_t col=0;
    for(int64_t i=0;i<s->len;i++){ unsigned char c=s->s[i];
        if(c=='\t'){ if(ts>0){ int64_t n=ts-col%ts; for(int64_t k=0;k<n;k++) sb_putc(&b,' '); col+=n; } }
        else if(c=='\n'||c=='\r'){ sb_putc(&b,(char)c); col=0; }
        else { sb_putc(&b,(char)c); col++; } }
    Value r=mp_bytes(b.s?b.s:"",b.n); free(b.s); return r;
}
static Value m_btranslate(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw); nargs("translate",np,1,2);
    Value table=argv[1], del= np>2 ? argv[2] : kwarg(argc,argv,kw,"delete",v_undef());
    const unsigned char *t=NULL, *d=NULL; int64_t tn=0, dn=0;
    if(!IS_NONE(table)){
        if(!mp_byteslike(table,&t,&tn)) mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(table));
        if(tn!=256) mp_raise_t(E_ValueError,"translation table must be 256 characters long"); }
    if(del.k!=V_UNDEF && !mp_byteslike(del,&d,&dn)) mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(del));
    unsigned char drop[256]={0}; for(int64_t i=0;i<dn;i++) drop[d[i]]=1;
    BytesObj *s=B(SELF); SBuf b={0};
    for(int64_t i=0;i<s->len;i++){ unsigned char c=s->s[i]; if(drop[c]) continue; sb_putc(&b,(char)(t?t[c]:c)); }
    Value r=mp_bytes(b.s?b.s:"",b.n); free(b.s); return r;
}
static Value m_bmaketrans(int argc, Value *argv, TupleObj *kw){
    (void)kw; int base= argc==3 ? 1 : 0;
    if(argc-base!=2) mp_raise_t(E_TypeError,"maketrans expected 2 arguments, got %d",argc-base);
    const unsigned char *a, *b; int64_t an, bn;
    if(!mp_byteslike(argv[base],&a,&an)) mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(argv[base]));
    if(!mp_byteslike(argv[base+1],&b,&bn)) mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(argv[base+1]));
    if(an!=bn) mp_raise_t(E_ValueError,"maketrans arguments must have same length");
    unsigned char t[256]; for(int i=0;i<256;i++) t[i]=(unsigned char)i;
    for(int64_t i=0;i<an;i++) t[a[i]]=b[i];
    return mp_bytes(t,256);
}

/* ---------------------------------------------------------------- list */
static ListObj *L(Value v){ return AS_LIST(v); }
static Value m_append(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("list.append",argc,1,1); mp_list_append(SELF,argv[1]); return v_none(); }
static Value m_extend(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("list.extend",argc,1,1);
    Value src= argv[1].k==V_OBJ && argv[1].u.o==SELF.u.o ? mp_list(L(SELF)->len,L(SELF)->items) : argv[1];
    if(IS_LIST(src)||IS_TUPLE(src)){ int64_t n= IS_LIST(src)?L(src)->len:AS_TUPLE(src)->len; Value *it= IS_LIST(src)?L(src)->items:AS_TUPLE(src)->items; for(int64_t i=0;i<n;i++) mp_list_append(SELF,it[i]); }
    else { Value it=mp_iter(src), x; while(mp_next(it,&x)) mp_list_append(SELF,x); }
    return v_none(); }
static Value m_insert(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("insert",argc,2,2);
    ListObj *l=L(SELF); int64_t i=mp_index(argv[1],"index");
    if(i<0){ i+=l->len; if(i<0) i=0; } if(i>l->len) i=l->len;
    mp_list_append(SELF,v_none()); l=L(SELF);
    memmove(l->items+i+1,l->items+i,sizeof(Value)*(size_t)(l->len-1-i)); l->items[i]=argv[2];
    return v_none(); }
static Value m_lpop(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("pop",argc,0,1);
    ListObj *l=L(SELF); if(!l->len) mp_raise_t(E_IndexError,"pop from empty list");
    int64_t i= argc>1 ? mp_index(argv[1],"index") : l->len-1;
    if(i<0) i+=l->len;
    if(i<0||i>=l->len) mp_raise_t(E_IndexError,"pop index out of range");
    Value v=l->items[i]; memmove(l->items+i,l->items+i+1,sizeof(Value)*(size_t)(l->len-i-1)); l->len--; return v; }
static int item_eq(Value a, Value b){ return (a.k==V_OBJ && b.k==V_OBJ && a.u.o==b.u.o) || mp_eq(a,b); }
static Value m_remove(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("remove",argc,1,1);
    ListObj *l=L(SELF);
    for(int64_t i=0;i<l->len;i++) if(item_eq(l->items[i],argv[1])){ l=L(SELF); if(i<l->len){ memmove(l->items+i,l->items+i+1,sizeof(Value)*(size_t)(l->len-i-1)); l->len--; } return v_none(); }
    mp_raise_t(E_ValueError,"list.remove(x): x not in list"); }
static Value m_lindex(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("index",argc,1,3);
    int is_list=IS_LIST(SELF); int64_t n= is_list?L(SELF)->len:AS_TUPLE(SELF)->len;
    int64_t a= argc>2 ? mp_index(argv[2],"start") : 0, e= argc>3 ? mp_index(argv[3],"stop") : n;
    if(a<0){ a+=n; if(a<0) a=0; } if(e<0){ e+=n; if(e<0) e=0; } if(e>n) e=n;
    for(int64_t i=a;i<e;i++){ Value x= is_list?L(SELF)->items[i]:AS_TUPLE(SELF)->items[i]; if(item_eq(x,argv[1])) return v_int(i); if(is_list && L(SELF)->len<e) e=L(SELF)->len; }
    mp_raise_t(E_ValueError,is_list?"list.index(x): x not in list":"tuple.index(x): x not in tuple"); }
static Value m_lcount(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("count",argc,1,1);
    int is_list=IS_LIST(SELF); int64_t n= is_list?L(SELF)->len:AS_TUPLE(SELF)->len, c=0;
    for(int64_t i=0;i<n;i++){ Value x= is_list?L(SELF)->items[i]:AS_TUPLE(SELF)->items[i]; if(item_eq(x,argv[1])) c++; }
    return v_int(c); }
static Value m_reverse(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("reverse",argc,0,0); ListObj *l=L(SELF);
    for(int64_t i=0,j=l->len-1;i<j;i++,j--){ Value t=l->items[i]; l->items[i]=l->items[j]; l->items[j]=t; } return v_none(); }
static Value m_lcopy(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("copy",argc,0,0); return mp_list(L(SELF)->len,L(SELF)->items); }
static Value m_lclear(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("clear",argc,0,0); L(SELF)->len=0; return v_none(); }
static Value m_sort(int argc, Value *argv, TupleObj *kw){
    if(npos_of(argc,kw)>1) mp_raise_t(E_TypeError,"sort() takes no positional arguments");
    Value key=kwarg(argc,argv,kw,"key",v_none()), rev=kwarg(argc,argv,kw,"reverse",v_bool(0));
    if(kw) for(int64_t i=0;i<kw->len;i++){ const char *n=mp_cstr(kw->items[i]); if(strcmp(n,"key")&&strcmp(n,"reverse")) mp_raise_t(E_TypeError,"sort() got an unexpected keyword argument '%s'",n); }
    mp_sort(SELF,key,mp_truth(rev));
    return v_none();
}

/* ---------------------------------------------------------------- dict */
static DictObj *D(Value v){ return AS_DICT(v); }
static Value view(Type *t, Value d){ IterObj *v=(IterObj*)mp_alloc(t,sizeof(IterObj)); v->src=d; v->aux=v_undef(); return v_obj(v); }
static Value m_keys(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("keys",argc,0,0); return view(T_dict_keys,SELF); }
static Value m_values(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("values",argc,0,0); return view(T_dict_values,SELF); }
static Value m_items(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("items",argc,0,0); return view(T_dict_items,SELF); }
static Value m_get(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("get",argc,1,2); Value v; if(mp_dict_get(D(SELF),argv[1],&v)) return v; return argc>2?argv[2]:v_none(); }
static Value m_dpop(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("pop",argc,1,2); Value v;
    if(mp_dict_get(D(SELF),argv[1],&v)){ mp_dict_del(D(SELF),argv[1]); return v; }
    if(argc>2) return argv[2];
    mp_raise(mp_exc_args(E_KeyError,mp_tuple(1,&argv[1]))); }
static Value m_popitem(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("popitem",argc,0,0); DictObj *d=D(SELF);
    for(int64_t i=d->nent-1;i>=0;i--) if(d->ent[i].key.k!=V_UNDEF){ Value kv[2]={d->ent[i].key,d->ent[i].val}; mp_dict_del(d,kv[0]); return mp_tuple(2,kv); }
    mp_raise_t(E_KeyError,"popitem(): dictionary is empty"); }
static Value m_setdefault(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("setdefault",argc,1,2); Value v;
    if(mp_dict_get(D(SELF),argv[1],&v)) return v;
    v= argc>2 ? argv[2] : v_none(); mp_dict_set(D(SELF),argv[1],v); return v; }
void mp_dict_update_from(DictObj *d, Value src){
    if(src.k==V_OBJ && src.u.o->type->layout==LY_DICT){ int64_t pos=0; Value k, v; Value copy=mp_list(0,NULL);
        while(mp_dict_next(AS_DICT(src),&pos,&k,&v)){ Value kv[2]={k,v}; mp_list_append(copy,mp_tuple(2,kv)); }
        for(int64_t i=0;i<AS_LIST(copy)->len;i++){ TupleObj *t=AS_TUPLE(AS_LIST(copy)->items[i]); mp_dict_set(d,t->items[0],t->items[1]); }
        return; }
    Value keys;
    if(mp_getattr_opt(src,mp_intern("keys"),&keys)){ Value it=mp_iter(mp_call0(keys)), k; while(mp_next(it,&k)) mp_dict_set(d,k,mp_getitem(src,k)); return; }
    Value it=mp_iter(src), x; int64_t i=0;
    while(mp_next(it,&x)){
        Value l=mp_list_of(x);
        if(AS_LIST(l)->len!=2) mp_raise_t(E_ValueError,"dictionary update sequence element #%lld has length %lld; 2 is required",(long long)i,(long long)AS_LIST(l)->len);
        mp_dict_set(d,AS_LIST(l)->items[0],AS_LIST(l)->items[1]); i++;
    }
}
static Value m_update(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw);
    if(np>2) mp_raise_t(E_TypeError,"update expected at most 1 argument, got %d",np-1);
    if(np==2) mp_dict_update_from(D(SELF),argv[1]);
    if(kw) for(int64_t i=0;i<kw->len;i++) mp_dict_set(D(SELF),kw->items[i],argv[np+i]);
    return v_none();
}
static Value m_dcopy(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("copy",argc,0,0); Value d=mp_dict(); int64_t pos=0; Value k, v; while(mp_dict_next(D(SELF),&pos,&k,&v)) mp_dict_set(AS_DICT(d),k,v); return d; }
static Value m_dclear(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("clear",argc,0,0); DictObj *d=D(SELF); free(d->ent); free(d->idx); d->ent=NULL; d->idx=NULL; d->nent=d->cap=d->used=d->isize=0; return v_none(); }
static Value m_fromkeys(int argc, Value *argv, TupleObj *kw){ (void)kw;
    int base= IS_TYPE(argv[0]) ? 1 : 0;
    Value d=mp_dict(), it=mp_iter(argv[base]), k, v= argc>base+1 ? argv[base+1] : v_none();
    while(mp_next(it,&k)) mp_dict_set(AS_DICT(d),k,v);
    return d; }

/* ---------------------------------------------------------------- set */
static SetObj *ST(Value v){ return (SetObj*)v.u.o; }
static Value as_set(Value v){ if(v.k==V_OBJ && v.u.o->type->layout==LY_SET) return v; Value s=mp_set(T_set); Value it=mp_iter(v), x; while(mp_next(it,&x)) mp_set_add(ST(s),x); return s; }
static Value m_add(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("set.add",argc,1,1); mp_set_add(ST(SELF),argv[1]); return v_none(); }
static Value m_discard(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("discard",argc,1,1); mp_set_del(ST(SELF),argv[1]); return v_none(); }
static Value m_sremove(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("remove",argc,1,1); if(!mp_set_del(ST(SELF),argv[1])) mp_raise(mp_exc_args(E_KeyError,mp_tuple(1,&argv[1]))); return v_none(); }
static Value m_spop(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("pop",argc,0,0); return mp_set_pop(ST(SELF)); }
void mp_set_clear(SetObj *s);
static Value m_sclear(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("clear",argc,0,0); mp_set_clear(ST(SELF)); return v_none(); }
static Value m_scopy(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("copy",argc,0,0); return mp_set_copy(ST(SELF),TYPE(SELF)); }
void mp_set_update(SetObj *s, Value it);
void mp_set_diff_update(SetObj *s, Value other);
void mp_set_xor_update(SetObj *s, Value other);
void mp_set_clear(SetObj *s);
int  mp_set_has_hashed(SetObj *s, Value v, uint64_t h);
void mp_set_add_hashed(SetObj *s, Value v, uint64_t h);
static int is_aset(Value v){ return v.k==V_OBJ && v.u.o->type->layout==LY_SET; }
/* CPython's set_intersection: a set as the binary operator, anything else walked */
static Value set_inter(Value s, Value o){
    if(is_aset(o)) return mp_binop(OP_BitAnd,s,o);
    Value r=mp_set(TYPE(s)), it=mp_iter(o), x;
    while(mp_next(it,&x)){ uint64_t h=mp_hash(x); if(mp_set_has_hashed(ST(s),x,h)) mp_set_add_hashed(ST(r),x,h); }
    return r;
}
static Value m_union(int argc, Value *argv, TupleObj *kw){ (void)kw;
    Value r=mp_set_copy(ST(SELF),TYPE(SELF));
    for(int i=1;i<argc;i++) mp_set_update(ST(r),argv[i]);
    return r; }
static Value m_intersection(int argc, Value *argv, TupleObj *kw){ (void)kw;
    if(argc==1) return mp_set_copy(ST(SELF),TYPE(SELF));
    Value r=SELF; for(int i=1;i<argc;i++) r=set_inter(r,argv[i]);
    return r; }
static Value m_difference(int argc, Value *argv, TupleObj *kw){ (void)kw;
    if(argc==1) return mp_set_copy(ST(SELF),TYPE(SELF));
    Value r;
    if(is_aset(argv[1])) r=mp_binop(OP_Sub,SELF,argv[1]);
    else { r=mp_set_copy(ST(SELF),TYPE(SELF)); mp_set_diff_update(ST(r),argv[1]); }
    for(int i=2;i<argc;i++) mp_set_diff_update(ST(r),argv[i]);
    return r; }
static Value m_symdiff(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("symmetric_difference",argc,1,1);
    Value r=mp_set(TYPE(SELF)); mp_set_update(ST(r),argv[1]); mp_set_xor_update(ST(r),SELF); return r; }
static Value m_supdate(int argc, Value *argv, TupleObj *kw){ (void)kw; for(int i=1;i<argc;i++) mp_set_update(ST(SELF),argv[i]); return v_none(); }
static Value m_iupdate(int argc, Value *argv, TupleObj *kw){ (void)kw;
    Value r=m_intersection(argc,argv,kw); SetObj *x=ST(SELF), *rs=ST(r);
    if(rs==x) return v_none();
    SEnt *t=x->table; int64_t m=x->mask, fl=x->fill, u=x->used;
    x->table=rs->table; x->mask=rs->mask; x->fill=rs->fill; x->used=rs->used; x->hashed=0;
    rs->table=t; rs->mask=m; rs->fill=fl; rs->used=u;
    return v_none(); }
static Value m_dupdate(int argc, Value *argv, TupleObj *kw){ (void)kw; for(int i=1;i<argc;i++) mp_set_diff_update(ST(SELF),argv[i]); return v_none(); }
static Value m_sdupdate(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("symmetric_difference_update",argc,1,1); mp_set_xor_update(ST(SELF),argv[1]); return v_none(); }
static Value m_issubset(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("issubset",argc,1,1); return mp_compare(OP_LtE,SELF,as_set(argv[1])); }
static Value m_issuperset(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("issuperset",argc,1,1); return mp_compare(OP_GtE,SELF,as_set(argv[1])); }
static Value m_isdisjoint(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("isdisjoint",argc,1,1); Value it=mp_iter(argv[1]), x; while(mp_next(it,&x)) if(mp_set_has(ST(SELF),x)) return v_bool(0); return v_bool(1); }

/* ---------------------------------------------------------------- int, float */
static Value m_bit_length(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; uint64_t a= SELF.u.i<0 ? (uint64_t)(-(SELF.u.i+1))+1 : (uint64_t)SELF.u.i; int n=0; while(a){ n++; a>>=1; } return v_int(n); }
static Value m_bit_count(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; uint64_t a= SELF.u.i<0 ? (uint64_t)(-(SELF.u.i+1))+1 : (uint64_t)SELF.u.i; int n=0; while(a){ n+=(int)(a&1); a>>=1; } return v_int(n); }
/* "big" / "little" -> 0 / 1 */
static int byteorder_of(Value ord){
    if(!IS_STR(ord)) mp_raise_t(E_TypeError,"to_bytes() argument 'byteorder' must be str, not %s",mp_type_name(ord));
    if(mp_str_eq_c(ord,"big")) return 0;
    if(mp_str_eq_c(ord,"little")) return 1;
    mp_raise_t(E_ValueError,"byteorder must be either 'little' or 'big'");
}
static Value m_to_bytes(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw);
    int64_t len= np>1 ? mp_index(argv[1],"length") : mp_index(kwarg(argc,argv,kw,"length",v_int(1)),"length");
    Value ord= np>2 ? argv[2] : kwarg(argc,argv,kw,"byteorder",mp_str("big"));
    int sig=mp_truth(kwarg(argc,argv,kw,"signed",v_bool(0)));
    if(len<0) mp_raise_t(E_ValueError,"length argument must be non-negative");
    int little=byteorder_of(ord);
    int64_t v=SELF.u.i;
    if(!sig && v<0) mp_raise_t(E_OverflowError,"can't convert negative int to unsigned");
    if(len<8){
        int fits= !sig ? (len==0 ? v==0 : ((uint64_t)v>>(8*len))==0)
                       : (len==0 ? v==0 : (v>>(8*len-1))==0 || (v>>(8*len-1))==-1);
        if(!fits) mp_raise_t(E_OverflowError,"int too big to convert");
    }
    unsigned char *buf=(unsigned char*)xmalloc((size_t)len+1);
    for(int64_t i=0;i<len;i++){ unsigned char c=(unsigned char)(i<8 ? (uint64_t)v>>(8*i) : (v<0?0xff:0)); buf[little?i:len-1-i]=c; }
    Value r=mp_bytes(buf,len); free(buf); return r;
}
static Value m_from_bytes(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw); int base= IS_TYPE(argv[0]) ? 1 : 0;
    const char *s; int64_t n; char one; bytes_arg(argv[base],0,&s,&n,&one);
    Value ord= np>base+1 ? argv[base+1] : kwarg(argc,argv,kw,"byteorder",mp_str("big"));
    int sig=mp_truth(kwarg(argc,argv,kw,"signed",v_bool(0)));
    int little=byteorder_of(ord);
    uint64_t v=0;                                   /* the low 8 bytes; the rest must only extend them */
    for(int64_t k=0;k<n && k<8;k++) v|=(uint64_t)(unsigned char)s[little?k:n-1-k]<<(8*k);
    int neg= sig && n>0 && ((unsigned char)s[little?(n<8?n-1:7):(n<8?0:n-8)]&0x80);
    for(int64_t k=8;k<n;k++) if((unsigned char)s[little?k:n-1-k]!=(neg?0xff:0)) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)");
    if(!sig && (v>>63)) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)");
    if(neg && n<8) v|=~(uint64_t)0<<(n*8);
    return v_int((int64_t)v);
}
static Value m_conjugate(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; if(IS(SELF,T_complex)) return mp_complex(((ComplexObj*)SELF.u.o)->re,-((ComplexObj*)SELF.u.o)->im); return SELF.k==V_BOOL?v_int(SELF.u.i):SELF; }
static Value m_is_integer(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; if(SELF.k==V_FLOAT) return v_bool(isfinite(SELF.u.f) && SELF.u.f==floor(SELF.u.f)); return v_bool(1); }
static Value m_as_integer_ratio(int argc, Value *argv, TupleObj *kw){
    (void)kw; (void)argc;
    if(SELF.k!=V_FLOAT){ Value r[2]={v_int(SELF.u.i),v_int(1)}; return mp_tuple(2,r); }
    double x=SELF.u.f;
    if(isinf(x)) mp_raise_t(E_OverflowError,"cannot convert Infinity to integer ratio");
    if(isnan(x)) mp_raise_t(E_ValueError,"cannot convert NaN to integer ratio");
    int e; double m=frexp(x,&e); int64_t den=1;
    for(int i=0;i<300 && m!=floor(m);i++){ m*=2; e--; }
    if(e>0){ if(e>62) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)"); m=ldexp(m,e); }
    else { if(-e>62) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)"); den=(int64_t)1<<(-e); }
    int64_t num=(int64_t)m;
    while(den>1 && !(num&1) && !(den&1)){ num/=2; den/=2; }
    Value r[2]={v_int(num),v_int(den)}; return mp_tuple(2,r);
}
static Value m_fhex(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; char b[64]; snprintf(b,sizeof b,"%a",SELF.u.f); return mp_str(b); }
/* float.fromhex(s): [sign] [0x] hex digits [. hex digits] [p exponent], rounded half-even (subnormals too) */
static int hexdig(int c){ return c>='0'&&c<='9' ? c-'0' : c>='a'&&c<='f' ? c-'a'+10 : c>='A'&&c<='F' ? c-'A'+10 : -1; }
static int ci_is(const char *p, size_t n, const char *w){ if(strlen(w)!=n) return 0; for(size_t i=0;i<n;i++) if(tolower((unsigned char)p[i])!=w[i]) return 0; return 1; }
static Value m_ffromhex(int argc, Value *argv, TupleObj *kw){
    (void)kw; if(argc!=2) mp_raise_t(E_TypeError,"fromhex() takes exactly one argument (%d given)",argc-1);
    if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"must be str, not %s",mp_type_name(argv[1]));
    StrObj *str=S(argv[1]); const char *p=str->s, *e=str->s+str->len;
    while(p<e && isspace((unsigned char)*p)) p++;
    while(e>p && isspace((unsigned char)e[-1])) e--;
    int neg=0; double r;
    if(p<e && (*p=='+'||*p=='-')){ neg= *p=='-'; p++; }
    if(ci_is(p,(size_t)(e-p),"inf") || ci_is(p,(size_t)(e-p),"infinity")){ r=neg?-INFINITY:INFINITY; goto done; }
    if(ci_is(p,(size_t)(e-p),"nan")){ r=NAN; goto done; }
    if(e-p>=2 && p[0]=='0' && (p[1]=='x'||p[1]=='X')) p+=2;
    { uint64_t m=0; int sticky=0, ndig=0, nsig=0; int64_t ex=0; int d;
      while(p<e && (d=hexdig(*p))>=0){ p++; ndig++; if(!m && !d) continue; if(nsig<15){ m=m*16+(uint64_t)d; nsig++; } else { ex+=4; if(d) sticky=1; } }
      if(p<e && *p=='.'){ p++;
          while(p<e && (d=hexdig(*p))>=0){ p++; ndig++; if(!m && !d){ ex-=4; continue; } if(nsig<15){ m=m*16+(uint64_t)d; nsig++; ex-=4; } else if(d) sticky=1; } }
      if(!ndig) goto bad;
      if(p<e && (*p=='p'||*p=='P')){ p++; int eneg=0; int64_t pe=0;
          if(p<e && (*p=='+'||*p=='-')){ eneg= *p=='-'; p++; }
          if(!(p<e && isdigit((unsigned char)*p))) goto bad;
          while(p<e && isdigit((unsigned char)*p)){ if(pe<100000000) pe=pe*10+(*p-'0'); p++; }
          ex+= eneg ? -pe : pe; }
      if(p!=e) goto bad;
      if(!m){ r=neg?-0.0:0.0; goto done; }
      int nb=64-__builtin_clzll(m); int64_t top=ex+nb-1;
      if(top>1023) goto big;
      int64_t keep= top>=-1022 ? 53 : 53-(-1022-top);
      int64_t shift=nb-keep;
      if(keep<0){ r=neg?-0.0:0.0; goto done; }
      if(shift>0){
          uint64_t rem= shift>=64 ? m : m&((1ULL<<shift)-1), half=1ULL<<(shift-1);
          m= shift>=64 ? 0 : m>>shift;
          if(rem>half || (rem==half && (sticky || (m&1)))) m++;
          ex+=shift; }
      r=ldexp((double)m,(int)ex);
      if(isinf(r)) goto big;
      if(neg) r=-r; }
done:
    if(IS_TYPE(argv[0]) && (Type*)argv[0].u.o!=T_float) return mp_call1(argv[0],v_float(r));
    return v_float(r);
bad: mp_raise_t(E_ValueError,"invalid hexadecimal floating-point string");
big: mp_raise_t(E_OverflowError,"hexadecimal value too large to represent as a float");
}
/* float.from_number(x): float(x) for numbers only (not str) */
static Value m_ffromnumber(int argc, Value *argv, TupleObj *kw){
    (void)kw; if(argc!=2) mp_raise_t(E_TypeError,"from_number() takes exactly one argument (%d given)",argc-1);
    Value x=argv[1], u=mp_unbox(x);
    double r;
    if(u.k==V_FLOAT) r=u.u.f; else if(IS_INTLIKE(u)) r=(double)u.u.i;
    else { Value m= x.k==V_OBJ ? mp_type_lookup_s(TYPE(x),"__float__") : v_undef(); if(m.k==V_UNDEF && x.k==V_OBJ) m=mp_type_lookup_s(TYPE(x),"__index__");
        if(m.k==V_UNDEF || IS_STR(x) || IS_BYTES(x)) mp_raise_t(E_TypeError,"must be real number, not %s",mp_type_name(x));
        Value v=mp_unbox(mp_call1(m,x)); r= v.k==V_FLOAT ? v.u.f : (double)v.u.i; }
    if(IS_TYPE(argv[0]) && (Type*)argv[0].u.o!=T_float) return mp_call1(argv[0],v_float(r));
    return v_float(r);
}
/* int / float .real .imag .numerator .denominator (as properties: they show in dir()) */
static Value n_real(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value u=mp_unbox(argv[0]); return u.k==V_FLOAT ? u : v_int(u.u.i); }
static Value n_imag(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value u=mp_unbox(argv[0]); return u.k==V_FLOAT ? v_float(0.0) : v_int(0); }
static Value n_one(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; (void)argv; return v_int(1); }
static void add_prop(Type *t, const char *name, NFn get){
    PropertyObj *p=(PropertyObj*)mp_alloc(T_property,sizeof(PropertyObj));
    p->get=mp_native(name,get); p->set=v_none(); p->del=v_none(); p->doc=v_none();
    mp_dict_set(t->dict,mp_intern(name),v_obj(p));
}

/* ---------------------------------------------------------------- generators, coroutines */
static Value m_send(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("send",argc,1,1);
    int done; Value r=mp_gen_send(SELF,argv[1],&done);
    if(done) mp_raise(mp_exc_args(E_StopIteration,IS_NONE(r)?mp_tuple(0,NULL):mp_tuple(1,&r)));
    return r; }
static Value m_throw(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("throw",argc,1,3);
    Value e=argv[1];
    if(argc>2 && !IS_NONE(argv[2]) && IS_TYPE(e)) e=mp_call1(e,argv[2]);
    int done; Value r=mp_gen_throw(SELF,e,&done);
    if(done) mp_raise(mp_exc_args(E_StopIteration,IS_NONE(r)?mp_tuple(0,NULL):mp_tuple(1,&r)));
    return r; }
static Value m_close(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; mp_gen_close(SELF); return v_none(); }
static Value m_gnext(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value v; if(!mp_next(SELF,&v)) mp_raise(mp_exc_args(E_StopIteration,mp_tuple(0,NULL))); return v; }
static Value m_self(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; return SELF; }

/* ---------------------------------------------------------------- object, exceptions, property */
static Value o_init(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_none(); }
static Value o_new(int argc, Value *argv, TupleObj *kw){ (void)kw;
    if(argc<1 || !IS_TYPE(argv[0])) mp_raise_t(E_TypeError,"object.__new__(X): X is not a type object");
    Type *t=AS_TYPE(argv[0]);
    if(t->flags&TF_SUBVAL){ const char *nm=t->qualname?t->qualname->s:t->name->s;      /* (a dict / list ... subclass: its own __new__) */
        mp_raise_t(E_TypeError,"object.__new__(%s) is not safe, use %s.__new__()",nm,nm); }
    return mp_instance(t); }
static Value o_eq(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; return argv[0].k==V_OBJ && argv[1].k==V_OBJ && argv[0].u.o==argv[1].u.o ? v_bool(1) : mp_NotImplemented; }
static Value o_ne(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value r=mp_compare(OP_Eq,argv[0],argv[1]); return v_bool(!mp_truth(r)); }
static Value o_hash(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; uint64_t p=(uint64_t)(uintptr_t)argv[0].u.o; return v_int((int64_t)((p>>4)|(p<<60))); }
static Value o_repr(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Type *t=TYPE(argv[0]); Value m=mp_type_lookup_s(t,"__module__");
    return mp_strf("<%s.%s object at %p>",IS_STR(m)?mp_cstr(m):"builtins",t->qualname?t->qualname->s:t->name->s,(void*)argv[0].u.o); }
static Value o_str(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; return mp_repr(argv[0]); }
static Value o_format(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; if(argc>1 && IS_STR(argv[1]) && AS_STR(argv[1])->len) mp_raise_t(E_TypeError,"unsupported format string passed to %s.__format__",mp_type_name(argv[0])); return mp_tostr(argv[0]); }
static Value o_setattr(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__setattr__",argc,2,2);
    InstObj *o=AS_INST(argv[0]);
    if(argv[0].u.o->type->layout==LY_INSTANCE){ mp_dict_set(o->dict,argv[1],argv[2]); return v_none(); }
    mp_setattr(argv[0],argv[1],argv[2]); return v_none(); }
static Value o_getattribute(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__getattribute__",argc,1,1);
    if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"attribute name must be string, not '%s'",mp_type_name(argv[1]));
    return mp_getattr_generic(argv[0],argv[1]); }
static Value o_delattr(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__delattr__",argc,1,1);
    if(argv[0].u.o->type->layout==LY_INSTANCE){ if(!mp_dict_del(AS_INST(argv[0])->dict,argv[1])) mp_raise_t(E_AttributeError,"'%s' object has no attribute '%s'",mp_type_name(argv[0]),mp_cstr(argv[1])); return v_none(); }
    mp_delattr(argv[0],argv[1]); return v_none(); }
static Value o_init_subclass(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_none(); }
Value mp_builtin_import(int argc, Value *argv, TupleObj *kw);
/* object.__reduce_ex__ / __reduce__ / __getstate__: copyreg's _object_* functions (what pickle and copy use) */
static Value copyreg_call(const char *fn, int argc, Value *argv){
    Value nm=mp_str("copyreg"), mod=mp_builtin_import(1,&nm,NULL);
    return mp_call(mp_getattr_s(mod,fn),argc,argv,NULL);
}
static Value o_reduce_ex(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__reduce_ex__",argc,1,1); return copyreg_call("_object_reduce_ex",2,argv); }
static Value o_reduce(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__reduce__",argc,0,0); return copyreg_call("_object_reduce",1,argv); }
static Value o_getstate(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__getstate__",argc,0,0); return copyreg_call("_object_getstate",1,argv); }
static Value o_lt(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; (void)argv; return mp_NotImplemented; }
static Value e_init(int argc, Value *argv, TupleObj *kw){ ExcObj *x=AS_EXC(argv[0]); int np=argc-1-(kw?(int)kw->len:0);
    if(kw && kw->len){                                                  /* NameError(name=), AttributeError(name=, obj=), ImportError(name=, path=) */
        const char *ok1= mp_isinstance(argv[0],E_NameError)||mp_isinstance(argv[0],E_AttributeError)||mp_isinstance(argv[0],E_ImportError) ? "name" : NULL;
        const char *ok2= mp_isinstance(argv[0],E_AttributeError) ? "obj" : mp_isinstance(argv[0],E_ImportError) ? "path" : NULL;
        if(!x->dict) x->dict=AS_DICT(mp_dict());
        for(int64_t i=0;i<kw->len;i++){ const char *k=mp_cstr(kw->items[i]);
            if(!(ok1 && !strcmp(k,ok1)) && !(ok2 && !strcmp(k,ok2))){
                if(!ok1) mp_raise_t(E_TypeError,"%s() takes no keyword arguments",TYPE(argv[0])->name->s);
                mp_raise_t(E_TypeError,"%s() got an unexpected keyword argument '%s'",TYPE(argv[0])->name->s,k); }
            mp_dict_set(x->dict,kw->items[i],argv[1+np+i]); } }
    if(np>=3 && np<=5 && mp_isinstance(argv[0],E_OSError) && IS_INTLIKE(argv[1])){   /* OSError(errno, strerror, filename[, winerror, filename2]) */
        x->args=mp_tuple(2,argv+1);
        if(!x->dict) x->dict=AS_DICT(mp_dict());
        mp_dict_set_s(x->dict,"filename",argv[3]);
        if(np==5) mp_dict_set_s(x->dict,"filename2",argv[5]);
        return v_none(); }
    x->args=mp_tuple(np,argv+1); return v_none(); }
static Value e_str(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; return mp_exc_str(argv[0]); }
static Value e_repr(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; ExcObj *e=AS_EXC(argv[0]); SBuf b={0};
    sb_puts(&b,TYPE(argv[0])->name->s);
    Value r=mp_repr(e->args); StrObj *s=AS_STR(r);
    if(AS_TUPLE(e->args)->len==1){ sb_putc(&b,'('); Value a=mp_repr(AS_TUPLE(e->args)->items[0]); sb_put(&b,AS_STR(a)->s,AS_STR(a)->len); sb_putc(&b,')'); }
    else sb_put(&b,s->s,s->len);
    return sb_value(&b); }
static Value e_with_tb(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("with_traceback",argc,1,1); AS_EXC(argv[0])->tb=argv[1]; return argv[0]; }
static Value e_add_note(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("add_note",argc,1,1); ExcObj *e=AS_EXC(argv[0]);
    if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"note must be a str, not '%s'",mp_type_name(argv[1]));
    if(e->notes.k==V_UNDEF) e->notes=mp_list(0,NULL); mp_list_append(e->notes,argv[1]); return v_none(); }
static Value p_copy_with(Value p, int slot, Value f){           /* p.getter(f) / .setter(f) / .deleter(f): a copy with f */
    PropertyObj *o=(PropertyObj*)p.u.o, *nn=(PropertyObj*)mp_alloc(T_property,sizeof(PropertyObj));
    nn->get=o->get; nn->set=o->set; nn->del=o->del; nn->doc=o->doc;
    if(slot==0) nn->get=f; else if(slot==1) nn->set=f; else nn->del=f;
    return v_obj(nn);
}
static Value p_getter(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("getter",argc,1,1); return p_copy_with(argv[0],0,argv[1]); }
static Value p_setter(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("setter",argc,1,1); return p_copy_with(argv[0],1,argv[1]); }
static Value p_deleter(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("deleter",argc,1,1); return p_copy_with(argv[0],2,argv[1]); }
static Value p_get(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; PropertyObj *p=(PropertyObj*)argv[0].u.o; if(IS_NONE(argv[1])) return argv[0]; return mp_call1(p->get,argv[1]); }
/* property.__init__ / staticmethod.__init__ / classmethod.__init__ (of a subclass: super().__init__(...)) */
static Value p_init(int argc, Value *argv, TupleObj *kw){
    PropertyObj *p=(PropertyObj*)argv[0].u.o; int np=npos_of(argc,kw);
    p->get= np>1 ? argv[1] : kwarg(argc,argv,kw,"fget",v_none());
    p->set= np>2 ? argv[2] : kwarg(argc,argv,kw,"fset",v_none());
    p->del= np>3 ? argv[3] : kwarg(argc,argv,kw,"fdel",v_none());
    p->doc= np>4 ? argv[4] : kwarg(argc,argv,kw,"doc",v_none());
    if(IS_NONE(p->doc) && !IS_NONE(p->get)){ Value d; if(mp_getattr_opt(p->get,mp_intern("__doc__"),&d)) p->doc=d; }
    return v_none(); }
static Value t_subclasses(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__subclasses__",argc,0,0);
    if(!IS_TYPE(argv[0])) mp_raise_t(E_TypeError,"descriptor '__subclasses__' requires a 'type' object");
    Value s=AS_TYPE(argv[0])->subs; return s.k==V_OBJ ? mp_list(AS_LIST(s)->len,AS_LIST(s)->items) : mp_list(0,NULL); }
static Value o_subclasshook(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; (void)argv; return mp_NotImplemented; }
static Value f_call(int argc, Value *argv, TupleObj *kw){ if(argc<1) mp_raise_t(E_TypeError,"__call__ needs its object"); return mp_call(argv[0],argc-1,argv+1,kw); }
static Value box_init(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__init__",argc,1,1); ((BoxObj*)argv[0].u.o)->v=argv[1]; return v_none(); }
static Value p_set(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; PropertyObj *p=(PropertyObj*)argv[0].u.o; mp_call2(p->set,argv[1],argv[2]); return v_none(); }
/* the built-in types' own dunders (what super().__getitem__(k) ... of a subclass reach): the operation
   with the subclass's methods skipped for self (mp_raw_obj) */
enum { RW_GETITEM, RW_SETITEM, RW_DELITEM, RW_CONTAINS, RW_LEN, RW_ITER, RW_REPR, RW_EQ, RW_NE, RW_HASH, RW_STR };
static Value raw_op(int what, Value self, Value a, Value b){
    Obj *save=mp_raw_obj; Value r=v_none();
    if(self.k==V_OBJ) mp_raw_obj=self.u.o;
    Catch c;
    if(!CATCH_BEGIN(c)){
        switch(what){
            case RW_GETITEM: r=mp_getitem(self,a); break;
            case RW_SETITEM: mp_setitem(self,a,b); break;
            case RW_DELITEM: mp_delitem(self,a); break;
            case RW_CONTAINS: r=v_bool(mp_contains(self,a)); break;
            case RW_LEN: r=v_int(mp_len(self)); break;
            case RW_ITER: r=mp_iter(self); break;
            case RW_REPR: r=mp_repr(self); break;
            case RW_STR: r= self.k==V_OBJ && self.u.o->type->layout==LY_STR ? mp_strn(AS_STR(self)->s,AS_STR(self)->len) : mp_repr(self); break;
            case RW_EQ: case RW_NE:{ Value x=mp_unbox(self), y=mp_unbox(a);
                int same= x.k==V_OBJ && y.k==V_OBJ ? x.u.o->type->layout==y.u.o->type->layout : (x.k!=V_OBJ)==(y.k!=V_OBJ);
                r= same ? v_bool(mp_builtin_eq(x,y)==(what==RW_EQ)) : mp_NotImplemented; break; }
            case RW_HASH: r=v_int((int64_t)mp_hash(mp_unbox(self))); break;
        }
        CATCH_END(c);
    } else { mp_raw_obj=save; mp_raise(mp_catch_exc(&c)); }
    mp_raw_obj=save;
    return r;
}
static Value d_getitem(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__getitem__",argc,1,1); return raw_op(RW_GETITEM,argv[0],argv[1],v_none()); }
static Value d_setitem(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__setitem__",argc,2,2); return raw_op(RW_SETITEM,argv[0],argv[1],argv[2]); }
static Value d_delitem(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__delitem__",argc,1,1); return raw_op(RW_DELITEM,argv[0],argv[1],v_none()); }
static Value d_contains(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__contains__",argc,1,1); return raw_op(RW_CONTAINS,argv[0],argv[1],v_none()); }
static Value d_len(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__len__",argc,0,0); return raw_op(RW_LEN,argv[0],v_none(),v_none()); }
static Value d_iter(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__iter__",argc,0,0); return raw_op(RW_ITER,argv[0],v_none(),v_none()); }
static Value d_repr(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__repr__",argc,0,0); return raw_op(RW_REPR,argv[0],v_none(),v_none()); }
static Value d_str(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__str__",argc,0,0); return raw_op(RW_STR,argv[0],v_none(),v_none()); }
static Value d_eq(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__eq__",argc,1,1); return raw_op(RW_EQ,argv[0],argv[1],v_none()); }
static Value d_ne(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__ne__",argc,1,1); return raw_op(RW_NE,argv[0],argv[1],v_none()); }
static Value d_hash(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__hash__",argc,0,0); return raw_op(RW_HASH,argv[0],v_none(),v_none()); }
/* list / tuple / str / bytes + and *: of plain copies (a subclass's own dunders not asked) */
static Value plain_seq(Value v){
    if(v.k!=V_OBJ || !(v.u.o->type->flags&TF_HEAP)) return v;
    switch(v.u.o->type->layout){
        case LY_LIST: return mp_list(AS_LIST(v)->len,AS_LIST(v)->items);
        case LY_TUPLE: return mp_tuple(AS_TUPLE(v)->len,AS_TUPLE(v)->items);
        case LY_STR: return mp_strn(AS_STR(v)->s,AS_STR(v)->len);
        case LY_BYTES: return mp_bytes(AS_BYTES(v)->s,AS_BYTES(v)->len);
        default: return v;
    }
}
static Value s_add(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__add__",argc,1,1);
    Layout ls=argv[0].u.o->type->layout;
    const char *nm= ls==LY_LIST?"list":ls==LY_TUPLE?"tuple":"str";
    if(!(argv[1].k==V_OBJ && (argv[1].u.o->type->layout==ls || (ls==LY_BYTES && (IS(argv[1],T_buffer) || IS_BA(argv[1])))))){
        if(ls==LY_BYTES) mp_raise_t(E_TypeError,"can't concat %s to bytes",mp_type_name(argv[1]));
        mp_raise_t(E_TypeError,"can only concatenate %s (not \"%s\") to %s",nm,mp_type_name(argv[1]),nm); }
    return mp_binop(OP_Add,plain_seq(argv[0]),plain_seq(argv[1])); }
static Value s_mul(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__mul__",argc,1,1);
    int64_t n=mp_index(argv[1],"__mul__"); return mp_binop(OP_Mult,plain_seq(argv[0]),v_int(n)); }
static Value s_iadd(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__iadd__",argc,1,1);
    Value l=mp_list_of(argv[1]); for(int64_t i=0;i<AS_LIST(l)->len;i++) mp_list_append(argv[0],AS_LIST(l)->items[i]);
    return argv[0]; }
static Value s_reversed(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__reversed__",argc,0,0); return mp_reversed_builtin(argv[0]); }
static Value it_next(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__next__",argc,0,0);
    Value v; if(mp_next(argv[0],&v)) return v;
    mp_raise(mp_exc_args(E_StopIteration,mp_tuple(0,NULL))); }
/* int / float subclasses: the arithmetic of their values (NotImplemented for other kinds of operands) */
static Value num_dunder(int op, int reflected, int argc, Value *argv){
    nargs("operator",argc,1,1);
    Value x=mp_unbox(argv[0]), y=mp_unbox(argv[1]);
    int numy= y.k==V_INT || y.k==V_BOOL || y.k==V_FLOAT;
    if(!numy) return mp_NotImplemented;
    return reflected ? mp_binop(op,y,x) : mp_binop(op,x,y);
}
#define NUMD(name,op,refl) static Value name(int argc, Value *argv, TupleObj *kw){ (void)kw; return num_dunder(op,refl,argc,argv); }
NUMD(n_add,OP_Add,0) NUMD(n_radd,OP_Add,1) NUMD(n_sub,OP_Sub,0) NUMD(n_rsub,OP_Sub,1) NUMD(n_mul,OP_Mult,0) NUMD(n_rmul,OP_Mult,1)
NUMD(n_truediv,OP_Div,0) NUMD(n_rtruediv,OP_Div,1) NUMD(n_floordiv,OP_FloorDiv,0) NUMD(n_rfloordiv,OP_FloorDiv,1)
NUMD(n_mod,OP_Mod,0) NUMD(n_rmod,OP_Mod,1) NUMD(n_pow,OP_Pow,0) NUMD(n_rpow,OP_Pow,1)
NUMD(n_and,OP_BitAnd,0) NUMD(n_rand,OP_BitAnd,1) NUMD(n_or,OP_BitOr,0) NUMD(n_ror,OP_BitOr,1) NUMD(n_xor,OP_BitXor,0) NUMD(n_rxor,OP_BitXor,1)
NUMD(n_lshift,OP_LShift,0) NUMD(n_rlshift,OP_LShift,1) NUMD(n_rshift,OP_RShift,0) NUMD(n_rrshift,OP_RShift,1)
static Value n_cmp(int op, int argc, Value *argv){ nargs("comparison",argc,1,1); Value x=mp_unbox(argv[0]), y=mp_unbox(argv[1]);
    if(!(y.k==V_INT || y.k==V_BOOL || y.k==V_FLOAT)) return mp_NotImplemented; return mp_compare(op,x,y); }
static Value n_lt(int argc, Value *argv, TupleObj *kw){ (void)kw; return n_cmp(OP_Lt,argc,argv); }
static Value n_le(int argc, Value *argv, TupleObj *kw){ (void)kw; return n_cmp(OP_LtE,argc,argv); }
static Value n_gt(int argc, Value *argv, TupleObj *kw){ (void)kw; return n_cmp(OP_Gt,argc,argv); }
static Value n_ge(int argc, Value *argv, TupleObj *kw){ (void)kw; return n_cmp(OP_GtE,argc,argv); }
static Value n_neg(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__neg__",argc,0,0); return mp_unary(OP_USub,mp_unbox(argv[0])); }
static Value n_pos(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__pos__",argc,0,0); return mp_unary(OP_UAdd,mp_unbox(argv[0])); }
static Value n_invert(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__invert__",argc,0,0); return mp_unary(OP_Invert,mp_unbox(argv[0])); }
static Value n_abs(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__abs__",argc,0,0); Value x=mp_unbox(argv[0]);
    if(x.k==V_FLOAT) return v_float(fabs(x.u.f)); return x.u.i<0 ? mp_unary(OP_USub,x) : v_int(x.u.i); }
static Value n_int(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__int__",argc,0,0); Value x=mp_unbox(argv[0]);
    if(x.k==V_FLOAT) return T_int->make(T_int,1,&x,NULL); return v_int(x.u.i); }
static Value n_float(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__float__",argc,0,0); return v_float(mp_float_of(mp_unbox(argv[0]))); }
static Value n_bool(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__bool__",argc,0,0); return v_bool(mp_truth(mp_unbox(argv[0]))); }
static Value n_format(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__format__",argc,1,1); return mp_format(mp_unbox(argv[0]),argv[1]); }
/* type.__setattr__ / type.__delattr__ (a metaclass's super().__setattr__): the class's own dict */
static Value t_setattr(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__setattr__",argc,2,2);
    if(!IS_TYPE(argv[0])) mp_raise_t(E_TypeError,"descriptor '__setattr__' requires a 'type' object");
    Obj *save=mp_raw_obj; mp_raw_obj=argv[0].u.o; Catch c;
    if(!CATCH_BEGIN(c)){ mp_setattr(argv[0],argv[1],argv[2]); CATCH_END(c); } else { mp_raw_obj=save; mp_raise(mp_catch_exc(&c)); }
    mp_raw_obj=save; return v_none(); }
static Value t_delattr(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__delattr__",argc,1,1);
    if(!IS_TYPE(argv[0])) mp_raise_t(E_TypeError,"descriptor '__delattr__' requires a 'type' object");
    Obj *save=mp_raw_obj; mp_raw_obj=argv[0].u.o; Catch c;
    if(!CATCH_BEGIN(c)){ mp_delattr(argv[0],argv[1]); CATCH_END(c); } else { mp_raw_obj=save; mp_raise(mp_catch_exc(&c)); }
    mp_raw_obj=save; return v_none(); }
/* the descriptor protocol of functions, staticmethod and classmethod objects: f.__get__(obj, type) */
static Value fn_get(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__get__",argc,1,2);
    if(IS_NONE(argv[1])) return argv[0];
    MethodObj *m=(MethodObj*)mp_alloc(T_method,sizeof(MethodObj)); m->func=argv[0]; m->self=argv[1]; return v_obj(m); }
static Value sm_get(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__get__",argc,1,2); return ((BoxObj*)argv[0].u.o)->v; }
static Value cm_get(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__get__",argc,1,2);
    Value owner= argc>2 && !IS_NONE(argv[2]) ? argv[2] : v_obj(TYPE(argv[1]));
    MethodObj *m=(MethodObj*)mp_alloc(T_method,sizeof(MethodObj)); m->func=((BoxObj*)argv[0].u.o)->v; m->self=owner; return v_obj(m); }
/* list.__init__(self[, iterable]) / dict.__init__(self, ...) / set.__init__(self[, iterable]): (re)fill self */
static Value m_list_init(int argc, Value *argv, TupleObj *kw){ if(kw && kw->len) mp_raise_t(E_TypeError,"list() takes no keyword arguments");
    if(argc>2) mp_raise_t(E_TypeError,"list expected at most 1 argument, got %d",argc-1);
    ListObj *l=AS_LIST(argv[0]); l->len=0;
    if(argc==2){ Value it=mp_iter(argv[1]), x; while(mp_next(it,&x)) mp_list_append(argv[0],x); }
    return v_none(); }
static Value m_dict_init(int argc, Value *argv, TupleObj *kw){
    if(argc-(kw?(int)kw->len:0)>2) mp_raise_t(E_TypeError,"dict expected at most 1 argument, got %d",argc-1-(kw?(int)kw->len:0));
    Value keep=mp_tuple(argc-1,argv+1);
    Value plain=T_dict->make(T_dict,argc-1,AS_TUPLE(keep)->items,kw);
    int64_t pos=0; Value k, v; while(mp_dict_next(AS_DICT(plain),&pos,&k,&v)) mp_dict_set(AS_DICT(argv[0]),k,v);
    return v_none(); }
static Value m_set_init(int argc, Value *argv, TupleObj *kw){ if(kw && kw->len) mp_raise_t(E_TypeError,"set() takes no keyword arguments");
    if(argc>2) mp_raise_t(E_TypeError,"set expected at most 1 argument, got %d",argc-1);
    SetObj *s=(SetObj*)argv[0].u.o;
    if(argc==2){ Value plain=T_set->make(T_set,1,argv+1,NULL); mp_set_merge(s,(SetObj*)plain.u.o); }
    return v_none(); }
static Value t_mro(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Type *t=AS_TYPE(argv[0]); Value l=mp_list(0,NULL); for(int i=0;i<t->nmro;i++) mp_list_append(l,v_obj(t->mro[i])); return l; }
static Value t_or(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value u[2]={argv[0],argv[1]}; return mp_tuple(2,u); }   /* int | str: a tuple isinstance understands */

/* ---------------------------------------------------------------- files */
static FileObj *F(Value v){ FileObj *f=(FileObj*)v.u.o; return f; }
static void need_open(FileObj *f){ if(f->closed) mp_raise_t(E_ValueError,"I/O operation on closed file."); }
int mp_file_flush(FileObj *f);
static Value m_read(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("read",argc,0,1); FileObj *f=F(SELF); need_open(f);
    if(f->mode!='r') mp_raise_t(E_OSError,"not readable");
    int64_t n= argc>1 && !IS_NONE(argv[1]) ? mp_index(argv[1],"size") : -1;
    int64_t rest=f->len-f->pos;
    if(f->binary){ if(n<0||n>rest) n=rest; Value r=mp_bytes(f->buf+f->pos,n); f->pos+=n; return r; }
    if(n<0 || n>=rest){ Value r=mp_strn(f->buf+f->pos,rest); f->pos=f->len; return r; }
    int64_t p=f->pos; for(int64_t i=0;i<n && p<f->len;i++) mp_utf8_decode(f->buf,f->len,&p);
    Value r=mp_strn(f->buf+f->pos,p-f->pos); f->pos=p; return r; }
int mp_file_readline(Value fv, Value *out){
    FileObj *f=F(fv); need_open(f);
    if(f->pos>=f->len) return 0;
    int64_t e=f->pos; while(e<f->len && f->buf[e]!='\n') e++;
    if(e<f->len) e++;
    *out= f->binary ? mp_bytes(f->buf+f->pos,e-f->pos) : mp_strn(f->buf+f->pos,e-f->pos);
    f->pos=e; return 1;
}
static Value m_readline(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value v; if(!mp_file_readline(SELF,&v)) return F(SELF)->binary?mp_bytes("",0):mp_str(""); return v; }
static Value m_readlines(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value l=mp_list(0,NULL), v; while(mp_file_readline(SELF,&v)) mp_list_append(l,v); return l; }
static Value m_write(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("write",argc,1,1); FileObj *f=F(SELF); need_open(f);
    if(f->mode=='r') mp_raise_t(E_OSError,"not writable");
    const char *s; int64_t n;
    if(f->binary){ char one; bytes_arg(argv[1],0,&s,&n,&one); }
    else { if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"write() argument must be str, not %s",mp_type_name(argv[1])); s=AS_STR(argv[1])->s; n=AS_STR(argv[1])->len; }
    if(f->len+n+1>f->cap){ f->cap=(f->len+n+1)*2; f->buf=(char*)xrealloc(f->buf,(size_t)f->cap); }
    memcpy(f->buf+f->len,s,(size_t)n); f->len+=n;
    return v_int(f->binary ? n : AS_STR(argv[1])->cplen); }
static Value m_writelines(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("writelines",argc,1,1); Value it=mp_iter(argv[1]), x; while(mp_next(it,&x)){ Value a[2]={SELF,x}; m_write(2,a,NULL); } return v_none(); }
static Value m_fclose(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; FileObj *f=F(SELF); if(!f->closed){ mp_file_flush(f); f->closed=1; } return v_none(); }
static Value m_fflush(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; need_open(F(SELF)); return v_none(); }
static Value m_fexit(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; m_fclose(1,argv,NULL); return v_bool(0); }
static Value m_fnext(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; Value v; if(!mp_file_readline(SELF,&v)) mp_raise(mp_exc_args(E_StopIteration,mp_tuple(0,NULL))); return v; }
static Value m_seek(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("seek",argc,1,2); FileObj *f=F(SELF); int64_t p=mp_index(argv[1],"offset"), w= argc>2 ? mp_index(argv[2],"whence") : 0;
    if(w==1) p+=f->pos; else if(w==2) p+=f->len; if(p<0) p=0; if(p>f->len) p=f->len; f->pos=p; return v_int(p); }
static Value m_tell(int argc, Value *argv, TupleObj *kw){ (void)kw; (void)argc; return v_int(F(SELF)->pos); }

/* ---------------------------------------------------------------- registration */
void mp_methods_init(void){
    Type *t=T_str;
    mp_type_add(t,"upper",m_upper); mp_type_add(t,"lower",m_lower); mp_type_add(t,"swapcase",m_swapcase); mp_type_add(t,"title",m_title);
    mp_type_add(t,"capitalize",m_capitalize); mp_type_add(t,"casefold",m_casefold);
    mp_type_add(t,"isalpha",m_isalpha); mp_type_add(t,"isdigit",m_isdigit); mp_type_add(t,"isalnum",m_isalnum); mp_type_add(t,"isspace",m_isspace);
    mp_type_add(t,"islower",m_islower); mp_type_add(t,"isupper",m_isupper); mp_type_add(t,"isdecimal",m_isdecimal); mp_type_add(t,"isascii",m_isascii);
    mp_type_add(t,"isnumeric",m_isnumeric); mp_type_add(t,"istitle",m_istitle); mp_type_add(t,"isprintable",m_isprintable); mp_type_add(t,"isidentifier",m_isidentifier);
    mp_type_add(t,"strip",m_strip); mp_type_add(t,"lstrip",m_lstrip); mp_type_add(t,"rstrip",m_rstrip);
    mp_type_add(t,"split",m_split); mp_type_add(t,"rsplit",m_rsplit); mp_type_add(t,"splitlines",m_splitlines); mp_type_add(t,"join",m_join);
    mp_type_add(t,"find",m_find); mp_type_add(t,"rfind",m_rfind); mp_type_add(t,"index",m_sindex); mp_type_add(t,"rindex",m_srindex); mp_type_add(t,"count",m_scount);
    mp_type_add(t,"replace",m_replace); mp_type_add(t,"startswith",m_startswith); mp_type_add(t,"endswith",m_endswith);
    mp_type_add(t,"ljust",m_ljust); mp_type_add(t,"rjust",m_rjust); mp_type_add(t,"center",m_center); mp_type_add(t,"zfill",m_zfill);
    mp_type_add(t,"partition",m_partition); mp_type_add(t,"rpartition",m_rpartition); mp_type_add(t,"removeprefix",m_removeprefix); mp_type_add(t,"removesuffix",m_removesuffix);
    mp_type_add(t,"expandtabs",m_expandtabs); mp_type_add(t,"encode",m_encode); mp_type_add(t,"format",m_sformat); mp_type_add(t,"format_map",m_format_map);
    mp_type_add(t,"translate",m_translate); mp_type_add(t,"__getnewargs__",m_str_getnewargs);
    { Value mt=mp_native("maketrans",m_maketrans); BoxObj *sm=(BoxObj*)mp_alloc(T_staticmethod,sizeof(BoxObj)); sm->v=mt; mp_dict_set(t->dict,mp_intern("maketrans"),v_obj(sm)); }
    t=T_bytes;
    mp_type_add(t,"decode",m_decode); mp_type_add(t,"hex",m_hex); mp_type_add(t,"find",m_bfind); mp_type_add(t,"rfind",m_brfind); mp_type_add(t,"index",m_bindex); mp_type_add(t,"rindex",m_brindex);
    mp_type_add(t,"count",m_bcount); mp_type_add(t,"startswith",m_bstartswith); mp_type_add(t,"endswith",m_bendswith);
    mp_type_add(t,"split",m_bsplit); mp_type_add(t,"rsplit",m_brsplit); mp_type_add(t,"strip",m_bstrip); mp_type_add(t,"lstrip",m_blstrip); mp_type_add(t,"rstrip",m_brstrip);
    mp_type_add(t,"join",m_bjoin); mp_type_add(t,"replace",m_breplace); mp_type_add(t,"upper",m_bupper); mp_type_add(t,"lower",m_blower); mp_type_add(t,"swapcase",m_bswapcase);
    mp_type_add(t,"title",m_btitle); mp_type_add(t,"capitalize",m_bcapitalize); mp_type_add(t,"isalpha",m_bisalpha); mp_type_add(t,"isdigit",m_bisdigit); mp_type_add(t,"isalnum",m_bisalnum);
    mp_type_add(t,"isspace",m_bisspace); mp_type_add(t,"islower",m_bislower); mp_type_add(t,"isupper",m_bisupper); mp_type_add(t,"istitle",m_bistitle); mp_type_add(t,"isascii",m_bisascii);
    mp_type_add(t,"partition",m_bpartition); mp_type_add(t,"rpartition",m_brpartition); mp_type_add(t,"removeprefix",m_bremoveprefix); mp_type_add(t,"removesuffix",m_bremovesuffix);
    mp_type_add(t,"splitlines",m_bsplitlines); mp_type_add(t,"center",m_bcenter); mp_type_add(t,"ljust",m_bljust); mp_type_add(t,"rjust",m_brjust);
    mp_type_add(t,"zfill",m_bzfill); mp_type_add(t,"expandtabs",m_bexpandtabs); mp_type_add(t,"translate",m_btranslate);
    { Value mt=mp_native("maketrans",m_bmaketrans); BoxObj *sm=(BoxObj*)mp_alloc(T_staticmethod,sizeof(BoxObj)); sm->v=mt; mp_dict_set(t->dict,mp_intern("maketrans"),v_obj(sm));
      if(T_bytearray){ BoxObj *sm2=(BoxObj*)mp_alloc(T_staticmethod,sizeof(BoxObj)); sm2->v=mt; mp_dict_set(T_bytearray->dict,mp_intern("maketrans"),v_obj(sm2)); } }
    { Value fh=mp_native("fromhex",m_fromhex); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=fh; mp_dict_set(t->dict,mp_intern("fromhex"),v_obj(cm)); }
    t=T_list;
    mp_type_add(t,"append",m_append); mp_type_add(t,"extend",m_extend); mp_type_add(t,"insert",m_insert); mp_type_add(t,"pop",m_lpop); mp_type_add(t,"remove",m_remove);
    mp_type_add(t,"index",m_lindex); mp_type_add(t,"count",m_lcount); mp_type_add(t,"reverse",m_reverse); mp_type_add(t,"copy",m_lcopy); mp_type_add(t,"clear",m_lclear);
    mp_type_add(t,"sort",m_sort);
    t=T_tuple; mp_type_add(t,"index",m_lindex); mp_type_add(t,"count",m_lcount);
    t=T_dict;
    mp_type_add(t,"keys",m_keys); mp_type_add(t,"values",m_values); mp_type_add(t,"items",m_items); mp_type_add(t,"get",m_get); mp_type_add(t,"pop",m_dpop);
    mp_type_add(t,"popitem",m_popitem); mp_type_add(t,"setdefault",m_setdefault); mp_type_add(t,"update",m_update); mp_type_add(t,"copy",m_dcopy); mp_type_add(t,"clear",m_dclear);
    { Value fk=mp_native("fromkeys",m_fromkeys); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=fk; mp_dict_set(t->dict,mp_intern("fromkeys"),v_obj(cm)); }
    t=T_set;
    mp_type_add(t,"add",m_add); mp_type_add(t,"discard",m_discard); mp_type_add(t,"remove",m_sremove); mp_type_add(t,"pop",m_spop); mp_type_add(t,"clear",m_sclear);
    mp_type_add(t,"update",m_supdate); mp_type_add(t,"intersection_update",m_iupdate); mp_type_add(t,"difference_update",m_dupdate); mp_type_add(t,"symmetric_difference_update",m_sdupdate);
    for(int k=0;k<2;k++){ t= k==0 ? T_set : T_frozenset;
        mp_type_add(t,"copy",m_scopy); mp_type_add(t,"union",m_union); mp_type_add(t,"intersection",m_intersection); mp_type_add(t,"difference",m_difference);
        mp_type_add(t,"symmetric_difference",m_symdiff); mp_type_add(t,"issubset",m_issubset); mp_type_add(t,"issuperset",m_issuperset); mp_type_add(t,"isdisjoint",m_isdisjoint); }
    t=T_int;
    mp_type_add(t,"bit_length",m_bit_length); mp_type_add(t,"bit_count",m_bit_count); mp_type_add(t,"to_bytes",m_to_bytes); mp_type_add(t,"conjugate",m_conjugate);
    mp_type_add(t,"is_integer",m_is_integer); mp_type_add(t,"as_integer_ratio",m_as_integer_ratio);
    { Value fb=mp_native("from_bytes",m_from_bytes); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=fb; mp_dict_set(t->dict,mp_intern("from_bytes"),v_obj(cm)); }
    add_prop(T_int,"real",n_real); add_prop(T_int,"imag",n_imag); add_prop(T_int,"numerator",n_real); add_prop(T_int,"denominator",n_one);
    add_prop(T_float,"real",n_real); add_prop(T_float,"imag",n_imag);
    { Value f=mp_native("fromhex",m_ffromhex); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=f; mp_dict_set(T_float->dict,mp_intern("fromhex"),v_obj(cm));
      Value g=mp_native("from_number",m_ffromnumber); BoxObj *cn=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cn->v=g; mp_dict_set(T_float->dict,mp_intern("from_number"),v_obj(cn)); }
    t=T_float; mp_type_add(t,"is_integer",m_is_integer); mp_type_add(t,"as_integer_ratio",m_as_integer_ratio); mp_type_add(t,"hex",m_fhex); mp_type_add(t,"conjugate",m_conjugate);
    t=T_complex; mp_type_add(t,"conjugate",m_conjugate);
    for(int k=0;k<2;k++){ t= k==0 ? T_generator : T_coroutine;
        mp_type_add(t,"send",m_send); mp_type_add(t,"throw",m_throw); mp_type_add(t,"close",m_close); }
    mp_asyncgen_init();
    mp_type_add(T_generator,"__next__",m_gnext); mp_type_add(T_generator,"__iter__",m_self);
    mp_type_add(T_coroutine,"__await__",m_self);
    {                                                       /* the built-in value types' __new__ (super().__new__(cls, ...)) and */
        Type *vt[]={T_str,T_bytes,T_tuple,T_list,T_dict,T_set,T_frozenset,T_int,T_float,NULL};   /* list/dict/set.__init__ */
        for(int k=0;vt[k];k++){ Value nw=mp_native("__new__",mp_builtin_new); BoxObj *sm=(BoxObj*)mp_alloc(T_staticmethod,sizeof(BoxObj)); sm->v=nw;
            mp_dict_set(vt[k]->dict,mp_intern("__new__"),v_obj(sm)); }
        mp_type_add(T_list,"__init__",m_list_init); mp_type_add(T_dict,"__init__",m_dict_init); mp_type_add(T_set,"__init__",m_set_init);
        mp_type_add(T_type,"__setattr__",t_setattr); mp_type_add(T_type,"__delattr__",t_delattr);
        mp_type_add(T_function,"__get__",fn_get); mp_type_add(T_staticmethod,"__get__",sm_get); mp_type_add(T_classmethod,"__get__",cm_get);
        Type *ct[]={T_str,T_bytes,T_tuple,T_list,T_dict,T_set,T_frozenset,T_bytearray,NULL};       /* their own dunders, for super() */
        for(int k=0;ct[k];k++){ Type *x=ct[k];
            mp_type_add(x,"__len__",d_len); mp_type_add(x,"__iter__",d_iter); mp_type_add(x,"__contains__",d_contains);
            mp_type_add(x,"__repr__",d_repr); mp_type_add(x,"__eq__",d_eq); mp_type_add(x,"__ne__",d_ne);
            if(x!=T_set && x!=T_frozenset) mp_type_add(x,"__getitem__",d_getitem);
            if(x==T_list || x==T_dict || x==T_bytearray){ mp_type_add(x,"__setitem__",d_setitem); mp_type_add(x,"__delitem__",d_delitem); }
            if(x==T_str || x==T_bytes || x==T_tuple || x==T_frozenset) mp_type_add(x,"__hash__",d_hash); }
        mp_type_add(T_str,"__str__",d_str);
        mp_dict_set_s(T_list->dict,"__hash__",v_none()); mp_dict_set_s(T_dict->dict,"__hash__",v_none()); mp_dict_set_s(T_set->dict,"__hash__",v_none());
        Type *sq[]={T_str,T_bytes,T_tuple,T_list,NULL};
        for(int k=0;sq[k];k++){ mp_type_add(sq[k],"__add__",s_add); mp_type_add(sq[k],"__mul__",s_mul); mp_type_add(sq[k],"__rmul__",s_mul); }
        mp_type_add(T_list,"__iadd__",s_iadd);
        Type *rv[]={T_list,T_dict,T_range,T_dict_keys,T_dict_values,T_dict_items,NULL};
        for(int k=0;rv[k];k++) mp_type_add(rv[k],"__reversed__",s_reversed);
        Type *vw[]={T_range,T_dict_keys,T_dict_values,T_dict_items,NULL};
        for(int k=0;vw[k];k++){ mp_type_add(vw[k],"__len__",d_len); mp_type_add(vw[k],"__iter__",d_iter); mp_type_add(vw[k],"__contains__",d_contains); }
        mp_type_add(T_range,"__getitem__",d_getitem);
        mp_type_add(T_iter,"__iter__",m_self); mp_type_add(T_iter,"__next__",it_next);
        Type *nt[]={T_int,T_float,NULL};
        for(int k=0;nt[k];k++){ Type *x=nt[k];
            mp_type_add(x,"__add__",n_add); mp_type_add(x,"__radd__",n_radd); mp_type_add(x,"__sub__",n_sub); mp_type_add(x,"__rsub__",n_rsub);
            mp_type_add(x,"__mul__",n_mul); mp_type_add(x,"__rmul__",n_rmul); mp_type_add(x,"__truediv__",n_truediv); mp_type_add(x,"__rtruediv__",n_rtruediv);
            mp_type_add(x,"__floordiv__",n_floordiv); mp_type_add(x,"__rfloordiv__",n_rfloordiv); mp_type_add(x,"__mod__",n_mod); mp_type_add(x,"__rmod__",n_rmod);
            mp_type_add(x,"__pow__",n_pow); mp_type_add(x,"__rpow__",n_rpow);
            mp_type_add(x,"__lt__",n_lt); mp_type_add(x,"__le__",n_le); mp_type_add(x,"__gt__",n_gt); mp_type_add(x,"__ge__",n_ge);
            mp_type_add(x,"__eq__",d_eq); mp_type_add(x,"__ne__",d_ne); mp_type_add(x,"__hash__",d_hash); mp_type_add(x,"__repr__",d_repr);
            mp_type_add(x,"__neg__",n_neg); mp_type_add(x,"__pos__",n_pos); mp_type_add(x,"__abs__",n_abs); mp_type_add(x,"__bool__",n_bool);
            mp_type_add(x,"__int__",n_int); mp_type_add(x,"__float__",n_float); mp_type_add(x,"__format__",n_format); }
        mp_type_add(T_int,"__and__",n_and); mp_type_add(T_int,"__rand__",n_rand); mp_type_add(T_int,"__or__",n_or); mp_type_add(T_int,"__ror__",n_ror);
        mp_type_add(T_int,"__xor__",n_xor); mp_type_add(T_int,"__rxor__",n_rxor); mp_type_add(T_int,"__lshift__",n_lshift); mp_type_add(T_int,"__rlshift__",n_rlshift);
        mp_type_add(T_int,"__rshift__",n_rshift); mp_type_add(T_int,"__rrshift__",n_rrshift); mp_type_add(T_int,"__invert__",n_invert); mp_type_add(T_int,"__index__",n_int);
    }
    t=T_object;
    mp_type_add(t,"__init__",o_init); mp_type_add(t,"__eq__",o_eq); mp_type_add(t,"__ne__",o_ne); mp_type_add(t,"__hash__",o_hash);
    mp_type_add(t,"__repr__",o_repr); mp_type_add(t,"__str__",o_str); mp_type_add(t,"__format__",o_format); mp_type_add(t,"__setattr__",o_setattr);
    mp_type_add(t,"__getattribute__",o_getattribute); mp_type_add(t,"__delattr__",o_delattr);
    mp_type_add(t,"__reduce_ex__",o_reduce_ex); mp_type_add(t,"__reduce__",o_reduce); mp_type_add(t,"__getstate__",o_getstate);
    mp_type_add(t,"__lt__",o_lt); mp_type_add(t,"__le__",o_lt); mp_type_add(t,"__gt__",o_lt); mp_type_add(t,"__ge__",o_lt);
    { Value nw=mp_native("__new__",o_new); BoxObj *sm=(BoxObj*)mp_alloc(T_staticmethod,sizeof(BoxObj)); sm->v=nw; mp_dict_set(t->dict,mp_intern("__new__"),v_obj(sm)); }
    { Value sh=mp_native("__subclasshook__",o_subclasshook); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=sh; mp_dict_set(t->dict,mp_intern("__subclasshook__"),v_obj(cm)); }
    mp_type_add(T_function,"__call__",f_call); mp_type_add(T_native,"__call__",f_call); mp_type_add(T_method,"__call__",f_call);
    { Value is=mp_native("__init_subclass__",o_init_subclass); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=is; mp_dict_set(t->dict,mp_intern("__init_subclass__"),v_obj(cm)); }
    t=E_BaseException;
    mp_type_add(t,"__init__",e_init); mp_type_add(t,"__str__",e_str); mp_type_add(t,"__repr__",e_repr); mp_type_add(t,"with_traceback",e_with_tb); mp_type_add(t,"add_note",e_add_note);
    mp_type_add(T_property,"__init__",p_init); mp_type_add(T_staticmethod,"__init__",box_init); mp_type_add(T_classmethod,"__init__",box_init);
    t=T_property; mp_type_add(t,"getter",p_getter); mp_type_add(t,"setter",p_setter); mp_type_add(t,"deleter",p_deleter); mp_type_add(t,"__get__",p_get); mp_type_add(t,"__set__",p_set);
    mp_type_add(T_type,"__subclasses__",t_subclasses);
    mp_type_add(T_type,"mro",t_mro); mp_type_add(T_type,"__or__",t_or); mp_type_add(T_type,"__ror__",t_or);
    { Value tn=mp_native("__new__",mp_type_new_m); mp_dict_set_s(T_type->dict,"__new__",tn);       /* (static: super().__new__(mcls, ...)) */
      mp_type_add(T_type,"__init__",mp_type_init_m); mp_type_add(T_type,"__call__",mp_type_call_m); }
    t=T_file;
    mp_type_add(t,"read",m_read); mp_type_add(t,"readline",m_readline); mp_type_add(t,"readlines",m_readlines); mp_type_add(t,"write",m_write); mp_type_add(t,"writelines",m_writelines);
    mp_type_add(t,"close",m_fclose); mp_type_add(t,"flush",m_fflush); mp_type_add(t,"__enter__",m_self); mp_type_add(t,"__exit__",m_fexit);
    mp_type_add(t,"__iter__",m_self); mp_type_add(t,"__next__",m_fnext); mp_type_add(t,"seek",m_seek); mp_type_add(t,"tell",m_tell);
}
