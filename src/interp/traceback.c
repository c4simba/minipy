/* ========================= Interpreter: tracebacks =========================
   The source lines of tracebacks, with carets under the failing part
   (CPython 3.13+); formatting and printing exceptions. Sources are not kept:
   a traceback reads its files again, as CPython's linecache does (a module
   of the standard library: unpacked again from plainpy.lib); only the text
   of compile() / exec() / the REPL, which no file holds, is remembered. */

#include "run.h"

static DictObj *sources;
void mp_remember_source(const char *file, const char *src){
    static Value keep;
    if(!sources){ keep=mp_dict(); sources=AS_DICT(keep); mp_gc_add_root(&keep); }
    mp_dict_set(sources,mp_str(file),mp_str(src));
}
/* the text of a file a traceback names, or NULL (the last one read is kept
   for the next frames, until the traceback is printed) */
static char *last_file, *last_text;
static Value last_code;                           /* (last_text compiled again with the places: places_of) */
static void forget_text(void){ free(last_file); free(last_text); last_file=last_text=NULL; last_code=v_none(); }
static const char *file_text(const char *file){
    Value s;
    if(sources && mp_dict_get(sources,mp_str(file),&s)) return mp_cstr(s);
    if(last_file && !strcmp(last_file,file)) return last_text;
    char *t=mp_file_source(file);
    if(!t) return NULL;
    forget_text(); last_file=xstrdup2(file); last_text=t;
    return t;
}
static void source_line(SBuf *b, const char *file, int line){
    const char *p=file_text(file);
    if(!p) return;
    for(int l=1;l<line && *p;p++) if(*p=='\n') l++;
    while(*p==' '||*p=='\t') p++;
    const char *e=p; while(*e && *e!='\n' && *e!='\r') e++;
    if(e>p){ sb_puts(b,"    "); sb_put(b,p,e-p); sb_putc(b,'\n'); }
}
/* line `line` of a file the interpreter ran (with its newline; "" if unknown): linecache's */
Value mp_source_getline(const char *file, int line){
    const char *p= line<0 ? NULL : file_text(file);
    if(!p) return mp_str("");
    if(line==0) return mp_str(p);                                    /* (0: all of it) */
    for(int l=1;l<line && *p;p++) if(*p=='\n') l++;
    if(!*p) return mp_str("");
    const char *e=p; while(*e && *e!='\n') e++;
    SBuf b={0}; sb_put(&b,p,e-p); sb_putc(&b,'\n'); return sb_value(&b);
}

/* ---------------------------------------------------------------- traceback source lines with carets (CPython 3.13+) */
typedef struct { uint32_t *c; int n; } CLine;              /* a line as code points */
static CLine cline(const char *s, int64_t n){
    CLine l; l.c=(uint32_t*)xmalloc(sizeof(uint32_t)*(size_t)(n+1)); l.n=0; int64_t p=0;
    while(p<n) l.c[l.n++]=(uint32_t)mp_utf8_decode(s,n,&p);
    return l;
}
static void cput(SBuf *b, const uint32_t *c, int n){ for(int i=0;i<n;i++){ char t[4]; int m=mp_utf8_encode(t,c[i]); sb_put(b,t,m); } }
static int cspace(uint32_t c){ return c==' '||c=='\t'||c=='\f'||c=='\v'||c=='\r'||c=='\n'; }
/* bytes [0, off) of a UTF-8 line: how many code points */
static int chars_of(const char *s, int64_t n, int64_t off){ if(off>n) off=n; int k=0; int64_t p=0; while(p<off){ mp_utf8_decode(s,n,&p); k++; } return k; }
typedef struct { CLine *L; int nl; int endoff; } Seg;
static int seg_len(Seg *g, int i){ return i==g->nl-1 ? g->endoff : g->L[i].n; }
static int seg_next_valid(Seg *g, int *li, int *ci){ while(*li<g->nl && *ci>=seg_len(g,*li)){ *ci=0; (*li)++; } return *li<g->nl && *ci<seg_len(g,*li); }
/* from (li, ci): the first char stop() accepts, skipping comments and line continuations */
static int seg_until(Seg *g, int *li, int *ci, int mode){
    for(int guard=0;guard<100000;guard++){
        if(!seg_next_valid(g,li,ci)) return 0;
        uint32_t ch=g->L[*li].c[*ci];
        if(ch=='\\' || ch=='#'){ *ci=0; (*li)++; continue; }
        int stop= mode==0 ? !cspace(ch) && ch!=')' : mode==1 ? ch=='[' : ch=='(';
        if(stop) return 1;
        (*ci)++;
    }
    return 0;
}
static int lead_ws(const char *s, int64_t n){ int k=0; while(k<n && (s[k]==' '||s[k]=='\t'||s[k]=='\f')) k++; return k; }
/* the code object of co's text with the carets' places: co, or the one of file's text compiled again
   that stands at the same place (qualified name, first line, as many instructions) - NULL if none */
