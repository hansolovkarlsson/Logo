// bytecode.c
//
// See bytecode.h for the instruction format and rationale. Just the
// fixed-pool bookkeeping -- compiler.c is what actually decides which
// instructions to emit, same split as ast.c/ast.h.

#include "bytecode.h"
#include <ctype.h>   // isalpha/isalnum/isdigit, used by bytecode_assemble's own line classification
#include <stdarg.h>  // va_list, used by asm_error
#include <stdio.h>   // snprintf, used by bytecode_add_word_literal
#include <stdlib.h>  // calloc/free/strtod/strtol, used by bytecode_assemble
#include <string.h>  // strchr/memcpy/memset/strncmp, used by bytecode_assemble
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
        case OP_REPCOUNT_PUSH: return "OP_REPCOUNT_PUSH";
        case OP_REPCOUNT_INCR: return "OP_REPCOUNT_INCR";
        case OP_REPCOUNT_POP: return "OP_REPCOUNT_POP";
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
    fprintf(out, "START: @%d\n", chunk->start_pc); // the whole program's own top-level entry point -- see BytecodeChunk.start_pc's own comment

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

// ---- Stage C: assembler (text -> chunk) ----
//
// A line-oriented, hand-rolled parser -- same "reimplement precisely,
// don't reach for a heavier abstraction" style lexer.c/parser.c
// already use for Logo source itself, just for this format instead.
// See bytecode.h's own bytecode_assemble comment for the accepted
// grammar (PROCS + CODE sections, "@N" or symbolic labels for jump
// targets).

// name -> pc, one entry per "name:" line encountered anywhere in the
// CODE section (a proc's own entry-point label, populated
// automatically from its own PROCS entry once resolved, or an
// ordinary hand-written one) -- a genuine label table, not just
// per-proc bookkeeping, since a hand-written jump can target any
// label, not only a procedure's own start.
typedef struct {
    char name[INSTR_MAX_TEXT];
    int pc;
} AsmLabel;

#define MAX_ASM_LABELS 4096
#define MAX_ASM_LINE 4096

static void asm_error(char *error, size_t error_size, const char *fmt, ...) {
    if (error == NULL || error_size == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error, error_size, fmt, ap);
    va_end(ap);
}

static const char *asm_skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int asm_at_end(const char *cursor) {
    return *asm_skip_ws(cursor) == '\0';
}

// Copies the line starting at `cursor` into `buf` (truncating, never
// overrunning, same policy as every other fixed-buffer copy in this
// file), stripping a trailing '\r' defensively (bytecode_disassemble
// itself never emits one). Returns a pointer to the start of the next
// line, or NULL once `cursor` is the last line in the text.
static const char *asm_next_line(const char *cursor, char *buf, size_t bufsize) {
    const char *nl = strchr(cursor, '\n');
    size_t len = nl ? (size_t)(nl - cursor) : strlen(cursor);
    if (len > 0 && cursor[len - 1] == '\r') len--;
    if (len >= bufsize) len = bufsize - 1;
    memcpy(buf, cursor, len);
    buf[len] = '\0';
    return nl ? nl + 1 : NULL;
}

// Copies one whitespace-delimited token starting at *cursor into
// `out`, advancing *cursor past it. Returns 0 (leaving *cursor
// unmoved) if there's nothing but whitespace left.
static int asm_parse_token(const char **cursor, char *out, size_t outsize) {
    const char *s = asm_skip_ws(*cursor);
    const char *start = s;
    while (*s != '\0' && *s != ' ' && *s != '\t') s++;
    size_t len = (size_t)(s - start);
    if (len == 0) return 0;
    if (len >= outsize) len = outsize - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    *cursor = s;
    return 1;
}

// Copies the text between a leading and matching trailing '"' (no
// escaping -- see bytecode_disassemble's own OP_PUSH_WORD comment: a
// literal word compiled from real Logo source never contains one)
// into `out`, advancing *cursor past the closing quote. Returns 0 on
// anything else (missing opening or closing quote).
static int asm_parse_quoted(const char **cursor, char *out, size_t outsize) {
    const char *s = asm_skip_ws(*cursor);
    if (*s != '"') return 0;
    s++;
    const char *start = s;
    while (*s != '\0' && *s != '"') s++;
    if (*s != '"') return 0;
    size_t len = (size_t)(s - start);
    if (len >= outsize) len = outsize - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    *cursor = s + 1;
    return 1;
}

