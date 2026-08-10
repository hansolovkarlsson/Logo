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
// can share it too, instead of a parallel reimplementation -- see
// compiler.c's own note on why PRINT is the one builtin this vertical
// slice recognizes by name.
void eval_print_value(LogoApp *app, EvalValue v);

// Finds the AST_PROC_DEF node named `name` in `pool` (a linear scan --
// same cost/shape as interpreter.c's own find_procedure), or -1 if
// there isn't one. Exposed (was eval.c-private) so vm.c's own
// OP_CALL_PROC handler can look up a called procedure's parameter
// names/count directly, the same way call_ast_procedure itself does,
// rather than the compiler encoding that information into bytecode.h's
// instruction format redundantly.
int find_proc_def(AstPool *pool, const char *name);

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

#endif
