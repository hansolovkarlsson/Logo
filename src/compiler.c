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
// compile_call's own builtin-vs-procedure dispatch is a real lookup,
// not a growing if-chain: `find_proc_def(pool, name)` (ast.h/ast.c --
// touches nothing but the AST, so compiler.c can call it directly
// without pulling in eval.h's interpreter.h dependency) tells us
// whether `name` is a hoisted user procedure anywhere in this program,
// regardless of whether *this* call's own procedure has been compiled
// yet (pass 1 may not have reached it -- see the backpatching note
// below); if not, it must be a builtin (the parser itself already
// guarantees every AST_CALL name resolves to one or the other -- see
// parser.c's own try_parse_call/BUILTIN_SIGNATURES), and vm.c's own
// call_builtin dispatch (which growing instruction coverage now only
// needs to touch there and in eval.c, never here) is responsible for
// recognizing it. Only OUTPUT/STOP/WHILE/MAKE/LOCAL/ERASE stay
// special-cased by name in compile_call itself, since their argument
// shapes are irregular (a variable name instead of a value expression,
// a block argument, control transfer) in ways the uniform "compile
// every child as an expression, then call" path can't express. SEND is
// also special-cased, for a different reason: see its own comment in
// compile_call below and exec_send in vm.c -- its callee isn't known
// until runtime, so it can't be compiled as an ordinary OP_CALL_PROC/
// OP_CALL_BUILTIN at all.
//
// Procedure calls need genuine backpatching, not just "compile
// procedures before top-level code": one procedure's own body can
// call another procedure that hasn't been compiled *yet* (pass 1 below
// walks pool->nodes[] in array order, which doesn't have to match
// call order), so its target address isn't known at the moment that
// call is compiled. Every OP_CALL_PROC emitted for a not-yet-resolved
// name gets a placeholder target (-1) and a pending-patch entry
// instead; once every procedure has been compiled (so every name in
// chunk->procs[] is final), a second pass fills in every placeholder.
// Top-level calls never need this: procedures are always compiled
// before the top-level code that might call them, so a top-level
// call's own target is already known the moment it's compiled.
// chunk->procs[] itself (not a Compiler-local table) is what makes
// this resolvable at all -- see bytecode.h's own comment on why it's
// kept on the chunk instead of discarded after compile_program
// returns (vm.c's own OP_SEND needs the same table at runtime).

#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FOREVER_MAX_ITERATIONS 1000000 // matches MAX_WHILE_ITERATIONS in logo_types.h -- can't include that header here (interpreter.h's own GTK dependency chain), same reasoning as MAX_CHUNK_PROCS/MAX_COMPILED_PROCS matching MAX_PROCEDURES

// Proc addresses (name -> start_pc) now live on BytecodeChunk itself
// (see bytecode.h's own ProcAddr/MAX_CHUNK_PROCS) rather than a
// Compiler-local table discarded after compile_program returns --
// vm.c's own OP_SEND needs that same table at RUNTIME (a message's
// target procedure isn't known until its prototype chain is resolved
// against live app state, so it can't be backpatched the way an
// ordinary call's target can). Backpatching itself is unaffected:
// bytecode_find_proc(chunk, name) is just where find_proc_addr used to
// look.

#define MAX_PENDING_PATCHES 256

typedef struct {
    int instr_index;
    char name[32];
} PendingPatch;

typedef struct {
    PendingPatch patches[MAX_PENDING_PATCHES];
    int patch_count;
    // MAP/FILTER/REDUCE/FOREACH's own compiled-once templates need a
    // real Logo variable name for their own "?"/"?1"/"?2" placeholder(s)
    // -- see compile_template_call's own file comment for why it can't
    // be one fixed shared name. Incremented once per call site (never
    // reset), so every template ever compiled in one program gets its
    // own distinct name, safe against both recursion and nesting.
    int template_counter;
} Compiler;

