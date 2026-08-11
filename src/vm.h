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
// only outcome possible until WAIT/WAITKEY/INPUT/PAUSE/ANIMATESPRITE.
// The SUSPENDED values mean vm_run returned *early*, mid-chunk, with
// `vm` left fully intact (its stack/frames untouched) so it can be
// resumed later via vm_resume/vm_resume_with_key/vm_resume_with_input/
// vm_resume_animatesprite -- see those functions' own comments, and
// bytecode.h's own OP_WAIT/OP_WAITKEY/OP_INPUT/OP_PAUSE/
// OP_ANIMATESPRITE/OP_LAUNCH/OP_AWAIT/OP_YIELD comments for why only
// these eight opcodes can ever produce a SUSPENDED result.
// VM_RUN_SUSPENDED_PAUSE resumes via the plain vm_resume (like
// VM_RUN_SUSPENDED_WAIT) -- PAUSE produces no value, same as WAIT, so
// no dedicated vm_resume_with_* function is needed for it.
// VM_RUN_SUSPENDED_ANIMATESPRITE is the one case that can be returned
// *repeatedly* for a single ANIMATESPRITE call (once per remaining
// frame) before finally falling through to a real vm_run continuation
// -- see vm_resume_animatesprite's own comment. VM_RUN_SUSPENDED_LAUNCH/
// AWAIT/YIELD are all void, same as WAIT/PAUSE -- resumed via the plain
// vm_resume too, but by `agent.c`'s own scheduler (see
// docs/CONCURRENT_AGENTS_DESIGN.md), never by ui.c's timer/keypress
// callbacks the way WAIT/WAITKEY/INPUT/PAUSE are: this first slice of
// concurrent agents never needs a real GTK timer or keypress at all,
// since every one of these three is resolved by the scheduler itself,
// synchronously, on its own next loop iteration.
// VM_RUN_SUSPENDED_MOTION_DELAY (SETSPEED, OP_MOTION_DELAY) resumes via
// the plain vm_resume like WAIT/PAUSE, sharing WAIT's own real-timer
// mechanism in ui.c (arms the same on_wait_timeout off suspend_seconds)
// -- but agent.c's scheduler treats it like YIELD, not like WAIT: see
// OP_MOTION_DELAY's own bytecode.h comment for why.
typedef enum {
    VM_RUN_HALTED,
    VM_RUN_SUSPENDED_WAIT,
    VM_RUN_SUSPENDED_WAITKEY,
    VM_RUN_SUSPENDED_INPUT,
    VM_RUN_SUSPENDED_PAUSE,
    VM_RUN_SUSPENDED_ANIMATESPRITE,
    VM_RUN_SUSPENDED_LAUNCH,
    VM_RUN_SUSPENDED_AWAIT,
    VM_RUN_SUSPENDED_YIELD,
    VM_RUN_SUSPENDED_MOTION_DELAY,
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
    // `suspend_seconds` is valid after VM_RUN_SUSPENDED_WAIT (OP_WAIT's
    // own already-evaluated, already-truncated-to-"was it > 0" argument)
    // and is REUSED after VM_RUN_SUSPENDED_ANIMATESPRITE (the per-frame
    // delay -- the same number for every one of that call's own
    // suspends, so one field suffices) -- the caller (ui.c) decides how
    // to actually wait that long; vm.c itself never touches a clock or
    // GTK. `pause_level` is valid only after VM_RUN_SUSPENDED_PAUSE --
    // OP_PAUSE's own already-incremented app->pause_depth, captured
    // once at suspend time; ui.c reads it to know which level on its
    // own pause stack this particular suspended run is waiting for
    // (CONTINUE/CO only ever decrement app->pause_depth by one, so at
    // most the single innermost -- highest-level -- paused run ever
    // becomes eligible to resume per CONTINUE, mirroring
    // interpreter.c's own do_pause loop condition exactly: it keeps
    // waiting while pause_depth >= my_level). `suspend_frames_remaining`
    // is valid only after VM_RUN_SUSPENDED_ANIMATESPRITE -- how many
    // MORE frame advances are still owed after the wait this particular
    // suspend represents elapses; see vm_resume_animatesprite.
    // `launch_target_pc` is valid only after VM_RUN_SUSPENDED_LAUNCH --
    // OP_LAUNCH's own already-resolved target procedure's compiled
    // start pc (via bytecode_find_proc, the same lookup OP_APPLY/OP_SEND
    // already do), for agent.c's own scheduler to start a fresh Agent's
    // own Vm at (see docs/CONCURRENT_AGENTS_DESIGN.md).
    int pc;
    double suspend_seconds;
    int pause_level;
    int suspend_frames_remaining;
    int launch_target_pc;

    // How many nested vm_run calls are currently on the C stack for
    // this one Vm -- 1 for an ordinary top-level run, >1 inside a
    // MAP/FILTER/REDUCE/FOREACH template's own recursive vm_run call
    // (see exec_map_compiled and friends) OR inside RUN/LOAD's own
    // (see exec_run/exec_load -- a freshly compiled, independent
    // scratch chunk, unlike templates' shared one, but the same
    // recursive-vm_run shape). WAIT/WAITKEY/INPUT/PAUSE/ANIMATESPRITE/
    // LAUNCH/AWAIT/YIELD all check this because a SUSPENDED return from
    // an inner, recursive vm_run call would only unwind that one C
    // frame -- straight back into whichever C loop/function made the
    // recursive call, not out to whatever's driving the outermost
    // vm_run (ui.c, or agent.c's own scheduler) -- silently losing the
    // suspend instead of delivering it. Rather than attempt that (a
    // real redesign, not a small fix -- see docs/BYTECODE_VM_DESIGN.md),
    // all eight refuse outright with a clear runtime message whenever
    // vm_run_depth > 1, the same "documented gap, not silent
    // corruption" spirit as the template batch's own frame_floor
    // mitigation for OUTPUT/STOP (which exec_run/exec_load also both
    // use, for the same underlying reason -- see their own comments).
    // OP_MOTION_DELAY checks it too, but silently no-ops instead of
    // refusing -- see its own bytecode.h comment for why an automatic
    // per-step throttle doesn't deserve the same reported refusal an
    // explicit WAIT-like call does.
    int vm_run_depth;
} Vm;

