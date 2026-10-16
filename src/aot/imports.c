/* ========================= AOT driver: the import pass =========================
   Every module the program imports, found and parsed at compile time;
   the library modules a program needs without importing them (sys.argv,
   codecs, ...). */

#include "driver.h"

AotUnit *find_unit(Units *us, const char *name){ for(int i=0;i<us->n;i++) if(!strcmp(us->v[i]->name,name)) return us->v[i]; return NULL; }
/* `if __name__ == "__main__":` at the top level of a module */
static int is_main_guard(AotUnit *u, Stmt *s){
    (void)u;
    Expr *e=s->expr;
    if(s->kind!=STMT_IF || !e || e->kind!=EXPR_COMPARE || e->count!=2 || e->items[1]->akind!=CMP_EQ) return 0;
    Expr *a=e->items[0], *b=e->items[1];
    if(a->kind!=EXPR_NAME){ Expr *t=a; a=b; b=t; }
    return a->kind==EXPR_NAME && !strcmp(a->name,"__name__") && b->kind==EXPR_LITERAL && b->tok->kind==T_STRING && !strcmp(b->tok->text,"__main__");
}
static int is_docstring(AotUnit *u, Stmt *s);
/* The guard's body runs in the program's main module and never in an imported
   one: it is put in place of the if (or its else part is). Imports in it move
   up to the module's other imports - a compiled module imports at its top. */
static void apply_main_guards(AotUnit *u, int is_main){
    Ast *m=u->ast; int n=m->body_count, header=0, any=0;
    for(int i=0;i<n;i++) if(is_main_guard(u,m->body[i])) any=1;
    if(!any) return;
    while(header<n && (m->body[header]->kind==STMT_IMPORT || m->body[header]->kind==STMT_FROM_IMPORT || (header==0 && is_docstring(u,m->body[0])))) header++;
    int cap=n; for(int i=0;i<n;i++) if(is_main_guard(u,m->body[i])) cap+=m->body[i]->body_count+m->body[i]->orelse_count;
    Stmt **out=PPY_NEW_ARR(Stmt*,cap), **rest=PPY_NEW_ARR(Stmt*,cap); int k=0, r=0;
    for(int i=0;i<header;i++) out[k++]=m->body[i];
    for(int i=header;i<n;i++){ Stmt *s=m->body[i];
        if(!is_main_guard(u,s)){ rest[r++]=s; continue; }
        Stmt **b=is_main?s->body:s->orelse; int bn=is_main?s->body_count:s->orelse_count;
        for(int j=0;j<bn;j++){
            if(b[j]->kind==STMT_IMPORT || b[j]->kind==STMT_FROM_IMPORT) out[k++]=b[j];
            else rest[r++]=b[j];
        }
    }
    for(int i=0;i<r;i++) out[k++]=rest[i];
    free(rest);
    m->body=out; m->body_count=k; m->body_cap=cap;
}
AotUnit *add_unit(Units *us, const char *name, const char *path, char *src){
    AotUnit *u=PPY_NEW0(AotUnit);
    u->name=xstrdup2(name); u->path=path?xstrdup2(path):NULL; u->src=src;
    if(src){
        if(!(u->ast=py_front(path,src))){
            if(!aot_check_mode) exit(1);
            u->failed=1; aot_check_errors++;                   /* (check mode: left out, the others go on) */
        } else apply_main_guards(u,!strcmp(name,"__main__"));
    }
    if(us->n==us->cap){ us->cap=us->cap?us->cap*2:8; us->v=(AotUnit**)xrealloc(us->v,sizeof(AotUnit*)*(size_t)us->cap); }
    us->v[us->n++]=u;
    return u;
}
static int is_builtin_module(const char *name){
    static const char *mods[]={"sys","thread","asyncio","math","typing","functools","__future__","collections.abc","ctypes","json","plainpy","dataclasses","abc",NULL};  /* (thread: above) */
    for(int i=0;mods[i];i++) if(!strcmp(mods[i],name)) return 1;
    return 0;
}

/* sys.stderr used as an object (print(..., file=sys.stderr) needs none) */
int has_stderr_value(const char *src){
    for(const char *p=strstr(src,".stderr");p;p=strstr(p+1,".stderr")){
        const char *q=p; if(q-src>=3 && !strncmp(q-3,"sys",3)) q-=3; else return 1;
        while(q>src && (q[-1]==' ')) q--;
        if(q>src && q[-1]=='='){ q--; while(q>src && q[-1]==' ') q--; if(q-src>=4 && !strncmp(q-4,"file",4)) continue; }
        return 1; }
    return 0;
}
/* `import name` at the top of the program's main module: a module the compiler adds
   (sys.argv's) runs its top level before the program, as an imported one would */
