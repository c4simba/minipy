/* ========================= Interpreter: tracebacks =========================
   The source lines of tracebacks, with carets under the failing part
   (CPython 3.13+); formatting and printing exceptions. */

#include "run.h"

static DictObj *sources;
void mp_remember_source(const char *file, const char *src){
    static Value keep;
    if(!sources){ keep=mp_dict(); sources=AS_DICT(keep); mp_gc_add_root(&keep); }
    mp_dict_set(sources,mp_str(file),mp_str(src));
}
static void source_line(SBuf *b, const char *file, int line){
    Value s;
    if(!sources || !mp_dict_get(sources,mp_str(file),&s)) return;
    const char *p=mp_cstr(s);
    for(int l=1;l<line && *p;p++) if(*p=='\n') l++;
    while(*p==' '||*p=='\t') p++;
    const char *e=p; while(*e && *e!='\n' && *e!='\r') e++;
    if(e>p){ sb_puts(b,"    "); sb_put(b,p,e-p); sb_putc(b,'\n'); }
}
/* line `line` of a file the interpreter ran (with its newline; "" if unknown): linecache's */
Value mp_source_getline(const char *file, int line){
    Value s;
    if(!sources || line<0 || !mp_dict_get(sources,mp_str(file),&s)) return mp_str("");
    if(line==0) return s;                                            /* (0: all of it) */
    const char *p=mp_cstr(s);
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
static int anc_find(CodeObj *co, int ip){
    int lo=0, hi=co->nanc-1;
    while(lo<=hi){ int m=(lo+hi)/2; int v=co->anc[7*m]; if(v==ip) return m; if(v<ip) lo=m+1; else hi=m-1; }
    return -1;
}
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
/* the source of traceback entry (file, line) at instruction ip of co: CPython's lines and carets */
static void frame_source(SBuf *out, const char *file, int line, CodeObj *co, int ip){
    Value src;
    if(!sources || !mp_dict_get(sources,mp_str(file),&src)) return;
    int eline=line, col=-1, ecol=-1;
    if(co && co->pos && ip>=0 && ip<co->ncode){ eline=co->pos[3*ip]; col=co->pos[3*ip+1]; ecol=co->pos[3*ip+2]; if(eline<line) eline=line; }
    if(eline-line>200) eline=line;
    const char *p=mp_cstr(src);
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
    int ai= co && co->anc ? anc_find(co,ip) : -1, special=-1;
    if(ai>=0){ int *r=co->anc+7*ai, kind=r[1];
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
        for(int64_t i=l->len-1;i>=0;i--){
            TupleObj *t=AS_TUPLE(l->items[i]);
            sb_printf(b,"  File \"%s\", line %lld, in %s\n",mp_cstr(t->items[0]),(long long)t->items[1].u.i,mp_cstr(t->items[2]));
            if(t->len>=5) frame_source(b,mp_cstr(t->items[0]),(int)t->items[1].u.i,IS(t->items[3],T_code)?(CodeObj*)t->items[3].u.o:NULL,(int)t->items[4].u.i);
            else source_line(b,mp_cstr(t->items[0]),(int)t->items[1].u.i);
        }
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
    SBuf b={0}; PCtx pc={0,0}; format_exc(&b,e,&pc,0); return sb_value(&b);
}
/* the running frames, outermost first: (file, line, function) (traceback.extract_stack) */
Value mp_stack_list(void){
    Value l=mp_list(0,NULL);
    for(Frame *f=mp_ts->frame;f;f=f->back){
        int ip= f->ip>0 ? f->ip-1 : 0;
        Value ent[5]={mp_str(f->code->file),v_int(f->code->ncode?f->code->lines[ip<f->code->ncode?ip:f->code->ncode-1]:f->code->firstline),v_obj(f->code->name),
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
    mp_flush_stdout();
    mp_write_err(b.s?b.s:"",b.n);
    free(b.s);
}

