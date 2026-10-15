/* ========================= Full Python: parser =========================
   Recursive descent over py_lex.c's tokens, following the grammar of
   Python 3.14 (Grammar/python.gram): every statement including match and
   type aliases, decorators, PEP 695 type parameters, positional-only and
   keyword-only parameters, walrus, star targets, comprehensions (async
   too), f-strings and t-strings with nested replacement fields, `except A,
   B:` (PEP 758). The tree is CPython's `ast` (py_ast.h); no symbol tables
   here - the compiler works them out. Errors are SyntaxErrors with a line. */

#include "py_lex.h"
#include <setjmp.h>
#include <ctype.h>

typedef struct {
    PyLexer *lx; PyTok *t; int n, i;
    const char *src; int len;
    jmp_buf *jb;
    char *err; int err_line, err_col;
} P;

/* ---------------------------------------------------------------- helpers */
PyNode *py_node(PyKind k, int line, int col){
    PyNode *n=(PyNode*)xmalloc(sizeof(PyNode)); memset(n,0,sizeof *n);
    n->kind=k; n->line=line; n->col=col; return n;
}
void py_list_add(PyList *l, PyNode *n){
    if(l->n==l->cap){ l->cap=l->cap?l->cap*2:4; l->v=(PyNode**)xrealloc(l->v,sizeof(PyNode*)*(size_t)l->cap); }
    l->v[l->n++]=n;
}
static PyTok *cur(P *p){ return &p->t[p->i]; }
static PyTok *peek(P *p, int k){ int j=p->i+k; return &p->t[j<p->n?j:p->n-1]; }
static void fail_at(P *p, PyTok *t, const char *msg){
    if(!p->err){ p->err=xstrdup2(msg); p->err_line=t->line; p->err_col=t->col; }
    longjmp(*p->jb,1);
}
static void fail(P *p, const char *msg){ fail_at(p,cur(p),msg); }
static PyNode *nd(P *p, PyKind k){ PyTok *t=cur(p); return py_node(k,t->line,t->col); }
/* the node ends where the last token taken ends (fin_if: unless it has its end already) */
static PyNode *fin(P *p, PyNode *n){ if(n && p->i>0){ PyTok *t=&p->t[p->i-1]; n->end_line=t->end_line; n->end_col=t->end_col; } return n; }
static PyNode *fin_if(P *p, PyNode *n){ if(n && !n->end_line) fin(p,n); return n; }
static PyNode *nd_at(PyKind k, PyNode *from){ return py_node(k,from->line,from->col); }
static int is_op(P *p, int op){ return cur(p)->kind==PT_OP && cur(p)->op==op; }
static int is_op_at(P *p, int k, int op){ PyTok *t=peek(p,k); return t->kind==PT_OP && t->op==op; }
static int is_kw(P *p, int kw){ return cur(p)->kind==PT_NAME && cur(p)->op==kw; }
static int accept_op(P *p, int op){ if(is_op(p,op)){ p->i++; return 1; } return 0; }
static int accept_kw(P *p, int kw){ if(is_kw(p,kw)){ p->i++; return 1; } return 0; }
static void expect_op(P *p, int op, const char *what){ if(!accept_op(p,op)){ char m[96]; snprintf(m,sizeof m,"expected '%s'",what); fail(p,m); } }
static void expect_kw(P *p, int kw, const char *what){ if(!accept_kw(p,kw)){ char m[96]; snprintf(m,sizeof m,"expected '%s'",what); fail(p,m); } }
/* a NAME that is not a hard keyword (soft keywords are names) */
static int is_name(P *p){ return cur(p)->kind==PT_NAME && (cur(p)->op==KW_NONE || cur(p)->op>KW_SOFT); }
static int is_name_at(P *p, int k){ PyTok *t=peek(p,k); return t->kind==PT_NAME && (t->op==KW_NONE || t->op>KW_SOFT); }
static char *tok_text(P *p, PyTok *t){ return xstrndup2(p->src+t->start,t->end-t->start); }
static char *expect_name(P *p){ if(!is_name(p)) fail(p,"expected a name"); char *s=tok_text(p,cur(p)); p->i++; return s; }
#define expect_name_any expect_name
static int is_id_char_fs(int c){ return isalnum(c)||c=='_'; }

typedef PyNode *(*ParseFn)(P*);
/* run fn speculatively: 1 and its result, or 0 with the position restored */
static int attempt(P *p, ParseFn fn, PyNode **out){
    int save=p->i; jmp_buf jb, *prev=p->jb; char *perr=p->err;
    p->jb=&jb; p->err=NULL;
    if(setjmp(jb)){ p->jb=prev; p->i=save; free(p->err); p->err=perr; return 0; }
    *out=fn(p);
    p->jb=prev; p->err=perr;
    return 1;
}

static PyNode *expression(P *p);
static PyNode *named_expression(P *p);
static PyNode *star_expressions(P *p);
static PyNode *star_named_expression(P *p);
static PyNode *bitwise_or(P *p);
static PyNode *disjunction(P *p);
static PyNode *yield_expr(P *p);
static PyNode *primary(P *p);
static void block(P *p, PyList *body);
static PyNode *statement_list(P *p, PyList *out);
static PyNode *parse_fstring_tokens(P *p, int first, int last);
static void call_args(P *p, PyList *args, PyList *kws, int close_op);
static PyNode *pattern(P *p);

/* ---------------------------------------------------------------- constants */
static PyConst *konst(PyConstKind k){ PyConst *c=(PyConst*)xmalloc(sizeof(PyConst)); memset(c,0,sizeof *c); c->kind=k; return c; }

static PyNode *number(P *p){
    PyTok *t=cur(p); PyNode *n=nd(p,PK_Constant);
    char *s=(char*)xmalloc((size_t)(t->end-t->start)+1); int k=0;
    for(int i=t->start;i<t->end;i++) if(p->src[i]!='_') s[k++]=p->src[i];
    s[k]=0;
    PyConstKind kind=PC_Int;
    if(k && (s[k-1]=='j'||s[k-1]=='J')){ kind=PC_Complex; s[--k]=0; }
    else if(!(s[0]=='0' && k>1 && strchr("xXoObB",s[1])) && (strchr(s,'.')||strchr(s,'e')||strchr(s,'E'))) kind=PC_Float;
    n->k=konst(kind); n->k->text=s;
    p->i++;
    return n;
}

/* code points */
typedef struct { uint32_t *v; int n, cap; } U32;
static void u32_add(U32 *b, uint32_t c){ if(b->n==b->cap){ b->cap=b->cap?b->cap*2:32; b->v=(uint32_t*)xrealloc(b->v,sizeof(uint32_t)*(size_t)b->cap); } b->v[b->n++]=c; }
/* one UTF-8 character at s[*i] (invalid bytes as themselves) */
static uint32_t utf8_next(const char *s, int len, int *i){
    unsigned char c=(unsigned char)s[*i];
    int n= c<0x80?1 : (c>>5)==6?2 : (c>>4)==14?3 : (c>>3)==30?4 : 1;
    if(*i+n>len) n=1;
    uint32_t v= n==1?c : n==2?(c&31u) : n==3?(c&15u) : (c&7u);
    for(int k=1;k<n;k++) v=(v<<6)|((unsigned char)s[*i+k]&63u);
    *i+=n; return v;
}
static int hexval(int c){ return isdigit(c)?c-'0' : (c>='a'&&c<='f')?c-'a'+10 : (c>='A'&&c<='F')?c-'A'+10 : -1; }

/* Decode the escapes of src[a..b) into code points (or bytes). \N{name}
   needs the Unicode database: the constant is then also kept as ASCII
   escape text (PyConst.text) for the C code to decode at run time. */
