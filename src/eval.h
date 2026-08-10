#ifndef LOGO_EVAL_H
#define LOGO_EVAL_H

// eval.h
//
// Stage 1's tree-walking evaluator (see docs/BYTECODE_VM_DESIGN.md):
// executes an AstNode tree (as built by parser.c) against a real
// LogoApp. Unlike lexer.h/ast.h/parser.h, this deliberately DOES
// depend on interpreter.h -- it shares LogoApp's turtle/variable/
// output state directly with eval_logo, via the same state helpers
// (current_turtle/move_turtle_to/find_var/etc., now exposed in
// interpreter.h specifically for this) rather than reimplementing that
// logic fresh. See the design doc's evaluator-design decision: this
// makes the two engines structurally unable to drift on turtle/
// variable behavior, which matters for diffing their output during
// the migration.
//
// A real but bounded slice of the language, matching parser.c's own
// BUILTIN_SIGNATURES coverage -- not full coverage of eval_logo's
// ~150 operators yet, and list/array values aren't evaluated yet
// either (see eval.c's own notes).

#include "ast.h"
#include "interpreter.h"
#include <stdio.h> // snprintf, used by word_val below

// A value produced by evaluating an expression subtree. Mirrors
// interpreter.c's own private Value type (same fields ValueType/
// Variable already expose the shape of in logo_types.h) -- given its
// own name here since that struct itself is private to interpreter.c,
// and there's no need to expose it just for this. Exposed here (not
// just eval.c-private, where it started) so vm.c/compiler.c (Stage 2)
// can share the exact same value representation rather than a
// parallel copy -- the same "one shared type, not drift-prone
// duplicates" reasoning already applied to LogoApp/AstPool state.
typedef struct {
    ValueType type;
    double number;
    char word[512];
    int list_head; // type == VALUE_LIST: index into app->list_pool, -1 = empty -- the SAME pool eval_logo's own list operators use
} EvalValue;

// Trivial constructors, kept `static inline` (each translation unit
// gets its own copy, no ODR risk) rather than moved to eval.c and
// exposed as ordinary declarations -- these are small enough that a
// real function-call boundary would be pure overhead.
static inline EvalValue num_val(double n) {
    EvalValue v = {0};
    v.type = VALUE_NUMBER;
    v.number = n;
    v.list_head = -1;
    return v;
}

static inline EvalValue word_val(const char *w) {
    EvalValue v = {0};
    v.type = VALUE_WORD;
    snprintf(v.word, sizeof(v.word), "%s", w);
    v.list_head = -1;
    return v;
}

static inline EvalValue list_val(int head) {
    EvalValue v = {0};
    v.type = VALUE_LIST;
    v.list_head = head;
    return v;
}

static inline EvalValue array_val(int start, int length) {
    EvalValue v = {0};
    v.type = VALUE_ARRAY;
    v.list_head = start;
    v.number = length; // an array's `number` field holds its length, not a value -- matches interpreter.c's own array_value
    return v;
}

// A handful of pure(-ish) value-level operations, exposed for the same
// reason EvalValue itself is: vm.c (Stage 2) needs the exact same
// value semantics ast_eval already has, not a parallel reimplementation
// that could quietly drift from it. None of these walk an AST -- only
// eval_expr/eval_condition (the tree-walker's own execution, staying
// eval.c-private) do that; the VM has its own separate execution loop
// and only needs this value layer underneath it.
void eval_value_to_text(LogoApp *app, EvalValue v, char *out, size_t out_size);
double eval_to_number(EvalValue v);
int eval_is_truthy(EvalValue v);
int eval_values_equal(LogoApp *app, EvalValue a, EvalValue b);

// PRINT's own value-taking core, split out of do_print (which now just
// calls eval_expr then this) so vm.c's OP_CALL_BUILTIN "PRINT" handler
// can share it too, instead of a parallel reimplementation.
void eval_print_value(LogoApp *app, EvalValue v);

