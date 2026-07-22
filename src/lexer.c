/* ========================= Lexer ========================= */

#include "lexer.h"

static void addtok(TokVec *tv,TokKind k,const char *s,int len,int64_t i,double f,int isf,int line){
    if(tv->n==tv->cap){ tv->cap=tv->cap?tv->cap*2:256; tv->v=(Tok*)xrealloc(tv->v,sizeof(Tok)*(size_t)tv->cap); }
    tv->v[tv->n].kind=k; tv->v[tv->n].text=s?xstrndup2(s,len):NULL; tv->v[tv->n].i=i; tv->v[tv->n].f=f; tv->v[tv->n].is_float=isf; tv->v[tv->n].line=line; tv->n++;
}

static void lex_line(TokVec *tv,const char *p,int line);
static void lex_fstring(TokVec *tv,const char **pp,int line);

static int my_isalpha(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
static int my_isalnum(int c){ return my_isalpha(c)||(c>='0'&&c<='9'); }
static int my_isdigit(int c){ return c>='0'&&c<='9'; }
static int my_strlen(const char *s){ int n=0; while(s[n])n++; return n; }
static const char *my_strchr(const char *s, int c){ while(*s){ if(*s==c)return s; s++; } return NULL; }
static int my_strncmp(const char *a, const char *b, int n){ for(int i=0;i<n;i++){ if(a[i]!=b[i])return 1; if(!a[i])break; } return 0; }

static int64_t my_strtoll(const char *s, char **end, int base){
    (void)base;
    long long v=0; int neg=0; while(*s==' '||*s=='\t')s++;
    if(*s=='-'){neg=1;s++;} else if(*s=='+')s++;
    while(*s>='0'&&*s<='9'){ v=v*10+(*s-'0'); s++; }
    if(end)*end=(char*)s; return neg?-v:v;
}

static double my_strtod(const char *s, char **end){
    double v=0; int neg=0; while(*s==' '||*s=='\t')s++;
    if(*s=='-'){neg=1;s++;} else if(*s=='+')s++;
    while(*s>='0'&&*s<='9'){ v=v*10.0+(*s-'0'); s++; }
    if(*s=='.'){ s++; double frac=0.1; while(*s>='0'&&*s<='9'){ v+=(*s-'0')*frac; frac*=0.1; s++; } }
    if(end)*end=(char*)s; return neg?-v:v;
}

/* Parse a decimal/float literal that may carry an exponent (1e3, 1.5e-2). */
static double lex_atof(const char *s){
    char *e; double m=my_strtod(s,&e);
    if(*e=='e'||*e=='E'){ e++; int neg=0; if(*e=='+')e++; else if(*e=='-'){neg=1;e++;} int ex=0; while(*e>='0'&&*e<='9'){ex=ex*10+(*e-'0');e++;} double p=1; for(int i=0;i<ex;i++)p*=10.0; m=neg?m/p:m*p; }
    return m;
}
static int lex_ishex(int c){ return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
static int lex_hv(int c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; return c-'A'+10; }
static int lex_utf8(char *out,int cp){
    if(cp<0x80){ out[0]=(char)cp; return 1; }
    if(cp<0x800){ out[0]=(char)(0xC0|(cp>>6)); out[1]=(char)(0x80|(cp&0x3F)); return 2; }
    out[0]=(char)(0xE0|(cp>>12)); out[1]=(char)(0x80|((cp>>6)&0x3F)); out[2]=(char)(0x80|(cp&0x3F)); return 3;
}
/* Lex a string literal starting at *pp (which points at the opening quote).
   Handles single and triple quotes, escapes, and raw strings.
   NOTE: triple-quoted strings must be closed on the same source line. */
static void lex_string(TokVec *tv, const char **pp, int line, int raw){
    const char *p=*pp;
    char q=*p; int triple=(p[1]==q && p[2]==q);
    p += triple?3:1;
    char buf[8192]; int b=0;
    while(*p){
        if(triple){ if(p[0]==q && p[1]==q && p[2]==q){ p+=3; goto done; } }
        else { if(*p==q){ p++; goto done; } }
        if(*p=='\\' && p[1]){
            if(raw){ buf[b++]='\\'; buf[b++]=p[1]; p+=2; }
            else { p++; char e=*p;
                if(e=='n'){buf[b++]='\n';p++;} else if(e=='t'){buf[b++]='\t';p++;} else if(e=='r'){buf[b++]='\r';p++;}
                else if(e=='\\'){buf[b++]='\\';p++;} else if(e=='\''){buf[b++]='\'';p++;} else if(e=='"'){buf[b++]='"';p++;}
                else if(e=='0'){buf[b++]='\0';p++;} else if(e=='a'){buf[b++]='\a';p++;} else if(e=='b'){buf[b++]='\b';p++;}
                else if(e=='f'){buf[b++]='\f';p++;} else if(e=='v'){buf[b++]='\v';p++;}
                else if(e=='x'){ p++; int h=0,cnt=0; while(cnt<2&&lex_ishex((unsigned char)*p)){h=h*16+lex_hv((unsigned char)*p);p++;cnt++;} buf[b++]=(char)h; }
                else if(e=='u'){ p++; int h=0,cnt=0; while(cnt<4&&lex_ishex((unsigned char)*p)){h=h*16+lex_hv((unsigned char)*p);p++;cnt++;} b+=lex_utf8(buf+b,h); }
                else { buf[b++]='\\'; buf[b++]=e; p++; }
            }
        } else buf[b++]=*p++;
        if(b>=8190) die("string too long");
    }
    fprintf(stderr,"unterminated string at line %d\n",line); exit(1);
done:
    addtok(tv,T_STRING,buf,b,0,0,0,line);
    *pp=p;
}
static TokKind kw(const char *s,int n){
    if(n==2&&!my_strncmp(s,"if",2))return T_IF;
    if(n==4&&!my_strncmp(s,"elif",4))return T_ELIF;
    if(n==4&&!my_strncmp(s,"else",4))return T_ELSE;
    if(n==5&&!my_strncmp(s,"while",5))return T_WHILE;
    if(n==3&&!my_strncmp(s,"for",3))return T_FOR;
    if(n==2&&!my_strncmp(s,"in",2))return T_IN;
    if(n==3&&!my_strncmp(s,"def",3))return T_DEF;
    if(n==6&&!my_strncmp(s,"return",6))return T_RETURN;
    if(n==5&&!my_strncmp(s,"print",5))return T_PRINT;
    if(n==5&&!my_strncmp(s,"class",5))return T_CLASS;
    if(n==6&&!my_strncmp(s,"import",6))return T_IMPORT;
    if(n==4&&!my_strncmp(s,"True",4))return T_TRUE;
    if(n==5&&!my_strncmp(s,"False",5))return T_FALSE;
    if(n==4&&!my_strncmp(s,"None",4))return T_NONE;
    if(n==3&&!my_strncmp(s,"and",3))return T_AND;
    if(n==2&&!my_strncmp(s,"or",2))return T_OR;
    if(n==3&&!my_strncmp(s,"not",3))return T_NOT;
    if(n==5&&!my_strncmp(s,"break",5))return T_BREAK;
    if(n==8&&!my_strncmp(s,"continue",8))return T_CONTINUE;
    if(n==4&&!my_strncmp(s,"pass",4))return T_PASS;
    if(n==4&&!my_strncmp(s,"from",4))return T_FROM;
    if(n==2&&!my_strncmp(s,"as",2))return T_AS;
    if(n==2&&!my_strncmp(s,"is",2))return T_IS;
    if(n==5&&!my_strncmp(s,"raise",5))return T_RAISE;
    if(n==4&&!my_strncmp(s,"with",4))return T_WITH;
    if(n==3&&!my_strncmp(s,"try",3))return T_TRY;
    if(n==6&&!my_strncmp(s,"except",6))return T_EXCEPT;
    if(n==7&&!my_strncmp(s,"finally",7))return T_FINALLY;
    if(n==6&&!my_strncmp(s,"global",6))return T_GLOBAL;
    if(n==8&&!my_strncmp(s,"nonlocal",8))return T_NONLOCAL;
    if(n==3&&!my_strncmp(s,"del",3))return T_DEL;
    if(n==6&&!my_strncmp(s,"lambda",6))return T_LAMBDA;
    if(n==5&&!my_strncmp(s,"yield",5))return T_YIELD;
    if(n==5&&!my_strncmp(s,"async",5))return T_ASYNC;
    if(n==5&&!my_strncmp(s,"await",5))return T_AWAIT;
    if(n==5&&!my_strncmp(s,"match",5))return T_MATCH;
    if(n==4&&!my_strncmp(s,"case",4))return T_CASE;
    if(n==6&&!my_strncmp(s,"assert",6))return T_ASSERT;
    return T_NAME;
}

/* f-string: transform  f"a{expr}b"  into token stream  ( "a" + ( expr ) + "b" ).
   {{ }} are literal braces; {expr:spec} becomes ( "%spec" % ( expr ) );
   the !r/!s/!a conversion is accepted and ignored. Embedded expressions are
   lexed inline via lex_line (so nesting/operators work). */
static void lex_fstring(TokVec *tv,const char **pp,int line){
    const char *p=*pp;
    p++;                      /* skip f/F */
    char q=*p++;              /* opening quote */
    addtok(tv,T_LP,"(",1,0,0,0,line);
    char lit[8192]; int li=0; int emitted=0;
    #define FLUSH_LIT() do{ lit[li]=0; if(emitted) addtok(tv,T_PLUS,"+",1,0,0,0,line); addtok(tv,T_STRING,lit,li,0,0,0,line); emitted=1; li=0; }while(0)
    while(*p && *p!=q){
        if(*p=='{' && p[1]=='{'){ lit[li++]='{'; p+=2; continue; }
        if(*p=='}' && p[1]=='}'){ lit[li++]='}'; p+=2; continue; }
        if(*p=='{'){
            if(!emitted || li>0) FLUSH_LIT();
            p++;
            char ex[4096]; int xi=0; char sp[64]; int spi=0; int have_spec=0; int depth=0;
            while(*p && !((*p=='}'||*p==':'||*p=='!') && depth==0)){
                if(*p=='{'||*p=='('||*p=='[') depth++;
                else if(*p=='}'||*p==')'||*p==']') depth--;
                if(xi<4094) ex[xi++]=*p; p++;
            }
            ex[xi]=0;
            if(*p=='!' && depth==0){ p++; if(*p) p++; }          /* !r / !s / !a : ignored */
            if(*p==':' && depth==0){ have_spec=1; p++; while(*p && *p!='}'){ if(spi<62) sp[spi++]=*p; p++; } }
            sp[spi]=0;
            if(*p=='}') p++;
            if(emitted) addtok(tv,T_PLUS,"+",1,0,0,0,line);
            if(have_spec){
                /* If the spec lacks a printf conversion letter (e.g. "5", ".2"),
                   default to 's' so width/precision apply to the stringified value. */
                if(spi>0 && !my_isalpha((unsigned char)sp[spi-1])){ if(spi<62){ sp[spi++]='s'; sp[spi]=0; } }
                char fbuf[80]; snprintf(fbuf,sizeof(fbuf),"%%%s",sp);
                addtok(tv,T_LP,"(",1,0,0,0,line);
                addtok(tv,T_STRING,fbuf,(int)strlen(fbuf),0,0,0,line);
                addtok(tv,T_PERCENT,"%",1,0,0,0,line);
                addtok(tv,T_LP,"(",1,0,0,0,line); lex_line(tv,ex,line); addtok(tv,T_RP,")",1,0,0,0,line);
                addtok(tv,T_RP,")",1,0,0,0,line);
            } else {
                /* wrap in str(...) so concatenation stays string-typed */
                addtok(tv,T_NAME,"str",3,0,0,0,line); addtok(tv,T_LP,"(",1,0,0,0,line); lex_line(tv,ex,line); addtok(tv,T_RP,")",1,0,0,0,line);
            }
            emitted=1;
            continue;
        }
        if(*p=='\\' && p[1]){ p++; if(*p=='n')lit[li++]='\n'; else if(*p=='t')lit[li++]='\t'; else lit[li++]=*p; p++; continue; }
        lit[li++]=*p++;
        if(li>=8190) die("f-string too long");
    }
    if(!emitted || li>0) FLUSH_LIT();
    if(*p==q) p++;
    addtok(tv,T_RP,")",1,0,0,0,line);
    *pp=p;
    #undef FLUSH_LIT
}

static void lex_line(TokVec *tv,const char *p,int line){
    while(*p){
        if(*p==' '||*p=='\r'||*p=='\t'){p++;continue;}
        if(*p=='#')break; /* comment */
        if((*p=='f'||*p=='F') && (p[1]=='"'||p[1]=='\'')){ lex_fstring(tv,&p,line); continue; }
        if((*p=='r'||*p=='R') && (p[1]=='"'||p[1]=='\'')){ p++; lex_string(tv,&p,line,1); continue; }
        if(my_isdigit((unsigned char)*p)){
            const char *s=p;
            if(*p=='0' && (p[1]=='x'||p[1]=='X'||p[1]=='o'||p[1]=='O'||p[1]=='b'||p[1]=='B')){
                int base=(p[1]=='x'||p[1]=='X')?16:(p[1]=='o'||p[1]=='O')?8:2; p+=2;
                int64_t v=0; while(1){ char c=*p; if(c=='_'){p++;continue;} int d; if(c>='0'&&c<='9')d=c-'0'; else if(c>='a'&&c<='f')d=c-'a'+10; else if(c>='A'&&c<='F')d=c-'A'+10; else break; if(d>=base) break; v=v*base+d; p++; }
                addtok(tv,T_NUMBER,s,(int)(p-s),v,0,0,line); continue;
            }
            int isf=0; while(my_isdigit((unsigned char)*p)||*p=='_')p++;
            if(*p=='.'){ isf=1; p++; while(my_isdigit((unsigned char)*p)||*p=='_')p++; }
            if(*p=='e'||*p=='E'){ isf=1; p++; if(*p=='+'||*p=='-')p++; while(my_isdigit((unsigned char)*p)||*p=='_')p++; }
            char tmp[128]; int ti=0; for(const char *s2=s;s2<p&&ti<127;s2++) if(*s2!='_') tmp[ti++]=*s2; tmp[ti]=0;
            if(isf){ double f=lex_atof(tmp); addtok(tv,T_NUMBER,tmp,ti,0,f,1,line); }
            else { int64_t i=my_strtoll(tmp,NULL,10); addtok(tv,T_NUMBER,tmp,ti,i,0,0,line); }
            continue;
        }
        if(my_isalpha((unsigned char)*p)||*p=='_'){ const char *s=p; while(my_isalnum((unsigned char)*p)||*p=='_')p++; int n=(int)(p-s); addtok(tv,kw(s,n),s,n,0,0,0,line); continue; }
        if(*p=='"'||*p=='\''){
            lex_string(tv,&p,line,0); continue;
        }
        switch(*p){
            case '(':addtok(tv,T_LP,p,1,0,0,0,line);p++;break; case ')':addtok(tv,T_RP,p,1,0,0,0,line);p++;break;
            case '[':addtok(tv,T_LB,p,1,0,0,0,line);p++;break; case ']':addtok(tv,T_RB,p,1,0,0,0,line);p++;break;
            case '{':addtok(tv,T_LC,p,1,0,0,0,line);p++;break; case '}':addtok(tv,T_RC,p,1,0,0,0,line);p++;break;
            case ':':addtok(tv,T_COLON,p,1,0,0,0,line);p++;break; case ',':addtok(tv,T_COMMA,p,1,0,0,0,line);p++;break; case '.':addtok(tv,T_DOT,p,1,0,0,0,line);p++;break; case '@':addtok(tv,T_AT,p,1,0,0,0,line);p++;break;
            case '+': if(p[1]=='='){addtok(tv,T_PLUS_ASSIGN,p,2,0,0,0,line);p+=2;} else {addtok(tv,T_PLUS,p,1,0,0,0,line);p++;} break;
            case '-': if(p[1]=='='){addtok(tv,T_MINUS_ASSIGN,p,2,0,0,0,line);p+=2;} else {addtok(tv,T_MINUS,p,1,0,0,0,line);p++;} break;
            case '*': if(p[1]=='*'&&p[2]=='='){addtok(tv,T_POWER_ASSIGN,p,3,0,0,0,line);p+=3;} else if(p[1]=='*'){addtok(tv,T_POWER,p,2,0,0,0,line);p+=2;} else if(p[1]=='='){addtok(tv,T_STAR_ASSIGN,p,2,0,0,0,line);p+=2;} else {addtok(tv,T_STAR,p,1,0,0,0,line);p++;} break;
            case '/': if(p[1]=='/'&&p[2]=='='){addtok(tv,T_FLOOR_DIV_ASSIGN,p,3,0,0,0,line);p+=3;} else if(p[1]=='/'){addtok(tv,T_FLOOR_DIV,p,2,0,0,0,line);p+=2;} else if(p[1]=='='){addtok(tv,T_SLASH_ASSIGN,p,2,0,0,0,line);p+=2;} else {addtok(tv,T_SLASH,p,1,0,0,0,line);p++;} break;
            case '%': if(p[1]=='='){addtok(tv,T_PERCENT_ASSIGN,p,2,0,0,0,line);p+=2;} else {addtok(tv,T_PERCENT,p,1,0,0,0,line);p++;} break;
            case '&': if(p[1]=='='){addtok(tv,T_AMP_ASSIGN,p,2,0,0,0,line);p+=2;} else {addtok(tv,T_AMP,p,1,0,0,0,line);p++;} break;
            case '|': if(p[1]=='='){addtok(tv,T_PIPE_ASSIGN,p,2,0,0,0,line);p+=2;} else {addtok(tv,T_PIPE,p,1,0,0,0,line);p++;} break;
            case '^': if(p[1]=='='){addtok(tv,T_CARET_ASSIGN,p,2,0,0,0,line);p+=2;} else {addtok(tv,T_CARET,p,1,0,0,0,line);p++;} break;
            case '~': addtok(tv,T_TILDE,p,1,0,0,0,line);p++; break;
            case '=': if(p[1]=='='){addtok(tv,T_EQ,p,2,0,0,0,line);p+=2;} else {addtok(tv,T_ASSIGN,p,1,0,0,0,line);p++;} break;
            case '!': if(p[1]=='='){addtok(tv,T_NE,p,2,0,0,0,line);p+=2;} else {fprintf(stderr,"bad token ! at line %d\n",line);exit(1);} break;
            case '<': if(p[1]=='<'&&p[2]=='='){addtok(tv,T_SHL_ASSIGN,p,3,0,0,0,line);p+=3;} else if(p[1]=='<'){addtok(tv,T_SHL,p,2,0,0,0,line);p+=2;} else if(p[1]=='='){addtok(tv,T_LE,p,2,0,0,0,line);p+=2;} else {addtok(tv,T_LT,p,1,0,0,0,line);p++;} break;
            case '>': if(p[1]=='>'&&p[2]=='='){addtok(tv,T_SHR_ASSIGN,p,3,0,0,0,line);p+=3;} else if(p[1]=='>'){addtok(tv,T_SHR,p,2,0,0,0,line);p+=2;} else if(p[1]=='='){addtok(tv,T_GE,p,2,0,0,0,line);p+=2;} else {addtok(tv,T_GT,p,1,0,0,0,line);p++;} break;
            default: fprintf(stderr,"bad char '%c' at line %d\n",*p,line); exit(1);
        }
    }
}

TokVec lex(const char *src){
    TokVec tv={0}; int ind[256]; int top=0; ind[0]=0; int line=1; const char *p=src;
    while(*p){
        const char *ls=p; const char *e=my_strchr(p,'\n'); int len=e?(int)(e-p):(int)my_strlen(p); p=e?e+1:p+len; int i=0,spaces=0; while(i<len&&ls[i]==' '){spaces++;i++;} int j=i; while(j<len&&(ls[j]==' '||ls[j]=='\t'||ls[j]=='\r'))j++; if(j>=len||ls[j]=='#'){line++;continue;}
        if(spaces>ind[top]){ ind[++top]=spaces; addtok(&tv,T_INDENT,NULL,0,0,0,0,line); }
        else while(spaces<ind[top]){ top--; addtok(&tv,T_DEDENT,NULL,0,0,0,0,line); }
        if(spaces!=ind[top]){ fprintf(stderr,"bad indentation at line %d\n",line); exit(1); }
        char *buf=xstrndup2(ls+i,len-i); lex_line(&tv,buf,line); free(buf); addtok(&tv,T_NEWLINE,NULL,0,0,0,0,line); line++; }
    while(top>0){ top--; addtok(&tv,T_DEDENT,NULL,0,0,0,0,line); } addtok(&tv,T_EOF,NULL,0,0,0,0,line);
    return tv;
}