// Matches a literal `key` (e.g. "argc=") immediately followed by a
// base-10 integer with no space in between -- the "argc=%d"/
// "depth=%d" shape bytecode_disassemble itself always emits. Returns 0
// (leaving *cursor unmoved) if `key` doesn't match here at all.
static int asm_parse_kv_int(const char **cursor, const char *key, int *out) {
    const char *s = asm_skip_ws(*cursor);
    size_t klen = strlen(key);
    if (strncmp(s, key, klen) != 0) return 0;
    s += klen;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return 0;
    *out = (int)v;
    *cursor = end;
    return 1;
}

// Same shape as asm_parse_kv_int, but the value is a bare token
// instead of an integer -- the "tmpl=%s" shape OP_MAP_COMPILED &co use.
static int asm_parse_kv_token(const char **cursor, const char *key, char *out, size_t outsize) {
    const char *s = asm_skip_ws(*cursor);
    size_t klen = strlen(key);
    if (strncmp(s, key, klen) != 0) return 0;
    s += klen;
    const char *start = s;
    while (*s != '\0' && *s != ' ' && *s != '\t') s++;
    size_t len = (size_t)(s - start);
    if (len == 0) return 0;
    if (len >= outsize) len = outsize - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    *cursor = s;
    return 1;
}

static int asm_find_label(const AsmLabel *labels, int label_count, const char *name) {
    for (int i = 0; i < label_count; i++) {
        if (strcasecmp(labels[i].name, name) == 0) return labels[i].pc;
    }
    return -1;
}

// Resolves an already-parsed jump-target token: either "@N" (an
// absolute pc, exactly what bytecode_disassemble itself emits) or a
// bare name looked up in `labels`. Returns 0 if it's neither a valid
// "@N" nor a defined label.
static int asm_resolve_target(const char *tok, const AsmLabel *labels, int label_count, int *out_pc) {
    if (tok[0] == '@') {
        char *end = NULL;
        long v = strtol(tok + 1, &end, 10);
        if (end == tok + 1 || *end != '\0') return 0;
        *out_pc = (int)v;
        return 1;
    }
    int pc = asm_find_label(labels, label_count, tok);
    if (pc < 0) return 0;
    *out_pc = pc;
    return 1;
}

// A bare "name:" line -- an identifier (same charset as any Logo
// identifier: alpha/underscore first, alnum/underscore after) followed
// by ':' and nothing else. Distinguishes a label definition from an
// ordinary "<pc>: OPNAME ..." instruction line, whose own prefix is
// digits (never a valid identifier start) rather than a name.
static int asm_is_label_line(const char *trimmed, char *name_out, size_t name_out_size) {
    const char *colon = strchr(trimmed, ':');
    if (!colon) return 0;
    size_t prefix_len = (size_t)(colon - trimmed);
    if (prefix_len == 0) return 0;
    if (!(isalpha((unsigned char)trimmed[0]) || trimmed[0] == '_')) return 0;
    for (size_t i = 1; i < prefix_len; i++) {
        char c = trimmed[i];
        if (!(isalnum((unsigned char)c) || c == '_')) return 0;
    }
    if (!asm_at_end(colon + 1)) return 0; // something follows the colon -- an instruction line, not a label
    if (prefix_len >= name_out_size) prefix_len = name_out_size - 1;
    memcpy(name_out, trimmed, prefix_len);
    name_out[prefix_len] = '\0';
    return 1;
}

// Strips an optional leading "<digits>:" address column (the one
// bytecode_disassemble's own CODE listing always prints, but which
// bytecode_assemble never trusts -- see bytecode.h's own comment) down
// to the actual mnemonic. A hand-written line with no such column is
// returned unchanged.
static const char *asm_strip_pc_prefix(const char *s) {
    const char *p = s;
    if (!isdigit((unsigned char)*p)) return s;
    while (isdigit((unsigned char)*p)) p++;
    p = asm_skip_ws(p);
    if (*p != ':') return s;
    return asm_skip_ws(p + 1);
}

static int asm_lookup_opcode(const char *name, OpCode *out) {
    for (int i = 0; i < OPCODE_COUNT; i++) {
        if (strcasecmp(bytecode_opcode_name((OpCode)i), name) == 0) {
            *out = (OpCode)i;
            return 1;
        }
    }
    return 0;
}

