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

// vm_run's own result: VM_RUN_HALTED means it ran to OP_HALT (or fell
// off the end of the chunk) exactly like before this existed -- the
// only outcome possible until WAIT/WAITKEY. The two SUSPENDED values
// mean vm_run returned *early*, mid-chunk, with `vm` left fully intact
// (its stack/frames untouched) so it can be resumed later via
// vm_resume/vm_resume_with_key -- see those functions' own comments,
// and bytecode.h's own OP_WAIT/OP_WAITKEY comment for why only these
// two opcodes can ever produce a SUSPENDED result.
typedef enum {
    VM_RUN_HALTED,
    VM_RUN_SUSPENDED_WAIT,
    VM_RUN_SUSPENDED_WAITKEY,
} VmRunResult;

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

    // The other half of that same "resolved && !produced" check --
    // whether the most recent call resolved to something real at all.
    // 0 only for a call to a genuinely unknown procedure (matching
    // do_user_procedure_call's own *resolved=0, which exists
    // specifically so its own "I don't know how to X" message isn't
    // *also* followed by the generic "X: didn't output a value"
    // wrapper). Everything else -- an ordinary builtin, a successful
    // procedure call, and even the recursion-too-deep case -- leaves
    // this 1: do_user_procedure_call itself never touches *resolved on
    // the MAX_SCOPE_DEPTH path, so a recursion-too-deep call used in
    // expression position genuinely does print both "Recursion too
    // deep, call ignored" and "X: didn't output a value" in ast_eval --
    // a real double message this VM has to reproduce, not avoid.
    int last_call_resolved;

    // SEND's own message name, valid only right after OP_SEND runs
    // (set on every one of its own paths, success or failure) and
    // read by the OP_CHECK_SEND_OUTPUT that always immediately follows
    // it in expression position. Unlike an ordinary call's own
    // OP_CHECK_OUTPUT (which reads its own .text, a compile-time
    // constant), SEND's target message is only known at runtime, so
    // there's no instruction field compile_call could have baked it
    // into -- this is that value's only home between the two
    // instructions (and, for a successful call, across however many
    // instructions the resolved procedure's own body needs to run
    // before finally returning).
    char last_send_message[INSTR_MAX_TEXT];

    // Suspend/resume state (see VmRunResult above). `pc` is valid only
    // right after vm_run returns a SUSPENDED result -- it's where
    // vm_resume/vm_resume_with_key continue from, replacing the plain
    // C-local `pc` every other opcode uses, since suspending means
    // *returning out of vm_run entirely*, so nothing else can hold it.
    // `suspend_seconds` is valid only after VM_RUN_SUSPENDED_WAIT
    // (OP_WAIT's own already-evaluated, already-truncated-to-"was it >
    // 0" argument) -- the caller (ui.c) decides how to actually wait
    // that long; vm.c itself never touches a clock or GTK.
    int pc;
    double suspend_seconds;

    // How many nested vm_run calls are currently on the C stack for
    // this one Vm -- 1 for an ordinary top-level run, >1 only inside a
    // MAP/FILTER/REDUCE/FOREACH template's own recursive vm_run call
    // (see exec_map_compiled and friends). WAIT/WAITKEY check this
    // because a SUSPENDED return from an inner, recursive vm_run call
    // would only unwind that one C frame -- straight back into
    // exec_map_compiled's own C loop, not out to whatever's driving the
    // outermost vm_run (ui.c) -- silently losing the suspend instead of
    // delivering it. Rather than attempt that (a real redesign, not a
    // small fix -- see docs/BYTECODE_VM_DESIGN.md), WAIT/WAITKEY refuse
    // outright with a clear runtime message whenever vm_run_depth > 1,
    // the same "documented gap, not silent corruption" spirit as the
    // template batch's own frame_floor mitigation for OUTPUT/STOP.
    int vm_run_depth;
} Vm;

// Runs `chunk` (as produced by compile_program) against `app`/`pool`
// starting at instruction `start_pc`, until OP_HALT (VM_RUN_HALTED) or
// a WAIT/WAITKEY suspend point (VM_RUN_SUSPENDED_*, see VmRunResult).
// `vm` must already be zeroed (a fresh `Vm vm = {0};` on the caller's
// own heap allocation) for a first call -- same "caller owns storage,
// callee just uses it" convention as ast_eval's own AstPool/LogoApp
// parameters. On a SUSPENDED result, `vm` (and `pool`/`chunk`) must be
// kept alive by the caller and handed to vm_resume/vm_resume_with_key
// later -- do not call vm_run again directly on a suspended `vm`.
VmRunResult vm_run(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, int start_pc);

// Continues a `vm` most recently suspended with VM_RUN_SUSPENDED_WAIT
// (i.e. by OP_WAIT), picking up exactly at `vm->pc`. Use this once
// whatever the caller waited for (a timer) has elapsed.
VmRunResult vm_resume(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk);

// Continues a `vm` most recently suspended with VM_RUN_SUSPENDED_WAITKEY
// (i.e. by OP_WAITKEY), pushing `key_name` as the value that instruction
// itself would have produced, then picking up at `vm->pc`.
VmRunResult vm_resume_with_key(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const char *key_name);

#endif
