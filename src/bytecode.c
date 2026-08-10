// bytecode.c
//
// See bytecode.h for the instruction format and rationale. Just the
// fixed-pool bookkeeping -- compiler.c is what actually decides which
// instructions to emit, same split as ast.c/ast.h.

#include "bytecode.h"
#include <strings.h> // strcasecmp, used by bytecode_find_proc

int bytecode_emit(BytecodeChunk *chunk, Instr instr) {
    if (chunk->count >= MAX_INSTRUCTIONS) return -1;
    chunk->code[chunk->count] = instr;
    return chunk->count++;
}

int bytecode_find_proc(const BytecodeChunk *chunk, const char *name) {
    for (int i = 0; i < chunk->proc_count; i++) {
        if (strcasecmp(chunk->procs[i].name, name) == 0) return chunk->procs[i].start_pc;
    }
    return -1;
}