// Parses one instruction's own operands (everything after the
// mnemonic) into `instr`, mirroring bytecode_disassemble's own
// per-opcode operand switch exactly -- same opcode groupings, same
// operand shapes, just the reverse direction.
static int asm_parse_operands(OpCode op, const char *cur, BytecodeChunk *chunk,
                               const AsmLabel *labels, int label_count,
                               Instr *instr, char *error, size_t error_size, int line_no) {
    switch (op) {
        case OP_PUSH_NUMBER: {
            const char *s = asm_skip_ws(cur);
            char *end = NULL;
            double v = strtod(s, &end);
            if (end == s) { asm_error(error, error_size, "line %d: expected a number", line_no); return 0; }
            instr->number = v;
            if (!asm_at_end(end)) { asm_error(error, error_size, "line %d: unexpected text after number", line_no); return 0; }
            return 1;
        }
        case OP_PUSH_WORD: {
            char word[AST_MAX_TEXT];
            const char *s = cur;
            if (!asm_parse_quoted(&s, word, sizeof(word))) { asm_error(error, error_size, "line %d: expected a quoted word", line_no); return 0; }
            if (!asm_at_end(s)) { asm_error(error, error_size, "line %d: unexpected text after word", line_no); return 0; }
            int idx = bytecode_add_word_literal(chunk, word);
            if (idx < 0) { asm_error(error, error_size, "line %d: too many word literals", line_no); return 0; }
            instr->a = idx;
            return 1;
        }
        case OP_PUSH_VAR:
        case OP_SET_VAR:
        case OP_CHECK_OUTPUT:
        case OP_LOCAL:
        case OP_ERASE:
        case OP_LOAD: {
            char tok[INSTR_MAX_TEXT];
            const char *s = cur;
            if (!asm_parse_token(&s, tok, sizeof(tok))) { asm_error(error, error_size, "line %d: expected a name", line_no); return 0; }
            if (!asm_at_end(s)) { asm_error(error, error_size, "line %d: unexpected text after name", line_no); return 0; }
            snprintf(instr->text, sizeof(instr->text), "%s", tok);
            return 1;
        }
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_CHECK_THROW: {
            char tok[INSTR_MAX_TEXT];
            const char *s = cur;
            if (!asm_parse_token(&s, tok, sizeof(tok))) { asm_error(error, error_size, "line %d: expected a jump target", line_no); return 0; }
            int pc;
            if (!asm_resolve_target(tok, labels, label_count, &pc)) { asm_error(error, error_size, "line %d: undefined label '%s'", line_no, tok); return 0; }
            if (!asm_at_end(s)) { asm_error(error, error_size, "line %d: unexpected text after jump target", line_no); return 0; }
            instr->a = pc;
            return 1;
        }
        case OP_CALL_BUILTIN: {
            char name[INSTR_MAX_TEXT];
            const char *s = cur;
            if (!asm_parse_token(&s, name, sizeof(name))) { asm_error(error, error_size, "line %d: expected a builtin name", line_no); return 0; }
            int argc;
            if (!asm_parse_kv_int(&s, "argc=", &argc)) { asm_error(error, error_size, "line %d: expected 'argc=<N>'", line_no); return 0; }
            if (!asm_at_end(s)) { asm_error(error, error_size, "line %d: unexpected text after argc", line_no); return 0; }
            snprintf(instr->text, sizeof(instr->text), "%s", name);
            instr->a = argc;
            return 1;
        }
        case OP_CALL_PROC: {
            char name[INSTR_MAX_TEXT];
            const char *s = cur;
            if (!asm_parse_token(&s, name, sizeof(name))) { asm_error(error, error_size, "line %d: expected a procedure name", line_no); return 0; }
            char tgt[INSTR_MAX_TEXT];
            if (!asm_parse_token(&s, tgt, sizeof(tgt))) { asm_error(error, error_size, "line %d: expected a jump target", line_no); return 0; }
            int pc;
            if (!asm_resolve_target(tgt, labels, label_count, &pc)) { asm_error(error, error_size, "line %d: undefined label '%s'", line_no, tgt); return 0; }
            int argc;
            if (!asm_parse_kv_int(&s, "argc=", &argc)) { asm_error(error, error_size, "line %d: expected 'argc=<N>'", line_no); return 0; }
            if (!asm_at_end(s)) { asm_error(error, error_size, "line %d: unexpected text after argc", line_no); return 0; }
            snprintf(instr->text, sizeof(instr->text), "%s", name);
            instr->a = pc;
            instr->b = argc;
            return 1;
        }
        case OP_PUSH_LIST_LITERAL: {
            const char *s = asm_skip_ws(cur);
            if (*s == '\0') { asm_error(error, error_size, "line %d: expected a list literal", line_no); return 0; }
            int idx = bytecode_add_list_literal(chunk, s);
            if (idx < 0) { asm_error(error, error_size, "line %d: too many list literals", line_no); return 0; }
            instr->a = idx;
            return 1;
        }
        case OP_PEEK:
        case OP_POKE: {
            const char *s = cur;
            int depth;
            if (!asm_parse_kv_int(&s, "depth=", &depth)) { asm_error(error, error_size, "line %d: expected 'depth=<N>'", line_no); return 0; }
            if (!asm_at_end(s)) { asm_error(error, error_size, "line %d: unexpected text after depth", line_no); return 0; }
            instr->a = depth;
            return 1;
        }
        case OP_MAP_COMPILED:
        case OP_FILTER_COMPILED:
        case OP_REDUCE_COMPILED:
        case OP_FOREACH_COMPILED: {
            char tgt[INSTR_MAX_TEXT];
            const char *s = cur;
            if (!asm_parse_token(&s, tgt, sizeof(tgt))) { asm_error(error, error_size, "line %d: expected a jump target", line_no); return 0; }
            int pc;
            if (!asm_resolve_target(tgt, labels, label_count, &pc)) { asm_error(error, error_size, "line %d: undefined label '%s'", line_no, tgt); return 0; }
            char tmplname[INSTR_MAX_TEXT];
            if (!asm_parse_kv_token(&s, "tmpl=", tmplname, sizeof(tmplname))) { asm_error(error, error_size, "line %d: expected 'tmpl=<name>'", line_no); return 0; }
            if (!asm_at_end(s)) { asm_error(error, error_size, "line %d: unexpected text after tmpl", line_no); return 0; }
            instr->a = pc;
            snprintf(instr->text, sizeof(instr->text), "%s", tmplname);
            return 1;
        }
        default:
            if (!asm_at_end(cur)) { asm_error(error, error_size, "line %d: '%s' takes no operands", line_no, bytecode_opcode_name(op)); return 0; }
            return 1;
    }
}