typedef struct { U32 u; int named; char *esc; size_t esclen, esccap; } Dec;
static void esc_put(Dec *d, const char *s, size_t n){
    if(d->esclen+n+1>d->esccap){ d->esccap=(d->esclen+n+1)*2; d->esc=(char*)xrealloc(d->esc,d->esccap); }
    memcpy(d->esc+d->esclen,s,n); d->esclen+=n; d->esc[d->esclen]=0;
}
static void dec_char(Dec *d, uint32_t c){
    u32_add(&d->u,c);
    char b[16]; if(c>=32 && c<127 && c!='\\'){ b[0]=(char)c; esc_put(d,b,1); } else { snprintf(b,sizeof b,"\\U%08x",c); esc_put(d,b,10); }
}
static void decode(P *p, PyTok *t, int a, int b, int raw, int bytes, Dec *d){
    const char *s=p->src;
    for(int i=a;i<b;){
        unsigned char c=(unsigned char)s[i];
        if(c=='\r'){ dec_char(d,'\n'); i++; if(i<b && s[i]=='\n') i++; continue; }   /* newlines are \n */
        if(c!='\\' || raw){
            if(raw && c=='\\' && i+1<b && s[i+1]=='\r'){ dec_char(d,'\\'); dec_char(d,'\n'); i+=2; if(i<b && s[i]=='\n') i++; continue; }
            if(bytes){ if(c>=0x80) fail_at(p,t,"bytes can only contain ASCII literal characters"); dec_char(d,c); i++; }
            else dec_char(d,utf8_next(s,b,&i));
            continue;
        }
        if(i+1>=b){ dec_char(d,'\\'); i++; continue; }
        char e=s[i+1]; i+=2;
        switch(e){
            case '\n': break;
            case '\r': if(i<b && s[i]=='\n') i++; break;
            case '\\': dec_char(d,'\\'); break;
            case '\'': dec_char(d,'\''); break;
            case '"': dec_char(d,'"'); break;
            case 'a': dec_char(d,7); break;
            case 'b': dec_char(d,8); break;
            case 'f': dec_char(d,12); break;
            case 'n': dec_char(d,10); break;
            case 'r': dec_char(d,13); break;
            case 't': dec_char(d,9); break;
            case 'v': dec_char(d,11); break;
            case 'x':{ int h1=i<b?hexval(s[i]):-1, h2=i+1<b?hexval(s[i+1]):-1;
                if(h1<0||h2<0) fail_at(p,t,bytes?"invalid \\x escape":"(unicode error) truncated \\xXX escape");
                dec_char(d,(uint32_t)(h1*16+h2)); i+=2; break; }
            case 'u': case 'U':
                if(bytes){ dec_char(d,'\\'); dec_char(d,(unsigned char)e); break; }
                { int n=e=='u'?4:8; uint32_t v=0;
                  for(int k=0;k<n;k++){ int h=i+k<b?hexval(s[i+k]):-1; if(h<0) fail_at(p,t,"(unicode error) truncated \\uXXXX escape"); v=v*16+(uint32_t)h; }
                  if(v>0x10FFFF) fail_at(p,t,"(unicode error) illegal Unicode character");
                  dec_char(d,v); i+=n; break; }
            case 'N':
                if(bytes){ dec_char(d,'\\'); dec_char(d,'N'); break; }
                { if(i>=b || s[i]!='{') fail_at(p,t,"(unicode error) malformed \\N character escape");
                  int j=i; while(j<b && s[j]!='}') j++;
                  if(j>=b) fail_at(p,t,"(unicode error) malformed \\N character escape");
                  d->named=1; u32_add(&d->u,0xFFFD);
                  esc_put(d,"\\N",2); esc_put(d,s+i,(size_t)(j-i+1));
                  i=j+1; break; }
            default:
                if(e>='0' && e<='7'){
                    uint32_t v=(uint32_t)(e-'0'); int k=0;
                    while(k<2 && i<b && s[i]>='0' && s[i]<='7'){ v=v*8+(uint32_t)(s[i]-'0'); i++; k++; }
                    if(bytes) v&=255;
                    dec_char(d,v); break;
                }
                dec_char(d,'\\');                           /* unknown escape: kept */
                i--; break;
        }
    }
}

/* prefix and body of a string token */
typedef struct { int raw, fmt, tmpl, bytes, u, body, bodyend; } StrTok;
static StrTok strtok_info(P *p, PyTok *t){
    StrTok st; memset(&st,0,sizeof st);
    int i=t->start;
    while(p->src[i]!='\'' && p->src[i]!='"'){
        int c=tolower((unsigned char)p->src[i]);
        if(c=='r') st.raw=1; else if(c=='f') st.fmt=1; else if(c=='t') st.tmpl=1; else if(c=='b') st.bytes=1; else if(c=='u') st.u=1;
        i++;
    }
    char q=p->src[i];
    int triple= i+2<t->end && p->src[i+1]==q && p->src[i+2]==q && t->end-i>=6;
    st.body=i+(triple?3:1); st.bodyend=t->end-(triple?3:1);
    return st;
}

/* ---- f-strings and t-strings */
typedef struct { P *p; PyTok *t; int raw, tmpl; PyList *vals; Dec lit; } FCtx;
static void f_flush(FCtx *f, int line, int col){
    if(!f->lit.u.n && !f->lit.named) return;
    PyNode *c=py_node(PK_Constant,line,col); c->k=konst(PC_Str);
    c->k->u=f->lit.u.v; c->k->ulen=f->lit.u.n;
    if(f->lit.named){ c->k->text=f->lit.esc; } else free(f->lit.esc);
    py_list_add(f->vals,c);
    memset(&f->lit,0,sizeof f->lit);
}
static int f_body(FCtx *f, int i, int end, int spec);
/* a replacement field: i just after its '{' -> after its '}' */
static int f_field(FCtx *f, int i, int end){
    P *p=f->p;
    PyLexer sub; memset(&sub,0,sizeof sub); sub.line_starts=p->lx->line_starts; sub.nlines=p->lx->nlines;
    int stop=0;
    if(py_lex(&sub,p->src,end,i,1,&stop)){ PyTok tt=*f->t; py_pos(p->lx,i,&tt.line,&tt.col); fail_at(p,&tt,sub.error); }
    if(sub.n==1) fail_at(p,f->t,"f-string: valid expression required before '}'");
    P q=*p; q.t=sub.v; q.n=sub.n; q.i=0; q.lx=&sub; q.err=NULL;
    jmp_buf jb2; q.jb=&jb2;
    if(setjmp(jb2)){                     /* (q changed after setjmp: read it from memory) */
        volatile P *vq=&q;
        if(!p->err){ p->err=vq->err; p->err_line=vq->err_line; p->err_col=vq->err_col; }
        longjmp(*p->jb,1);
    }
    PyNode *e= is_kw(&q,KW_yield) ? yield_expr(&q) : star_expressions(&q);
    if(cur(&q)->kind!=PT_END) fail_at(&q,cur(&q),"f-string: expecting '}'");
    int text_start=i, text_end=stop;
    int conv=-1; PyNode *spec=NULL; int debug=0;
    int j=stop;
    int debug_end=j, debug_skip=-1, debug_skip_end=-1;  /* the debug text: the expression, '=', whitespace (not comments) */
    if(p->src[j]=='='){
        debug=1; j++;
        for(;;){
            while(j<end && (p->src[j]==' '||p->src[j]=='\t'||p->src[j]=='\n'||p->src[j]=='\r')) j++;
            if(j<end && p->src[j]=='#'){ debug_skip=j; while(j<end && p->src[j]!='\n') j++; debug_skip_end=j; continue; }
            break;
        }
        debug_end=j;
    }
    if(p->src[j]=='!'){
        j++;
        if(j>=end || !strchr("sra",p->src[j]) || is_id_char_fs(p->src[j+1])) fail_at(p,f->t,"f-string: invalid conversion character: expected 's', 'r', or 'a'");
        conv=p->src[j]; j++;
        while(j<end && (p->src[j]==' '||p->src[j]=='\t'||p->src[j]=='\n'||p->src[j]=='\r')) j++;
    }
    if(p->src[j]==':'){
        PyNode *js=py_node(PK_JoinedStr,f->t->line,f->t->col);
        FCtx g; memset(&g,0,sizeof g); g.p=p; g.t=f->t; g.raw=f->raw; g.tmpl=0; g.vals=&js->L[0];
        j=f_body(&g,j+1,end,1);
        f_flush(&g,f->t->line,f->t->col);
        spec=js;
    }
    if(j>=end || p->src[j]!='}') fail_at(p,f->t,"f-string: expecting '}'");
    if(debug){
        int k=text_start;
        while(k<debug_end){ if(k==debug_skip){ k=debug_skip_end; continue; } dec_char(&f->lit,utf8_next(p->src,debug_end,&k)); }
        if(conv<0 && !spec) conv='r';
    }
    f_flush(f,e->line,e->col);
    PyNode *fv;
    if(f->tmpl){
        fv=nd_at(PK_Interpolation,e); fv->n[0]=e; fv->op=conv; fv->n[1]=spec;
        PyConst *s=konst(PC_Str); Dec d; memset(&d,0,sizeof d);
        int te=text_end; while(te>text_start && (p->src[te-1]==' '||p->src[te-1]=='\t'||p->src[te-1]=='\n'||p->src[te-1]=='\r')) te--;
        int k=text_start; while(k<te) u32_add(&d.u,utf8_next(p->src,te,&k));
        s->u=d.u.v; s->ulen=d.u.n; fv->k=s;
    } else { fv=nd_at(PK_FormattedValue,e); fv->n[0]=e; fv->op=conv; fv->n[1]=spec; }
    py_list_add(f->vals,fv);
    free(sub.v);
    return j+1;
}
/* literal text and fields up to end (spec: up to the '}' closing the field) */
static int f_body(FCtx *f, int i, int end, int spec){
    P *p=f->p; const char *s=p->src;
    while(i<end){
        char c=s[i];
        if(c=='{'){
            if(!spec && i+1<end && s[i+1]=='{'){ dec_char(&f->lit,'{'); i+=2; continue; }
            i=f_field(f,i+1,end); continue;
        }
        if(c=='}'){
            if(spec) return i;
            if(i+1<end && s[i+1]=='}'){ dec_char(&f->lit,'}'); i+=2; continue; }
            fail_at(p,f->t,"f-string: single '}' is not allowed");
        }
        /* a run of literal text: up to the next brace */
        int j=i;
        while(j<end && s[j]!='{' && s[j]!='}'){
            if(s[j]=='\\' && !f->raw && j+2<end && s[j+1]=='N' && s[j+2]=='{'){ j+=3; while(j<end && s[j]!='}') j++; if(j<end) j++; continue; }
            if(s[j]=='\\' && j+1<end && s[j+1]!='{' && s[j+1]!='}') j+=2; else j++;
        }
        if(j>end) j=end;
        decode(p,f->t,i,j,f->raw,0,&f->lit);
        i=j;
    }
    if(spec) fail_at(p,f->t,"f-string: expecting '}'");
    return i;
}

