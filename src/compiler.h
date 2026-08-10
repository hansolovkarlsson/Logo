#ifndef LOGO_COMPILER_H
#define LOGO_COMPILER_H

// compiler.h
//
// Stage 2's compiler (see docs/BYTECODE_VM_DESIGN.md): turns Stage 1's
// already-real AST (ast.h, as built by parser.c) into bytecode.h's
// instruction format, for vm.c to execute. No dependency on
// GTK/GLib/interpreter.h -- same "a compiler is a pure structural
// transform" reasoning parser.c itself already follows, and what keeps
// this independently testable (tests/test_vm.c can compile without
// ever constructing a LogoApp).
//
// Started small (see docs/ROADMAP.md's own Phase 5 Stage 2 checklist,
// item 1): literals, arithmetic, PRINT, IF/WHILE, MAKE, and procedure
// calls with OUTPUT/STOP, enough to prove the compiler +
// explicit-frame-array VM mechanism actually works, shadow-diffed
// against ast_eval. Item 2 (grow instruction coverage) has since added
// lists/arrays/MAKE-adjacent ops (FIRST/BUTFIRST/LAST/BUTLAST/COUNT/
// EMPTY?/FPUT/LPUT/WORD/SENTENCE/SE/LIST, list literals, ARRAY/ITEM/
// SETITEM/FILLARRAY, THING/LOCAL/NAMES) -- compile_call below no
// longer recognizes builtins by an if-chain of names at all; it uses
// find_proc_def (ast.h) to tell a user procedure from a builtin, so
// growing coverage further only touches vm.c's own call_builtin
// dispatch and eval.c's value-taking cores, not this file (see
// compiler.c's own note). Still not full BUILTIN_SIGNATURES parity:
// property lists, THROW/CATCH, TELL, FOR, and MAP/FILTER/REDUCE/
// FOREACH's own templates remain, incremental follow-up work
// (checklist items 2-4), not part of getting the mechanism itself
// right.

#include "ast.h"
#include "bytecode.h"

// Compiles `pool`'s whole program (as produced by logo_parse) into
// `chunk`. Every top-level AST_PROC_DEF's own body is compiled first
// (each into its own instruction range, addresses recorded as they're
// compiled and backpatched into any call that referenced one before it
// was compiled -- see compiler.c's own file comment on why this needs
// backpatching, not a simpler two-pass split), then the top-level
// statements themselves, ending in OP_HALT. Returns the instruction
// index execution should actually start at (the top-level code's own
// first instruction, *not* index 0 -- procedure bodies are compiled
// first and come before it in `chunk`), or -1 if `chunk` ran out of
// room (MAX_INSTRUCTIONS) partway through.
int compile_program(AstPool *pool, int program_node, BytecodeChunk *chunk);

#endif