int bytecode_assemble(const char *text, BytecodeChunk *chunk, char *error, size_t error_size) {
    if (error && error_size > 0) error[0] = '\0';

    AsmLabel *labels = calloc(MAX_ASM_LABELS, sizeof(AsmLabel));
    if (!labels) { asm_error(error, error_size, "out of memory"); return 0; }
    int label_count = 0;
    int ok = 1;

    char line[MAX_ASM_LINE];
    const char *p = text;
    int line_no = 0;
    int mode = 0; // 0 = before any section marker, 1 = PROCS, 2 = CODE (label-scan pass)
    const char *code_start = NULL;
    int code_start_line = 0;
    int code_pc = 0;
    int in_source_block = 0;
    ProcAddr *cur_proc = NULL;
    char start_tok[INSTR_MAX_TEXT];
    int have_start = 0;

    // Pass 1: walk the whole text once, populating chunk->procs[] from
    // the PROCS section and recording every "name:" label's own pc
    // from the CODE section -- but NOT yet parsing/emitting CODE's own
    // instructions (a jump earlier in CODE may target a label defined
    // later, so every label has to be known before any operand gets
    // resolved).
    while (p != NULL) {
        const char *next = asm_next_line(p, line, sizeof(line));
        line_no++;
        const char *trimmed = asm_skip_ws(line);

        if (in_source_block) {
            if (strcmp(trimmed, "---- end source ----") == 0) {
                in_source_block = 0;
                cur_proc = NULL;
            } else if (cur_proc) {
                size_t used = strlen(cur_proc->source_text);
                size_t room = sizeof(cur_proc->source_text) - used;
                if (room > 1) {
                    if (used > 0) {
                        cur_proc->source_text[used++] = '\n';
                        cur_proc->source_text[used] = '\0';
                        room--;
                    }
                    snprintf(cur_proc->source_text + used, room, "%s", line);
                }
            }
            p = next;
            continue;
        }

        if (*trimmed == '\0' || *trimmed == ';') { p = next; continue; }

        if (strncmp(trimmed, "START:", 6) == 0) {
            if (have_start) {
                asm_error(error, error_size, "line %d: duplicate 'START:' line", line_no);
                ok = 0;
                goto done;
            }
            const char *s = asm_skip_ws(trimmed + 6);
            if (!asm_parse_token(&s, start_tok, sizeof(start_tok)) || !asm_at_end(s)) {
                asm_error(error, error_size, "line %d: expected 'START: @N' or 'START: <label>'", line_no);
                ok = 0;
                goto done;
            }
            have_start = 1;
            p = next;
            continue;
        }
        if (strcmp(trimmed, "PROCS:") == 0) { mode = 1; p = next; continue; }
        if (strcmp(trimmed, "CODE:") == 0) {
            mode = 2;
            code_start = next;
            code_start_line = line_no + 1;
            p = next;
            continue;
        }

        if (mode == 0) {
            asm_error(error, error_size, "line %d: expected 'START:', 'PROCS:', or 'CODE:'", line_no);
            ok = 0;
            goto done;
        }

        if (mode == 1) {
            if (strcmp(trimmed, "---- source ----") == 0) {
                if (!cur_proc) {
                    asm_error(error, error_size, "line %d: '---- source ----' with no preceding procedure header", line_no);
                    ok = 0;
                    goto done;
                }
                in_source_block = 1;
                p = next;
                continue;
            }
            const char *cur = trimmed;
            char name[INSTR_MAX_TEXT];
            if (!asm_parse_token(&cur, name, sizeof(name))) {
                asm_error(error, error_size, "line %d: expected a procedure name", line_no);
                ok = 0;
                goto done;
            }
            if (chunk->proc_count >= MAX_CHUNK_PROCS) {
                asm_error(error, error_size, "line %d: too many procedures (max %d)", line_no, MAX_CHUNK_PROCS);
                ok = 0;
                goto done;
            }
            ProcAddr *pa = &chunk->procs[chunk->proc_count++];
            memset(pa, 0, sizeof(*pa));
            snprintf(pa->name, sizeof(pa->name), "%s", name);

            char starttok[INSTR_MAX_TEXT];
            if (strncmp(asm_skip_ws(cur), "start=", 6) != 0) {
                asm_error(error, error_size, "line %d: expected 'start=' after procedure name", line_no);
                ok = 0;
                goto done;
            }
            cur = asm_skip_ws(cur) + 6;
            // The value itself is read but deliberately not trusted --
            // see bytecode.h's own comment on why start_pc always comes
            // from resolving "<name>:" below instead.
            if (!asm_parse_token(&cur, starttok, sizeof(starttok))) {
                asm_error(error, error_size, "line %d: expected a start address after 'start='", line_no);
                ok = 0;
                goto done;
            }

            int argc;
            if (!asm_parse_kv_int(&cur, "argc=", &argc)) {
                asm_error(error, error_size, "line %d: expected 'argc=<N>'", line_no);
                ok = 0;
                goto done;
            }
            pa->param_count = argc;

            const char *s = asm_skip_ws(cur);
            if (strncmp(s, "params=(", 8) != 0) {
                asm_error(error, error_size, "line %d: expected 'params=(...)'", line_no);
                ok = 0;
                goto done;
            }
            s += 8;
            int pi = 0;
            for (;;) {
                s = asm_skip_ws(s);
                if (*s == ')') { s++; break; }
                const char *tokstart = s;
                while (*s != '\0' && *s != ' ' && *s != '\t' && *s != ')') s++;
                size_t len = (size_t)(s - tokstart);
                if (len == 0) {
                    asm_error(error, error_size, "line %d: malformed params list", line_no);
                    ok = 0;
                    goto done;
                }
                if (pi >= AST_MAX_PARAMS) {
                    asm_error(error, error_size, "line %d: too many params (max %d)", line_no, AST_MAX_PARAMS);
                    ok = 0;
                    goto done;
                }
                size_t cplen = len >= sizeof(pa->param_names[pi]) ? sizeof(pa->param_names[pi]) - 1 : len;
                memcpy(pa->param_names[pi], tokstart, cplen);
                pa->param_names[pi][cplen] = '\0';
                pi++;
                if (*s == ')') { s++; break; }
            }
            if (pi != pa->param_count) {
                asm_error(error, error_size, "line %d: params list has %d name(s) but argc=%d", line_no, pi, pa->param_count);
                ok = 0;
                goto done;
            }
            if (!asm_at_end(s)) {
                asm_error(error, error_size, "line %d: unexpected text after params list", line_no);
                ok = 0;
                goto done;
            }
            cur_proc = pa;
        } else { // mode == 2, CODE label-scan
            char label_name[INSTR_MAX_TEXT];
            if (asm_is_label_line(trimmed, label_name, sizeof(label_name))) {
                if (asm_find_label(labels, label_count, label_name) >= 0) {
                    asm_error(error, error_size, "line %d: duplicate label '%s'", line_no, label_name);
                    ok = 0;
                    goto done;
                }
                if (label_count >= MAX_ASM_LABELS) {
                    asm_error(error, error_size, "line %d: too many labels (max %d)", line_no, MAX_ASM_LABELS);
                    ok = 0;
                    goto done;
                }
                snprintf(labels[label_count].name, sizeof(labels[label_count].name), "%s", label_name);
                labels[label_count].pc = code_pc;
                label_count++;
            } else {
                code_pc++;
            }
        }

        p = next;
    }

    if (in_source_block) {
        asm_error(error, error_size, "unterminated '---- source ----' block");
        ok = 0;
        goto done;
    }
    if (code_start == NULL) {
        asm_error(error, error_size, "missing 'CODE:' section");
        ok = 0;
        goto done;
    }
    if (!have_start) {
        asm_error(error, error_size, "missing 'START:' line");
        ok = 0;
        goto done;
    }
    if (!asm_resolve_target(start_tok, labels, label_count, &chunk->start_pc)) {
        asm_error(error, error_size, "'START:' target '%s' is not a defined label", start_tok);
        ok = 0;
        goto done;
    }

    for (int i = 0; i < chunk->proc_count; i++) {
        int pc = asm_find_label(labels, label_count, chunk->procs[i].name);
        if (pc < 0) {
            asm_error(error, error_size, "procedure '%s' has no matching '%s:' label in CODE", chunk->procs[i].name, chunk->procs[i].name);
            ok = 0;
            goto done;
        }
        chunk->procs[i].start_pc = pc;
    }

    // Pass 2: re-walk just the CODE section, this time actually
    // parsing and emitting each instruction -- every label is already
    // known from pass 1, so a forward reference resolves correctly.
    p = code_start;
    line_no = code_start_line - 1;
    while (p != NULL) {
        const char *next = asm_next_line(p, line, sizeof(line));
        line_no++;
        const char *trimmed = asm_skip_ws(line);

        if (*trimmed == '\0' || *trimmed == ';') { p = next; continue; }

        char label_name[INSTR_MAX_TEXT];
        if (asm_is_label_line(trimmed, label_name, sizeof(label_name))) { p = next; continue; }

        const char *body = asm_strip_pc_prefix(trimmed);
        char mnemonic[INSTR_MAX_TEXT];
        const char *cur = body;
        if (!asm_parse_token(&cur, mnemonic, sizeof(mnemonic))) {
            asm_error(error, error_size, "line %d: expected an instruction", line_no);
            ok = 0;
            goto done;
        }
        OpCode op;
        if (!asm_lookup_opcode(mnemonic, &op)) {
            asm_error(error, error_size, "line %d: unknown opcode '%s'", line_no, mnemonic);
            ok = 0;
            goto done;
        }

        Instr instr;
        memset(&instr, 0, sizeof(instr));
        instr.op = op;
        if (!asm_parse_operands(op, cur, chunk, labels, label_count, &instr, error, error_size, line_no)) {
            ok = 0;
            goto done;
        }
        if (bytecode_emit(chunk, instr) < 0) {
            asm_error(error, error_size, "line %d: too many instructions (max %d)", line_no, MAX_INSTRUCTIONS);
            ok = 0;
            goto done;
        }

        p = next;
    }

done:
    free(labels);
    return ok;
}