/* adjacent string tokens: one Constant, or a JoinedStr / TemplateStr */
static PyNode *strings(P *p){
    int first=p->i;
    while(cur(p)->kind==PT_STRING) p->i++;
    int last=p->i;
    int any_f=0, any_t=0, any_b=0, any_s=0;
    for(int k=first;k<last;k++){ StrTok st=strtok_info(p,&p->t[k]); if(st.tmpl) any_t=1; if(st.fmt) any_f=1; if(st.bytes) any_b=1; else any_s=1; }
    if(any_b && any_s) fail_at(p,&p->t[first],"cannot mix bytes and nonbytes literals");
    if(any_t || any_f) return parse_fstring_tokens(p,first,last);
    PyNode *n=py_node(PK_Constant,p->t[first].line,p->t[first].col);
    Dec d; memset(&d,0,sizeof d);
    for(int k=first;k<last;k++){ StrTok st=strtok_info(p,&p->t[k]); decode(p,&p->t[k],st.body,st.bodyend,st.raw,st.bytes,&d); }
    if(any_b){
        n->k=konst(PC_Bytes); n->k->blen=d.u.n; n->k->b=(unsigned char*)xmalloc((size_t)d.u.n+1);
        for(int k=0;k<d.u.n;k++) n->k->b[k]=(unsigned char)d.u.v[k];
        free(d.u.v); free(d.esc);
    } else {
        n->k=konst(PC_Str); n->k->u=d.u.v; n->k->ulen=d.u.n;
        if(d.named) n->k->text=d.esc; else free(d.esc);
        n->k->is_u=strtok_info(p,&p->t[first]).u;
    }
    return n;
}
static PyNode *parse_fstring_tokens(P *p, int first, int last){
    int tmpl=0; for(int k=first;k<last;k++) if(strtok_info(p,&p->t[k]).tmpl) tmpl=1;
    if(tmpl) for(int k=first;k<last;k++) if(!strtok_info(p,&p->t[k]).tmpl) fail_at(p,&p->t[k],"cannot mix t-string literals with string or bytes literals");
    PyNode *js=py_node(tmpl?PK_TemplateStr:PK_JoinedStr,p->t[first].line,p->t[first].col);
    FCtx f; memset(&f,0,sizeof f); f.p=p; f.vals=&js->L[0]; f.tmpl=tmpl;
    for(int k=first;k<last;k++){
        PyTok *t=&p->t[k]; StrTok st=strtok_info(p,t);
        f.t=t; f.raw=st.raw;
        if(st.fmt||st.tmpl) f_body(&f,st.body,st.bodyend,0);
        else decode(p,t,st.body,st.bodyend,st.raw,0,&f.lit);
    }
    f_flush(&f,js->line,js->col);
    return js;
}

/* ---------------------------------------------------------------- expressions */
static void set_ctx(P *p, PyNode *e, int ctx){
    switch(e->kind){
        case PK_Name: case PK_Attribute: case PK_Subscript: e->op=ctx; return;
        case PK_Starred: e->op=ctx; set_ctx(p,e->n[0],ctx); return;
        case PK_List: case PK_Tuple: e->op=ctx; for(int i=0;i<e->L[0].n;i++) set_ctx(p,e->L[0].v[i],ctx); return;
        default:{
            PyTok t; memset(&t,0,sizeof t); t.line=e->line; t.col=e->col;
            fail_at(p,&t,ctx==CTX_Del?"cannot delete expression":"cannot assign to expression");
        }
    }
}

static PyNode *arguments_node(P *p){ PyNode *a=nd(p,PK_arguments); return a; }
/* lambda (no annotations) and def parameters, up to `close` (':' or ')') */
static PyNode *parameters(P *p, int lambda, int close){
    PyNode *a=arguments_node(p);
    int kwonly=0, seen_default=0;
    PyList pos; memset(&pos,0,sizeof pos);
    while(!is_op(p,close)){
        if(accept_op(p,O_SLASH)){
            for(int k=0;k<pos.n;k++) py_list_add(&a->L[0],pos.v[k]);
            pos.n=0;
        } else if(accept_op(p,O_DOUBLESTAR)){
            PyNode *arg=nd(p,PK_arg); arg->id[0]=expect_name(p);
            if(!lambda && accept_op(p,O_COLON)) arg->n[0]=expression(p);
            a->n[1]=arg;
        } else if(accept_op(p,O_STAR)){
            kwonly=1;
            if(is_name(p)){
                PyNode *arg=nd(p,PK_arg); arg->id[0]=expect_name(p);
                if(!lambda && accept_op(p,O_COLON)){
                    if(is_op(p,O_STAR)){ PyNode *s=nd(p,PK_Starred); p->i++; s->n[0]=bitwise_or(p); s->op=CTX_Load; arg->n[0]=s; }
                    else arg->n[0]=expression(p);
                }
                a->n[0]=arg;
            }
        } else {
            PyNode *arg=nd(p,PK_arg); arg->id[0]=expect_name(p);
            if(!lambda && accept_op(p,O_COLON)) arg->n[0]=expression(p);
            PyNode *def=NULL;
            if(accept_op(p,O_EQUAL)) def=expression(p);
            if(kwonly){ py_list_add(&a->L[2],arg); py_list_add(&a->L[3],def); }
            else {
                py_list_add(&pos,arg);
                if(def){ py_list_add(&a->L[4],def); seen_default=1; }
                else if(seen_default) fail(p,"parameter without a default follows parameter with a default");
            }
        }
        if(!accept_op(p,O_COMMA)) break;
    }
    for(int k=0;k<pos.n;k++) py_list_add(&a->L[1],pos.v[k]);
    free(pos.v);
    return a;
}

static PyNode *lambdef(P *p){
    PyNode *n=nd(p,PK_Lambda); expect_kw(p,KW_lambda,"lambda");
    n->n[0]=parameters(p,1,O_COLON);
    expect_op(p,O_COLON,":");
    n->n[1]=expression(p);
    return fin(p,n);
}

/* for_if_clauses of a comprehension */
static PyNode *star_targets(P *p);
static void comp_for(P *p, PyList *gens){
    while(is_kw(p,KW_for) || (is_kw(p,KW_async) && peek(p,1)->kind==PT_NAME && peek(p,1)->op==KW_for)){
        PyNode *c=nd(p,PK_comprehension);
        if(accept_kw(p,KW_async)) c->op=1;
        expect_kw(p,KW_for,"for");
        c->n[0]=star_targets(p);
        expect_kw(p,KW_in,"in");
        c->n[1]=disjunction(p);
        while(is_kw(p,KW_if)){ p->i++; py_list_add(&c->L[0],disjunction(p)); }
        fin(p,c); py_list_add(gens,c);
    }
}
static int at_comp_for(P *p){ return is_kw(p,KW_for) || (is_kw(p,KW_async) && peek(p,1)->kind==PT_NAME && peek(p,1)->op==KW_for); }

