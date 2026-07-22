#ifndef MPY_BYTECODE_H
#define MPY_BYTECODE_H

#include "value.h"

/* ========================= Bytecode ========================= */

typedef enum {
    OP_CONST, OP_NONE, OP_TRUE, OP_FALSE,
    OP_LOAD, OP_STORE, OP_STORE_GLOBAL, OP_POP,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_FLOOR_DIV, OP_MOD, OP_POWER, OP_NEG,
    OP_BITAND, OP_BITOR, OP_BITXOR, OP_SHL, OP_SHR, OP_BITNOT,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_IS, OP_IS_NOT, OP_CONTAINS, OP_RAISE, OP_SETUP_EXCEPT, OP_POP_EXCEPT,
    OP_JUMP, OP_JUMP_IF_FALSE, OP_JUMP_IF_FALSE_KEEP, OP_JUMP_IF_TRUE_KEEP,
    OP_PRINT, OP_CALL, OP_RETURN, OP_YIELD, OP_NOT, OP_ITER, OP_FOR_NEXT,
    OP_DUP, OP_DUP2, OP_UNPACK,
    OP_LIST_APPEND, OP_LIST_EXTEND, OP_DICT_SETNAME, OP_DICT_MERGE, OP_CALL_EX,
    OP_MAKE_LIST, OP_MAKE_TUPLE, OP_MAKE_SET, OP_MAKE_DICT, OP_GET_INDEX, OP_GET_SLICE, OP_SET_INDEX,
    OP_GET_ATTR, OP_SET_ATTR,
    OP_DEF, OP_CLASS, OP_IMPORT
} Op;

typedef struct Chunk { int *code; int *line; int count, cap; Value *consts; int ccount, ccap; int has_yield; } Chunk;

Chunk *chunk_new(void);
int    add_const(Chunk *c, Value v);
int    emit(Chunk *c, int x, int line);
int    emit_op(Chunk *c, Op op, int line);
int    emit_arg(Chunk *c, Op op, int arg, int line);
void   patch(Chunk *c, int at, int val);

void   dump_function_bytecode(Function *fn);

#endif /* MPY_BYTECODE_H */