static int emit(BytecodeChunk *chunk, Instr instr) {
    return bytecode_emit(chunk, instr);
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
static void compile_block(Compiler *c, AstPool *pool, int block_node, BytecodeChunk *chunk, int is_top_level);
static int compile_template_call(Compiler *c, AstPool *pool, const char *name, int template_node, int list_node, BytecodeChunk *chunk, int want_value);
static void finish_call(BytecodeChunk *chunk, const char *name, int want_value);

// Deep-copies `src_idx`'s own subtree (in `src`) into `dest`, node for
// node, returning the new root's index in `dest` -- see
// compile_template_call's own file comment for why a freshly
// (re-)parsed template body can't just be compiled directly out of its
// own scratch AstPool: every AstPool referenced by this compiler's own
// `pool` parameter, all the way down through compile_program's initial
// call, is the SAME one pool throughout (never swapped for a
// procedure body, an IF/WHILE/FOR block, or -- once grafted -- a
// template body either), which is what lets find_proc_def(pool, name)
// and OP_PUSH_LIST_LITERAL's own stored node index stay meaningful
// anywhere in the compiled program. Grafting into that same pool
// (rather than compiling straight out of the scratch one
// logo_parse_expr/logo_parse_condition/logo_parse just built) keeps
// that invariant intact, including for a nested template (whose own
// grafted body also grafts into the very same pool).
static int ast_graft(AstPool *dest, AstPool *src, int src_idx) {
    if (src_idx < 0) return -1;
    AstNode *s = &src->nodes[src_idx];
    int dest_idx = ast_alloc(dest, s->type, s->line, s->col);
    if (dest_idx < 0) return -1; // dest pool exhausted (MAX_AST_NODES) -- extremely unlikely for a real script
    AstNode *d = &dest->nodes[dest_idx];
    d->number = s->number;
    snprintf(d->text, sizeof(d->text), "%s", s->text);
    d->binop = s->binop;
    d->cmpop = s->cmpop;
    d->param_count = s->param_count;
    memcpy(d->param_names, s->param_names, sizeof(s->param_names));
    d->body_text = s->body_text;
    d->body_len = s->body_len;
    for (int ch = s->first_child; ch >= 0; ch = src->nodes[ch].next_sibling) {
        int new_ch = ast_graft(dest, src, ch);
        if (new_ch < 0) return dest_idx; // stop grafting further children on exhaustion; the partial result is discarded by compile_template_call's own caller anyway
        ast_append_child(dest, dest_idx, new_ch);
    }
    return dest_idx;
}

// Renders `node_idx` (an AST_LIST_LITERAL)'s own contents back into
// Logo source text -- the same whitespace-joined, bracket-wrapped shape
// eval.c's own list_elements_to_text/append_node_text build from the
// equivalent RUNTIME list, just read directly off the AST instead (no
// runtime list_pool round-trip needed at compile time, since a list
// literal's own children were never operator-precedence-parsed to
// begin with -- see ast.h's own AST_LIST_LITERAL comment). Every
// element whose own raw text exactly equals `ph1` (or `ph2`, if
// non-NULL) is replaced with `repl1`/`repl2` instead of copied
// verbatim -- a whole-token comparison, matching eval.c's own
// eval_substitute_placeholder exactly (so "?2" is correctly left alone
// when only substituting "?1", and vice versa).
static void render_list_literal_source(AstPool *pool, int node_idx,
                                        const char *ph1, const char *repl1,
                                        const char *ph2, const char *repl2,
                                        char *out, size_t out_size) {
    out[0] = '\0';
    int first = 1;
    for (int ch = pool->nodes[node_idx].first_child; ch >= 0; ch = pool->nodes[ch].next_sibling) {
        if (!first) strncat(out, " ", out_size - strlen(out) - 1);
        first = 0;
        AstNode *node = &pool->nodes[ch];
        if (node->type == AST_LIST_LITERAL) {
            strncat(out, "[", out_size - strlen(out) - 1);
            char sub[512];
            render_list_literal_source(pool, ch, ph1, repl1, ph2, repl2, sub, sizeof(sub));
            strncat(out, sub, out_size - strlen(out) - 1);
            strncat(out, "]", out_size - strlen(out) - 1);
        } else {
            const char *text = node->text;
            if (ph1 && strcmp(text, ph1) == 0) text = repl1;
            else if (ph2 && strcmp(text, ph2) == 0) text = repl2;
            strncat(out, text, out_size - strlen(out) - 1);
        }
    }
}

// MAP/FILTER/REDUCE/FOREACH's own "compiled once" fast path -- only
// attempted when the template argument is a literal `[...]` visible
// right here at compile time (checked by compile_call, the only
// caller); returns 0 (do nothing, emit nothing) if the rendered
// template text doesn't actually parse cleanly, so compile_call falls
// through to the ordinary generic dispatch instead -- the exact same
// OP_CALL_BUILTIN path a runtime-computed template already has to take
// (see compile_call), which routes through eval_map_value/
// eval_filter_value/eval_reduce_value/eval_foreach_value: the same
// text-substitute-and-reparse-every-iteration machinery eval.c's own
// do_map/do_filter/do_reduce/do_foreach have always used, including
// their own defensive per-iteration behavior on a parse failure
// (num_val(0) elements for MAP, no elements kept for FILTER, acc left
// unchanged for REDUCE, a silent no-op statement for FOREACH). Falling
// through here reproduces that exactly, rather than this function
// trying to separately reinvent it for the "known broken at compile
// time" case.
//
// The fast path itself works completely differently from that runtime
// text-substitution, though: since a list literal's own elements are
// already bare, never-precedence-parsed AST_WORD/AST_LIST_LITERAL
// leaves, "?"/"?1"/"?2" placeholders are just ordinary tokens among
// them -- render_list_literal_source above rewrites every occurrence
// into a real Logo variable reference (":__tmplN__", a
// compiler-generated, per-call-site-unique name via
// Compiler.template_counter) directly in the reconstructed source
// text, which is then lexed and parsed ONCE, right now, into a real
// expression/condition/block AST -- never re-substituted-and-reparsed
// per iteration the way eval.c's own runtime path has to. At runtime
// (see vm.c's own exec_map_compiled/exec_filter_compiled/
// exec_reduce_compiled/exec_foreach_compiled), each iteration just
// binds the placeholder variable(s) to the current element's actual
// EvalValue and re-executes the SAME already-compiled bytecode -- a
// real value binding, not a text round-trip, so it handles
// list-valued elements, recursion, and nesting for free, and doesn't
// need eval_value_to_source_text's own quote-escaping dance at all.
//
// REDUCE gets two placeholder variables instead of one:
// "<base>_1"/"<base>_2" for "?1"/"?2" (the accumulator and the current
// element respectively) -- `instr->text` on the opcode itself only
// ever holds `base`; vm.c's own exec_reduce_compiled derives the same
// two suffixed names from it independently, so this naming convention
// must stay in sync between the two files (documented here and again
// there).
static int compile_template_call(Compiler *c, AstPool *pool, const char *name, int template_node, int list_node, BytecodeChunk *chunk, int want_value) {
    int is_filter = strcasecmp(name, "FILTER") == 0;
    int is_reduce = strcasecmp(name, "REDUCE") == 0;
    int is_foreach = strcasecmp(name, "FOREACH") == 0;

    char base[24];
    snprintf(base, sizeof(base), "__tmpl%d__", c->template_counter++);
    char var1[32], var2[32] = {0};
    char varref1[40], varref2[40] = {0};
    if (is_reduce) {
        snprintf(var1, sizeof(var1), "%s_1", base);
        snprintf(var2, sizeof(var2), "%s_2", base);
    } else {
        snprintf(var1, sizeof(var1), "%s", base);
    }
    snprintf(varref1, sizeof(varref1), ":%s", var1);
    if (is_reduce) snprintf(varref2, sizeof(varref2), ":%s", var2);

    char rendered[1024];
    render_list_literal_source(pool, template_node,
                                is_reduce ? "?1" : "?", varref1,
                                is_reduce ? "?2" : NULL, is_reduce ? varref2 : NULL,
                                rendered, sizeof(rendered));

    LogoToken tokens[128];
    int ntok = logo_lex(rendered, tokens, 128);
    if (ntok < 0) return 0;

    ParseResult *scratch = calloc(1, sizeof(ParseResult));
    if (!scratch) return 0;
    int body_node;
    if (is_filter) {
        body_node = logo_parse_condition(tokens, ntok, scratch);
    } else if (is_foreach) {
        logo_parse(tokens, ntok, scratch);
        body_node = scratch->program;
    } else {
        body_node = logo_parse_expr(tokens, ntok, scratch);
    }
    if (body_node < 0 || scratch->error_count > 0) {
        parse_result_destroy(scratch);
        return 0;
    }

    int grafted_node = ast_graft(pool, &scratch->pool, body_node);
    parse_result_destroy(scratch);
    if (grafted_node < 0) return 0;

    // The template compiles cleanly -- now actually emit code: the
    // list argument, a jump around the template's own one-time
    // compiled body, the body itself (ending in OP_HALT, only ever
    // reached via the new opcode's own recursive vm_run call -- see
    // bytecode.h's own comment on these opcodes), then the opcode
    // itself.
    compile_expr(c, pool, list_node, chunk);

    int jump_around = emit(chunk, (Instr){.op = OP_JUMP, .a = -1});
    int template_start = chunk->count;
    if (is_filter) {
        compile_condition(c, pool, grafted_node, chunk);
    } else if (is_foreach) {
        compile_block(c, pool, grafted_node, chunk, /*is_top_level=*/0);
    } else {
        compile_expr(c, pool, grafted_node, chunk);
    }
    emit(chunk, (Instr){.op = OP_HALT});
    if (jump_around >= 0) chunk->code[jump_around].a = chunk->count;

    Instr instr = {0};
    instr.a = template_start;
    snprintf(instr.text, sizeof(instr.text), "%s", base);
    if (strcasecmp(name, "MAP") == 0) instr.op = OP_MAP_COMPILED;
    else if (is_filter) instr.op = OP_FILTER_COMPILED;
    else if (is_reduce) instr.op = OP_REDUCE_COMPILED;
    else instr.op = OP_FOREACH_COMPILED;
    emit(chunk, instr);
    finish_call(chunk, name, want_value);
    return 1;
}

// The uniform tail every call form ends with, whatever shape it was
// compiled as (an ordinary builtin/procedure call, or one of the
// special forms below that has just emitted its own OP_VOID_RESULT):
// in expression position, OP_CHECK_OUTPUT (named `name`, for its own
// "didn't output a value" message) inspects whatever the call form
// above just pushed; in statement position, a plain OP_POP discards it
// -- mirrors eval.c's own exec_call, where *result already defaults to
// num_val(0) and a statement-position caller simply never reads it.
static void finish_call(BytecodeChunk *chunk, const char *name, int want_value) {
    if (want_value) {
        Instr check = {0};
        check.op = OP_CHECK_OUTPUT;
        snprintf(check.text, sizeof(check.text), "%s", name);
        emit(chunk, check);
    } else {
        emit(chunk, (Instr){.op = OP_POP});
    }
}

// SETSPEED's own throttle point -- every builtin OP_MOTION_DELAY gets
// emitted after (see compile_call's generic dispatch below), matching
// exactly the set parser.c's own turtle-motion signatures cover: the
// four step commands and their long forms, the four absolute setters,
// HOME, and ARC. Deliberately excludes TELL (switches which turtle is
// current, doesn't move one) and every pen/color/canvas/label/fill
// command (no turtle position or heading changes, nothing to see slow
// down).
static int is_motion_command(const char *name) {
    static const char *const names[] = {
        "FD", "FORWARD", "BK", "BACK", "RT", "RIGHT", "LT", "LEFT",
        "SETXY", "SETHEADING", "SETH", "SETX", "SETY", "HOME", "ARC",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(name, names[i]) == 0) return 1;
    }
    return 0;
}