static PyNode *atom_(P *p);
static PyNode *atom(P *p){ PyNode *n=atom_(p); return fin_if(p,n); }
static PyNode *atom_(P *p){
    PyTok *t=cur(p);
    if(t->kind==PT_NAME){
        switch(t->op){
            case KW_True: p->i++; { PyNode *n=py_node(PK_Constant,t->line,t->col); n->k=konst(PC_True); return n; }
            case KW_False: p->i++; { PyNode *n=py_node(PK_Constant,t->line,t->col); n->k=konst(PC_False); return n; }
            case KW_None: p->i++; { PyNode *n=py_node(PK_Constant,t->line,t->col); n->k=konst(PC_None); return n; }
            default: break;
        }
        if(!is_name(p)) fail(p,"invalid syntax");
        PyNode *n=nd(p,PK_Name); n->id[0]=tok_text(p,t); n->op=CTX_Load; p->i++; return n;
    }
    if(t->kind==PT_NUMBER) return number(p);
    if(t->kind==PT_STRING) return strings(p);
    if(t->kind!=PT_OP) fail(p,"invalid syntax");
    switch(t->op){
        case O_ELLIPSIS: p->i++; { PyNode *n=py_node(PK_Constant,t->line,t->col); n->k=konst(PC_Ellipsis); return n; }
        case O_LPAR:{
            p->i++;
            if(accept_op(p,O_RPAR)){ PyNode *n=py_node(PK_Tuple,t->line,t->col); n->op=CTX_Load; return n; }
            if(is_kw(p,KW_yield)){ PyNode *y=yield_expr(p); expect_op(p,O_RPAR,")"); y->paren=1; return y; }
            PyNode *first=star_named_expression(p);
            if(at_comp_for(p)){
                PyNode *g=py_node(PK_GeneratorExp,t->line,t->col); g->n[0]=first; comp_for(p,&g->L[0]);
                expect_op(p,O_RPAR,")"); return g;
            }
            if(is_op(p,O_COMMA)){
                PyNode *tp=py_node(PK_Tuple,t->line,t->col); tp->op=CTX_Load; py_list_add(&tp->L[0],first);
                while(accept_op(p,O_COMMA)){ if(is_op(p,O_RPAR)) break; py_list_add(&tp->L[0],star_named_expression(p)); }
                expect_op(p,O_RPAR,")"); tp->paren=1; return tp;
            }
            expect_op(p,O_RPAR,")");
            if(first->kind==PK_Starred) fail_at(p,t,"cannot use starred expression here");
            first->paren=1;
            return first;
        }
        case O_LSQB:{
            p->i++;
            PyNode *l=py_node(PK_List,t->line,t->col); l->op=CTX_Load;
            if(accept_op(p,O_RSQB)) return l;
            PyNode *first=star_named_expression(p);
            if(at_comp_for(p)){
                PyNode *c=py_node(PK_ListComp,t->line,t->col); c->n[0]=first; comp_for(p,&c->L[0]);
                expect_op(p,O_RSQB,"]"); return c;
            }
            py_list_add(&l->L[0],first);
            while(accept_op(p,O_COMMA)){ if(is_op(p,O_RSQB)) break; py_list_add(&l->L[0],star_named_expression(p)); }
            expect_op(p,O_RSQB,"]");
            return l;
        }
        case O_LBRACE:{
            p->i++;
            if(accept_op(p,O_RBRACE)) return py_node(PK_Dict,t->line,t->col);
            if(is_op(p,O_DOUBLESTAR) || !is_op(p,O_STAR)){
                PyNode *k=NULL, *v=NULL;
                if(accept_op(p,O_DOUBLESTAR)) v=bitwise_or(p);
                else {
                    k=named_expression(p);
                    if(!accept_op(p,O_COLON)){
                        /* a set */
                        if(at_comp_for(p)){ PyNode *c=py_node(PK_SetComp,t->line,t->col); c->n[0]=k; comp_for(p,&c->L[0]); expect_op(p,O_RBRACE,"}"); return c; }
                        PyNode *s=py_node(PK_Set,t->line,t->col); py_list_add(&s->L[0],k);
                        while(accept_op(p,O_COMMA)){ if(is_op(p,O_RBRACE)) break; py_list_add(&s->L[0],star_named_expression(p)); }
                        expect_op(p,O_RBRACE,"}"); return s;
                    }
                    v=expression(p);
                    if(at_comp_for(p)){ PyNode *c=py_node(PK_DictComp,t->line,t->col); c->n[0]=k; c->n[1]=v; comp_for(p,&c->L[0]); expect_op(p,O_RBRACE,"}"); return c; }
                }
                PyNode *d=py_node(PK_Dict,t->line,t->col);
                py_list_add(&d->L[0],k); py_list_add(&d->L[1],v);
                while(accept_op(p,O_COMMA)){
                    if(is_op(p,O_RBRACE)) break;
                    if(accept_op(p,O_DOUBLESTAR)){ py_list_add(&d->L[0],NULL); py_list_add(&d->L[1],bitwise_or(p)); continue; }
                    PyNode *kk=expression(p); expect_op(p,O_COLON,":");
                    py_list_add(&d->L[0],kk); py_list_add(&d->L[1],expression(p));
                }
                expect_op(p,O_RBRACE,"}");
                return d;
            }
            PyNode *s=py_node(PK_Set,t->line,t->col);
            py_list_add(&s->L[0],star_named_expression(p));
            if(at_comp_for(p)) fail(p,"iterable unpacking cannot be used in comprehension");
            while(accept_op(p,O_COMMA)){ if(is_op(p,O_RBRACE)) break; py_list_add(&s->L[0],star_named_expression(p)); }
            expect_op(p,O_RBRACE,"}");
            return s;
        }
        default: fail(p,"invalid syntax");
    }
    return NULL;
}

/* subscript: slices */
static PyNode *slice_item(P *p){
    PyTok *t=cur(p);
    if(is_op(p,O_STAR)){ PyNode *s=nd(p,PK_Starred); p->i++; s->n[0]=bitwise_or(p); s->op=CTX_Load; return fin(p,s); }
    PyNode *lo=NULL;
    if(!is_op(p,O_COLON)){
        lo=named_expression(p);
        if(!is_op(p,O_COLON)) return lo;
    }
    PyNode *s=py_node(PK_Slice,t->line,t->col); s->n[0]=lo;
    expect_op(p,O_COLON,":");
    if(!is_op(p,O_COLON) && !is_op(p,O_RSQB) && !is_op(p,O_COMMA)) s->n[1]=expression(p);
    if(accept_op(p,O_COLON)){ if(!is_op(p,O_RSQB) && !is_op(p,O_COMMA)) s->n[2]=expression(p); }
    return fin(p,s);
}
static PyNode *slices(P *p){
    PyTok *t=cur(p);
    PyNode *first=slice_item(p);
    if(!is_op(p,O_COMMA) && first->kind!=PK_Starred) return first;
    PyNode *tp=py_node(PK_Tuple,t->line,t->col); tp->op=CTX_Load; py_list_add(&tp->L[0],first);
    while(accept_op(p,O_COMMA)){ if(is_op(p,O_RSQB)) break; py_list_add(&tp->L[0],slice_item(p)); }
    return fin(p,tp);
}

/* call arguments up to close_op (consumed): positional, *x, name=x, **x, or one generator expression */
static void call_args(P *p, PyList *args, PyList *kws, int close_op){
    PyTok *open=&p->t[p->i-1];                     /* f(x for x in y): the genexp is at the '(' */
    PyNode *gx=NULL;
    while(!is_op(p,close_op)){
        if(is_op(p,O_STAR)){ PyNode *s=nd(p,PK_Starred); p->i++; s->n[0]=expression(p); s->op=CTX_Load; py_list_add(args,fin(p,s)); }
        else if(is_op(p,O_DOUBLESTAR)){ PyNode *k=nd(p,PK_keyword); p->i++; k->n[0]=expression(p); py_list_add(kws,fin(p,k)); }
        else if(is_name(p) && is_op_at(p,1,O_EQUAL)){ PyNode *k=nd(p,PK_keyword); k->id[0]=expect_name(p); p->i++; k->n[0]=expression(p); py_list_add(kws,fin(p,k)); }
        else {
            PyNode *e=named_expression(p);
            if(at_comp_for(p)){
                PyNode *g=py_node(PK_GeneratorExp,open->line,open->col); g->n[0]=e; comp_for(p,&g->L[0]);
                e=g; gx=g;
            }
            py_list_add(args,e);
        }
        if(!accept_op(p,O_COMMA)) break;
    }
    expect_op(p,close_op,close_op==O_RPAR?")":"]");
    if(gx) fin(p,gx);                               /* (its parentheses: the call's) */
}