// The lists/arrays/MAKE-adjacent batch's own value-taking cores (see
// docs/BYTECODE_VM_DESIGN.md's Progress log) -- same split as
// eval_print_value above, each corresponding do_* in eval.c now just
// calls eval_expr on its own AST argument(s) then forwards here, and
// vm.c's OP_CALL_BUILTIN dispatch (compiler.c no longer special-cases
// builtin names at all -- see compiler.c's own note -- so growing this
// list is now the *only* step instruction coverage needs) calls the
// exact same function directly on its own already-popped stack values.
// Where a do_* was already this shape internally (FPUT/LPUT/WORD/
// SENTENCE/LIST/BUTLAST all already delegated to a plain
// (app, EvalValue...) -> EvalValue helper), this is just removing
// `static`, not new code.
EvalValue eval_build_list_literal(LogoApp *app, AstPool *pool, int node_idx);
EvalValue eval_thing_value(LogoApp *app, EvalValue name_val);
void eval_local_declare(LogoApp *app, const char *varname);
EvalValue eval_first_value(LogoApp *app, EvalValue arg);
EvalValue eval_butfirst_value(LogoApp *app, EvalValue arg);
EvalValue eval_last_value(LogoApp *app, EvalValue arg);
EvalValue eval_list_butlast(LogoApp *app, EvalValue arg); // LIST case only -- see eval_butlast_value below for the full WORD/number-aware dispatch
EvalValue eval_butlast_value(LogoApp *app, EvalValue arg);
EvalValue eval_count_value(LogoApp *app, EvalValue arg);
EvalValue eval_empty_value(EvalValue arg);
EvalValue eval_list_fput(LogoApp *app, EvalValue thing, EvalValue list);
EvalValue eval_list_lput(LogoApp *app, EvalValue thing, EvalValue list);
EvalValue eval_word_concat(LogoApp *app, EvalValue a, EvalValue b);
EvalValue eval_list_sentence(LogoApp *app, EvalValue a, EvalValue b);
EvalValue eval_list_wrap_pair(LogoApp *app, EvalValue a, EvalValue b);
EvalValue eval_array_value(LogoApp *app, EvalValue size_val);
EvalValue eval_item_value(LogoApp *app, EvalValue index_val, EvalValue thing);
void eval_setitem_value(LogoApp *app, EvalValue index_val, EvalValue array_val_, EvalValue new_val);
void eval_fillarray_value(LogoApp *app, EvalValue array_val_, EvalValue new_val);
EvalValue eval_names_value(LogoApp *app);

// Property lists / NEW (see eval.c's own "Property lists" section for
// the shared app->plist_entries state). Same split as the batch above
// -- SEND is deliberately NOT here: unlike these, it dynamically
// resolves which procedure to call at runtime (through obj's prototype
// chain) and has its own resolved/produced error-suppression shape
// (see do_send's own comment), closer to a whole new opcode than an
// ordinary builtin call -- left for its own dedicated follow-up batch.
EvalValue eval_getprop(LogoApp *app, EvalValue name_val, EvalValue key_val);
void eval_setprop(LogoApp *app, EvalValue name_val, EvalValue key_val, EvalValue val);
void eval_removeprop(LogoApp *app, EvalValue name_val, EvalValue key_val);
EvalValue eval_proplist(LogoApp *app, EvalValue name_val);
void eval_new_declare(LogoApp *app, EvalValue obj_val, EvalValue proto_val);

// Turtle/drawing commands. Same split as every batch above: an
// eval_X_value core wherever there's a real eval_expr call to factor
// out of the corresponding do_X, exposed here for vm.c's
// call_builtin. The zero-argument ones (do_getx/do_gety/do_heading/
// do_pos/do_canvassize/do_who/do_penup/do_pendown/do_home/do_clear/
// do_clean/do_hideturtle/do_showturtle/do_wrap/do_fence/do_window/
// do_fill) never had an eval_expr call to begin with -- each already
// took only `app`, so it's exposed directly under its own do_ name
// (just `static` removed) rather than renamed to match the eval_
// convention, since there's no separate "wrapper vs. core" left to
// distinguish.
EvalValue do_getx(LogoApp *app);
EvalValue do_gety(LogoApp *app);
EvalValue do_heading(LogoApp *app);
EvalValue do_pos(LogoApp *app);
EvalValue do_canvassize(LogoApp *app);
EvalValue do_who(LogoApp *app);
void do_penup(LogoApp *app);
void do_pendown(LogoApp *app);
void do_home(LogoApp *app);
void do_clear(LogoApp *app);
void do_clean(LogoApp *app);
void do_hideturtle(LogoApp *app);
void do_showturtle(LogoApp *app);
void do_wrap(LogoApp *app);
void do_fence(LogoApp *app);
void do_window(LogoApp *app);
void do_fill(LogoApp *app);

