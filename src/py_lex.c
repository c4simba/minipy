/* ========================= Full Python: tokenizer =========================
   Python 3.14's tokens: NEWLINE / INDENT / DEDENT from the indentation
   (tabs to multiples of 8), implicit line joining inside brackets and after
   a backslash, names (any non-ASCII byte is a letter), numbers, operators,
   and string literals with their prefixes. f- and t-strings (PEP 701) are one
   token each: finding their end means following the replacement fields, whose
   expressions may hold strings with the same quotes; py_parse.c tokenizes the
   fields again (mode 1) to parse them. */

#include "py_lex.h"
#include <ctype.h>

static int is_id_start(int c){ return isalpha(c) || c=='_' || c>=0x80; }
static int is_id_char(int c){ return isalnum(c) || c=='_' || c>=0x80; }

void py_lex_lines(PyLexer *lx, const char *src, int len){
    int cap=256, n=0; int *v=(int*)xmalloc(sizeof(int)*(size_t)cap);
    v[n++]=0;
    for(int i=0;i<len;i++){
        int nl= src[i]=='\n' || (src[i]=='\r' && !(i+1<len && src[i+1]=='\n'));
        if(nl){ if(n==cap){ cap*=2; v=(int*)xrealloc(v,sizeof(int)*(size_t)cap); } v[n++]=i+1; }
    }
    lx->line_starts=v; lx->nlines=n;
}
void py_pos(PyLexer *lx, int off, int *line, int *col){
    int lo=0, hi=lx->nlines-1;
    while(lo<hi){ int mid=(lo+hi+1)/2; if(lx->line_starts[mid]<=off) lo=mid; else hi=mid-1; }
    *line=lo+1; *col=off-lx->line_starts[lo];
}

static int lerr(PyLexer *lx, int off, const char *msg){
    if(!lx->error){ lx->error=xstrdup2(msg); py_pos(lx,off,&lx->error_line,&lx->error_col); }
    return 1;
}

/* ---- strings */
/* a valid prefix (r u b br rb f fr rf t tr rt, any case) of n letters at p, followed by a quote */
static int string_prefix(const char *src, int len, int pos, int *raw, int *fmt, int *bytes){
    int n=0, r=0, f=0, b=0, u=0, t=0;
    while(n<3 && pos+n<len){
        int c=tolower((unsigned char)src[pos+n]);
        if(c=='r'){ if(r) return -1; r=1; } else if(c=='f'){ if(f) return -1; f=1; } else if(c=='b'){ if(b) return -1; b=1; }
        else if(c=='u'){ if(u) return -1; u=1; } else if(c=='t'){ if(t) return -1; t=1; }
        else break;
        n++;
    }
    if(pos+n>=len || (src[pos+n]!='\'' && src[pos+n]!='"')) return -1;
    if(u && n>1) return -1;
    if((f||t) && b) return -1;
    if(f && t) return -1;
    if(raw) *raw=r;
    if(fmt) *fmt=f||t;
    if(bytes) *bytes=b;
    return n;
}
static int scan_fbody(const char *src, int len, int i, char q, int triple, int raw, char **error);
static int scan_ffield(const char *src, int len, int i, char q, int triple, char **error);
static int ends_quote(const char *src, int len, int i, char q, int triple){
    if(src[i]!=q) return 0;
    if(!triple) return 1;
    return i+2<len && src[i+1]==q && src[i+2]==q;
}
static int seterr(char **error, const char *msg){ if(!*error) *error=xstrdup2(msg); return -1; }

