// bytecode.c
//
// See bytecode.h for the instruction format and rationale. Just the
// fixed-pool bookkeeping -- compiler.c is what actually decides which
// instructions to emit, same split as ast.c/ast.h.

#include "bytecode.h"
#include <stdio.h>   // snprintf, used by bytecode_add_word_literal
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

const ProcAddr *bytecode_find_proc_entry(const BytecodeChunk *chunk, const char *name) {
    for (int i = 0; i < chunk->proc_count; i++) {
        if (strcasecmp(chunk->procs[i].name, name) == 0) return &chunk->procs[i];
    }
    return NULL;
}

void bytecode_erase_proc(BytecodeChunk *chunk, const char *name) {
    for (int i = 0; i < chunk->proc_count; i++) {
        if (strcasecmp(chunk->procs[i].name, name) == 0) {
            chunk->procs[i].name[0] = '\0';
            return;
        }
    }
}

int bytecode_add_word_literal(BytecodeChunk *chunk, const char *text) {
    if (chunk->word_literal_count >= MAX_CHUNK_WORD_LITERALS) return -1;
    int index = chunk->word_literal_count++;
    snprintf(chunk->word_literals[index], AST_MAX_TEXT, "%s", text);
    return index;
}

int bytecode_add_list_literal(BytecodeChunk *chunk, const char *text) {
    if (chunk->list_literal_count >= MAX_CHUNK_LIST_LITERALS) return -1;
    int index = chunk->list_literal_count++;
    snprintf(chunk->list_literals[index], MAX_LIST_LITERAL_TEXT, "%s", text);
    return index;
}
