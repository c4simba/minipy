/* ========================= Interpreter, methods: codecs =========================
   str.encode / bytes.decode: utf-8, latin-1, ascii and the code pages,
   error handlers. */

#include "methods.h"

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
int codec_known(Value enc){
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