void eval_fd_value(LogoApp *app, EvalValue dist_val);
void eval_bk_value(LogoApp *app, EvalValue dist_val);
void eval_rt_value(LogoApp *app, EvalValue angle_val);
void eval_lt_value(LogoApp *app, EvalValue angle_val);
void eval_setxy_value(LogoApp *app, EvalValue x_val, EvalValue y_val);
void eval_setheading_value(LogoApp *app, EvalValue angle_val);
void eval_setx_value(LogoApp *app, EvalValue x_val);
void eval_sety_value(LogoApp *app, EvalValue y_val);
EvalValue eval_distance_value(LogoApp *app, EvalValue a, EvalValue b);
EvalValue eval_towards_value(LogoApp *app, EvalValue p);
void eval_tell_value(LogoApp *app, EvalValue index_val);
void eval_arc_value(LogoApp *app, EvalValue angle_val, EvalValue radius_val);
void eval_setpencolor_value(LogoApp *app, EvalValue r_val, EvalValue g_val, EvalValue b_val);
void eval_setpenwidth_value(LogoApp *app, EvalValue width_val);
void eval_setbackground_value(LogoApp *app, EvalValue r_val, EvalValue g_val, EvalValue b_val);
void eval_setcanvassize_value(LogoApp *app, EvalValue width_val, EvalValue height_val);
void eval_label_value(LogoApp *app, EvalValue val);
void eval_eraserect_value(LogoApp *app, EvalValue w_val, EvalValue h_val);

// SEND's own two pure pieces (see eval.c's own "prototype-style
// objects" section) -- neither one calls another procedure, so both
// are safe to reuse directly in vm.c's own exec_send, which cannot
// reuse do_send/call_ast_procedure themselves (those run a resolved
// procedure's body via the tree-walker's exec_block; the VM instead
// has to push a VmFrame and jump into that procedure's own *compiled*
// body, an entirely different calling mechanism -- see vm.c's own
// exec_send comment).
int eval_resolve_method(LogoApp *app, AstPool *pool, const char *objname, const char *message);
int eval_send_unpack_args(LogoApp *app, const char *obj_text, EvalValue arglist_val, EvalValue *arg_vals, int max_args);

// THROW/CATCH's own shared pieces (see eval.c's own comments on each
// and bytecode.h's own file comment for the VM's cooperative-unwind
// mechanism built on top of these). eval_throw_value is an ordinary
// value-taking core (THROW routes through the generic OP_CALL_BUILTIN
// path, no special opcode needed); eval_catch_check and
// eval_report_uncaught_throw are exposed because vm.c's own
// OP_CATCH_CHECK/OP_CHECK_UNCAUGHT_THROW need the exact same logic
// do_catch/ast_eval_from already have, not a parallel copy.
void eval_throw_value(LogoApp *app, EvalValue tag_val);
void eval_catch_check(LogoApp *app, const char *tag_text);
void eval_report_uncaught_throw(LogoApp *app);

// INT -- exposed alongside THROW/CATCH's own pieces for an unrelated
// reason: compiler.c's own REPEAT compilation needs this exact
// truncation for its count, matching do_repeat's own upfront
// `(int)eval_to_number(...)` (without it, REPEAT 3.5 [...] would run
// one extra iteration under a naive stack-based decrement-until-
// positive loop). Not a wholesale "port every math operator" batch --
// just this one, because REPEAT genuinely needs it.
EvalValue eval_int_value(EvalValue v);

// Shared list-building primitives, exposed for vm.c's own MAP/FILTER/
// REDUCE compiled-template loops (see docs/BYTECODE_VM_DESIGN.md's
// Progress log): the same list_pool bookkeeping do_map/do_filter/
// do_reduce themselves already use, not a parallel reimplementation.
EvalValue list_pool_exhausted(LogoApp *app);
EvalValue node_to_value(const ListNode *node);
int value_to_node(LogoApp *app, EvalValue v);

// Removes the global variable named `name` (case-insensitive), if one
// exists -- a swap-with-last removal, same pattern as REMOVEPROP's own
// property-list removal. Used by vm.c's own MAP/FILTER/REDUCE/FOREACH
// compiled-template loops to clean up their own internal placeholder
// variable(s) after the loop finishes, so a script that inspects
// NAMES afterward sees exactly what it would have with ast_eval's own
// text-substitution-based templates (which never touch the variable
// namespace at all). Only ever searches app->variables[] (globals) --
// the placeholder is never scope-local.
void eval_delete_var(LogoApp *app, const char *name);

// MAP/FILTER/REDUCE/FOREACH's own dynamic (runtime-computed-template)
// cores -- the same re-lex/re-parse-per-element machinery do_map/
// do_filter/do_reduce/do_foreach themselves now call, exposed for
// vm.c's own call_builtin dispatch to use as its fallback when
// compiler.c's own compile_template_call couldn't see the template at
// compile time (e.g. `MAP :tmpl [1 2 3]`, where the template is a
// computed value, not a literal `[...]`). The VM's own "compiled
// once" fast path (see docs/BYTECODE_VM_DESIGN.md's Progress log) is
// a completely separate mechanism in vm.c, not built on these.
EvalValue eval_map_value(LogoApp *app, EvalValue template_val, EvalValue list_arg);
EvalValue eval_filter_value(LogoApp *app, EvalValue template_val, EvalValue list_arg);
EvalValue eval_reduce_value(LogoApp *app, EvalValue template_val, EvalValue list_arg);
void eval_foreach_value(LogoApp *app, EvalValue template_val, EvalValue list_arg);

