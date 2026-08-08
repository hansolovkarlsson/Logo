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

// Runs `pool`'s program (the AST_BLOCK at `program_node`, as produced
// by logo_parse) against `app`.
void ast_eval(LogoApp *app, AstPool *pool, int program_node);

#endif