// One AST_CALL node -- OUTPUT/STOP/WHILE/MAKE/LOCAL (see this file's
// own comment on why these are recognized by name) or an ordinary
// builtin/user-procedure call, in either expression position
// (want_value: leaves exactly one value on the stack) or statement
// position (see finish_call above).
static void compile_call(Compiler *c, AstPool *pool, int node_idx, BytecodeChunk *chunk, int want_value) {
    AstNode *node = &pool->nodes[node_idx];
    const char *name = node->text;
    int args[AST_MAX_PARAMS];

    if (strcasecmp(name, "OUTPUT") == 0) {
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        compile_expr(c, pool, args[0], chunk);
        emit(chunk, (Instr){.op = OP_OUTPUT});
        return; // control transfer -- never reaches finish_call, same as eval.c's own OUTPUT never falling through to a "did I produce a value" check of its own
    }
    if (strcasecmp(name, "STOP") == 0) {
        emit(chunk, (Instr){.op = OP_STOP});
        return; // same reasoning as OUTPUT above
    }
    if (strcasecmp(name, "WHILE") == 0) {
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        int loop_start = chunk->count;
        compile_condition(c, pool, args[0], chunk);
        int jf = emit(chunk, (Instr){.op = OP_JUMP_IF_FALSE, .a = -1});
        compile_block(c, pool, args[1], chunk, /*is_top_level=*/0);
        // do_while's own equivalent: "if (stop_requested ||
        // throw_requested) break" after exec_block returns, before
        // looping again -- STOP already can't reach here (OP_STOP
        // jumps directly out of the whole frame, never falling through
        // to this point at all), so only throw_requested needs its own
        // check, skipping the loop-back jump entirely on a throw still
        // propagating out of the body.
        int throw_check = emit(chunk, (Instr){.op = OP_CHECK_THROW, .a = -1});
        emit(chunk, (Instr){.op = OP_JUMP, .a = loop_start});
        int loop_end = chunk->count;
        if (jf >= 0) chunk->code[jf].a = loop_end;
        if (throw_check >= 0) chunk->code[throw_check].a = loop_end;
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "WHILE", want_value);
        return;
    }
    if (strcasecmp(name, "REPEAT") == 0) {
        // count [block] -- count is evaluated and truncated to an
        // integer ONCE, up front (matching do_repeat's own upfront
        // `(int)eval_to_number(...)`), then left on the value stack as
        // a persistent remaining-iterations counter for the loop's own
        // duration -- see bytecode.h's own OP_PEEK comment for why a
        // stack slot, not a hidden Logo variable: it has to survive a
        // recursive call inside the loop body without colliding with
        // that recursive call's own (separate) REPEAT of the same
        // compiled statement. No MAX_WHILE_ITERATIONS cap needed here,
        // unlike WHILE/FOREVER/FOR -- do_repeat's own C for-loop is
        // already naturally bounded by count itself.
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        compile_expr(c, pool, args[0], chunk);
        Instr trunc_instr = {0};
        trunc_instr.op = OP_CALL_BUILTIN;
        trunc_instr.a = 1;
        snprintf(trunc_instr.text, sizeof(trunc_instr.text), "INT");
        emit(chunk, trunc_instr);
        // REPCOUNT's own bookkeeping (see bytecode.h's own comment):
        // pushed/popped once each, bracketing the whole loop exactly
        // like the remaining-count's own OP_POP below, regardless of
        // whether the body ever actually runs (REPEAT 0 [...]).
        emit(chunk, (Instr){.op = OP_REPCOUNT_PUSH});

        int loop_start = chunk->count;
        emit(chunk, (Instr){.op = OP_PEEK, .a = 0}); // copy of the remaining count
        emit(chunk, (Instr){.op = OP_PUSH_NUMBER, .number = 0});
        emit(chunk, (Instr){.op = OP_CMP_GT});
        int jf = emit(chunk, (Instr){.op = OP_JUMP_IF_FALSE, .a = -1});
        compile_block(c, pool, args[1], chunk, /*is_top_level=*/0);
        int throw_check = emit(chunk, (Instr){.op = OP_CHECK_THROW, .a = -1});
        emit(chunk, (Instr){.op = OP_REPCOUNT_INCR}); // this pass is done; the next one (if any) is REPCOUNT+1
        // count = count - 1 -- ordinary subtraction replaces the
        // persistent count in place (pops the "1" just pushed and the
        // original count, pushes their difference at the very same
        // stack depth), no separate "write back" opcode needed.
        emit(chunk, (Instr){.op = OP_PUSH_NUMBER, .number = 1});
        emit(chunk, (Instr){.op = OP_SUB});
        emit(chunk, (Instr){.op = OP_JUMP, .a = loop_start});
        int loop_end = chunk->count;
        if (jf >= 0) chunk->code[jf].a = loop_end;
        if (throw_check >= 0) chunk->code[throw_check].a = loop_end;
        emit(chunk, (Instr){.op = OP_POP}); // discard the remaining count
        emit(chunk, (Instr){.op = OP_REPCOUNT_POP});
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "REPEAT", want_value);
        return;
    }
    if (strcasecmp(name, "FOREVER") == 0) {
        // [block] -- an infinite loop capped by the same
        // MAX_WHILE_ITERATIONS ceiling do_forever itself uses, so a
        // script that forgets its own STOP doesn't hang the VM. The
        // running iteration count lives on the value stack for the
        // same recursion-safety reason REPEAT's own count does (see
        // above and bytecode.h's own OP_PEEK comment).
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        emit(chunk, (Instr){.op = OP_PUSH_NUMBER, .number = 0}); // iteration counter

        int loop_start = chunk->count;
        emit(chunk, (Instr){.op = OP_PEEK, .a = 0});
        emit(chunk, (Instr){.op = OP_PUSH_NUMBER, .number = FOREVER_MAX_ITERATIONS});
        emit(chunk, (Instr){.op = OP_CMP_GE});
        int jf = emit(chunk, (Instr){.op = OP_JUMP_IF_FALSE, .a = -1});
        Instr msg = {0};
        msg.op = OP_PUSH_WORD;
        msg.a = bytecode_add_word_literal(chunk, "FOREVER: stopped after too many iterations");
        emit(chunk, msg);
        Instr print_instr = {0};
        print_instr.op = OP_CALL_BUILTIN;
        print_instr.a = 1;
        snprintf(print_instr.text, sizeof(print_instr.text), "PRINT");
        emit(chunk, print_instr);
        emit(chunk, (Instr){.op = OP_POP});
        int j_done = emit(chunk, (Instr){.op = OP_JUMP, .a = -1});

        int continue_loop = chunk->count;
        if (jf >= 0) chunk->code[jf].a = continue_loop;
        compile_block(c, pool, args[0], chunk, /*is_top_level=*/0);
        int throw_check = emit(chunk, (Instr){.op = OP_CHECK_THROW, .a = -1});
        emit(chunk, (Instr){.op = OP_PUSH_NUMBER, .number = 1});
        emit(chunk, (Instr){.op = OP_ADD}); // counter = counter + 1, in place
        emit(chunk, (Instr){.op = OP_JUMP, .a = loop_start});

        int forever_end = chunk->count;
        if (j_done >= 0) chunk->code[j_done].a = forever_end;
        if (throw_check >= 0) chunk->code[throw_check].a = forever_end;
        emit(chunk, (Instr){.op = OP_POP}); // discard the iteration counter
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "FOREVER", want_value);
        return;
    }
    if (strcasecmp(name, "CATCH") == 0) {
        // "tag [block] -- tag is ARG_EXPR (an ordinary, possibly
        // computed expression, unlike MAKE/LOCAL/ERASE's raw
        // ARG_QUOTED_WORD name), so it's compiled as one, left sitting
        // on the value stack for the block's own entire duration
        // (compile_block is always stack-neutral overall, so nothing
        // the block does disturbs it), and only popped by OP_CATCH_CHECK
        // once the block finishes -- NOT stashed in a VM-level scratch
        // field, since CATCH can nest and a nested CATCH's own tag
        // would clobber an outer one's there (a real bug caught by
        // test_vm.c's own nested-CATCH test, not guessed). Mirrors
        // do_catch's own exact order either way: evaluate the tag,
        // THEN run the block, THEN check.
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        compile_expr(c, pool, args[0], chunk);
        compile_block(c, pool, args[1], chunk, /*is_top_level=*/0);
        emit(chunk, (Instr){.op = OP_CATCH_CHECK});
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "CATCH", want_value);
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
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "MAKE", want_value);
        return;
    }
    if (strcasecmp(name, "LOCAL") == 0) {
        // Same ARG_QUOTED_WORD shape as MAKE's own first argument.
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        const char *varname = pool->nodes[args[0]].text;
        Instr instr = {0};
        instr.op = OP_LOCAL;
        snprintf(instr.text, sizeof(instr.text), "%s", varname);
        emit(chunk, instr);
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "LOCAL", want_value);
        return;
    }
    if (strcasecmp(name, "ERASE") == 0) {
        // Same ARG_QUOTED_WORD shape as MAKE/LOCAL's own first argument.
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        const char *procname = pool->nodes[args[0]].text;
        Instr instr = {0};
        instr.op = OP_ERASE;
        snprintf(instr.text, sizeof(instr.text), "%s", procname);
        emit(chunk, instr);
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "ERASE", want_value);
        return;
    }
    if (strcasecmp(name, "WAIT") == 0) {
        // See vm.h's own VmRunResult/bytecode.h's OP_WAIT comments: WAIT
        // never itself produces a value, just like MAKE/LOCAL/ERASE
        // above, but unlike those, OP_WAIT can make vm_run *return*
        // right here mid-chunk (a suspend) -- the OP_VOID_RESULT below
        // is exactly the instruction vm_resume continues at once the
        // wait is over, so it must stay immediately after OP_WAIT, not
        // just conceptually paired with it.
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        compile_expr(c, pool, args[0], chunk);
        emit(chunk, (Instr){.op = OP_WAIT});
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "WAIT", want_value);
        return;
    }
    if (strcasecmp(name, "WAITKEY") == 0) {
        // Unlike WAIT, WAITKEY does produce a value (the pressed key's
        // name) -- but not from this opcode's own handler; see
        // vm_resume_with_key. finish_call's own OP_CHECK_OUTPUT/OP_POP
        // is exactly what vm_resume_with_key continues into after
        // pushing that value, the same role OP_VOID_RESULT plays for
        // WAIT above.
        emit(chunk, (Instr){.op = OP_WAITKEY});
        finish_call(chunk, "WAITKEY", want_value);
        return;
    }
    if (strcasecmp(name, "INPUT") == 0) {
        // Same shape as WAITKEY -- see vm_resume_with_input.
        emit(chunk, (Instr){.op = OP_INPUT});
        finish_call(chunk, "INPUT", want_value);
        return;
    }
    if (strcasecmp(name, "PAUSE") == 0) {
        // Same shape as WAIT -- produces no value itself (the
        // OP_VOID_RESULT immediately after is what runs once resumed).
        // CONTINUE/CO deliberately have no branch here at all: they
        // never suspend, so the generic OP_CALL_BUILTIN dispatch below
        // already handles them correctly.
        emit(chunk, (Instr){.op = OP_PAUSE});
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "PAUSE", want_value);
        return;
    }
    if (strcasecmp(name, "ANIMATESPRITE") == 0) {
        // delay frames -- both plain expressions, pushed delay-then-
        // frames (matching interpreter.c's own parse_expr order), so
        // OP_ANIMATESPRITE's own handler pops frames first, then delay.
        // Same shape as WAIT/PAUSE otherwise: produces no value itself.
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        compile_expr(c, pool, args[0], chunk);
        compile_expr(c, pool, args[1], chunk);
        emit(chunk, (Instr){.op = OP_ANIMATESPRITE});
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "ANIMATESPRITE", want_value);
        return;
    }
    if (strcasecmp(name, "SEND") == 0) {
        // Unlike every other call form, SEND's own callee isn't known
        // until runtime (resolved through obj's prototype chain --
        // see exec_send in vm.c), so it can't use finish_call's own
        // OP_CHECK_OUTPUT (which needs a compile-time-known name for
        // its "didn't output a value" message; SEND's message is a
        // runtime value). All 3 arguments (obj, message, arglist) are
        // still ordinary expressions, though -- only the call
        // mechanism itself differs.
        int send_argc = collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        for (int i = 0; i < send_argc; i++) compile_expr(c, pool, args[i], chunk);
        emit(chunk, (Instr){.op = OP_SEND});
        if (want_value) {
            emit(chunk, (Instr){.op = OP_CHECK_SEND_OUTPUT});
        } else {
            emit(chunk, (Instr){.op = OP_POP});
        }
        return;
    }
    if (strcasecmp(name, "APPLY") == 0) {
        // Same reasoning as SEND above (dynamic callee, resolved at
        // runtime -- see exec_apply in vm.c) plus one more wrinkle:
        // APPLY never hands back a value at all (matching RUN,
        // confirmed in docs/LANGUAGE.md), even when the applied
        // procedure itself calls OUTPUT -- OP_VOID_DISCARD, not
        // finish_call's own OP_CHECK_OUTPUT, is what enforces that.
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        compile_expr(c, pool, args[0], chunk);
        compile_expr(c, pool, args[1], chunk);
        emit(chunk, (Instr){.op = OP_APPLY});
        emit(chunk, (Instr){.op = OP_VOID_DISCARD});
        finish_call(chunk, "APPLY", want_value);
        return;
    }
    if (strcasecmp(name, "RUN") == 0) {
        // See bytecode.h's own OP_RUN comment -- a plain expression
        // argument, unlike LOAD's own compile-time-known path below.
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        compile_expr(c, pool, args[0], chunk);
        emit(chunk, (Instr){.op = OP_RUN});
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "RUN", want_value);
        return;
    }
    if (strcasecmp(name, "LOAD") == 0) {
        // Same ARG_QUOTED_WORD shape as ERASE/MAKE/LOCAL's own raw-name
        // arguments -- LOAD's grammar can only ever take a literal
        // path, never a computed expression (see bytecode.h's own
        // OP_LOAD comment).
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        const char *path = pool->nodes[args[0]].text;
        Instr instr = {0};
        instr.op = OP_LOAD;
        snprintf(instr.text, sizeof(instr.text), "%s", path);
        emit(chunk, instr);
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "LOAD", want_value);
        return;
    }
    if (strcasecmp(name, "LAUNCH") == 0) {
        // Two plain ARG_EXPR arguments (name, arglist), matching APPLY
        // exactly -- see bytecode.h's own OP_LAUNCH comment for why
        // (both computed at runtime is fine here, unlike LOAD's
        // compile-time-literal path).
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        compile_expr(c, pool, args[0], chunk);
        compile_expr(c, pool, args[1], chunk);
        emit(chunk, (Instr){.op = OP_LAUNCH});
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "LAUNCH", want_value);
        return;
    }
    if (strcasecmp(name, "AWAIT") == 0) {
        emit(chunk, (Instr){.op = OP_AWAIT});
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "AWAIT", want_value);
        return;
    }
    if (strcasecmp(name, "YIELD") == 0) {
        emit(chunk, (Instr){.op = OP_YIELD});
        emit(chunk, (Instr){.op = OP_VOID_RESULT});
        finish_call(chunk, "YIELD", want_value);
        return;
    }
    if (strcasecmp(name, "MAP") == 0 || strcasecmp(name, "FILTER") == 0 ||
        strcasecmp(name, "REDUCE") == 0 || strcasecmp(name, "FOREACH") == 0) {
        // Only the "compiled once" fast path is special-cased here --
        // see compile_template_call's own file comment. When the
        // template argument isn't a literal `[...]` visible right now
        // (a runtime-computed template, e.g. `MAP :tmpl [1 2 3]`),
        // this falls through to the ordinary generic dispatch below
        // exactly like any other builtin, which emits OP_CALL_BUILTIN
        // -- vm.c's own call_builtin has a matching dynamic-fallback
        // branch for all four names.
        collect_children(pool, node_idx, args, AST_MAX_PARAMS);
        if (pool->nodes[args[0]].type == AST_LIST_LITERAL) {
            if (compile_template_call(c, pool, name, args[0], args[1], chunk, want_value)) return;
            // Malformed template, known already at compile time -- fall
            // through to the ordinary generic dispatch below instead
            // (see compile_template_call's own comment).
        }
    }

    int argc = collect_children(pool, node_idx, args, AST_MAX_PARAMS);
    for (int i = 0; i < argc; i++) compile_expr(c, pool, args[i], chunk);

    if (find_proc_def(pool, name) >= 0) {
        int target = bytecode_find_proc(chunk, name);
        Instr instr = {0};
        instr.op = OP_CALL_PROC;
        instr.a = target;
        instr.b = argc;
        snprintf(instr.text, sizeof(instr.text), "%s", name);
        int idx = emit(chunk, instr);
        if (target < 0) add_pending_patch(c, idx, name);
    } else {
        Instr instr = {0};
        instr.op = OP_CALL_BUILTIN;
        instr.a = argc;
        snprintf(instr.text, sizeof(instr.text), "%s", name);
        emit(chunk, instr);
        if (is_motion_command(name)) emit(chunk, (Instr){.op = OP_MOTION_DELAY});
    }
    finish_call(chunk, name, want_value);
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
            instr.a = bytecode_add_word_literal(chunk, node->text);
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
        case AST_LIST_LITERAL: {
            // Rendered back into ordinary Logo source text right here
            // (render_list_literal_source, the same renderer
            // compile_template_call already trusts) and stored on the
            // chunk itself instead of left as a pointer into this AST
            // -- see bytecode.h's own list_literals[] comment. No
            // placeholder substitution (NULL/NULL twice): this is a
            // plain literal, not a MAP/FILTER/REDUCE template body.
            char inner[MAX_LIST_LITERAL_TEXT];
            render_list_literal_source(pool, node_idx, NULL, NULL, NULL, NULL, inner, sizeof(inner));
            char full[MAX_LIST_LITERAL_TEXT];
            snprintf(full, sizeof(full), "[%s]", inner);
            Instr instr = {0};
            instr.op = OP_PUSH_LIST_LITERAL;
            instr.a = bytecode_add_list_literal(chunk, full);
            emit(chunk, instr);
            return;
        }
        default:
            // Not reachable for this vertical slice's own grammar
            // subset (AST_IF/AST_BLOCK/AST_PROC_DEF/AST_FOR/the
            // AST_COMPARE-and-friends condition nodes never appear in
            // expression position -- see ast.h's own node-shape
            // comments). A harmless placeholder rather than reading
            // uninitialized instruction fields, matching this
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
    compile_block(c, pool, true_block, chunk, /*is_top_level=*/0);
    if (false_block >= 0) {
        int jend = emit(chunk, (Instr){.op = OP_JUMP, .a = -1});
        if (jf >= 0) chunk->code[jf].a = chunk->count;
        compile_block(c, pool, false_block, chunk, /*is_top_level=*/0);
        if (jend >= 0) chunk->code[jend].a = chunk->count;
    } else if (jf >= 0) {
        chunk->code[jf].a = chunk->count;
    }
}