void main_imports(AotUnit *main, const char *name){
    char src[160]; snprintf(src,sizeof src,"import %s\n",name);
    Stmt *blk=py_front_stmts(main->path,src,1);
    if(!blk || !blk->body_count) return;
    Ast *m=main->ast; Stmt **nb=PPY_NEW_ARR(Stmt*,m->body_count+1);
    nb[0]=blk->body[0]; for(int i=0;i<m->body_count;i++) nb[i+1]=m->body[i];
    m->body=nb; m->body_count++; m->body_cap=m->body_count;
}
/* code run when the main module's code is done (as CPython's interpreter ends: threading._shutdown(), atexit) */
void main_epilogue(AotUnit *main, const char *code){
    Stmt *blk=py_front_stmts(main->path,code,main->ast->body_count?main->ast->body[main->ast->body_count-1]->line:1);
    if(!blk || !blk->body_count) return;
    Ast *m=main->ast; Stmt **nb=PPY_NEW_ARR(Stmt*,m->body_count+blk->body_count);
    for(int i=0;i<m->body_count;i++) nb[i]=m->body[i];
    for(int i=0;i<blk->body_count;i++) nb[m->body_count+i]=blk->body[i];
    m->body=nb; m->body_count+=blk->body_count; m->body_cap=m->body_count;
}
/* ---- does a program need _codecs_more (the code pages, utf-16 / utf-32: 17 KB)? It does when
   its code (not its comments) names one of those encodings in a string literal ('cp866',
   "UTF-16-LE" ...) or hands an encoding from elsewhere to what takes one: s.encode(enc),
   str(b, enc), open(f, encoding=name), codecs.lookup(x) ... */
static int word_char(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||(unsigned char)c>=0x80; }
/* p at a quote: just past its literal (a triple-quoted one too) */
static const char *skip_quoted(const char *p){
    char q=*p; int triple=(p[1]==q && p[2]==q);
    p+=triple?3:1;
    while(*p){
        if(*p=='\\'){ if(p[1]) p++; p++; continue; }
        if(*p==q && (!triple || (p[1]==q && p[2]==q))) return p+(triple?3:1);
        if(*p=='\n' && !triple) return p;
        p++; }
    return p;
}
/* a word that is a string literal's prefix (b'..', rb"..", f'..'): its quote, else NULL */
static const char *prefixed_quote(const char *s, const char *e){
    if(e-s>2 || (*e!='"' && *e!='\'')) return NULL;
    for(const char *p=s;p<e;p++) if(!strchr("rRbBfFuUtT",*p)) return NULL;
    return e;
}
/* a literal's text (between its quotes) is one of _codecs_more's names (as codecs.py: case
   and '_' / ' ' against '-' do not matter) */
static int more_codec_text(const char *s, size_t n){
    static const char *const names[]={"cp437","cp850","cp866","cp1250","cp1251","cp1252","koi8-r","koi8-u","iso8859-2","iso8859-5",
        "iso8859-15","mac-roman","437","ibm437","850","ibm850","866","ibm866","windows-1250","windows-1251","windows-1252","iso-8859-2",
        "latin2","l2","iso-8859-5","cyrillic","iso-8859-15","latin9","l9","macroman","utf-16","utf16","utf-16-le","utf-16le","utf-16-be",
        "utf-16be","utf-32","utf32","utf-32-le","utf-32le","utf-32-be","utf-32be","raw-unicode-escape","unicode-escape",NULL};
    char t[24]; if(n>=sizeof t) return 0;
    for(size_t i=0;i<n;i++){ char c=s[i]; if(c>='A'&&c<='Z') c=(char)(c+32); if(c=='_'||c==' ') c='-'; t[i]=c; }
    t[n]=0;
    for(int k=0;names[k];k++) if(!strcmp(t,names[k])) return 1;
    return 0;
}
/* the literal from its quote q to e names one of them */
static int more_codec_literal(const char *q, const char *e){
    int triple=(e-q>=6 && q[1]==*q && q[2]==*q);
    const char *a=q+(triple?3:1), *b=e-(triple?3:1);
    return b>a && more_codec_text(a,(size_t)(b-a));
}
/* an argument (or a keyword's value) [s, e) given to what takes an encoding: 1 if it may be one
   of _codecs_more's - a literal naming one, or anything but a literal and None */