static PyNode *primary(P *p){
    PyNode *e=atom(p);
    for(;;){
        PyTok *t=cur(p);
        if(accept_op(p,O_DOT)){ PyNode *a=nd_at(PK_Attribute,e); a->n[0]=e; a->id[0]=expect_name_any(p); a->op=CTX_Load; e=fin(p,a); }
        else if(accept_op(p,O_LPAR)){ PyNode *c=nd_at(PK_Call,e); c->n[0]=e; call_args(p,&c->L[0],&c->L[1],O_RPAR); e=fin(p,c); }
        else if(accept_op(p,O_LSQB)){ PyNode *s=nd_at(PK_Subscript,e); s->n[0]=e; s->n[1]=slices(p); s->op=CTX_Load; expect_op(p,O_RSQB,"]"); e=fin(p,s); }
        else break;
        (void)t;
    }
    return e;
}
static PyNode *await_primary(P *p){
    if(is_kw(p,KW_await)){ PyNode *a=nd(p,PK_Await); p->i++; a->n[0]=primary(p); return fin(p,a); }
    return primary(p);
}
static PyNode *factor(P *p);
static PyNode *power(P *p){
    PyNode *e=await_primary(p);
    if(is_op(p,O_DOUBLESTAR)){ PyNode *b=nd_at(PK_BinOp,e); p->i++; b->n[0]=e; b->op=OP_Pow; b->n[1]=factor(p); return fin(p,b); }
    return e;
}
static PyNode *factor(P *p){
    int op= is_op(p,O_PLUS)?OP_UAdd : is_op(p,O_MINUS)?OP_USub : is_op(p,O_TILDE)?OP_Invert : 0;
    if(op){ PyNode *u=nd(p,PK_UnaryOp); p->i++; u->op=op; u->n[0]=factor(p); return fin(p,u); }
    return power(p);
}
static PyNode *binary_level(P *p, int level);
static int binop_at(P *p, int level){
    if(cur(p)->kind!=PT_OP) return 0;
    int o=cur(p)->op;
    switch(level){
        case 0: return o==O_VBAR?OP_BitOr:0;
        case 1: return o==O_CIRCUMFLEX?OP_BitXor:0;
        case 2: return o==O_AMPER?OP_BitAnd:0;
        case 3: return o==O_LEFTSHIFT?OP_LShift : o==O_RIGHTSHIFT?OP_RShift : 0;
        case 4: return o==O_PLUS?OP_Add : o==O_MINUS?OP_Sub : 0;
        case 5: return o==O_STAR?OP_Mult : o==O_SLASH?OP_Div : o==O_DOUBLESLASH?OP_FloorDiv : o==O_PERCENT?OP_Mod : o==O_AT?OP_MatMult : 0;
    }
    return 0;
}
static PyNode *binary_level(P *p, int level){
    if(level==6) return factor(p);
    PyNode *e=binary_level(p,level+1);
    int op;
    while((op=binop_at(p,level))){
        PyNode *b=nd_at(PK_BinOp,e); p->i++; b->n[0]=e; b->op=op; b->n[1]=binary_level(p,level+1); e=fin(p,b);
    }
    return e;
}
static PyNode *bitwise_or(P *p){ return binary_level(p,0); }
static int cmp_op(P *p, int *len){
    PyTok *t=cur(p); *len=1;
    if(t->kind==PT_OP) switch(t->op){
        case O_EQEQUAL: return OP_Eq; case O_NOTEQUAL: return OP_NotEq; case O_LESS: return OP_Lt; case O_LESSEQUAL: return OP_LtE;
        case O_GREATER: return OP_Gt; case O_GREATEREQUAL: return OP_GtE; default: return 0;
    }
    if(t->kind==PT_NAME){
        if(t->op==KW_in) return OP_In;
        if(t->op==KW_not && peek(p,1)->kind==PT_NAME && peek(p,1)->op==KW_in){ *len=2; return OP_NotIn; }
        if(t->op==KW_is){ if(peek(p,1)->kind==PT_NAME && peek(p,1)->op==KW_not){ *len=2; return OP_IsNot; } return OP_Is; }
    }
    return 0;
}
static PyNode *comparison(P *p){
    PyNode *e=bitwise_or(p); int len, op=cmp_op(p,&len);
    if(!op) return e;
    PyNode *c=nd_at(PK_Compare,e); c->n[0]=e;
    while((op=cmp_op(p,&len))){
        PyNode *o=nd(p,PK_ident); o->op=op; p->i+=len;
        py_list_add(&c->L[0],o); py_list_add(&c->L[1],bitwise_or(p));
    }
    return fin(p,c);
}
static PyNode *inversion(P *p){
    if(is_kw(p,KW_not)){ PyNode *u=nd(p,PK_UnaryOp); p->i++; u->op=OP_Not; u->n[0]=inversion(p); return fin(p,u); }
    return comparison(p);
}
static PyNode *boolop(P *p, int kw, int op, PyNode *(*sub)(P*)){
    PyNode *e=sub(p);
    if(!is_kw(p,kw)) return e;
    PyNode *b=nd_at(PK_BoolOp,e); b->op=op; py_list_add(&b->L[0],e);
    while(accept_kw(p,kw)) py_list_add(&b->L[0],sub(p));
    return fin(p,b);
}
static PyNode *conjunction(P *p){ return boolop(p,KW_and,OP_And,inversion); }
static PyNode *disjunction(P *p){ return boolop(p,KW_or,OP_Or,conjunction); }
static PyNode *expression(P *p){
    if(is_kw(p,KW_lambda)) return lambdef(p);
    PyNode *e=disjunction(p);
    if(is_kw(p,KW_if)){
        PyNode *c=nd_at(PK_IfExp,e); p->i++;
        c->n[1]=e; c->n[0]=disjunction(p);
        expect_kw(p,KW_else,"else");
        c->n[2]=expression(p);
        return fin(p,c);
    }
    return e;
}
static PyNode *named_expression(P *p){
    if(is_name(p) && is_op_at(p,1,O_COLONEQUAL)){
        PyNode *n=nd(p,PK_NamedExpr); PyNode *t=nd(p,PK_Name); t->id[0]=expect_name(p); t->op=CTX_Store; fin(p,t);
        p->i++; n->n[0]=t; n->n[1]=expression(p); return fin(p,n);
    }
    return expression(p);
}
static PyNode *star_named_expression(P *p){
    if(is_op(p,O_STAR)){ PyNode *s=nd(p,PK_Starred); p->i++; s->n[0]=bitwise_or(p); s->op=CTX_Load; return fin(p,s); }
    return named_expression(p);
}
static PyNode *star_expression(P *p){
    if(is_op(p,O_STAR)){ PyNode *s=nd(p,PK_Starred); p->i++; s->n[0]=bitwise_or(p); s->op=CTX_Load; return fin(p,s); }
    return expression(p);
}
/* can an expression start here (for optional parts: return, yield, ...) */
static int expr_start(P *p){
    PyTok *t=cur(p);
    if(t->kind==PT_NAME) return t->op==KW_NONE || t->op>KW_SOFT || t->op==KW_True || t->op==KW_False || t->op==KW_None || t->op==KW_not || t->op==KW_lambda || t->op==KW_await;
    if(t->kind==PT_NUMBER || t->kind==PT_STRING) return 1;
    if(t->kind==PT_OP) switch(t->op){ case O_LPAR: case O_LSQB: case O_LBRACE: case O_MINUS: case O_PLUS: case O_TILDE: case O_STAR: case O_ELLIPSIS: return 1; default: return 0; }
    return 0;
}
static PyNode *star_expressions(P *p){
    PyNode *first=star_expression(p);
    if(!is_op(p,O_COMMA)) return first;
    PyNode *tp=nd_at(PK_Tuple,first); tp->op=CTX_Load; py_list_add(&tp->L[0],first);
    while(accept_op(p,O_COMMA)){ if(!expr_start(p)) break; py_list_add(&tp->L[0],star_expression(p)); }
    return fin(p,tp);
}
static PyNode *yield_expr(P *p){
    PyTok *t=cur(p); expect_kw(p,KW_yield,"yield");
    if(accept_kw(p,KW_from)){ PyNode *y=py_node(PK_YieldFrom,t->line,t->col); y->n[0]=expression(p); return fin(p,y); }
    PyNode *y=py_node(PK_Yield,t->line,t->col);
    if(expr_start(p)) y->n[0]=star_expressions(p);
    return fin(p,y);
}
/* targets of for / comprehensions: no comparisons (the `in` follows) */
static PyNode *star_target(P *p){
    if(is_op(p,O_STAR)){ PyNode *s=nd(p,PK_Starred); p->i++; s->n[0]=star_target(p); s->op=CTX_Store; return fin(p,s); }
    PyNode *e=bitwise_or(p);
    set_ctx(p,e,CTX_Store);
    return e;
}
static PyNode *star_targets(P *p){
    PyNode *first=star_target(p);
    if(!is_op(p,O_COMMA)) return first;
    PyNode *tp=nd_at(PK_Tuple,first); tp->op=CTX_Store; py_list_add(&tp->L[0],first);
    while(accept_op(p,O_COMMA)){ if(is_kw(p,KW_in) || is_op(p,O_EQUAL)) break; py_list_add(&tp->L[0],star_target(p)); }
    return fin(p,tp);
}

/* ---------------------------------------------------------------- statements */
static PyNode *type_params(P *p, PyList *out){
    expect_op(p,O_LSQB,"[");
    while(!is_op(p,O_RSQB)){
        PyNode *tp;
        if(accept_op(p,O_STAR)){ tp=nd(p,PK_TypeVarTuple); tp->id[0]=expect_name(p); if(accept_op(p,O_EQUAL)) tp->n[1]=star_expression(p); }
        else if(accept_op(p,O_DOUBLESTAR)){ tp=nd(p,PK_ParamSpec); tp->id[0]=expect_name(p); if(accept_op(p,O_EQUAL)) tp->n[1]=expression(p); }
        else { tp=nd(p,PK_TypeVar); tp->id[0]=expect_name(p); if(accept_op(p,O_COLON)) tp->n[0]=expression(p); if(accept_op(p,O_EQUAL)) tp->n[1]=expression(p); }
        py_list_add(out,tp);
        if(!accept_op(p,O_COMMA)) break;
    }
    expect_op(p,O_RSQB,"]");
    return NULL;
}