// ERASE -- a special form (like MAKE/LOCAL), not an ordinary builtin:
// its argument is a raw procedure name (ARG_QUOTED_WORD), never an
// evaluated expression, and it directly mutates `pool` (see eval.c's
// own comment on eval_erase_declare).
void eval_erase_declare(LogoApp *app, AstPool *pool, const char *name);

// PROCEDURES -- every currently-defined procedure's name (skipping any
// ERASEd/blank-text AST_PROC_DEF), as a list. Takes `pool` directly
// (like eval_erase_declare above) rather than an EvalValue argument --
// it has none, zero-arity, but still needs `pool` itself, unlike every
// other zero-arg builtin in this batch (do_who/do_penup/etc., which
// only need `app`).
EvalValue do_procedures(LogoApp *app, AstPool *pool);

// find_proc_def itself now lives in ast.h/ast.c (it never touched
// LogoApp, only AstPool -- see ast.h's own comment); eval.h re-exposes
// nothing extra for it since ast.h is already transitively included
// here and by every caller.

// The scope-push half of an ordinary procedure call: binds arg_vals as
// def's own parameters and pushes app->scopes/scope_depth, exactly as
// call_ast_procedure's own setup does (that function now just calls
// this). Returns 0 (recursion too deep, MAX_SCOPE_DEPTH already
// reached) or 1 (scope pushed). Exposed so vm.c's OP_CALL_PROC can
// reuse the exact same scope-binding logic -- see the design's own
// frame-layout decision (docs/BYTECODE_VM_DESIGN.md): the VM's value
// stack and call frames are new, VM-only state, but variable bindings
// still go through this same app->scopes[] mechanism unchanged, so a
// VM-compiled procedure's AST_VARREF/OP_PUSH_VAR reads and an
// interpreter-run procedure's variable reads can't drift.
int eval_push_scope_for_call(LogoApp *app, AstNode *def, EvalValue *arg_vals, int arg_count);

// Runs `pool`'s program (the AST_BLOCK at `program_node`, as produced
// by logo_parse) against `app`.
void ast_eval(LogoApp *app, AstPool *pool, int program_node);

// Same as ast_eval, but starting from an arbitrary node in a sibling
// chain (`start_node`, and everything reachable via its own
// `next_sibling` links) rather than always a whole program's first
// statement -- lets a caller that reruns a growing accumulated source
// from scratch each time (see tools/logo_new_cli.c's REPL) skip
// re-executing statements it already ran in an earlier pass, while
// still parsing (and so still resolving find_proc_def against) the
// full accumulated source. `-1` runs nothing, matching an empty block.
void ast_eval_from(LogoApp *app, AstPool *pool, int start_node);

// TEXT/SHOW/SAVE and general file I/O -- same eval_X_value-core split
// as every batch above, exposed for vm.c's call_builtin (added
// 2026-08-10, found never wired into the VM at all despite being in
// parser.c's own BUILTIN_SIGNATURES since Stage 1). LOAD is
// deliberately not here: it runs a loaded file's own top-level
// statements via exec_block, which needs its own dedicated opcode in
// the VM, not just a value-taking core -- see vm.c's own OP_LOAD.
EvalValue eval_text_value(LogoApp *app, AstPool *pool, EvalValue name_val);
void eval_show_value(LogoApp *app, AstPool *pool, EvalValue name_val);
void eval_save_value(LogoApp *app, AstPool *pool, EvalValue path_val);
void eval_deletefile_value(LogoApp *app, EvalValue path_val);
EvalValue eval_openread_value(LogoApp *app, EvalValue path_val);
EvalValue eval_openwrite_value(LogoApp *app, EvalValue path_val);
EvalValue eval_openappend_value(LogoApp *app, EvalValue path_val);
EvalValue eval_readline_value(LogoApp *app, EvalValue idx_val);
EvalValue eval_eofp_value(LogoApp *app, EvalValue idx_val);
EvalValue eval_directory_value(LogoApp *app);
void eval_close_value(LogoApp *app, EvalValue idx_val);
void eval_fileprint_value(LogoApp *app, EvalValue idx_val, EvalValue text_val);

#endif