static int more_codec_value(const char *s, const char *e){
    while(s<e && (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\\')) s++;
    while(e>s && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\n'||e[-1]=='\r')) e--;
    if(s==e) return 0;
    const char *w=s; while(w<e && word_char(*w)) w++;
    if(w-s==4 && !strncmp(s,"None",4) && w==e) return 0;
    const char *q=(*s=='"'||*s=='\'')?s:prefixed_quote(s,w);
    if(q){ const char *end=skip_quoted(q); if(end==e) return more_codec_literal(q,end); }
    return 1;
}
/* p just past a call's '(': may its positional argument k be one of _codecs_more's encodings?
   (k<0: p is a keyword's value, up to a ',', ')' or the line's end) */
static int more_codec_arg(const char *p, int k){
    int depth=0, idx=0; const char *start=p;
    while(*p){
        char c=*p;
        if(c=='"'||c=='\''){ p=skip_quoted(p); continue; }
        if(c=='#'){ while(*p && *p!='\n') p++; continue; }
        if(c=='('||c=='['||c=='{'){ depth++; p++; continue; }
        if(depth==0 && (c==')'||c==']'||c=='}'||c==','||(c=='\n' && k<0))){
            if(idx==k || k<0){
                const char *w=start; while(*w==' '||*w=='\n'||*w=='\t') w++;
                const char *we=w; while(word_char(*we)) we++;
                const char *x=we; while(*x==' ') x++;
                if(k>=0 && we>w && *x=='=' && x[1]!='=') return 0;                 /* (keywords: no positional ones after) */
                if(k>=0 && *w=='*') return 1;
                return more_codec_value(start,p); }
            if(c!=',') return 0;
            idx++; start=++p; continue; }
        if(c==')'||c==']'||c=='}') depth--;
        p++; }
    return 0;
}
/* what takes an encoding as its positional argument `arg`: its name and what is before its dot
   ("": nothing, ".": any other object) */
static const struct { const char *name, *qual; int arg; } codec_takers[]={
    {"encode","codecs",1},{"decode","codecs",1},{"encode",".",0},{"decode",".",0},{"str","",1},{"bytes","",1},{"bytearray","",1},
    {"open","",3},{"open","io",3},{"open","codecs",2},{"TextIOWrapper",".",1},{"TextIOWrapper","",1},{"lookup","codecs",0},
    {"getencoder","codecs",0},{"getdecoder","codecs",0},{"getincrementalencoder","codecs",0},{"getincrementaldecoder","codecs",0},
    {"getreader","codecs",0},{"getwriter","codecs",0},{"iterencode","codecs",1},{"iterdecode","codecs",1},{NULL,NULL,0}};
int names_codec(const char *t){
    if(strstr(t,"from codecs import")) return 1;
    for(const char *p=t;*p;){
        char c=*p;
        if(c=='#'){ while(*p && *p!='\n') p++; continue; }
        if(c=='"'||c=='\''){ const char *e=skip_quoted(p); if(more_codec_literal(p,e)) return 1; p=e; continue; }
        if(!word_char(c)){ p++; continue; }
        const char *s=p, *e=p; while(word_char(*e)) e++;
        const char *q=prefixed_quote(s,e);
        if(q){ const char *end=skip_quoted(q); if(more_codec_literal(q,end)) return 1; p=end; continue; }
        p=e;
        if(*s>='0' && *s<='9') continue;
        const char *qs=NULL; size_t ql=0; int dotted=(s>t && s[-1]=='.');
        if(dotted){ const char *b=s-1; while(b>t && word_char(b[-1])) b--; qs=b; ql=(size_t)(s-1-b); }
        const char *r=e; while(*r==' ') r++;
        size_t n=(size_t)(e-s);
        if(n==8 && !strncmp(s,"encoding",8) && !dotted && *r=='=' && r[1]!='='){     /* encoding=...: a keyword, a default, a variable */
            if(more_codec_arg(r+1,-1)) return 1;
            continue; }
        if(*r!='(') continue;
        for(int k=0;codec_takers[k].name;k++){
            const char *nm=codec_takers[k].name, *qu=codec_takers[k].qual;
            if(strlen(nm)!=n || strncmp(nm,s,n)) continue;
            if(*qu=='.' ? !dotted : *qu ? !(dotted && strlen(qu)==ql && !strncmp(qu,qs,ql)) : dotted) continue;
            if(more_codec_arg(r+1,codec_takers[k].arg)) return 1;
            break; } }
    return 0;
}
static int is_package_path(const char *path){ const char *b=strrchr(path,'/'); b=b?b+1:path; return !strncmp(b,"__init__.",9); }
static char *concat3(const char *a, const char *b, const char *c){ size_t la=strlen(a), lb=strlen(b), lc=strlen(c); char *r=(char*)xmalloc(la+lb+lc+1); memcpy(r,a,la); memcpy(r+la,b,lb); memcpy(r+la+lb,c,lc+1); return r; }
/* the file of module a.b.c when package a.b is loaded: in its folder */
static char *package_member(Units *us, const char *dotted){
    const char *dot=strrchr(dotted,'.'); if(!dot) return NULL;
    char *parent=xstrndup2(dotted,(int)(dot-dotted)); AotUnit *t=find_unit(us,parent); free(parent);
    if(!t || !t->path || !is_package_path(t->path)) return NULL;
    char *dir=ppy_fs_dirname(t->path);
    char *r=ppy_fs_find_module(dir,dot+1);
    if(r && strncmp(r,dir,strlen(dir))){ free(r); r=NULL; }            /* (only the package's own) */
    free(dir);
    return r;
}
/* Make `dotted` and each of its prefixes available, as vm_import_dotted would. */
/* standard library modules written for the interpreter only (metaclasses, introspection ...): not in plainpyc */
static int interp_only_module(const char *name){ return ppy_stdlib_left_out(name); }
static int resolve_import(Units *us, AotUnit *from, const char *dotted, int line){
    char *dir=ppy_fs_dirname(from->path);
    int ok=1;
    for(const char *p=dotted;;){
        const char *dot=strchr(p,'.');
        char *prefix=xstrndup2(dotted,dot?(int)(dot-dotted):(int)strlen(dotted));
        int leaf=(dot==NULL);
        if(!find_unit(us,prefix)){
            int pkg=0; const char *ss; const char *al=ppy_stdlib_alias(prefix);
            if(al){ if(!find_unit(us,al) && (ss=ppy_stdlib_source(al,&pkg))){ char path[300]; snprintf(path,sizeof path,"<stdlib>/%s.py",al); add_unit(us,al,path,xstrdup2(ss)); } }   /* os.path: posixpath */
            else if(is_builtin_module(prefix)){
                if(!leaf && strcmp(dotted,"collections.abc")){ fprintf(stderr,"%s:%d: error: built-in module '%s' has no submodules\n",from->path,line,prefix); ok=0; }
            } else if(interp_only_module(prefix)){
                fprintf(stderr,"%s:%d: error: module '%s' is only available in the interpreter (not in compiled programs)\n",from->path,line,prefix); ok=0;
            } else if((ss=ppy_stdlib_source(prefix,&pkg))){            /* the standard library written in Python */
                char path[300]; char *slashed=xstrdup2(prefix); for(char *q=slashed;*q;q++) if(*q=='.') *q='/';
                snprintf(path,sizeof path,pkg?"<stdlib>/%s/__init__.py":"<stdlib>/%s.py",slashed); free(slashed);
                add_unit(us,prefix,path,xstrdup2(ss));                 /* (its imports: scanned with every unit's) */
            } else {
                char *path=strchr(prefix,'.') ? package_member(us,prefix) : NULL, *err=NULL;    /* pkg.sub: next to the package */
                if(!path) path=ppy_fs_find_module(dir,prefix);       /* the importer's folder, PLAINPYPATH, <plainpy>/lib */
                if(!path && us->n>0 && us->v[0]->path){              /* the main program's folder (as the interpreter: a project's */
                    char *main_dir=ppy_fs_dirname(us->v[0]->path);  /* packages import its top-level modules) */
                    path=ppy_fs_find_module(main_dir,prefix); free(main_dir); }
                char *src=path?ppy_fs_try_read_file(path,&err):NULL;
                free(err);
                char *why=(src && !aot_check_mode) ? ppy_fs_package_problem(path,prefix) : NULL;   /* a package of the project: checked? */
                if(why){ fprintf(stderr,"%s:%d: error: %s\n",from->path,line,why); free(why); free(src); ok=0; }
                else if(src) add_unit(us,prefix,path,src);
                else if(!leaf) add_unit(us,prefix,NULL,NULL);          /* namespace package */
                else { fprintf(stderr,"%s:%d: error: No module named '%s'\n",from->path,line,prefix); ok=0; }
                free(path);
            }
        }
        free(prefix);
        if(leaf || !ok) break;
        p=dot+1;
    }
    free(dir);
    return ok;
}

/* What an import nested in statement `s` would be inside of. */
static const char *construct_name(Stmt *s, const char *outer){
    switch(s->kind){
        case STMT_FUNCTION_DEF: return "a function";
        case STMT_CLASS_DEF: return "a class";
        case STMT_IF: return "an if statement";
        case STMT_WHILE: case STMT_FOR: return "a loop";
        case STMT_TRY: return "a try statement";
        case STMT_WITH: return "a with statement";
        default: return outer;
    }
}
static int nested_import_error(AotUnit *u, Stmt **b, int n, const char *where){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_IMPORT || s->kind==STMT_FROM_IMPORT){
            fprintf(stderr,"%s:%d: error: import inside %s; in compiled programs every import must be at the top of the module\n",u->path,s->line,where);
            return 1; }
        const char *w=construct_name(s,where);
        if(nested_import_error(u,s->body,s->body_count,w) || nested_import_error(u,s->orelse,s->orelse_count,w)) return 1;
    }
    return 0;
}
static int is_docstring(AotUnit *u, Stmt *s){
    (void)u;
    return s->kind==STMT_EXPR && !s->ann_only && s->expr && s->expr->kind==EXPR_LITERAL && s->expr->tok->kind==T_STRING;
}
/* The imported modules: from import statements anywhere (one inside a
   function or a block runs the module's top level where it is, as in Python). */
/* `sys._compiled` (1) / `not sys._compiled` (0) as an if's test, else -1: the interpreter's branches import nothing here */
static int compiled_test(Expr *e){
    if(!e) return -1;
    if(e->kind==EXPR_UNARY && e->op==T_NOT){ int r=compiled_test(e->a); return r<0?-1:!r; }
    if(e->kind==EXPR_ATTRIBUTE && !strcmp(e->name,"_compiled") && e->a && e->a->kind==EXPR_NAME && !strcmp(e->a->name,"sys")) return 1;
    return -1;
}
static int scan_imports_in(Units *us, AotUnit *u, Stmt **b, int n){
    for(int i=0;i<n;i++){ Stmt *s=b[i];
        if(s->kind==STMT_IF){ int r=compiled_test(s->expr);
            if(r==1){ if(!scan_imports_in(us,u,s->body,s->body_count)) return 0; continue; }
            if(r==0){ if(!scan_imports_in(us,u,s->orelse,s->orelse_count)) return 0; continue; } }
        if(s->kind==STMT_IMPORT || s->kind==STMT_FROM_IMPORT){
            char *mod=s->kind==STMT_IMPORT ? xstrdup2(s->name2?s->name2:s->name) : aot_from_import_module(u,s);
            if(!mod) return 0;
            int ok=resolve_import(us,u,mod,s->line);
            if(ok && s->kind==STMT_FROM_IMPORT && !is_builtin_module(mod))    /* from pkg import sub: a submodule, when there is one */
                for(int k=0;k<s->nnames;k++){ if(!strcmp(s->names[k],"*")) continue;
                    char *sub=concat3(mod,".",s->names[k]); AotUnit *pk=find_unit(us,mod);
                    if(pk && pk->path && is_package_path(pk->path) && !find_unit(us,sub)){ char *path=package_member(us,sub);
                        if(path){ char *err=NULL, *src=ppy_fs_try_read_file(path,&err); free(err); if(src) add_unit(us,sub,path,src); free(path); } }
                    free(sub); }
            free(mod);
            if(!ok) return 0;
            continue;
        }
        if(!scan_imports_in(us,u,s->body,s->body_count) || !scan_imports_in(us,u,s->orelse,s->orelse_count)) return 0;
    }
    return 1;
}
int scan_imports(Units *us, AotUnit *u){ (void)nested_import_error; return scan_imports_in(us,u,u->ast->body,u->ast->body_count); }

