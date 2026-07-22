/* ========================= Bytecode ========================= */

#include "bytecode.h"

Chunk *chunk_new(void){ Chunk *c=MPY_NEW0(Chunk); return c; }
int add_const(Chunk *c,Value v){ if(c->ccount==c->ccap){ c->ccap=c->ccap?c->ccap*2:32; c->consts=(Value*)xrealloc(c->consts,sizeof(Value)*(size_t)c->ccap); } c->consts[c->ccount]=v; return c->ccount++; }
int emit(Chunk *c,int x,int line){ if(c->count==c->cap){ c->cap=c->cap?c->cap*2:128; c->code=(int*)xrealloc(c->code,sizeof(int)*(size_t)c->cap); c->line=(int*)xrealloc(c->line,sizeof(int)*(size_t)c->cap); } c->code[c->count]=x; c->line[c->count]=line; return c->count++; }
int emit_op(Chunk *c,Op op,int line){ if(op==OP_YIELD) c->has_yield=1; return emit(c,(int)op,line); }
int emit_arg(Chunk *c,Op op,int arg,int line){ emit_op(c,op,line); return emit(c,arg,line); }
void patch(Chunk *c,int at,int val){ c->code[at]=val; }

static const char *op_name(int op){
    switch((Op)op){
        case OP_CONST: return "OP_CONST"; case OP_NONE: return "OP_NONE"; case OP_TRUE: return "OP_TRUE"; case OP_FALSE: return "OP_FALSE";
        case OP_LOAD: return "OP_LOAD"; case OP_STORE: return "OP_STORE"; case OP_STORE_GLOBAL: return "OP_STORE_GLOBAL"; case OP_POP: return "OP_POP";
        case OP_ADD: return "OP_ADD"; case OP_SUB: return "OP_SUB"; case OP_MUL: return "OP_MUL"; case OP_DIV: return "OP_DIV"; case OP_FLOOR_DIV: return "OP_FLOOR_DIV"; case OP_MOD: return "OP_MOD"; case OP_NEG: return "OP_NEG";
        case OP_BITAND: return "OP_BITAND"; case OP_BITOR: return "OP_BITOR"; case OP_BITXOR: return "OP_BITXOR"; case OP_SHL: return "OP_SHL"; case OP_SHR: return "OP_SHR"; case OP_BITNOT: return "OP_BITNOT";
        case OP_EQ: return "OP_EQ"; case OP_NE: return "OP_NE"; case OP_LT: return "OP_LT"; case OP_LE: return "OP_LE"; case OP_GT: return "OP_GT"; case OP_GE: return "OP_GE";
        case OP_IS: return "OP_IS"; case OP_IS_NOT: return "OP_IS_NOT"; case OP_CONTAINS: return "OP_CONTAINS"; case OP_RAISE: return "OP_RAISE"; case OP_SETUP_EXCEPT: return "OP_SETUP_EXCEPT"; case OP_POP_EXCEPT: return "OP_POP_EXCEPT";
        case OP_JUMP: return "OP_JUMP"; case OP_JUMP_IF_FALSE: return "OP_JUMP_IF_FALSE"; case OP_JUMP_IF_FALSE_KEEP: return "OP_JUMP_IF_FALSE_KEEP"; case OP_JUMP_IF_TRUE_KEEP: return "OP_JUMP_IF_TRUE_KEEP";
        case OP_PRINT: return "OP_PRINT"; case OP_CALL: return "OP_CALL"; case OP_RETURN: return "OP_RETURN"; case OP_YIELD: return "OP_YIELD"; case OP_NOT: return "OP_NOT"; case OP_ITER: return "OP_ITER"; case OP_FOR_NEXT: return "OP_FOR_NEXT";
        case OP_DUP: return "OP_DUP"; case OP_DUP2: return "OP_DUP2"; case OP_UNPACK: return "OP_UNPACK";
        case OP_LIST_APPEND: return "OP_LIST_APPEND"; case OP_LIST_EXTEND: return "OP_LIST_EXTEND"; case OP_DICT_SETNAME: return "OP_DICT_SETNAME"; case OP_DICT_MERGE: return "OP_DICT_MERGE"; case OP_CALL_EX: return "OP_CALL_EX"; case OP_EXC_MATCH: return "OP_EXC_MATCH"; case OP_RERAISE: return "OP_RERAISE";
        case OP_MAKE_LIST: return "OP_MAKE_LIST"; case OP_MAKE_TUPLE: return "OP_MAKE_TUPLE"; case OP_MAKE_SET: return "OP_MAKE_SET"; case OP_MAKE_DICT: return "OP_MAKE_DICT"; case OP_GET_INDEX: return "OP_GET_INDEX"; case OP_GET_SLICE: return "OP_GET_SLICE"; case OP_SET_INDEX: return "OP_SET_INDEX"; case OP_DEL_INDEX: return "OP_DEL_INDEX"; case OP_DEL_ATTR: return "OP_DEL_ATTR"; case OP_DELETE_NAME: return "OP_DELETE_NAME";
        case OP_GET_ATTR: return "OP_GET_ATTR"; case OP_SET_ATTR: return "OP_SET_ATTR"; case OP_DEF: return "OP_DEF"; case OP_CLASS: return "OP_CLASS"; case OP_IMPORT: return "OP_IMPORT";
        default: return "OP_?";
    }
}
static int op_has_arg(int op){
    switch((Op)op){
        case OP_CONST: case OP_LOAD: case OP_STORE: case OP_STORE_GLOBAL: case OP_SETUP_EXCEPT: case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_FALSE_KEEP: case OP_JUMP_IF_TRUE_KEEP:
        case OP_CALL: case OP_FOR_NEXT: case OP_UNPACK: case OP_DICT_SETNAME: case OP_MAKE_LIST: case OP_MAKE_TUPLE: case OP_MAKE_SET: case OP_MAKE_DICT: case OP_GET_ATTR: case OP_SET_ATTR: case OP_DEL_ATTR: case OP_DELETE_NAME: case OP_DEF: case OP_CLASS: case OP_IMPORT:
            return 1;
        default: return 0;
    }
}
static void dump_const_brief(Value v){
    if(v.type==V_NONE) printf("None");
    else if(v.type==V_BOOL) printf(v.as.boolean?"True":"False");
    else if(v.type==V_INT) printf("%lld",(long long)v.as.i);
    else if(v.type==V_FLOAT) printf("%.15g",v.as.f);
    else if(is_obj(v,O_STRING)) printf("\"%s\"",v.as.obj->as.str.s);
    else if(is_obj(v,O_FUNCTION)) printf("<function %s>",v.as.obj->as.fn.name);
    else printf("<object>");
}
static void dump_chunk(Chunk *c,const char *name){
    printf("Bytecode %s:\n",name?name:"<chunk>");
    for(int ip=0; ip<c->count; ip++){
        int op=c->code[ip];
        printf("  %04d  line %-3d %-24s",ip,c->line[ip],op_name(op));
        if(op_has_arg(op)){
            int arg=c->code[++ip];
            printf(" %d",arg);
            if((Op)op==OP_CONST || (Op)op==OP_LOAD || (Op)op==OP_STORE || (Op)op==OP_STORE_GLOBAL || (Op)op==OP_GET_ATTR || (Op)op==OP_SET_ATTR || (Op)op==OP_DEF || (Op)op==OP_CLASS || (Op)op==OP_IMPORT){
                if(arg>=0 && arg<c->ccount){ printf(" ; "); dump_const_brief(c->consts[arg]); }
            }
        }
        printf("\n");
    }
}
void dump_function_bytecode(Function *fn){
    dump_chunk(fn->chunk,fn->name);
    for(int i=0;i<fn->chunk->ccount;i++){
        Value v=fn->chunk->consts[i];
        if(is_obj(v,O_FUNCTION)) dump_function_bytecode(&v.as.obj->as.fn);
    }
}
