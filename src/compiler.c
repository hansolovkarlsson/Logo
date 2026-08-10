// compiler.c
//
// See compiler.h for scope/rationale. Structure deliberately mirrors
// eval.c's own tree-walking shape (compile_expr/compile_condition/
// compile_statement/compile_block parallel eval_expr/eval_condition/
// exec_statement/exec_block almost node-for-node) -- the grammar
// itself doesn't change between "walk the tree and compute" and "walk
// the tree and emit instructions that will compute," same lesson
// Stage 1's own design doc already drew about retargeting
// interpreter.c's grammar rather than redesigning it.
//
// A deliberate simplification worth knowing up front, not an
// oversight: compile_call below only recognizes PRINT/OUTPUT/STOP/
// WHILE/MAKE by name -- every other AST_CALL is assumed to be a user
// procedure. This is safe for exactly this vertical slice (the parser
// itself already guarantees any OTHER resolved call name is a hoisted
// user procedure, never an unrecognized builtin -- see parser.c's own
// try_parse_call/BUILTIN_SIGNATURES), but it means compile_call's own
// builtin dispatch has to grow into something more general (a real
// name lookup, not an if-chain) once instruction coverage grows past
// this batch -- see docs/ROADMAP.md's own Phase 5 Stage 2 checklist,
// item 2.
//
// Procedure calls need genuine backpatching, not just "compile
// procedures before top-level code": one procedure's own body can
// call another procedure that hasn't been compiled *yet* (pass 1 below
// walks pool->nodes[] in array order, which doesn't have to match
// call order), so its target address isn't known at the moment that
// call is compiled. Every OP_CALL_PROC emitted for a not-yet-resolved
// name gets a placeholder target (-1) and a pending-patch entry
// instead; once every procedure has been compiled (so every name in
// c.procs[] is final), a second pass fills in every placeholder. Top-
// level calls never need this: procedures are always compiled before
// the top-level code that might call them, so a top-level call's own
// target is already known the moment it's compiled.

#include "compiler.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MAX_COMPILED_PROCS 50 // matches MAX_PROCEDURES in logo_types.h

typedef struct {
    char name[32];
    int start_pc;
} ProcAddr;

#define MAX_PENDING_PATCHES 256

typedef struct {
    int instr_index;
    char name[32];
} PendingPatch;

typedef struct {
    ProcAddr procs[MAX_COMPILED_PROCS];
    int proc_count;
    PendingPatch patches[MAX_PENDING_PATCHES];
    int patch_count;
} Compiler;

static int emit(BytecodeChunk *chunk, Instr instr) {
    return bytecode_emit(chunk, instr);
}

static int find_proc_addr(Compiler *c, const char *name) {
    for (int i = 0; i < c->proc_count; i++) {
        if (strcasecmp(c->procs[i].name, name) == 0) return c->procs[i].start_pc;
    }
    return -1;
}

static void add_pending_patch(Compiler *c, int instr_index, const char *name) {
    if (instr_index < 0 || c->patch_count >= MAX_PENDING_PATCHES) return;
    PendingPatch *p = &c->patches[c->patch_count++];
    p->instr_index = instr_index;
    snprintf(p->name, sizeof(p->name), "%s", name);
}

// Collects `node_idx`'s own children (its argument subtrees, in
// source order) into `out`, returning how many there are -- the same
// first_child/next_sibling walk exec_call's own arg_idx[] build uses.
static int collect_children(AstPool *pool, int node_idx, int *out, int max) {
    int n = 0;
    for (int ch = pool->nodes[node_idx].first_child; ch >= 0; ch = pool->nodes[ch].next_sibling) {
        if (n < max) out[n] = ch;
        n++;
    }
    return n;
}

static void compile_expr(Compiler *c, AstPool *pool, int node_idx, BytecodeChunk *chunk);
static void compile_condition(Compiler *c, AstPool *pool, int node_idx, BytecodeChunk *chunk);
static void compile_statement(Compiler *c, AstPool *pool, int node_idx, BytecodeChunk *chunk);
static void compile_block(Compiler *c, AstPool *pool, int block_node, BytecodeChunk *chunk);