// FOR [var start limit step] [block] -- mirrors exec_for exactly (see
// its own comment in eval.c for the header-parsing/default-step
// reach), but with a genuinely different compiled shape from an
// ordinary WHILE-style loop: `limit` and `step` are each evaluated
// ONCE, up front, and stay readable for the entire loop, on the value
// stack (see bytecode.h's own OP_PEEK comment for why: recursion-
// safety, the same reasoning as REPEAT/FOREVER's own counters).
//
// The loop's own INTERNAL counter is a THIRD persistent stack slot,
// separate from the Logo-visible loop variable (`node->text`, still
// set via ordinary OP_SET_VAR each iteration, still deliberately NOT
// recursion-safe -- exactly mirroring exec_for's own plain
// `set_var(node->text, i)`, "no push_scope ... unlike a real
// parameter"). This split matters and was NOT the first design tried:
// exec_for's own loop control reads a plain C-local `double i`, which
// is naturally protected by the C call stack and *never reads back*
// the Logo-visible variable of the same name -- an earlier version of
// this function instead re-read the loop variable via OP_PUSH_VAR for
// its own condition/increment, which broke the instant a recursive
// call within the body ran its OWN FOR loop over the same variable
// name (global, so the inner call's own final value leaked out and
// corrupted the outer loop's next comparison) -- caught by
// test_vm.c's own recursion test, not guessed. The fix needed a new
// opcode, OP_POKE (see bytecode.h): the internal counter sits
// *underneath* limit/step on the stack, so a plain "push 1; OP_ADD"
// (which works fine for REPEAT/FOREVER's own lone top-of-stack
// counter) can't update it in place -- OP_POKE pops the freshly
// computed value and writes it back into that specific persistent
// slot instead.
//
// The loop itself is compiled as two near-identical variants (ascending
// via OP_CMP_LE, descending via OP_CMP_GE), selected once via a
// runtime branch on step's own sign before either ever runs -- avoiding
// re-deriving the direction on every single iteration, at the cost of
// duplicating the (short) per-iteration body. No MAX_WHILE_ITERATIONS
// cap here, matching WHILE's own still-open gap (see
// docs/BYTECODE_VM_DESIGN.md's Progress log) -- FOR's own termination
// is normally guaranteed by the start/limit/step arithmetic itself,
// unlike FOREVER's, so this is treated as a smaller, deferrable gap
// rather than the same kind of "could hang the VM outright" risk
// FOREVER's own cap exists to prevent.
//
// Stack layout once setup finishes (baseline B): [..., i_internal,
// limit, step] -- i_internal at B-3 (OP_PEEK/OP_POKE .a=2 from a clean
// baseline), limit at B-2 (.a=1), step at B-1/top (.a=0). Every
// hand-written sequence below returns to depth B before its next
// OP_PEEK, same discipline compile_block itself already guarantees for
// ordinary statements.
static void compile_for(Compiler *c, AstPool *pool, int for_node, BytecodeChunk *chunk) {
    AstNode *node = &pool->nodes[for_node];
    int start_node = node->first_child;
    int limit_node = pool->nodes[start_node].next_sibling;
    int third_node = pool->nodes[limit_node].next_sibling;
    int fourth_node = pool->nodes[third_node].next_sibling;
    int step_node = (fourth_node >= 0) ? third_node : -1;
    int block_node = (fourth_node >= 0) ? fourth_node : third_node;

    // i_internal = start -- this push IS the internal counter from
    // here on (never separately duplicated); mutated in place later
    // via OP_POKE.
    compile_expr(c, pool, start_node, chunk);

    // limit
    compile_expr(c, pool, limit_node, chunk);

    // step -- evaluated if present; otherwise (limit >= start) ? 1 : -1,
    // computed from copies of the already-pushed limit/i_internal
    // (never the Logo variable). Either way, exactly one new value
    // ends up on top, landing at baseline B either branch takes.
    if (step_node >= 0) {
        compile_expr(c, pool, step_node, chunk);
    } else {
        emit(chunk, (Instr){.op = OP_PEEK, .a = 0}); // copy of limit
        emit(chunk, (Instr){.op = OP_PEEK, .a = 2}); // copy of i_internal (== start)
        emit(chunk, (Instr){.op = OP_CMP_GE}); // limit >= start
        int jf = emit(chunk, (Instr){.op = OP_JUMP_IF_FALSE, .a = -1});
        emit(chunk, (Instr){.op = OP_PUSH_NUMBER, .number = 1});
        int j = emit(chunk, (Instr){.op = OP_JUMP, .a = -1});
        if (jf >= 0) chunk->code[jf].a = chunk->count;
        emit(chunk, (Instr){.op = OP_PUSH_NUMBER, .number = -1});
        if (j >= 0) chunk->code[j].a = chunk->count;
    }
    // Stack now: [..., i_internal, limit, step] -- baseline B.

    // Logo-visible i = i_internal (once, before the loop starts).
    emit(chunk, (Instr){.op = OP_PEEK, .a = 2});
    Instr set_i_initial = {0};
    set_i_initial.op = OP_SET_VAR;
    snprintf(set_i_initial.text, sizeof(set_i_initial.text), "%s", node->text);
    emit(chunk, set_i_initial);

    // step == 0 -- matches exec_for's own "FOR: step must not be 0"
    // and early return (no loop runs at all, either variant).
    emit(chunk, (Instr){.op = OP_PEEK, .a = 0});
    emit(chunk, (Instr){.op = OP_PUSH_NUMBER, .number = 0});
    emit(chunk, (Instr){.op = OP_CMP_EQ});
    int step_ok_jf = emit(chunk, (Instr){.op = OP_JUMP_IF_FALSE, .a = -1});
    Instr msg = {0};
    msg.op = OP_PUSH_WORD;
    msg.a = bytecode_add_word_literal(chunk, "FOR: step must not be 0");
    emit(chunk, msg);
    Instr print_instr = {0};
    print_instr.op = OP_CALL_BUILTIN;
    print_instr.a = 1;
    snprintf(print_instr.text, sizeof(print_instr.text), "PRINT");
    emit(chunk, print_instr);
    emit(chunk, (Instr){.op = OP_POP});
    int j_skip_loop = emit(chunk, (Instr){.op = OP_JUMP, .a = -1});

    int loop_setup_done = chunk->count;
    if (step_ok_jf >= 0) chunk->code[step_ok_jf].a = loop_setup_done;

    // Pick a direction once: step > 0 -> ascending (i <= limit),
    // otherwise -> descending (i >= limit). Matches exec_for's own
    // ternary `(step > 0) ? (i <= limit) : (i >= limit)` exactly.
    emit(chunk, (Instr){.op = OP_PEEK, .a = 0}); // copy of step
    emit(chunk, (Instr){.op = OP_PUSH_NUMBER, .number = 0});
    emit(chunk, (Instr){.op = OP_CMP_GT});
    int jf_desc = emit(chunk, (Instr){.op = OP_JUMP_IF_FALSE, .a = -1});

    int ascending_start = chunk->count;
    emit(chunk, (Instr){.op = OP_PEEK, .a = 2}); // copy of i_internal
    emit(chunk, (Instr){.op = OP_PEEK, .a = 2}); // copy of limit (2, not 1: i_internal's own copy is now on top)
    emit(chunk, (Instr){.op = OP_CMP_LE});
    int asc_jf = emit(chunk, (Instr){.op = OP_JUMP_IF_FALSE, .a = -1});
    compile_block(c, pool, block_node, chunk, /*is_top_level=*/0);
    int asc_throw_check = emit(chunk, (Instr){.op = OP_CHECK_THROW, .a = -1});
    // i_internal = i_internal + step; then sync the Logo-visible i.
    emit(chunk, (Instr){.op = OP_PEEK, .a = 2}); // copy of i_internal
    emit(chunk, (Instr){.op = OP_PEEK, .a = 1}); // copy of step (1: i_internal's own copy is now on top)
    emit(chunk, (Instr){.op = OP_ADD});
    emit(chunk, (Instr){.op = OP_POKE, .a = 2}); // write the sum back into i_internal's own slot
    emit(chunk, (Instr){.op = OP_PEEK, .a = 2}); // copy of the (now updated) i_internal
    Instr set_i_asc = {0};
    set_i_asc.op = OP_SET_VAR;
    snprintf(set_i_asc.text, sizeof(set_i_asc.text), "%s", node->text);
    emit(chunk, set_i_asc);
    emit(chunk, (Instr){.op = OP_JUMP, .a = ascending_start});
    int ascending_end = chunk->count;
    if (asc_jf >= 0) chunk->code[asc_jf].a = ascending_end;
    if (asc_throw_check >= 0) chunk->code[asc_throw_check].a = ascending_end;
    int j_skip_descending = emit(chunk, (Instr){.op = OP_JUMP, .a = -1});

    int descending_start = chunk->count;
    if (jf_desc >= 0) chunk->code[jf_desc].a = descending_start;
    emit(chunk, (Instr){.op = OP_PEEK, .a = 2}); // copy of i_internal
    emit(chunk, (Instr){.op = OP_PEEK, .a = 2}); // copy of limit
    emit(chunk, (Instr){.op = OP_CMP_GE});
    int desc_jf = emit(chunk, (Instr){.op = OP_JUMP_IF_FALSE, .a = -1});
    compile_block(c, pool, block_node, chunk, /*is_top_level=*/0);
    int desc_throw_check = emit(chunk, (Instr){.op = OP_CHECK_THROW, .a = -1});
    emit(chunk, (Instr){.op = OP_PEEK, .a = 2});
    emit(chunk, (Instr){.op = OP_PEEK, .a = 1});
    emit(chunk, (Instr){.op = OP_ADD});
    emit(chunk, (Instr){.op = OP_POKE, .a = 2});
    emit(chunk, (Instr){.op = OP_PEEK, .a = 2});
    Instr set_i_desc = {0};
    set_i_desc.op = OP_SET_VAR;
    snprintf(set_i_desc.text, sizeof(set_i_desc.text), "%s", node->text);
    emit(chunk, set_i_desc);
    emit(chunk, (Instr){.op = OP_JUMP, .a = descending_start});
    int descending_end = chunk->count;
    if (desc_jf >= 0) chunk->code[desc_jf].a = descending_end;
    if (desc_throw_check >= 0) chunk->code[desc_throw_check].a = descending_end;

    int variants_done = chunk->count;
    if (j_skip_descending >= 0) chunk->code[j_skip_descending].a = variants_done;

    int for_end = chunk->count;
    if (j_skip_loop >= 0) chunk->code[j_skip_loop].a = for_end;
    // Stack is back to [..., i_internal, limit, step] here regardless
    // of which path was taken (the step==0 error path, or either loop
    // variant finishing) -- discard all three.
    emit(chunk, (Instr){.op = OP_POP});
    emit(chunk, (Instr){.op = OP_POP});
    emit(chunk, (Instr){.op = OP_POP});
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
    if (node->type == AST_FOR) {
        compile_for(c, pool, node_idx, chunk);
        return;
    }
    compile_call(c, pool, node_idx, chunk, /*want_value=*/0);
}