int py_scan_string(const char *src, int len, int pos, char **error){
    int raw=0, fmt=0;
    int n=string_prefix(src,len,pos,&raw,&fmt,NULL);
    if(n<0) n=0;
    int i=pos+n; char q=src[i];
    int triple= i+2<len && src[i+1]==q && src[i+2]==q;
    i+= triple?3:1;
    if(fmt) return scan_fbody(src,len,i,q,triple,raw,error);
    for(;;){
        if(i>=len) return seterr(error,triple?"unterminated triple-quoted string literal":"unterminated string literal");
        char c=src[i];
        if(c=='\\'){ i+=2; continue; }
        if(ends_quote(src,len,i,q,triple)) return i+(triple?3:1);
        if((c=='\n'||c=='\r') && !triple) return seterr(error,"unterminated string literal");
        i++;
    }
}
static int scan_fspec(const char *src, int len, int i, char q, int triple, char **error){
    for(;;){
        if(i>=len) return seterr(error,"unterminated f-string");
        char c=src[i];
        if(c=='\\'){ i+=2; continue; }
        if(c=='{'){ i=scan_ffield(src,len,i+1,q,triple,error); if(i<0) return -1; continue; }
        if(c=='}') return i+1;
        if(ends_quote(src,len,i,q,triple)) return seterr(error,"f-string: expecting '}'");
        i++;
    }
}
/* after the '{' of a replacement field -> after its '}' */
static int scan_ffield(const char *src, int len, int i, char q, int triple, char **error){
    int depth=0;
    for(;;){
        if(i>=len) return seterr(error,"f-string: expecting '}'");
        unsigned char c=(unsigned char)src[i];
        if(c=='\'' || c=='"' || (is_id_start(c) && string_prefix(src,len,i,NULL,NULL,NULL)>0 && (i==0 || !is_id_char((unsigned char)src[i-1])))){
            if(depth==0 && ends_quote(src,len,i,q,triple) && (c=='\'' || c=='"')){
                /* the f-string's own quote: in Python 3.12+ a nested string may reuse it; it is one when it is not the end */
            }
            i=py_scan_string(src,len,i,error); if(i<0) return -1; continue;
        }
        if(is_id_start(c)){ while(i<len && is_id_char((unsigned char)src[i])) i++; continue; }
        if(c=='#'){ while(i<len && src[i]!='\n') i++; continue; }
        if(c=='('||c=='['||c=='{'){ depth++; i++; continue; }
        if(c==')'||c==']'){ depth--; i++; continue; }
        if(c=='}'){ if(depth==0) return i+1; depth--; i++; continue; }
        if(depth==0 && c=='!' && i+1<len && src[i+1]!='='){ i++; while(i<len && is_id_char((unsigned char)src[i])) i++; continue; }
        if(depth==0 && c==':') return scan_fspec(src,len,i+1,q,triple,error);
        i++;
    }
}
static int scan_fbody(const char *src, int len, int i, char q, int triple, int raw, char **error){
    for(;;){
        if(i>=len) return seterr(error,triple?"unterminated triple-quoted f-string literal":"unterminated f-string literal");
        char c=src[i];
        if(c=='\\'){
            if(!raw && i+2<len && src[i+1]=='N' && src[i+2]=='{'){ i+=3; while(i<len && src[i]!='}') i++; i++; continue; }
            i+= (i+1<len && (src[i+1]=='{'||src[i+1]=='}')) ? 1 : 2;   /* a backslash does not escape a brace */
            continue;
        }
        if(ends_quote(src,len,i,q,triple)) return i+(triple?3:1);
        if((c=='\n'||c=='\r') && !triple) return seterr(error,"unterminated f-string literal");
        if(c=='{'){ if(i+1<len && src[i+1]=='{'){ i+=2; continue; } i=scan_ffield(src,len,i+1,q,triple,error); if(i<0) return -1; continue; }
        if(c=='}'){ i+= (i+1<len && src[i+1]=='}') ? 2 : 1; continue; }
        i++;
    }
}

/* ---- keywords and operators */
static const struct { const char *s; int kw; } kwtab[]={
    {"False",KW_False},{"None",KW_None},{"True",KW_True},{"and",KW_and},{"as",KW_as},{"assert",KW_assert},
    {"async",KW_async},{"await",KW_await},{"break",KW_break},{"class",KW_class},{"continue",KW_continue},
    {"def",KW_def},{"del",KW_del},{"elif",KW_elif},{"else",KW_else},{"except",KW_except},{"finally",KW_finally},
    {"for",KW_for},{"from",KW_from},{"global",KW_global},{"if",KW_if},{"import",KW_import},{"in",KW_in},
    {"is",KW_is},{"lambda",KW_lambda},{"nonlocal",KW_nonlocal},{"not",KW_not},{"or",KW_or},{"pass",KW_pass},
    {"raise",KW_raise},{"return",KW_return},{"try",KW_try},{"while",KW_while},{"with",KW_with},{"yield",KW_yield},
    {"match",KW_match},{"case",KW_case},{"type",KW_type},{"_",KW_underscore},{NULL,0}};
