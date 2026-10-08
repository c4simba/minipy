/* ========================= Full Python: tree dump =========================
   `minipy --pyast file.py` prints the tree in a canonical text form that
   tests/pyast_check.py also prints for CPython's own `ast` of the same file,
   so the two parsers can be compared over whole libraries. The schema below
   says where each field of each node kind lives in PyNode (py_ast.h); its
   order is CPython's `_fields` (without type_comment / type_ignores). */

#include "py_ast.h"

typedef enum { FT_NONE, FT_NODE, FT_LIST, FT_ID, FT_IDLIST, FT_INT, FT_OP, FT_CTX, FT_CONST, FT_KIND, FT_STR, FT_OPLIST } FieldType;
typedef struct { const char *name; FieldType t; int slot; } Field;
typedef struct { const char *name; Field f[8]; } Schema;

#define N(nm,s) {nm,FT_NODE,s}
#define L(nm,s) {nm,FT_LIST,s}
#define I(nm,s) {nm,FT_ID,s}
static const Schema schema[PK__COUNT]={
    [PK_Module]={"Module",{L("body",0)}},
    [PK_FunctionDef]={"FunctionDef",{I("name",0),N("args",0),L("body",0),L("decorator_list",1),N("returns",1),L("type_params",2)}},
    [PK_AsyncFunctionDef]={"AsyncFunctionDef",{I("name",0),N("args",0),L("body",0),L("decorator_list",1),N("returns",1),L("type_params",2)}},
    [PK_ClassDef]={"ClassDef",{I("name",0),L("bases",3),L("keywords",4),L("body",0),L("decorator_list",1),L("type_params",2)}},
    [PK_Return]={"Return",{N("value",0)}},
    [PK_Delete]={"Delete",{L("targets",0)}},
    [PK_Assign]={"Assign",{L("targets",0),N("value",0)}},
    [PK_TypeAlias]={"TypeAlias",{N("name",0),L("type_params",2),N("value",1)}},
    [PK_AugAssign]={"AugAssign",{N("target",0),{"op",FT_OP,0},N("value",1)}},
    [PK_AnnAssign]={"AnnAssign",{N("target",0),N("annotation",1),N("value",2),{"simple",FT_INT,0}}},
    [PK_For]={"For",{N("target",0),N("iter",1),L("body",0),L("orelse",1)}},
    [PK_AsyncFor]={"AsyncFor",{N("target",0),N("iter",1),L("body",0),L("orelse",1)}},
    [PK_While]={"While",{N("test",0),L("body",0),L("orelse",1)}},
    [PK_If]={"If",{N("test",0),L("body",0),L("orelse",1)}},
    [PK_With]={"With",{L("items",3),L("body",0)}},
    [PK_AsyncWith]={"AsyncWith",{L("items",3),L("body",0)}},
    [PK_Match]={"Match",{N("subject",0),L("cases",3)}},
    [PK_Raise]={"Raise",{N("exc",0),N("cause",1)}},
    [PK_Try]={"Try",{L("body",0),L("handlers",3),L("orelse",1),L("finalbody",2)}},
    [PK_TryStar]={"TryStar",{L("body",0),L("handlers",3),L("orelse",1),L("finalbody",2)}},
    [PK_Assert]={"Assert",{N("test",0),N("msg",1)}},
    [PK_Import]={"Import",{L("names",3)}},
    [PK_ImportFrom]={"ImportFrom",{I("module",0),L("names",3),{"level",FT_INT,0}}},
    [PK_Global]={"Global",{{"names",FT_IDLIST,3}}},
    [PK_Nonlocal]={"Nonlocal",{{"names",FT_IDLIST,3}}},
    [PK_Expr]={"Expr",{N("value",0)}},
    [PK_Pass]={"Pass",{{0}}}, [PK_Break]={"Break",{{0}}}, [PK_Continue]={"Continue",{{0}}},
    [PK_BoolOp]={"BoolOp",{{"op",FT_OP,0},L("values",0)}},
    [PK_NamedExpr]={"NamedExpr",{N("target",0),N("value",1)}},
    [PK_BinOp]={"BinOp",{N("left",0),{"op",FT_OP,0},N("right",1)}},
    [PK_UnaryOp]={"UnaryOp",{{"op",FT_OP,0},N("operand",0)}},
    [PK_Lambda]={"Lambda",{N("args",0),N("body",1)}},
    [PK_IfExp]={"IfExp",{N("test",0),N("body",1),N("orelse",2)}},
    [PK_Dict]={"Dict",{L("keys",0),L("values",1)}},
    [PK_Set]={"Set",{L("elts",0)}},
    [PK_ListComp]={"ListComp",{N("elt",0),L("generators",0)}},
    [PK_SetComp]={"SetComp",{N("elt",0),L("generators",0)}},
    [PK_DictComp]={"DictComp",{N("key",0),N("value",1),L("generators",0)}},
    [PK_GeneratorExp]={"GeneratorExp",{N("elt",0),L("generators",0)}},
    [PK_Await]={"Await",{N("value",0)}},
    [PK_Yield]={"Yield",{N("value",0)}},
    [PK_YieldFrom]={"YieldFrom",{N("value",0)}},
    [PK_Compare]={"Compare",{N("left",0),{"ops",FT_OPLIST,0},L("comparators",1)}},
    [PK_Call]={"Call",{N("func",0),L("args",0),L("keywords",1)}},
    [PK_FormattedValue]={"FormattedValue",{N("value",0),{"conversion",FT_INT,0},N("format_spec",1)}},
    [PK_Interpolation]={"Interpolation",{N("value",0),{"str",FT_STR,0},{"conversion",FT_INT,0},N("format_spec",1)}},
    [PK_JoinedStr]={"JoinedStr",{L("values",0)}},
    [PK_TemplateStr]={"TemplateStr",{L("values",0)}},
    [PK_Constant]={"Constant",{{"value",FT_CONST,0},{"kind",FT_KIND,0}}},
    [PK_Attribute]={"Attribute",{N("value",0),I("attr",0),{"ctx",FT_CTX,0}}},
    [PK_Subscript]={"Subscript",{N("value",0),N("slice",1),{"ctx",FT_CTX,0}}},
    [PK_Starred]={"Starred",{N("value",0),{"ctx",FT_CTX,0}}},
    [PK_Name]={"Name",{I("id",0),{"ctx",FT_CTX,0}}},
    [PK_List]={"List",{L("elts",0),{"ctx",FT_CTX,0}}},
    [PK_Tuple]={"Tuple",{L("elts",0),{"ctx",FT_CTX,0}}},
    [PK_Slice]={"Slice",{N("lower",0),N("upper",1),N("step",2)}},
    [PK_comprehension]={"comprehension",{N("target",0),N("iter",1),L("ifs",0),{"is_async",FT_INT,0}}},
    [PK_ExceptHandler]={"ExceptHandler",{N("type",0),I("name",0),L("body",0)}},
    [PK_arguments]={"arguments",{L("posonlyargs",0),L("args",1),N("vararg",0),L("kwonlyargs",2),L("kw_defaults",3),N("kwarg",1),L("defaults",4)}},
    [PK_arg]={"arg",{I("arg",0),N("annotation",0)}},
    [PK_keyword]={"keyword",{I("arg",0),N("value",0)}},
    [PK_alias]={"alias",{I("name",0),I("asname",1)}},
    [PK_withitem]={"withitem",{N("context_expr",0),N("optional_vars",1)}},
    [PK_match_case]={"match_case",{N("pattern",0),N("guard",1),L("body",0)}},
    [PK_MatchValue]={"MatchValue",{N("value",0)}},
    [PK_MatchSingleton]={"MatchSingleton",{{"value",FT_CONST,0}}},
    [PK_MatchSequence]={"MatchSequence",{L("patterns",0)}},
    [PK_MatchMapping]={"MatchMapping",{L("keys",0),L("patterns",1),I("rest",0)}},
    [PK_MatchClass]={"MatchClass",{N("cls",0),L("patterns",0),{"kwd_attrs",FT_IDLIST,1},L("kwd_patterns",2)}},
    [PK_MatchStar]={"MatchStar",{I("name",0)}},
    [PK_MatchAs]={"MatchAs",{N("pattern",0),I("name",0)}},
    [PK_MatchOr]={"MatchOr",{L("patterns",0)}},
    [PK_TypeVar]={"TypeVar",{I("name",0),N("bound",0),N("default_value",1)}},
    [PK_ParamSpec]={"ParamSpec",{I("name",0),N("default_value",1)}},
    [PK_TypeVarTuple]={"TypeVarTuple",{I("name",0),N("default_value",1)}},
    [PK_ident]={"ident",{I("id",0)}},
};
const char *py_kind_name(PyKind k){ return k<PK__COUNT && schema[k].name ? schema[k].name : "?"; }