// Mirrors exec_block's own per-statement loop, including its
// cooperative THROW propagation ("if (throw_requested) break", checked
// after every single statement -- see eval.c's own exec_block) --
// reimplemented here as a forward jump instead of a runtime recursive
// break, since this VM has no C call stack to unwind through the way
// the tree-walker does. After every compiled statement, OP_CHECK_THROW
// is emitted with a placeholder target; once every statement in this
// block has been compiled, every one of those placeholders is patched
// to jump to `chunk->count` (this block's own true end) -- so a throw
// at any point skips the rest of *this* block only, then falls through
// to whatever comes next (WHILE's own extra check before its loop-back
// jump, a procedure body's auto-appended OP_STOP, an enclosing block's
// own next OP_CHECK_THROW, ...). Composed this way, N levels of nested
// blocks correctly cascade a throw all the way up to whichever CATCH
// (or the top level) actually stops it, without this function needing
// to know anything about what encloses it.
//
// `is_top_level` (only ever 1 for compile_program's own top-level
// statements, never for a procedure body/loop body/if branch/catch
// block) swaps OP_CHECK_THROW for OP_CHECK_UNCAUGHT_THROW instead:
// ast_eval_from's own top-level loop doesn't skip to the end of the
// script on an uncaught throw the way a nested exec_block skips to its
// own end -- it reports "no CATCH found" and *keeps running* the rest
// of the script, so no jump/patch is needed there at all.
#define MAX_BLOCK_STATEMENTS 1024

