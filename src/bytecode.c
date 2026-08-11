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

const char *bytecode_opcode_name(OpCode op) {
    switch (op) {
        case OP_PUSH_NUMBER: return "OP_PUSH_NUMBER";
        case OP_PUSH_WORD: return "OP_PUSH_WORD";
        case OP_PUSH_VAR: return "OP_PUSH_VAR";
        case OP_SET_VAR: return "OP_SET_VAR";
        case OP_POP: return "OP_POP";
        case OP_ADD: return "OP_ADD";
        case OP_SUB: return "OP_SUB";
        case OP_MUL: return "OP_MUL";
        case OP_DIV: return "OP_DIV";
        case OP_NEG: return "OP_NEG";
        case OP_CMP_LT: return "OP_CMP_LT";
        case OP_CMP_GT: return "OP_CMP_GT";
        case OP_CMP_EQ: return "OP_CMP_EQ";
        case OP_CMP_LE: return "OP_CMP_LE";
        case OP_CMP_GE: return "OP_CMP_GE";
        case OP_CMP_NE: return "OP_CMP_NE";
        case OP_NOT: return "OP_NOT";
        case OP_AND: return "OP_AND";
        case OP_OR: return "OP_OR";
        case OP_JUMP: return "OP_JUMP";
        case OP_JUMP_IF_FALSE: return "OP_JUMP_IF_FALSE";
        case OP_CALL_BUILTIN: return "OP_CALL_BUILTIN";
        case OP_CALL_PROC: return "OP_CALL_PROC";
        case OP_CHECK_OUTPUT: return "OP_CHECK_OUTPUT";
        case OP_OUTPUT: return "OP_OUTPUT";
        case OP_STOP: return "OP_STOP";
        case OP_HALT: return "OP_HALT";
        case OP_PUSH_LIST_LITERAL: return "OP_PUSH_LIST_LITERAL";
        case OP_LOCAL: return "OP_LOCAL";
        case OP_ERASE: return "OP_ERASE";
        case OP_VOID_RESULT: return "OP_VOID_RESULT";
        case OP_SEND: return "OP_SEND";
        case OP_CHECK_SEND_OUTPUT: return "OP_CHECK_SEND_OUTPUT";
        case OP_APPLY: return "OP_APPLY";
        case OP_VOID_DISCARD: return "OP_VOID_DISCARD";
        case OP_RUN: return "OP_RUN";
        case OP_LOAD: return "OP_LOAD";
        case OP_CHECK_THROW: return "OP_CHECK_THROW";
        case OP_CATCH_CHECK: return "OP_CATCH_CHECK";
        case OP_CHECK_UNCAUGHT_THROW: return "OP_CHECK_UNCAUGHT_THROW";
        case OP_PEEK: return "OP_PEEK";
        case OP_POKE: return "OP_POKE";
        case OP_MAP_COMPILED: return "OP_MAP_COMPILED";
        case OP_FILTER_COMPILED: return "OP_FILTER_COMPILED";
        case OP_REDUCE_COMPILED: return "OP_REDUCE_COMPILED";
        case OP_FOREACH_COMPILED: return "OP_FOREACH_COMPILED";
        case OP_WAIT: return "OP_WAIT";
        case OP_WAITKEY: return "OP_WAITKEY";
        case OP_INPUT: return "OP_INPUT";
        case OP_PAUSE: return "OP_PAUSE";
        case OP_ANIMATESPRITE: return "OP_ANIMATESPRITE";
        case OP_LAUNCH: return "OP_LAUNCH";
        case OP_AWAIT: return "OP_AWAIT";
        case OP_YIELD: return "OP_YIELD";
        case OP_MOTION_DELAY: return "OP_MOTION_DELAY";
    }
    return "OP_UNKNOWN";
}

void bytecode_disassemble(const BytecodeChunk *chunk, FILE *out) {
    fprintf(out, "; %d instruction%s, %d proc%s, %d word literal%s, %d list literal%s\n",
            chunk->count, chunk->count == 1 ? "" : "s",
            chunk->proc_count, chunk->proc_count == 1 ? "" : "s",
            chunk->word_literal_count, chunk->word_literal_count == 1 ? "" : "s",
            chunk->list_literal_count, chunk->list_literal_count == 1 ? "" : "s");

    if (chunk->proc_count > 0) {
        fprintf(out, "\nPROCS:\n");
        for (int i = 0; i < chunk->proc_count; i++) {
            const ProcAddr *p = &chunk->procs[i];
            if (p->name[0] == '\0') continue; // erased -- see bytecode_erase_proc
            fprintf(out, "  %s start=@%d argc=%d params=(", p->name, p->start_pc, p->param_count);
            for (int j = 0; j < p->param_count; j++) {
                fprintf(out, "%s%s", j > 0 ? " " : "", p->param_names[j]);
            }
            fprintf(out, ")\n  ---- source ----\n%s\n  ---- end source ----\n", p->source_text);
        }
    }

    fprintf(out, "\nCODE:\n");
    for (int pc = 0; pc < chunk->count; pc++) {
        for (int i = 0; i < chunk->proc_count; i++) {
            if (chunk->procs[i].name[0] != '\0' && chunk->procs[i].start_pc == pc) {
                fprintf(out, "%s:\n", chunk->procs[i].name);
            }
        }
        const Instr *instr = &chunk->code[pc];
        fprintf(out, "  %4d: %s", pc, bytecode_opcode_name(instr->op));
        switch (instr->op) {
            case OP_PUSH_NUMBER:
                fprintf(out, " %g", instr->number);
                break;
            case OP_PUSH_WORD:
                fprintf(out, " \"%s\"",
                        (instr->a >= 0 && instr->a < chunk->word_literal_count) ? chunk->word_literals[instr->a] : "");
                break;
            case OP_PUSH_VAR:
            case OP_SET_VAR:
            case OP_CHECK_OUTPUT:
            case OP_LOCAL:
            case OP_ERASE:
            case OP_LOAD:
                fprintf(out, " %s", instr->text);
                break;
            case OP_JUMP:
            case OP_JUMP_IF_FALSE:
            case OP_CHECK_THROW:
                fprintf(out, " @%d", instr->a);
                break;
            case OP_CALL_BUILTIN:
                fprintf(out, " %s argc=%d", instr->text, instr->a);
                break;
            case OP_CALL_PROC:
                fprintf(out, " %s @%d argc=%d", instr->text, instr->a, instr->b);
                break;
            case OP_PUSH_LIST_LITERAL:
                fprintf(out, " %s",
                        (instr->a >= 0 && instr->a < chunk->list_literal_count) ? chunk->list_literals[instr->a] : "[]");
                break;
            case OP_PEEK:
            case OP_POKE:
                fprintf(out, " depth=%d", instr->a);
                break;
            case OP_MAP_COMPILED:
            case OP_FILTER_COMPILED:
            case OP_REDUCE_COMPILED:
            case OP_FOREACH_COMPILED:
                fprintf(out, " @%d tmpl=%s", instr->a, instr->text);
                break;
            default:
                break; // no operand of its own
        }
        fprintf(out, "\n");
    }
}