static CodeObj *find_code(CodeObj *in, CodeObj *co){
    if(in->firstline==co->firstline && in->ncode==co->ncode && !strcmp(in->qualname->s,co->qualname->s)) return in;
    for(int i=0;i<in->nconsts;i++) if(IS(in->consts[i],T_code)){ CodeObj *r=find_code((CodeObj*)in->consts[i].u.o,co); if(r) return r; }
    return NULL;
}
static CodeObj *places_of(CodeObj *co, const char *file){
    int el,c,ec;
    if(!co || !co->loc || co->loc[0]==1 || !co->ncode) return co;
    if(mp_code_place(co,0,&el,&c,&ec)) return co;
    const char *text=file_text(file);
    if(!text || !last_text || text!=last_text) return co;     /* (only a file's text: compiled again) */
    if(last_code.k!=V_OBJ){
        static int rooted; if(!rooted){ rooted=1; last_code=v_none(); mp_gc_add_root(&last_code); }
        CodeObj *m=mp_compile_places(text,file);
        if(!m) return co;
        last_code=v_obj(m);
    }
    CodeObj *r=find_code((CodeObj*)last_code.u.o,co);
    return r ? r : co;
}
/* the source of traceback entry (file, line) at instruction ip of co: CPython's lines and carets */
static void frame_source(SBuf *out, const char *file, int line, CodeObj *co, int ip){
    const char *p=file_text(file);
    if(!p) return;
    co=places_of(co,file);
    p=file_text(file);                                          /* (places_of may have read it again) */
    if(!p) return;
    int eline=line, col=-1, ecol=-1;
    if(co && mp_code_place(co,ip,&eline,&col,&ecol)){ if(eline<line) eline=line; }
    if(eline-line>200) eline=line;
    for(int l=1;l<line && *p;p++) if(*p=='\n') l++;
    int nl=eline-line+1;
    const char **ls=(const char**)xmalloc(sizeof(char*)*(size_t)nl); int64_t *ln=(int64_t*)xmalloc(sizeof(int64_t)*(size_t)nl);
    for(int i=0;i<nl;i++){ const char *e=p; while(*e && *e!='\n') e++;
        ls[i]=p; int64_t n=e-p; while(n>0 && cspace((unsigned char)p[n-1])) n--; ln[i]=n;   /* (rstripped) */
        p= *e ? e+1 : e; }
    int blank=1; for(int i=0;i<nl;i++) if(ln[i]) blank=0;
    if(blank){ free(ls); free(ln); return; }
    if(col<0 || ecol<0){                                       /* no columns: the first line, stripped */
        int k=lead_ws(ls[0],ln[0]); if(ln[0]>k){ sb_puts(out,"    "); sb_put(out,ls[0]+k,ln[0]-k); sb_putc(out,'\n'); }
        free(ls); free(ln); return; }
    /* textwrap.dedent: the common leading whitespace of the lines that are not blank */
    int64_t mlen=-1; const char *mref=NULL;
    for(int i=0;i<nl;i++){ if(!ln[i]) continue; int k=lead_ws(ls[i],ln[i]);
        if(mlen<0){ mlen=k; mref=ls[i]; continue; }
        int64_t j=0; while(j<mlen && j<k && ls[i][j]==mref[j]) j++; mlen=j; }
    if(mlen<0) mlen=0;
    CLine *L=(CLine*)xmalloc(sizeof(CLine)*(size_t)nl);
    for(int i=0;i<nl;i++) L[i]= ln[i] ? cline(ls[i]+mlen,ln[i]-mlen) : cline("",0);
    int start=chars_of(ls[0],ln[0],col)-(int)mlen, end=chars_of(ls[nl-1],ln[nl-1],ecol)-(int)mlen;
    if(start<0) start=0;
    if(end<0) end=0;
    if(end>L[nl-1].n) end=L[nl-1].n;
    /* the anchors (~~^~~): from what the compiler kept */
    int has=0, ll=0, lc=0, rl=0, rc=0; uint32_t prim='^', sec='^';
    int anc[7], special=-1;
    if(co && mp_code_anchor(co,ip,anc+1)){ int *r=anc, kind=r[1];
        Seg g={L,nl,end};
        int li=r[2]-line; int ci= li>=0 && li<nl ? chars_of(ls[li],ln[li],r[3])-(int)mlen : 0;
        special=r[6];
        if(li>=0 && li<nl){
            if(kind==1){
                if(seg_until(&g,&li,&ci,0)){
                    int rli=r[4]-line, rci= rli>=0 && rli<nl ? chars_of(ls[rli],ln[rli],r[5])-(int)mlen : 0;
                    int right=ci+1;
                    if(right<L[li].n && (rli>li || right<rci) && !cspace(L[li].c[right]) && L[li].c[right]!='\\' && L[li].c[right]!='#') right++;
                    has=1; ll=li; lc=ci; rl=li; rc=right; prim='~'; }
            } else if(kind==2 || kind==3){
                if(seg_until(&g,&li,&ci,kind==2?1:2)){ has=1; ll=li; lc=ci; rl=nl-1; rc=end; prim='~'; } }
        } }
    int show;
    if(special>=0 && lead_ws(ls[0],ln[0])==special) show=0;          /* return f(...) / x = f(...): CPython shows no carets */
    else if(has) show=1;
    else { show=0; for(int i=0;i<start && i<L[0].n;i++) if(!cspace(L[0].c[i])) show=1;
        for(int i=end;i<L[nl-1].n;i++) if(!cspace(L[nl-1].c[i])) show=1; }
    /* the lines that matter: first, last, around the anchors */
    char *sig=(char*)xmalloc((size_t)nl+2); memset(sig,0,(size_t)nl+2); sig[0]=1; sig[nl-1]=1;
    if(has){ for(int d=-1;d<=1;d++){ if(ll+d>=0 && ll+d<nl) sig[ll+d]=1; if(rl+d>=0 && rl+d<nl) sig[rl+d]=1; } }
    SBuf res={0}; int prev=-1;
    for(int k=0;k<nl;k++){ if(!sig[k]) continue;
        int outk[2], no=0;
        if(prev>=0){ int diff=k-prev; if(diff==2) outk[no++]=k-1; else if(diff>2) sb_printf(&res,"...<%d lines>...\n",diff-1); }
        outk[no++]=k;
        for(int q=0;q<no;q++){ int x=outk[q];
            cput(&res,L[x].c,L[x].n); sb_putc(&res,'\n');
            if(!show) continue;
            int sp=0; while(sp<L[x].n && cspace(L[x].c[sp])) sp++;
            int nc= x==nl-1 ? end : L[x].n;
            for(int c=0;c<nc;c++){
                if(c<sp || (x==0 && c<start)) sb_putc(&res,' ');
                else if(has && (x>ll || (x==ll && c>=lc)) && (x<rl || (x==rl && c<rc))) sb_putc(&res,(char)sec);
                else sb_putc(&res,(char)prim); }
            sb_putc(&res,'\n'); }
        prev=k; }
    /* textwrap.indent(textwrap.dedent(result), '    ', every line) */
    int64_t rm=-1; const char *rref=NULL;
    for(const char *q=res.s;q && q<res.s+res.n;){ const char *e=q; while(e<res.s+res.n && *e!='\n') e++;
        int k=0; while(q+k<e && (q[k]==' '||q[k]=='\t')) k++;
        if(q+k<e){ if(rm<0){ rm=k; rref=q; } else { int64_t j=0; while(j<rm && j<k && q[j]==rref[j]) j++; rm=j; } }
        q=e+1; }
    if(rm<0) rm=0;
    for(const char *q=res.s;q && q<res.s+res.n;){ const char *e=q; while(e<res.s+res.n && *e!='\n') e++;
        sb_puts(out,"    "); int k=0; while(q+k<e && (q[k]==' '||q[k]=='\t')) k++;
        if(q+k<e) sb_put(out,q+rm,e-q-rm);
        sb_putc(out,'\n'); q=e+1; }
    free(res.s); free(sig);
    for(int i=0;i<nl;i++) free(L[i].c);
    free(L); free(ls); free(ln);
}
/* sys._frame_source(file, line, code, ip): those lines as one string (traceback.py) */
Value mp_frame_source(const char *file, int line, Value code, int ip){
    SBuf b={0}; frame_source(&b,file,line,IS(code,T_code)?(CodeObj*)code.u.o:NULL,ip); return sb_value(&b);
}
/* CPython's layout: exception groups print their members inside a frame of | and +--- */
typedef struct { int depth, need_close; } PCtx;
static void emit_lines(SBuf *b, PCtx *c, const char *text, int64_t n, char margin){
    const char *p=text, *end=text+n;
    while(p<end){ const char *q=p; while(q<end && *q!='\n') q++;
        for(int i=0;i<2*c->depth;i++) sb_putc(b,' ');
        if(c->depth){ sb_putc(b,margin); sb_putc(b,' '); }
        sb_put(b,p,q-p); sb_putc(b,'\n'); p= q<end ? q+1 : q; }
}
static void print_one(SBuf *b, Value e);
static void format_exc(SBuf *b, Value e, PCtx *c, int chain_depth){
    ExcObj *x=AS_EXC(e);
    if(chain_depth<16){
        SBuf m={0};
        if(x->cause.k==V_OBJ){ format_exc(b,x->cause,c,chain_depth+1); sb_puts(&m,"\nThe above exception was the direct cause of the following exception:\n\n"); }
        else if(x->context.k==V_OBJ && !x->suppress){ format_exc(b,x->context,c,chain_depth+1); sb_puts(&m,"\nDuring handling of the above exception, another exception occurred:\n\n"); }
        if(m.n) emit_lines(b,c,m.s,m.n,'|');
        free(m.s);
    }
    Value excs=v_undef();
    if(E_BaseExceptionGroup && mp_isinstance(e,E_BaseExceptionGroup)){ Catch k; if(!CATCH_BEGIN(k)){ excs=mp_getattr_s(e,"exceptions"); CATCH_END(k); } else { mp_catch_exc(&k); excs=v_undef(); } }
    if(!IS_TUPLE(excs)){ SBuf one={0}; print_one(&one,e); emit_lines(b,c,one.s?one.s:"",one.n,'|'); free(one.s); return; }
    int top= c->depth==0; if(top) c->depth++;
    SBuf one={0}; print_one(&one,e);
    if(one.n>=35 && !strncmp(one.s,"Traceback (most recent call last):\n",35)){       /* its own heading, then the frames */
        emit_lines(b,c,"Exception Group Traceback (most recent call last):",50,top?'+':'|');
        emit_lines(b,c,one.s+35,one.n-35,'|'); }
    else emit_lines(b,c,one.s?one.s:"",one.n,'|');
    free(one.s);
    TupleObj *t=AS_TUPLE(excs); int64_t n= t->len<=15 ? t->len : 16;
    c->need_close=0;
    for(int64_t i=0;i<n;i++){ int last= i==n-1; if(last) c->need_close=1;
        for(int k=0;k<2*c->depth;k++) sb_putc(b,' ');
        if(i<15) sb_printf(b,"%s+---------------- %lld ----------------\n",i==0?"+-":"  ",(long long)i+1);
        else sb_printf(b,"  +---------------- ... ----------------\n");
        c->depth++;
        if(i<15) format_exc(b,t->items[i],c,chain_depth);
        else { char m[96]; snprintf(m,sizeof m,"and %lld more exception%s",(long long)(t->len-15),t->len-15>1?"s":""); emit_lines(b,c,m,(int64_t)strlen(m),'|'); }
        if(last && c->need_close){ for(int k=0;k<2*c->depth;k++) sb_putc(b,' '); sb_puts(b,"+------------------------------------\n"); c->need_close=0; }
        c->depth--; }
    if(top) c->depth=0;
}
static void print_one(SBuf *b, Value e){
    ExcObj *x=AS_EXC(e);
    if(x->tb.k==V_OBJ && AS_LIST(x->tb)->len){
        sb_puts(b,"Traceback (most recent call last):\n");
        ListObj *l=AS_LIST(x->tb);
        TupleObj *prev=NULL; int64_t same=0;                      /* as CPython: a frame repeated more than 3 times in a row -> */
        for(int64_t i=l->len-1;i>=0;i--){                         /* "[Previous line repeated N more times]" */
            TupleObj *t=AS_TUPLE(l->items[i]);
            if(!prev || strcmp(mp_cstr(prev->items[0]),mp_cstr(t->items[0])) || prev->items[1].k!=t->items[1].k || prev->items[1].u.i!=t->items[1].u.i || strcmp(mp_cstr(prev->items[2]),mp_cstr(t->items[2]))){
                if(same>3) sb_printf(b,"  [Previous line repeated %lld more time%s]\n",(long long)(same-3),same-3>1?"s":"");
                prev=t; same=0; }
            if(++same>3) continue;
            if(t->items[1].k!=V_INT){ sb_printf(b,"  File \"%s\", in %s\n",mp_cstr(t->items[0]),mp_cstr(t->items[2])); continue; }   /* (no line numbers kept: --strip-lines) */
            sb_printf(b,"  File \"%s\", line %lld, in %s\n",mp_cstr(t->items[0]),(long long)t->items[1].u.i,mp_cstr(t->items[2]));
            if(t->len>=5) frame_source(b,mp_cstr(t->items[0]),(int)t->items[1].u.i,IS(t->items[3],T_code)?(CodeObj*)t->items[3].u.o:NULL,(int)t->items[4].u.i);
            else source_line(b,mp_cstr(t->items[0]),(int)t->items[1].u.i);
        }
        if(same>3) sb_printf(b,"  [Previous line repeated %lld more time%s]\n",(long long)(same-3),same-3>1?"s":"");
    }
    Type *t=TYPE(e);
    Value mod=mp_type_lookup_s(t,"__module__");
    if(IS_STR(mod) && strcmp(mp_cstr(mod),"builtins") && strcmp(mp_cstr(mod),"__main__")) sb_printf(b,"%s.",mp_cstr(mod));
    sb_puts(b,t->qualname?t->qualname->s:t->name->s);
    Value s=mp_tostr(e), sg=mp_exc_suggestion(e,1);
    if(AS_STR(s)->len){ sb_puts(b,": "); sb_put(b,AS_STR(s)->s,AS_STR(s)->len); }
    sb_put(b,AS_STR(sg)->s,AS_STR(sg)->len);
    sb_putc(b,'\n');
}
/* the text mp_print_exception writes (sys._format_exception: the traceback module's) */
Value mp_format_exception(Value e){
    SBuf b={0}; PCtx pc={0,0}; format_exc(&b,e,&pc,0); forget_text(); return sb_value(&b);
}
/* the running frames, outermost first: (file, line, function) (traceback.extract_stack) */
Value mp_stack_list(void){
    Value l=mp_list(0,NULL);
    for(Frame *f=mp_ts->frame;f;f=f->back){
        int ip= f->ip>0 ? f->ip-1 : 0;
        Value ent[5]={mp_str(f->code->file),mp_code_lineno(f->code,ip),v_obj(f->code->name),
                      v_obj(f->code),v_int(ip<f->code->ncode?ip:-1)};
        mp_list_append(l,mp_tuple(5,ent)); }
    ListObj *x=AS_LIST(l); for(int64_t i=0,j=x->len-1;i<j;i++,j--){ Value t=x->items[i]; x->items[i]=x->items[j]; x->items[j]=t; }
    return l;
}
void mp_print_exception(Value e){
    SBuf b={0};
    Catch c;
    if(!CATCH_BEGIN(c)){ PCtx pc={0,0}; format_exc(&b,e,&pc,0); CATCH_END(c); }
    else { mp_catch_exc(&c); sb_puts(&b,"(error printing the exception)\n"); }
    forget_text();
    mp_flush_stdout();
    mp_write_err(b.s?b.s:"",b.n);
    free(b.s);
}