static void compile_block(Compiler *c, AstPool *pool, int block_node, BytecodeChunk *chunk, int is_top_level) {
    int pending[MAX_BLOCK_STATEMENTS];
    int pending_count = 0;
    for (int ch = pool->nodes[block_node].first_child; ch >= 0; ch = pool->nodes[ch].next_sibling) {
        compile_statement(c, pool, ch, chunk);
        if (is_top_level) {
            emit(chunk, (Instr){.op = OP_CHECK_UNCAUGHT_THROW});
        } else {
            int chk = emit(chunk, (Instr){.op = OP_CHECK_THROW, .a = -1});
            if (chk >= 0 && pending_count < MAX_BLOCK_STATEMENTS) pending[pending_count++] = chk;
        }
    }
    int block_end = chunk->count;
    for (int i = 0; i < pending_count; i++) chunk->code[pending[i]].a = block_end;
}

int compile_program(AstPool *pool, int program_node, BytecodeChunk *chunk) {
    Compiler c = {0};

    // Pass 1: every top-level procedure's own body, in whatever order
    // they appear in `pool` -- recording each one's start address as
    // it's compiled, backpatching any call made *before* its own
    // target was known (see this file's own top comment).
    for (int i = 0; i < pool->node_count; i++) {
        if (pool->nodes[i].type != AST_PROC_DEF) continue;
        if (chunk->proc_count < MAX_CHUNK_PROCS) {
            AstNode *proc_def = &pool->nodes[i];
            ProcAddr *pa = &chunk->procs[chunk->proc_count++];
            snprintf(pa->name, sizeof(pa->name), "%s", proc_def->text);
            pa->start_pc = chunk->count;
            // Own copy of what eval_push_scope_for_call/TEXT/SAVE/SHOW
            // used to read straight off this AstNode at every call --
            // see ProcAddr's own bytecode.h comment for why it lives
            // here now instead.
            pa->param_count = proc_def->param_count;
            for (int p = 0; p < proc_def->param_count; p++) {
                snprintf(pa->param_names[p], sizeof(pa->param_names[p]), "%s", proc_def->param_names[p]);
            }
            const char *body = proc_def->body_text != NULL ? proc_def->body_text : "";
            int body_len = proc_def->body_len;
            if (body_len >= MAX_PROC_SOURCE_TEXT) body_len = MAX_PROC_SOURCE_TEXT - 1;
            snprintf(pa->source_text, sizeof(pa->source_text), "%.*s", body_len, body);
        }
        compile_block(&c, pool, pool->nodes[i].first_child, chunk, /*is_top_level=*/0);
        // Guarantees a return even if this body never explicitly
        // OUTPUTs/STOPs -- matches call_ast_procedure's own "fell off
        // the end" default (has_output_value stays FALSE).
        emit(chunk, (Instr){.op = OP_STOP});
    }

    // Pass 2: resolve every pending patch, now that every procedure's
    // own name is final in chunk->procs[]. A target that's still
    // unresolved here can't happen -- the parser already guarantees
    // every AST_CALL name it built resolves to either a builtin or a
    // hoisted procedure (see parser.c's own try_parse_call), the same
    // guarantee eval.c's own do_user_procedure_call/find_proc_def rely
    // on.
    for (int i = 0; i < c.patch_count; i++) {
        int target = bytecode_find_proc(chunk, c.patches[i].name);
        if (target >= 0) chunk->code[c.patches[i].instr_index].a = target;
    }

    // Top-level statements, compiled last -- every procedure already
    // has a resolved address by now, so no top-level call ever needs
    // patching.
    int program_start = chunk->count;
    compile_block(&c, pool, program_node, chunk, /*is_top_level=*/1);
    emit(chunk, (Instr){.op = OP_HALT});
    chunk->start_pc = program_start; // see bytecode.h's own BytecodeChunk.start_pc comment
    return program_start;
}
