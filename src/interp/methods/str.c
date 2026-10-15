/* ========================= Interpreter, methods: str =========================
   Code points over UTF-8, character classes and case (the Unicode
   tables), the methods of str. */

#include "methods.h"

#include "interp/unicode.h"
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
void put_utf8(SBuf *b, uint32_t c){ char t[4]; int m=mp_utf8_encode(t,c); sb_put(b,t,m); }
int64_t cp_index(StrObj *s, int64_t off){
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
int64_t find_bytes(const char *h, int64_t hs, int64_t he, const char *n, int64_t nl, int rev){
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

/* an encoding the interpreter does not have itself: _codecs_more.py's (code pages, utf-16, utf-32), through codecs.py */
Value py_codec(const char *fn, Value obj, Value enc, Value errs){
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
        Value r; Catch ct; volatile int found=0;             /* (volatile: read after a longjmp) */
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
/* the methods of str (mp_methods_init) */
void mp_str_methods_init(void){
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
}