static PyNode *funcdef(P *p, PyList *decos, int is_async, PyTok *start){
    PyNode *f=py_node(is_async?PK_AsyncFunctionDef:PK_FunctionDef,start->line,start->col);
    expect_kw(p,KW_def,"def");
    f->id[0]=expect_name(p);
    if(is_op(p,O_LSQB)) type_params(p,&f->L[2]);
    expect_op(p,O_LPAR,"(");
    f->n[0]=parameters(p,0,O_RPAR);
    expect_op(p,O_RPAR,")");
    if(accept_op(p,O_RARROW)) f->n[1]=expression(p);
    expect_op(p,O_COLON,":");
    block(p,&f->L[0]);
    if(decos) f->L[1]=*decos;
    return f;
}
static PyNode *classdef(P *p, PyList *decos, PyTok *start){
    PyNode *c=py_node(PK_ClassDef,start->line,start->col);
    expect_kw(p,KW_class,"class");
    c->id[0]=expect_name(p);
    if(is_op(p,O_LSQB)) type_params(p,&c->L[2]);
    if(accept_op(p,O_LPAR)) call_args(p,&c->L[3],&c->L[4],O_RPAR);
    expect_op(p,O_COLON,":");
    block(p,&c->L[0]);
    if(decos) c->L[1]=*decos;
    return c;
}

static PyNode *with_item(P *p){
    PyNode *w=nd(p,PK_withitem); w->n[0]=expression(p);
    if(accept_kw(p,KW_as)){ w->n[1]=star_target(p); }
    return w;
}
static PyNode *with_parenthesized(P *p){
    PyNode *holder=nd(p,PK_With);
    expect_op(p,O_LPAR,"(");
    do { if(is_op(p,O_RPAR)) break; py_list_add(&holder->L[3],with_item(p)); } while(accept_op(p,O_COMMA));
    expect_op(p,O_RPAR,")");
    if(!is_op(p,O_COLON)) fail(p,"expected ':'");
    return holder;
}
static PyNode *with_stmt(P *p, int is_async, PyTok *start){
    PyNode *w=py_node(is_async?PK_AsyncWith:PK_With,start->line,start->col);
    expect_kw(p,KW_with,"with");
    PyNode *h=NULL;
    if(is_op(p,O_LPAR) && attempt(p,with_parenthesized,&h)) w->L[3]=h->L[3];
    else { do py_list_add(&w->L[3],with_item(p)); while(accept_op(p,O_COMMA)); }
    expect_op(p,O_COLON,":");
    block(p,&w->L[0]);
    return w;
}
static PyNode *for_stmt(P *p, int is_async, PyTok *start){
    PyNode *f=py_node(is_async?PK_AsyncFor:PK_For,start->line,start->col);
    expect_kw(p,KW_for,"for");
    f->n[0]=star_targets(p);
    expect_kw(p,KW_in,"in");
    f->n[1]=star_expressions(p);
    expect_op(p,O_COLON,":");
    block(p,&f->L[0]);
    if(accept_kw(p,KW_else)){ expect_op(p,O_COLON,":"); block(p,&f->L[1]); }
    return f;
}
static PyNode *if_stmt(P *p){
    PyNode *n=nd(p,PK_If); p->i++;            /* if / elif */
    n->n[0]=named_expression(p);
    expect_op(p,O_COLON,":");
    block(p,&n->L[0]);
    if(is_kw(p,KW_elif)) py_list_add(&n->L[1],if_stmt(p));
    else if(accept_kw(p,KW_else)){ expect_op(p,O_COLON,":"); block(p,&n->L[1]); }
    return n;
}
static PyNode *try_stmt(P *p){
    PyNode *t=nd(p,PK_Try); expect_kw(p,KW_try,"try"); expect_op(p,O_COLON,":");
    block(p,&t->L[0]);
    int star=-1;
    while(is_kw(p,KW_except)){
        PyNode *h=nd(p,PK_ExceptHandler); p->i++;
        int s=accept_op(p,O_STAR);
        if(star>=0 && star!=s) fail(p,"cannot have both 'except' and 'except*' on the same 'try'");
        star=s;
        if(!is_op(p,O_COLON)){
            PyNode *e=expression(p);
            if(is_op(p,O_COMMA)){                          /* except A, B:  (PEP 758) */
                PyNode *tp=nd_at(PK_Tuple,e); tp->op=CTX_Load; py_list_add(&tp->L[0],e);
                while(accept_op(p,O_COMMA)){ if(is_op(p,O_COLON)) break; py_list_add(&tp->L[0],expression(p)); }
                e=tp;
                if(is_kw(p,KW_as)) fail(p,"multiple exception types must be parenthesized when using 'as'");
            }
            h->n[0]=e;
            if(accept_kw(p,KW_as)) h->id[0]=expect_name(p);
        }
        expect_op(p,O_COLON,":");
        block(p,&h->L[0]);
        py_list_add(&t->L[3],h);
    }
    if(star==1) t->kind=PK_TryStar;
    if(accept_kw(p,KW_else)){ if(!t->L[3].n) fail(p,"expected 'except' or 'finally' block"); expect_op(p,O_COLON,":"); block(p,&t->L[1]); }
    if(accept_kw(p,KW_finally)){ expect_op(p,O_COLON,":"); block(p,&t->L[2]); }
    if(!t->L[3].n && !t->L[2].n) fail(p,"expected 'except' or 'finally' block");
    return t;
}