static const char *op_name(int op){
    static const char *n[]={"?","Add","Sub","Mult","MatMult","Div","Mod","Pow","LShift","RShift","BitOr","BitXor","BitAnd","FloorDiv",
        "Invert","Not","UAdd","USub","And","Or","Eq","NotEq","Lt","LtE","Gt","GtE","Is","IsNot","In","NotIn"};
    return op>0 && op<=OP_NotIn ? n[op] : "?";
}
static const char *ctx_name(int c){ return c==CTX_Store?"Store":c==CTX_Del?"Del":"Load"; }

static void put_cp(FILE *o, uint32_t c){
    if(c>=32 && c<127 && c!='\\' && c!='\'') fputc((int)c,o);
    else fprintf(o,"\\u{%x}",c);
}
static void put_ident(FILE *o, const char *s){
    if(!s){ fputs("None",o); return; }
    fputc('\'',o);
    const unsigned char *u=(const unsigned char*)s;
    while(*u){
        uint32_t c=*u; int n= c<0x80?1 : (c>>5)==6?2 : (c>>4)==14?3 : 4;
        uint32_t v= n==1?c : n==2?(c&31u) : n==3?(c&15u) : (c&7u);
        for(int k=1;k<n && u[k];k++) v=(v<<6)|(u[k]&63u);
        put_cp(o,v); u+=n;
    }
    fputc('\'',o);
}
/* an integer literal (decimal, 0x, 0o, 0b; any size) in hex */
static void put_int(FILE *o, const char *t){
    int base=10; const char *s=t;
    if(s[0]=='0' && (s[1]=='x'||s[1]=='X')){ base=16; s+=2; }
    else if(s[0]=='0' && (s[1]=='o'||s[1]=='O')){ base=8; s+=2; }
    else if(s[0]=='0' && (s[1]=='b'||s[1]=='B')){ base=2; s+=2; }
    int n=1, cap=8; uint32_t *l=(uint32_t*)calloc((size_t)cap,4);
    for(;*s;s++){
        int d= *s>='0'&&*s<='9' ? *s-'0' : *s>='a'&&*s<='f' ? *s-'a'+10 : *s>='A'&&*s<='F' ? *s-'A'+10 : -1;
        if(d<0) continue;
        uint64_t carry=(uint64_t)d;
        for(int i=0;i<n;i++){ uint64_t v=(uint64_t)l[i]*(uint64_t)base+carry; l[i]=(uint32_t)v; carry=v>>32; }
        if(carry){ if(n==cap){ cap*=2; l=(uint32_t*)realloc(l,(size_t)cap*4); } l[n++]=(uint32_t)carry; }
    }
    while(n>1 && !l[n-1]) n--;
    fprintf(o,"int:%x",l[n-1]);
    for(int i=n-2;i>=0;i--) fprintf(o,"%08x",l[i]);
    free(l);
}
static void put_bits(FILE *o, const char *t){ double d=strtod(t,NULL); uint64_t b; memcpy(&b,&d,8); fprintf(o,"%016llx",(unsigned long long)b); }
static void put_const(FILE *o, PyConst *k){
    switch(k->kind){
        case PC_None: fputs("None",o); return;
        case PC_True: fputs("True",o); return;
        case PC_False: fputs("False",o); return;
        case PC_Ellipsis: fputs("Ellipsis",o); return;
        case PC_Int: put_int(o,k->text); return;
        case PC_Float: fputs("float:",o); put_bits(o,k->text); return;
        case PC_Complex: fputs("complex:",o); put_bits(o,k->text); return;
        case PC_Bytes: fputs("b'",o); for(int i=0;i<k->blen;i++) fprintf(o,"%02x",k->b[i]); fputc('\'',o); return;
        case PC_Str:
            if(k->text) fputc('N',o);
            fputc('\'',o); for(int i=0;i<k->ulen;i++) put_cp(o,k->u[i]); fputc('\'',o); return;
    }
}
static void dump(PyNode *n, FILE *o){
    if(!n){ fputs("None",o); return; }
    const Schema *s=&schema[n->kind];
    fprintf(o,"%s(",s->name);
    for(int i=0;i<8 && s->f[i].name;i++){
        const Field *f=&s->f[i];
        if(i) fputs(", ",o);
        fprintf(o,"%s=",f->name);
        switch(f->t){
            case FT_NODE: dump(n->n[f->slot],o); break;
            case FT_LIST:{ PyList *l=&n->L[f->slot]; fputc('[',o); for(int k=0;k<l->n;k++){ if(k) fputs(", ",o); dump(l->v[k],o); } fputc(']',o); break; }
            case FT_ID: put_ident(o,n->id[f->slot]); break;
            case FT_IDLIST:{ PyList *l=&n->L[f->slot]; fputc('[',o); for(int k=0;k<l->n;k++){ if(k) fputs(", ",o); put_ident(o,l->v[k]->id[0]); } fputc(']',o); break; }
            case FT_OPLIST:{ PyList *l=&n->L[f->slot]; fputc('[',o); for(int k=0;k<l->n;k++){ if(k) fputs(", ",o); fputs(op_name(l->v[k]->op),o); } fputc(']',o); break; }
            case FT_INT: fprintf(o,"%d",n->op); break;
            case FT_OP: fputs(op_name(n->op),o); break;
            case FT_CTX: fputs(ctx_name(n->op),o); break;
            case FT_CONST: put_const(o,n->k); break;
            case FT_KIND: fputs(n->k && n->k->is_u ? "'u'" : "None",o); break;
            case FT_STR: put_const(o,n->k); break;
            default: break;
        }
    }
    fputc(')',o);
}
void py_dump(PyNode *n, FILE *out){ dump(n,out); fputc('\n',out); }

/* minipy --pyast [--quiet] file.py ...: the tree of each file (--quiet: only errors) */
int py_dump_main(int argc, char **argv){
    int quiet=0, rc=0;
    for(int i=0;i<argc;i++){
        if(!strcmp(argv[i],"--quiet")){ quiet=1; continue; }
        FILE *f=fopen(argv[i],"rb");
        if(!f){ fprintf(stderr,"%s: cannot open\n",argv[i]); rc=1; continue; }
        fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
        char *src=(char*)xmalloc((size_t)n+1); if(fread(src,1,(size_t)n,f)!=(size_t)n){ fclose(f); rc=1; continue; } src[n]=0; fclose(f);
        PyParse pp;
        if(py_parse(&pp,argv[i],src,(size_t)n)){ printf("%s:%d:%d: SyntaxError: %s\n",argv[i],pp.error_line,pp.error_col,pp.error); rc=1; continue; }
        if(!quiet) py_dump(pp.mod,stdout);
    }
    return rc;
}