// Runs `chunk` (as produced by compile_program) against `app`/`pool`
// starting at instruction `start_pc`, until OP_HALT (VM_RUN_HALTED) or
// a WAIT/WAITKEY/INPUT/PAUSE/ANIMATESPRITE suspend point
// (VM_RUN_SUSPENDED_*, see VmRunResult). `vm` must already be zeroed (a
// fresh `Vm vm = {0};` on the caller's own heap allocation) for a first
// call -- same "caller owns storage, callee just uses it" convention as
// ast_eval's own AstPool/LogoApp parameters. On a SUSPENDED result,
// `vm` (and `pool`/`chunk`) must be kept alive by the caller and handed
// to vm_resume/vm_resume_with_key/vm_resume_with_input/
// vm_resume_animatesprite later -- do not call vm_run again directly on
// a suspended `vm`.
VmRunResult vm_run(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, int start_pc);

// Continues a `vm` most recently suspended with VM_RUN_SUSPENDED_WAIT
// (i.e. by OP_WAIT) or VM_RUN_SUSPENDED_PAUSE (i.e. by OP_PAUSE),
// picking up exactly at `vm->pc`. Use this once whatever the caller
// waited for (a timer, or app->pause_depth dropping low enough) has
// happened -- both WAIT and PAUSE produce no value, so neither needs a
// dedicated vm_resume_with_* variant the way WAITKEY/INPUT do.
VmRunResult vm_resume(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk);

// Continues a `vm` most recently suspended with VM_RUN_SUSPENDED_WAITKEY
// (i.e. by OP_WAITKEY), pushing `key_name` as the value that instruction
// itself would have produced, then picking up at `vm->pc`.
VmRunResult vm_resume_with_key(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const char *key_name);

// Continues a `vm` most recently suspended with VM_RUN_SUSPENDED_INPUT
// (i.e. by OP_INPUT), pushing `line` (the whole submitted entry-box
// line) as the value that instruction itself would have produced, then
// picking up at `vm->pc`. Mechanically identical to
// vm_resume_with_key's own push-then-continue shape, but kept as its
// own function (not a shared helper) since INPUT/WAITKEY are gated by
// different ui.c flags (waiting_for_input vs. waiting_for_key) and are
// conceptually distinct resumption events, matching this codebase's own
// convention of dedicated functions per concept over one parameterized
// one (e.g. eval_first_value/eval_last_value).
VmRunResult vm_resume_with_input(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const char *line);

// Continues a `vm` most recently suspended with
// VM_RUN_SUSPENDED_ANIMATESPRITE (i.e. by OP_ANIMATESPRITE, or by a
// previous call to this same function). Unlike every other vm_resume*
// function, this one does NOT necessarily re-enter vm_run at all: if
// `vm->suspend_frames_remaining` is still > 0, it just advances one
// more sprite frame and returns VM_RUN_SUSPENDED_ANIMATESPRITE again
// (with `suspend_seconds` unchanged, so the caller re-arms the same
// per-frame delay) -- only once frames are exhausted does it finally
// call vm_run(vm->pc) to continue real bytecode execution. Use this
// once the per-frame delay the previous suspend asked for has elapsed;
// the caller doesn't need to know or care how many frames remain, just
// keep calling this each time its own timer fires until it returns
// something other than VM_RUN_SUSPENDED_ANIMATESPRITE.
VmRunResult vm_resume_animatesprite(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk);

#endif