/* ---- match */
static PyNode *signed_number(P *p){
    if(is_op(p,O_MINUS)){ PyNode *u=nd(p,PK_UnaryOp); p->i++; u->op=OP_USub; if(cur(p)->kind!=PT_NUMBER) fail(p,"invalid pattern"); u->n[0]=number(p); return u; }
    if(cur(p)->kind!=PT_NUMBER) fail(p,"invalid pattern");
    return number(p);
}
static PyNode *literal_expr(P *p){                 /* numbers (complex too) and strings as patterns or mapping keys */
    if(cur(p)->kind==PT_STRING) return strings(p);
    PyNode *e=signed_number(p);
    if(is_op(p,O_PLUS)||is_op(p,O_MINUS)){
        PyNode *b=nd_at(PK_BinOp,e); b->op=is_op(p,O_PLUS)?OP_Add:OP_Sub; p->i++; b->n[0]=e;
        if(cur(p)->kind!=PT_NUMBER) fail(p,"invalid pattern");
        b->n[1]=number(p); return b;
    }
    return e;
}
static PyNode *name_or_attr(P *p){
    PyNode *e=nd(p,PK_Name); e->id[0]=expect_name(p); e->op=CTX_Load;
    while(accept_op(p,O_DOT)){ PyNode *a=nd_at(PK_Attribute,e); a->n[0]=e; a->id[0]=expect_name(p); a->op=CTX_Load; e=a; }
    return e;
}
static PyNode *star_pattern(P *p){
    PyNode *s=nd(p,PK_MatchStar); expect_op(p,O_STAR,"*");
    if(is_kw(p,KW_underscore)) p->i++; else s->id[0]=expect_name(p);
    return s;
}
static PyNode *maybe_star_pattern(P *p){ return is_op(p,O_STAR)?star_pattern(p):pattern(p); }
static PyNode *closed_pattern(P *p){
    PyTok *t=cur(p);
    if(t->kind==PT_NUMBER || t->kind==PT_STRING || is_op(p,O_MINUS)){ PyNode *v=nd(p,PK_MatchValue); v->n[0]=literal_expr(p); return v; }
    if(t->kind==PT_NAME && (t->op==KW_None||t->op==KW_True||t->op==KW_False)){
        PyNode *s=nd(p,PK_MatchSingleton); s->k=konst(t->op==KW_None?PC_None:t->op==KW_True?PC_True:PC_False); p->i++; return s; }
    if(accept_op(p,O_LPAR)){
        if(accept_op(p,O_RPAR)){ PyNode *s=py_node(PK_MatchSequence,t->line,t->col); return s; }
        PyNode *first=maybe_star_pattern(p);
        if(is_op(p,O_COMMA)){
            PyNode *s=py_node(PK_MatchSequence,t->line,t->col); py_list_add(&s->L[0],first);
            while(accept_op(p,O_COMMA)){ if(is_op(p,O_RPAR)) break; py_list_add(&s->L[0],maybe_star_pattern(p)); }
            expect_op(p,O_RPAR,")"); return s;
        }
        expect_op(p,O_RPAR,")");
        if(first->kind==PK_MatchStar){ PyNode *s=py_node(PK_MatchSequence,t->line,t->col); py_list_add(&s->L[0],first); return s; }
        return first;
    }
    if(accept_op(p,O_LSQB)){
        PyNode *s=py_node(PK_MatchSequence,t->line,t->col);
        while(!is_op(p,O_RSQB)){ py_list_add(&s->L[0],maybe_star_pattern(p)); if(!accept_op(p,O_COMMA)) break; }
        expect_op(p,O_RSQB,"]"); return s;
    }
    if(accept_op(p,O_LBRACE)){
        PyNode *m=py_node(PK_MatchMapping,t->line,t->col);
        while(!is_op(p,O_RBRACE)){
            if(accept_op(p,O_DOUBLESTAR)){ m->id[0]=expect_name(p); accept_op(p,O_COMMA); break; }
            PyNode *k;
            if(cur(p)->kind==PT_NAME && (cur(p)->op==KW_None||cur(p)->op==KW_True||cur(p)->op==KW_False)) k=atom(p);
            else if(is_name(p)) k=name_or_attr(p);
            else k=literal_expr(p);
            expect_op(p,O_COLON,":");
            py_list_add(&m->L[0],k); py_list_add(&m->L[1],pattern(p));
            if(!accept_op(p,O_COMMA)) break;
        }
        expect_op(p,O_RBRACE,"}"); return m;
    }
    if(is_kw(p,KW_underscore) && !is_op_at(p,1,O_DOT) && !is_op_at(p,1,O_LPAR)){ PyNode *a=nd(p,PK_MatchAs); p->i++; return a; }
    if(is_name(p)){
        if(!is_op_at(p,1,O_DOT) && !is_op_at(p,1,O_LPAR)){ PyNode *a=nd(p,PK_MatchAs); a->id[0]=expect_name(p); return a; }
        PyNode *cls=name_or_attr(p);
        if(!accept_op(p,O_LPAR)){ PyNode *v=nd_at(PK_MatchValue,cls); v->n[0]=cls; return v; }
        PyNode *c=nd_at(PK_MatchClass,cls); c->n[0]=cls;
        while(!is_op(p,O_RPAR)){
            if(is_name(p) && is_op_at(p,1,O_EQUAL)){
                PyNode *id=nd(p,PK_ident); id->id[0]=expect_name(p); p->i++;
                py_list_add(&c->L[1],id); py_list_add(&c->L[2],pattern(p));
            } else {
                if(c->L[1].n) fail(p,"positional patterns follow keyword patterns");
                py_list_add(&c->L[0],pattern(p));
            }
            if(!accept_op(p,O_COMMA)) break;
        }
        expect_op(p,O_RPAR,")"); return c;
    }
    fail(p,"invalid pattern");
    return NULL;
}
static PyNode *or_pattern(P *p){
    PyNode *first=closed_pattern(p);
    if(!is_op(p,O_VBAR)) return first;
    PyNode *o=nd_at(PK_MatchOr,first); py_list_add(&o->L[0],first);
    while(accept_op(p,O_VBAR)) py_list_add(&o->L[0],closed_pattern(p));
    return o;
}
static PyNode *pattern(P *p){
    PyNode *e=or_pattern(p);
    if(accept_kw(p,KW_as)){
        PyNode *a=nd_at(PK_MatchAs,e); a->n[0]=e;
        if(is_kw(p,KW_underscore)) fail(p,"cannot use '_' as a target");
        a->id[0]=expect_name(p); return a;
    }
    return e;
}
static PyNode *patterns(P *p){                    /* case top level: an open sequence is a MatchSequence */
    PyTok *t=cur(p);
    PyNode *first=maybe_star_pattern(p);
    if(!is_op(p,O_COMMA)){
        if(first->kind==PK_MatchStar){ PyNode *s=py_node(PK_MatchSequence,t->line,t->col); py_list_add(&s->L[0],first); return s; }
        return first;
    }
    PyNode *s=py_node(PK_MatchSequence,t->line,t->col); py_list_add(&s->L[0],first);
    while(accept_op(p,O_COMMA)){ if(is_op(p,O_COLON)||is_kw(p,KW_if)) break; py_list_add(&s->L[0],maybe_star_pattern(p)); }
    return s;
}
static PyNode *match_stmt(P *p){
    PyNode *m=nd(p,PK_Match); p->i++;            /* match */
    PyTok *t=cur(p);
    PyNode *first=star_named_expression(p);
    if(is_op(p,O_COMMA)){
        PyNode *tp=py_node(PK_Tuple,t->line,t->col); tp->op=CTX_Load; py_list_add(&tp->L[0],first);
        while(accept_op(p,O_COMMA)){ if(is_op(p,O_COLON)) break; py_list_add(&tp->L[0],star_named_expression(p)); }
        first=tp;
    }
    m->n[0]=first;
    expect_op(p,O_COLON,":");
    if(cur(p)->kind!=PT_NEWLINE) fail(p,"expected a newline");
    p->i++;
    if(cur(p)->kind!=PT_INDENT) fail(p,"expected an indented block");
    p->i++;
    while(cur(p)->kind==PT_NAME && cur(p)->op==KW_case){
        PyNode *c=nd(p,PK_match_case); p->i++;
        c->n[0]=patterns(p);
        if(accept_kw(p,KW_if)) c->n[1]=named_expression(p);
        expect_op(p,O_COLON,":");
        block(p,&c->L[0]);
        py_list_add(&m->L[3],c);
    }
    if(!m->L[3].n) fail(p,"expected 'case'");
    if(cur(p)->kind!=PT_DEDENT) fail(p,"expected 'case'");
    p->i++;
    return m;
}

static int augassign_op(P *p){
    if(cur(p)->kind!=PT_OP) return 0;
    switch(cur(p)->op){
        case O_PLUSEQUAL: return OP_Add; case O_MINEQUAL: return OP_Sub; case O_STAREQUAL: return OP_Mult;
        case O_ATEQUAL: return OP_MatMult; case O_SLASHEQUAL: return OP_Div; case O_PERCENTEQUAL: return OP_Mod;
        case O_AMPEREQUAL: return OP_BitAnd; case O_VBAREQUAL: return OP_BitOr; case O_CIRCUMFLEXEQUAL: return OP_BitXor;
        case O_LEFTSHIFTEQUAL: return OP_LShift; case O_RIGHTSHIFTEQUAL: return OP_RShift; case O_DOUBLESTAREQUAL: return OP_Pow;
        case O_DOUBLESLASHEQUAL: return OP_FloorDiv; default: return 0;
    }
}
static char *dotted_name(P *p){
    char *s=expect_name(p);
    while(is_op(p,O_DOT) && peek(p,1)->kind==PT_NAME){
        p->i++; char *t=expect_name_any(p);
        size_t a=strlen(s), c=strlen(t); s=(char*)xrealloc(s,a+c+2); s[a]='.'; memcpy(s+a+1,t,c+1); free(t);
    }
    return s;
}