static int keyword(const char *s, int n){
    for(int i=0;kwtab[i].s;i++) if((int)strlen(kwtab[i].s)==n && !memcmp(kwtab[i].s,s,(size_t)n)) return kwtab[i].kw;
    return KW_NONE;
}
static const struct { const char *s; int op; } optab[]={
    {"**=",O_DOUBLESTAREQUAL},{"//=",O_DOUBLESLASHEQUAL},{">>=",O_RIGHTSHIFTEQUAL},{"<<=",O_LEFTSHIFTEQUAL},{"...",O_ELLIPSIS},
    {"!=",O_NOTEQUAL},{"%=",O_PERCENTEQUAL},{"&=",O_AMPEREQUAL},{"**",O_DOUBLESTAR},{"*=",O_STAREQUAL},{"+=",O_PLUSEQUAL},
    {"-=",O_MINEQUAL},{"->",O_RARROW},{"//",O_DOUBLESLASH},{"/=",O_SLASHEQUAL},{":=",O_COLONEQUAL},{"<<",O_LEFTSHIFT},
    {"<=",O_LESSEQUAL},{"==",O_EQEQUAL},{">=",O_GREATEREQUAL},{">>",O_RIGHTSHIFT},{"@=",O_ATEQUAL},{"^=",O_CIRCUMFLEXEQUAL},
    {"|=",O_VBAREQUAL},{"(",O_LPAR},{")",O_RPAR},{"[",O_LSQB},{"]",O_RSQB},{"{",O_LBRACE},{"}",O_RBRACE},{":",O_COLON},
    {",",O_COMMA},{";",O_SEMI},{"+",O_PLUS},{"-",O_MINUS},{"*",O_STAR},{"/",O_SLASH},{"|",O_VBAR},{"&",O_AMPER},
    {"<",O_LESS},{">",O_GREATER},{"=",O_EQUAL},{".",O_DOT},{"%",O_PERCENT},{"~",O_TILDE},{"^",O_CIRCUMFLEX},
    {"@",O_AT},{"!",O_EXCLAMATION},{NULL,0}};

static void tok_add(PyLexer *lx, PyTokKind k, int start, int end, int op){
    if(lx->n==lx->cap){ lx->cap=lx->cap?lx->cap*2:1024; lx->v=(PyTok*)xrealloc(lx->v,sizeof(PyTok)*(size_t)lx->cap); }
    PyTok *t=&lx->v[lx->n++]; t->kind=k; t->start=start; t->end=end; t->op=op;
    py_pos(lx,start,&t->line,&t->col); py_pos(lx,end,&t->end_line,&t->end_col);
}

