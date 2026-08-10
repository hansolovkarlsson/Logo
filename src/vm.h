#ifndef LOGO_VM_H
#define LOGO_VM_H

// vm.h
//
// Stage 2's bytecode VM (see docs/BYTECODE_VM_DESIGN.md): runs a
// BytecodeChunk (as produced by compiler.c) against a real LogoApp.
// Unlike bytecode.h/compiler.h, this deliberately DOES depend on
// interpreter.h/eval.h -- same reasoning as eval.c's own precedent
// (see eval.h's own comment): it shares LogoApp's turtle/variable/
// output state and EvalValue's own value semantics directly, rather
// than reimplementing either.
//
// Frame-layout decision (see docs/BYTECODE_VM_DESIGN.md's Progress
// log): the value stack and call frames below are new, VM-only state,
// but variable *bindings* still go through app->scopes[]/scope_depth
// unchanged (via eval_push_scope_for_call) -- so real recursion depth
// is still capped at MAX_SCOPE_DEPTH (200) for this vertical slice, not
// yet VM-owned/decoupled. A deliberate follow-up, not an oversight.

#include "ast.h"
#include "bytecode.h"
#include "interpreter.h"
#include "eval.h"

#define MAX_VM_STACK 4096
#define MAX_VM_FRAMES 256

// One in-flight OP_CALL_PROC: where to resume in `code` when this
// procedure returns (return_pc), and this call's own value-stack
// base (value_stack_base -- OP_OUTPUT/OP_STOP/a HALT-by-underflow all
// know to leave exactly one value at that base and truncate stack_top
// back to base+1). Deliberately doesn't duplicate app->scope_depth
// here: a frame's own scope is always app->scope_depth-1 by
// construction (each OP_CALL_PROC pushes exactly one of each, in
// lockstep), so popping a frame and decrementing scope_depth are the
// same event, not two things that could drift apart.
typedef struct {
    int return_pc;
    int value_stack_base;
} VmFrame;

// Heap-only, like BytecodeChunk/LogoApp/AstPool/ParseResult -- stack[]
// alone is MAX_VM_STACK * sizeof(EvalValue), and EvalValue is ~528
// bytes (dominated by its own word[512]), so this struct is multiple
// MB. Never a stack local or pass/return-by-value.
typedef struct {
    EvalValue stack[MAX_VM_STACK];
    int stack_top; // next free slot; stack[0..stack_top-1] are live

    VmFrame frames[MAX_VM_FRAMES];
    int frame_count;

    // Whether the most recently completed OP_CALL_PROC actually
    // OUTPUT'd a value (1) or fell through to OP_STOP/failed outright
    // -- recursion too deep, unknown procedure, frame-stack overflow
    // (0). Read by the OP_CHECK_OUTPUT that always immediately follows
    // an expression-position call, mirroring eval_expr's own AST_CALL
    // case ("resolved && !produced" -> the "didn't output a value"
    // diagnostic). A single scalar, not per-frame state: nothing else
    // can run between a call's own return and its OP_CHECK_OUTPUT, so
    // there's never more than one pending answer to track.
    int last_call_produced_output;
} Vm;

// Runs `chunk` (as produced by compile_program) against `app`/`pool`
// starting at instruction `start_pc`, until OP_HALT. `vm` must already
// be zeroed (a fresh `Vm vm = {0};` on the caller's own heap
// allocation) -- same "caller owns storage, callee just uses it"
// convention as ast_eval's own AstPool/LogoApp parameters.
void vm_run(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, int start_pc);

#endif