static PyNode *simple_stmt(P *p){
    PyTok *t=cur(p);
    if(t->kind==PT_NAME) switch(t->op){
        case KW_pass: p->i++; return py_node(PK_Pass,t->line,t->col);
        case KW_break: p->i++; return py_node(PK_Break,t->line,t->col);
        case KW_continue: p->i++; return py_node(PK_Continue,t->line,t->col);
        case KW_return:{ PyNode *r=nd(p,PK_Return); p->i++; if(expr_start(p)) r->n[0]=star_expressions(p); return r; }
        case KW_raise:{ PyNode *r=nd(p,PK_Raise); p->i++;
            if(expr_start(p)){ r->n[0]=expression(p); if(accept_kw(p,KW_from)) r->n[1]=expression(p); }
            return r; }
        case KW_global: case KW_nonlocal:{
            PyNode *g=nd(p,t->op==KW_global?PK_Global:PK_Nonlocal); p->i++;
            do { PyNode *id=nd(p,PK_ident); id->id[0]=expect_name(p); py_list_add(&g->L[3],id); } while(accept_op(p,O_COMMA));
            return g; }
        case KW_del:{
            PyNode *d=nd(p,PK_Delete); p->i++;
            do { if(!expr_start(p)) break; PyNode *e=bitwise_or(p); set_ctx(p,e,CTX_Del); py_list_add(&d->L[0],e); } while(accept_op(p,O_COMMA));
            return d; }
        case KW_assert:{ PyNode *a=nd(p,PK_Assert); p->i++; a->n[0]=expression(p); if(accept_op(p,O_COMMA)) a->n[1]=expression(p); return a; }
        case KW_import:{
            PyNode *im=nd(p,PK_Import); p->i++;
            do { PyNode *al=nd(p,PK_alias); al->id[0]=dotted_name(p); if(accept_kw(p,KW_as)) al->id[1]=expect_name(p); py_list_add(&im->L[3],al); } while(accept_op(p,O_COMMA));
            return im; }
        case KW_from:{
            PyNode *im=nd(p,PK_ImportFrom); p->i++;
            int level=0;
            for(;;){ if(accept_op(p,O_DOT)) level++; else if(accept_op(p,O_ELLIPSIS)) level+=3; else break; }
            if(!is_kw(p,KW_import)) im->id[0]=dotted_name(p);
            else if(!level) fail(p,"invalid syntax");
            im->op=level;
            expect_kw(p,KW_import,"import");
            if(is_op(p,O_STAR)){ PyNode *al=nd(p,PK_alias); p->i++; al->id[0]=xstrdup2("*"); py_list_add(&im->L[3],al); return im; }
            int paren=accept_op(p,O_LPAR);
            do {
                if(paren && is_op(p,O_RPAR)) break;
                PyNode *al=nd(p,PK_alias); al->id[0]=expect_name(p); if(accept_kw(p,KW_as)) al->id[1]=expect_name(p); py_list_add(&im->L[3],al);
            } while(accept_op(p,O_COMMA));
            if(paren) expect_op(p,O_RPAR,")");
            return im; }
        case KW_type:
            if(is_name_at(p,1) && (is_op_at(p,2,O_EQUAL) || is_op_at(p,2,O_LSQB))){
                PyNode *ta=nd(p,PK_TypeAlias); p->i++;
                PyNode *name=nd(p,PK_Name); name->id[0]=expect_name(p); name->op=CTX_Store; ta->n[0]=name;
                if(is_op(p,O_LSQB)) type_params(p,&ta->L[2]);
                expect_op(p,O_EQUAL,"=");
                ta->n[1]=expression(p);
                return ta;
            }
            break;
        default: break;
    }
    /* expressions, assignments */
    PyNode *e= is_kw(p,KW_yield) ? yield_expr(p) : star_expressions(p);
    if(is_op(p,O_COLON)){
        PyNode *a=nd_at(PK_AnnAssign,e); p->i++;
        if(e->kind==PK_Name) a->op=!e->paren;
        else if(e->kind!=PK_Attribute && e->kind!=PK_Subscript) fail_at(p,t,"illegal target for annotation");
        set_ctx(p,e,CTX_Store);
        a->n[0]=e; a->n[1]=expression(p);
        if(accept_op(p,O_EQUAL)) a->n[2]= is_kw(p,KW_yield) ? yield_expr(p) : star_expressions(p);
        return a;
    }
    int aug=augassign_op(p);
    if(aug){
        if(e->kind!=PK_Name && e->kind!=PK_Attribute && e->kind!=PK_Subscript) fail_at(p,t,"illegal expression for augmented assignment");
        PyNode *a=nd_at(PK_AugAssign,e); p->i++;
        set_ctx(p,e,CTX_Store);
        a->n[0]=e; a->op=aug; a->n[1]= is_kw(p,KW_yield) ? yield_expr(p) : star_expressions(p);
        return a;
    }
    if(is_op(p,O_EQUAL)){
        PyNode *a=nd_at(PK_Assign,e);
        PyNode *v=e;
        while(accept_op(p,O_EQUAL)){
            set_ctx(p,v,CTX_Store); py_list_add(&a->L[0],v);
            v= is_kw(p,KW_yield) ? yield_expr(p) : star_expressions(p);
        }
        a->n[0]=v;
        return a;
    }
    PyNode *x=nd_at(PK_Expr,e); x->n[0]=e;
    return x;
}
static void simple_stmts(P *p, PyList *out){
    for(;;){
        int soff=cur(p)->start;
        PyNode *st=simple_stmt(p);
        st->soff=soff; st->eoff=p->t[p->i-1].end;
        fin(p,st);
        py_list_add(out,st);
        if(!accept_op(p,O_SEMI)) break;
        if(cur(p)->kind==PT_NEWLINE) break;
    }
    if(cur(p)->kind!=PT_NEWLINE) fail(p,"invalid syntax");
    p->i++;
}
static PyNode *match_try(P *p){ return match_stmt(p); }
static void statement1(P *p, PyList *out);
/* a statement, with its source extent */
static void statement(P *p, PyList *out){
    int first=out->n, soff=cur(p)->start;
    statement1(p,out);
    int j=p->i-1;
    while(j>0 && (p->t[j].kind==PT_NEWLINE || p->t[j].kind==PT_DEDENT || p->t[j].kind==PT_INDENT)) j--;
    for(int k=first;k<out->n;k++) if(!out->v[k]->eoff){ out->v[k]->soff=soff; out->v[k]->eoff=p->t[j].end; }
}
static void statement1(P *p, PyList *out){
    PyTok *t=cur(p);
    if(t->kind==PT_OP && t->op==O_AT){
        PyList decos; memset(&decos,0,sizeof decos);
        while(accept_op(p,O_AT)){
            py_list_add(&decos,named_expression(p));
            if(cur(p)->kind!=PT_NEWLINE) fail(p,"invalid syntax");
            p->i++;
        }
        PyTok *s=cur(p);
        if(is_kw(p,KW_def)){ py_list_add(out,funcdef(p,&decos,0,s)); return; }
        if(is_kw(p,KW_class)){ py_list_add(out,classdef(p,&decos,s)); return; }
        if(is_kw(p,KW_async) && peek(p,1)->kind==PT_NAME && peek(p,1)->op==KW_def){ p->i++; py_list_add(out,funcdef(p,&decos,1,s)); return; }
        fail(p,"invalid syntax");
    }
    if(t->kind==PT_NAME) switch(t->op){
        case KW_def: py_list_add(out,funcdef(p,NULL,0,t)); return;
        case KW_class: py_list_add(out,classdef(p,NULL,t)); return;
        case KW_if: py_list_add(out,if_stmt(p)); return;
        case KW_while:{
            PyNode *w=nd(p,PK_While); p->i++;
            w->n[0]=named_expression(p); expect_op(p,O_COLON,":"); block(p,&w->L[0]);
            if(accept_kw(p,KW_else)){ expect_op(p,O_COLON,":"); block(p,&w->L[1]); }
            py_list_add(out,w); return; }
        case KW_for: py_list_add(out,for_stmt(p,0,t)); return;
        case KW_try: py_list_add(out,try_stmt(p)); return;
        case KW_with: py_list_add(out,with_stmt(p,0,t)); return;
        case KW_async:{
            PyTok *n=peek(p,1);
            if(n->kind==PT_NAME){
                if(n->op==KW_def){ p->i++; py_list_add(out,funcdef(p,NULL,1,t)); return; }
                if(n->op==KW_for){ p->i++; py_list_add(out,for_stmt(p,1,t)); return; }
                if(n->op==KW_with){ p->i++; py_list_add(out,with_stmt(p,1,t)); return; }
            }
            break; }
        case KW_match:{
            PyNode *m=NULL;
            /* `match` is a statement only when it parses as one */
            if(!is_op_at(p,1,O_EQUAL) && !is_op_at(p,1,O_DOT) && !is_op_at(p,1,O_LPAR) && attempt(p,match_try,&m)){ py_list_add(out,m); return; }
            if(is_op_at(p,1,O_LPAR) && attempt(p,match_try,&m)){ py_list_add(out,m); return; }
            break; }
        default: break;
    }
    simple_stmts(p,out);
}
static void block(P *p, PyList *body){
    if(cur(p)->kind==PT_NEWLINE){
        p->i++;
        if(cur(p)->kind!=PT_INDENT) fail(p,"expected an indented block");
        p->i++;
        while(cur(p)->kind!=PT_DEDENT && cur(p)->kind!=PT_END) statement(p,body);
        if(cur(p)->kind==PT_DEDENT) p->i++;
        return;
    }
    simple_stmts(p,body);
}
static PyNode *statement_list(P *p, PyList *out){ while(cur(p)->kind!=PT_END) statement(p,out); return NULL; }

/* the nodes whose end the parser did not set (compound statements, handlers ...): where their last part ends */
static void node_ends(PyNode *n){
    if(!n) return;
    int el=0, ec=0;
    for(int i=0;i<4;i++) if(n->n[i]){ node_ends(n->n[i]); if(n->n[i]->end_line>el || (n->n[i]->end_line==el && n->n[i]->end_col>ec)){ el=n->n[i]->end_line; ec=n->n[i]->end_col; } }
    for(int j=0;j<5;j++) for(int k=0;k<n->L[j].n;k++){ PyNode *c=n->L[j].v[k]; if(!c) continue; node_ends(c);
        if(c->end_line>el || (c->end_line==el && c->end_col>ec)){ el=c->end_line; ec=c->end_col; } }
    if(!n->end_line){ n->end_line= el ? el : n->line; n->end_col= el ? ec : n->col; }
}
int py_parse(PyParse *pp, const char *path, const char *src, size_t len){
    memset(pp,0,sizeof *pp); pp->path=path; pp->src=src; pp->len=len;
    PyLexer *lx=(PyLexer*)xmalloc(sizeof(PyLexer)); memset(lx,0,sizeof *lx);
    int start=0;
    if(len>=3 && (unsigned char)src[0]==0xEF && (unsigned char)src[1]==0xBB && (unsigned char)src[2]==0xBF) start=3;
    if(py_lex(lx,src,(int)len,start,0,NULL)){ pp->error=lx->error; pp->error_line=lx->error_line; pp->error_col=lx->error_col; return 1; }
    P p; memset(&p,0,sizeof p); p.lx=lx; p.t=lx->v; p.n=lx->n; p.src=src; p.len=(int)len;
    jmp_buf jb; p.jb=&jb;
    if(setjmp(jb)){                      /* (p changed after setjmp: read it from memory) */
        volatile P *vp=&p;
        pp->error=vp->err?vp->err:xstrdup2("invalid syntax"); pp->error_line=vp->err_line; pp->error_col=vp->err_col; return 1;
    }
    PyNode *m=py_node(PK_Module,1,0);
    statement_list(&p,&m->L[0]);
    node_ends(m);
    pp->mod=m;
    return 0;
}