// One AST_CALL node -- PRINT/OUTPUT/STOP/WHILE/MAKE (see this file's
// own comment on why these are recognized by name) or a user
// procedure, in either expression position (want_value: leaves exactly
// one value on the stack) or statement position (an OP_POP right
// after, discarding whatever the call left -- mirrors eval.c's own
// exec_call, where *result already defaults to num_val(0) and a
// statement-position caller simply never reads it).
static void compile_call(Compiler *c, AstPool *pool, int node_idx, BytecodeChunk *chunk, int want_value) {
    AstNode *node = &pool->nodes[node_idx];
    const char *name = node->text;
    int args[AST_MAX_PARAMS];

    if (strcasecmp(name, "OUTPUT") == 0) {
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        compile_expr(c, pool, args[0], chunk);
        emit(chunk, (Instr){.op = OP_OUTPUT});
        return;
    }
    if (strcasecmp(name, "STOP") == 0) {
        emit(chunk, (Instr){.op = OP_STOP});
        return;
    }
    if (strcasecmp(name, "WHILE") == 0) {
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        int loop_start = chunk->count;
        compile_condition(c, pool, args[0], chunk);
        int jf = emit(chunk, (Instr){.op = OP_JUMP_IF_FALSE, .a = -1});
        compile_block(c, pool, args[1], chunk);
        emit(chunk, (Instr){.op = OP_JUMP, .a = loop_start});
        if (jf >= 0) chunk->code[jf].a = chunk->count;
        return;
    }
    if (strcasecmp(name, "MAKE") == 0) {
        // arg_idx[0] is an AST_WORD holding the varname literally
        // (ARG_QUOTED_WORD already guarantees this -- see eval.c's own
        // do_make for the same reach).
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        const char *varname = pool->nodes[args[0]].text;
        compile_expr(c, pool, args[1], chunk);
        Instr instr = {0};
        instr.op = OP_SET_VAR;
        snprintf(instr.text, sizeof(instr.text), "%s", varname);
        emit(chunk, instr);
        return;
    }

    int argc = collect_children(pool, node_idx, args, AST_MAX_PARAMS);
    for (int i = 0; i < argc; i++) compile_expr(c, pool, args[i], chunk);

    if (strcasecmp(name, "PRINT") == 0) {
        Instr instr = {0};
        instr.op = OP_CALL_BUILTIN;
        instr.a = argc;
        snprintf(instr.text, sizeof(instr.text), "%s", name);
        emit(chunk, instr);
    } else {
        int target = find_proc_addr(c, name);
        Instr instr = {0};
        instr.op = OP_CALL_PROC;
        instr.a = target;
        instr.b = argc;
        snprintf(instr.text, sizeof(instr.text), "%s", name);
        int idx = emit(chunk, instr);
        if (target < 0) add_pending_patch(c, idx, name);
        if (want_value) {
            Instr check = {0};
            check.op = OP_CHECK_OUTPUT;
            snprintf(check.text, sizeof(check.text), "%s", name);
            emit(chunk, check);
        }
    }
    if (!want_value) emit(chunk, (Instr){.op = OP_POP});
}

// Mirrors eval_expr's own node-type switch exactly.
static void compile_expr(Compiler *c, AstPool *pool, int node_idx, BytecodeChunk *chunk) {
    AstNode *node = &pool->nodes[node_idx];
    switch (node->type) {
        case AST_NUMBER:
            emit(chunk, (Instr){.op = OP_PUSH_NUMBER, .number = node->number});
            return;
        case AST_WORD: {
            Instr instr = {0};
            instr.op = OP_PUSH_WORD;
            snprintf(instr.text, sizeof(instr.text), "%s", node->text);
            emit(chunk, instr);
            return;
        }
        case AST_VARREF: {
            Instr instr = {0};
            instr.op = OP_PUSH_VAR;
            snprintf(instr.text, sizeof(instr.text), "%s", node->text);
            emit(chunk, instr);
            return;
        }
        case AST_BINOP: {
            int left = node->first_child;
            int right = pool->nodes[left].next_sibling;
            compile_expr(c, pool, left, chunk);
            compile_expr(c, pool, right, chunk);
            OpCode op;
            switch (node->binop) {
                case AST_OP_ADD: op = OP_ADD; break;
                case AST_OP_SUB: op = OP_SUB; break;
                case AST_OP_MUL: op = OP_MUL; break;
                default: op = OP_DIV; break;
            }
            emit(chunk, (Instr){.op = op});
            return;
        }
        case AST_NEG:
            compile_expr(c, pool, node->first_child, chunk);
            emit(chunk, (Instr){.op = OP_NEG});
            return;
        case AST_CALL:
            compile_call(c, pool, node_idx, chunk, /*want_value=*/1);
            return;
        default:
            // Not reachable for this vertical slice's own grammar
            // subset (AST_LIST_LITERAL/AST_IF/AST_BLOCK/AST_PROC_DEF/
            // AST_FOR/the AST_COMPARE-and-friends condition nodes never
            // appear in expression position -- see ast.h's own node-
            // shape comments). A harmless placeholder rather than
            // reading uninitialized instruction fields, matching this
            // codebase's established "quietly inert on a genuinely
            // unreachable path" convention.
            emit(chunk, (Instr){.op = OP_PUSH_NUMBER, .number = 0});
            return;
    }
}