int py_lex(PyLexer *lx, const char *src, int len, int start, int mode, int *stop){
    lx->src=src; lx->len=len;
    if(!lx->line_starts) py_lex_lines(lx,src,len);
    int indents[200], nind=1; indents[0]=0;
    int depth=0, i=start, line_start=(mode==0), line_has_tokens=0;
    for(;;){
        if(line_start && depth==0 && mode==0){
            /* indentation of a new logical line */
            int col=0, j=i;
            for(;j<len;j++){
                if(src[j]==' ') col++;
                else if(src[j]=='\t') col=(col/8+1)*8;
                else if(src[j]=='\f') col=0;
                else break;
            }
            if(j>=len){ i=j; break; }
            if(src[j]=='#' || src[j]=='\n' || src[j]=='\r'){          /* blank or comment-only line */
                while(j<len && src[j]!='\n' && src[j]!='\r') j++;
                if(j<len && src[j]=='\r' && j+1<len && src[j+1]=='\n') j++;
                i=j+1; continue;
            }
            if(src[j]=='\\' && j+1<len && (src[j+1]=='\n'||src[j+1]=='\r')){ /* a line of just a continuation */ }
            if(col>indents[nind-1]){
                if(nind==200) return lerr(lx,j,"too many levels of indentation");
                indents[nind++]=col; tok_add(lx,PT_INDENT,j,j,0);
            } else {
                while(col<indents[nind-1]){ nind--; tok_add(lx,PT_DEDENT,j,j,0); }
                if(col!=indents[nind-1]) return lerr(lx,j,"unindent does not match any outer indentation level");
            }
            i=j; line_start=0; line_has_tokens=0;
        }
        if(i>=len) break;
        unsigned char c=(unsigned char)src[i];
        if(c==' '||c=='\t'||c=='\f'){ i++; continue; }
        if(c=='\\' && i+1<len && (src[i+1]=='\n'||src[i+1]=='\r')){
            i+=2; if(src[i-1]=='\r' && i<len && src[i]=='\n') i++;
            if(i>=len) return lerr(lx,i,"unexpected EOF while parsing");
            continue;
        }
        if(c=='#'){ while(i<len && src[i]!='\n' && src[i]!='\r') i++; continue; }
        if(c=='\n'||c=='\r'){
            int nl=i; i++; if(c=='\r' && i<len && src[i]=='\n') i++;
            if(mode==0 && depth==0){ if(line_has_tokens) tok_add(lx,PT_NEWLINE,nl,nl+1,0); line_start=1; }
            continue;
        }
        if(mode==1 && depth==0){
            if(c=='}' || c==':' || (c=='!' && !(i+1<len && src[i+1]=='=')) || (c=='=' && !(i+1<len && src[i+1]=='='))){ *stop=i; tok_add(lx,PT_END,i,i,0); return 0; }
        }
        line_has_tokens=1;
        if(is_id_start(c)){
            if(string_prefix(src,len,i,NULL,NULL,NULL)>0){
                char *e=NULL; int end=py_scan_string(src,len,i,&e);
                if(end<0){ lerr(lx,i,e); free(e); return 1; }
                tok_add(lx,PT_STRING,i,end,0); i=end; continue;
            }
            int j=i; while(j<len && is_id_char((unsigned char)src[j])) j++;
            tok_add(lx,PT_NAME,i,j,keyword(src+i,j-i)); i=j; continue;
        }
        if(c=='\'' || c=='"'){
            char *e=NULL; int end=py_scan_string(src,len,i,&e);
            if(end<0){ lerr(lx,i,e); free(e); return 1; }
            tok_add(lx,PT_STRING,i,end,0); i=end; continue;
        }
        if(isdigit(c) || (c=='.' && i+1<len && isdigit((unsigned char)src[i+1]))){
            int j=i;
            if(c=='0' && j+1<len && strchr("xXoObB",src[j+1])){ j+=2; while(j<len && (isxdigit((unsigned char)src[j])||src[j]=='_')) j++; }
            else {
                while(j<len && (isdigit((unsigned char)src[j])||src[j]=='_')) j++;
                if(j<len && src[j]=='.'){ j++; while(j<len && (isdigit((unsigned char)src[j])||src[j]=='_')) j++; }
                if(j<len && (src[j]=='e'||src[j]=='E')){
                    int k=j+1; if(k<len && (src[k]=='+'||src[k]=='-')) k++;
                    if(k<len && isdigit((unsigned char)src[k])){ j=k; while(j<len && (isdigit((unsigned char)src[j])||src[j]=='_')) j++; }
                }
                if(j<len && (src[j]=='j'||src[j]=='J')) j++;
            }
            tok_add(lx,PT_NUMBER,i,j,0); i=j; continue;
        }
        int op=0, ol=0;
        for(int k=0;optab[k].s;k++){ int n=(int)strlen(optab[k].s); if(i+n<=len && !memcmp(src+i,optab[k].s,(size_t)n)){ op=optab[k].op; ol=n; break; } }
        if(!op){ char m[64]; snprintf(m,sizeof m,"invalid character '%c' (U+%04X)",c>=32&&c<127?c:'?',c); return lerr(lx,i,m); }
        if(op==O_LPAR||op==O_LSQB||op==O_LBRACE) depth++;
        else if(op==O_RPAR||op==O_RSQB||op==O_RBRACE){ if(depth>0) depth--; }
        tok_add(lx,PT_OP,i,i+ol,op); i+=ol;
    }
    if(mode==1) return lerr(lx,i,"f-string: expecting '}'");
    if(line_has_tokens && lx->n && lx->v[lx->n-1].kind!=PT_NEWLINE) tok_add(lx,PT_NEWLINE,len,len,0);
    while(nind>1){ nind--; tok_add(lx,PT_DEDENT,len,len,0); }
    tok_add(lx,PT_END,len,len,0);
    return 0;
}
