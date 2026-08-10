// bytecode.c
//
// See bytecode.h for the instruction format and rationale. Just the
// fixed-pool bookkeeping -- compiler.c is what actually decides which
// instructions to emit, same split as ast.c/ast.h.

#include "bytecode.h"

int bytecode_emit(BytecodeChunk *chunk, Instr instr) {
    if (chunk->count >= MAX_INSTRUCTIONS) return -1;
    chunk->code[chunk->count] = instr;
    return chunk->count++;
}