// Mirrors eval_condition's own node-type switch exactly, including its
// default case (a bare expression's truthiness) -- see bytecode.h's
// own file comment on why AND/OR need no short-circuit jump logic
// here.
static void compile_condition(Compiler *c, AstPool *pool, int node_idx, BytecodeChunk *chunk) {
    AstNode *node = &pool->nodes[node_idx];
    switch (node->type) {
        case AST_NOT:
            compile_condition(c, pool, node->first_child, chunk);
            emit(chunk, (Instr){.op = OP_NOT});
            return;
        case AST_AND: {
            int left = node->first_child;
            int right = pool->nodes[left].next_sibling;
            compile_condition(c, pool, left, chunk);
            compile_condition(c, pool, right, chunk);
            emit(chunk, (Instr){.op = OP_AND});
            return;
        }
        case AST_OR: {
            int left = node->first_child;
            int right = pool->nodes[left].next_sibling;
            compile_condition(c, pool, left, chunk);
            compile_condition(c, pool, right, chunk);
            emit(chunk, (Instr){.op = OP_OR});
            return;
        }
        case AST_COMPARE: {
            int left = node->first_child;
            int right = pool->nodes[left].next_sibling;
            compile_expr(c, pool, left, chunk);
            compile_expr(c, pool, right, chunk);
            OpCode op;
            switch (node->cmpop) {
                case AST_CMP_LT: op = OP_CMP_LT; break;
                case AST_CMP_GT: op = OP_CMP_GT; break;
                case AST_CMP_EQ: op = OP_CMP_EQ; break;
                case AST_CMP_LE: op = OP_CMP_LE; break;
                case AST_CMP_GE: op = OP_CMP_GE; break;
                default: op = OP_CMP_NE; break;
            }
            emit(chunk, (Instr){.op = op});
            return;
        }
        default:
            // A bare expression used as a condition -- OP_JUMP_IF_FALSE
            // itself calls eval_is_truthy on whatever's here, exactly
            // matching eval_condition's own default case.
            compile_expr(c, pool, node_idx, chunk);
            return;
    }
}

// Mirrors exec_if exactly.
static void compile_if(Compiler *c, AstPool *pool, int if_node, BytecodeChunk *chunk) {
    AstNode *node = &pool->nodes[if_node];
    int cond_node = node->first_child;
    int true_block = pool->nodes[cond_node].next_sibling;
    int false_block = pool->nodes[true_block].next_sibling; // -1 if there's no else

    compile_condition(c, pool, cond_node, chunk);
    int jf = emit(chunk, (Instr){.op = OP_JUMP_IF_FALSE, .a = -1});
    compile_block(c, pool, true_block, chunk);
    if (false_block >= 0) {
        int jend = emit(chunk, (Instr){.op = OP_JUMP, .a = -1});
        if (jf >= 0) chunk->code[jf].a = chunk->count;
        compile_block(c, pool, false_block, chunk);
        if (jend >= 0) chunk->code[jend].a = chunk->count;
    } else if (jf >= 0) {
        chunk->code[jf].a = chunk->count;
    }
}

// Mirrors exec_statement exactly: AST_PROC_DEF is already resolvable
// (compiled separately in compile_program's own pass 1), so there's
// nothing to do here.
static void compile_statement(Compiler *c, AstPool *pool, int node_idx, BytecodeChunk *chunk) {
    AstNode *node = &pool->nodes[node_idx];
    if (node->type == AST_PROC_DEF) return;
    if (node->type == AST_IF) {
        compile_if(c, pool, node_idx, chunk);
        return;
    }
    compile_call(c, pool, node_idx, chunk, /*want_value=*/0);
}

// Mirrors exec_block exactly.
static void compile_block(Compiler *c, AstPool *pool, int block_node, BytecodeChunk *chunk) {
    for (int ch = pool->nodes[block_node].first_child; ch >= 0; ch = pool->nodes[ch].next_sibling) {
        compile_statement(c, pool, ch, chunk);
    }
}

int compile_program(AstPool *pool, int program_node, BytecodeChunk *chunk) {
    Compiler c = {0};

    // Pass 1: every top-level procedure's own body, in whatever order
    // they appear in `pool` -- recording each one's start address as
    // it's compiled, backpatching any call made *before* its own
    // target was known (see this file's own top comment).
    for (int i = 0; i < pool->node_count; i++) {
        if (pool->nodes[i].type != AST_PROC_DEF) continue;
        if (c.proc_count < MAX_COMPILED_PROCS) {
            ProcAddr *pa = &c.procs[c.proc_count++];
            snprintf(pa->name, sizeof(pa->name), "%s", pool->nodes[i].text);
            pa->start_pc = chunk->count;
        }
        compile_block(&c, pool, pool->nodes[i].first_child, chunk);
        // Guarantees a return even if this body never explicitly
        // OUTPUTs/STOPs -- matches call_ast_procedure's own "fell off
        // the end" default (has_output_value stays FALSE).
        emit(chunk, (Instr){.op = OP_STOP});
    }

    // Pass 2: resolve every pending patch, now that every procedure's
    // own name is final in c.procs[]. A target that's still
    // unresolved here can't happen -- the parser already guarantees
    // every AST_CALL name it built resolves to either a builtin or a
    // hoisted procedure (see parser.c's own try_parse_call), the same
    // guarantee eval.c's own do_user_procedure_call/find_proc_def rely
    // on.
    for (int i = 0; i < c.patch_count; i++) {
        int target = find_proc_addr(&c, c.patches[i].name);
        if (target >= 0) chunk->code[c.patches[i].instr_index].a = target;
    }

    // Top-level statements, compiled last -- every procedure already
    // has a resolved address by now, so no top-level call ever needs
    // patching.
    int program_start = chunk->count;
    compile_block(&c, pool, program_node, chunk);
    emit(chunk, (Instr){.op = OP_HALT});
    return program_start;
}
