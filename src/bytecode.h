#ifndef LOGO_BYTECODE_H
#define LOGO_BYTECODE_H

// bytecode.h
//
// Stage 2's instruction format (see docs/BYTECODE_VM_DESIGN.md): what
// compiler.c produces from Stage 1's AST and vm.c executes. No
// dependency on GTK/GLib/interpreter.h -- same "an X is just data"
// reasoning as ast.h/lexer.h/parser.h, and what keeps this
// independently testable before any VM exists to run it.
//
// Deliberately an array of tagged structs, not packed bytes the way a
// "real" bytecode format usually is: none of this project's own four
// motivations for a VM (see docs/BYTECODE_VM_DESIGN.md's "Why")
// depend on compact encoding, and a flat struct matches AstNode's own
// established "some unused space per node, in exchange for simplicity"
// convention exactly. Every call (builtin or user procedure) always
// leaves exactly one value on vm.c's own value stack -- mirroring
// eval.c's own exec_call, whose *result already defaults to num_val(0)
// unless a real operator overwrites it -- so OP_POP is what a
// statement-position call compiles to afterward, not a special case.
//
// Conditions are ordinary values, not a separate sub-language: this
// language's own AND/OR never short-circuit (confirmed directly in
// eval.c's eval_condition -- both sides are always evaluated), so
// OP_AND/OP_OR/OP_NOT/the OP_CMP_* family are plain pop-N-push-1
// operators pushing num_val(0)/num_val(1), consumed by
// OP_JUMP_IF_FALSE via the same eval_is_truthy every other truthiness
// check in this codebase already uses. No fused compare-and-jump
// instructions needed.

#include "ast.h" // AST_MAX_TEXT -- the one size word_literals[] below needs to match
#include <stdio.h> // FILE -- bytecode_disassemble's own output sink

typedef enum {
    OP_PUSH_NUMBER,   // .number -> push num_val(.number)
    OP_PUSH_WORD,     // .a -> index into chunk->word_literals[]; push word_val(...)
    OP_PUSH_VAR,      // .text (variable name) -> push its current value (num_val(0) if unbound, matching eval_expr's own AST_VARREF default)
    OP_SET_VAR,       // .text (variable name); pop 1 -> MAKE-equivalent assignment
    OP_POP,           // discard the top of the value stack

    OP_ADD, OP_SUB, OP_MUL, OP_DIV,   // pop 2, push 1
    OP_NEG,                             // pop 1, push 1

    OP_CMP_LT, OP_CMP_GT, OP_CMP_EQ, OP_CMP_LE, OP_CMP_GE, OP_CMP_NE, // pop 2, push num_val(0/1)
    OP_NOT,                                                             // pop 1, push num_val(0/1)
    OP_AND, OP_OR,                                                     // pop 2, push num_val(0/1) -- no short-circuiting, see the file comment above

    OP_JUMP,           // .a = target instruction index
    OP_JUMP_IF_FALSE,  // pop 1 (via eval_is_truthy); .a = target instruction index if falsy

    OP_CALL_BUILTIN,   // .text = builtin name, .a = argc; pops .a values (in argument order), pushes exactly 1 result (a real value, or a dummy num_val(0) for a void builtin like SETITEM -- vm.c's own call_builtin dispatch tracks which per call, for OP_CHECK_OUTPUT's sake)
    OP_CALL_PROC,      // .a = target pc (this procedure's first instruction, resolved by compiler.c's own backpatching -- see its file comment), .b = argc, .text = procedure name (for OP_CHECK_OUTPUT's own error message); pops .a args, pushes a new VM frame, pushes exactly 1 result once the call returns
    OP_CHECK_OUTPUT,   // .text = the call's own name (builtin or procedure); only ever emitted right after an OP_CALL_BUILTIN/OP_CALL_PROC/OP_VOID_RESULT used in *expression* position -- if that call didn't actually produce a value (vm.c's own last_call_produced_output flag), reports "<name>: didn't output a value" and replaces the top of the value stack with word_val(""), mirroring eval.c's own exec_call/eval_expr wrapper exactly. A statement-position call gets a plain OP_POP instead, never this.

    OP_OUTPUT,   // pop 1 -> return from the current procedure with that value (this call's own OP_CHECK_OUTPUT, if any, sees a real output)
    OP_STOP,     // return from the current procedure with num_val(0) and no real output -- also what the compiler appends after every procedure body's own last statement, so falling off the end behaves exactly like interpreter.c's own call_procedure/eval.c's own call_ast_procedure defaulting to "no OUTPUT was ever called"

    OP_HALT,     // stop the whole run (the very last instruction of the top-level program)

    OP_PUSH_LIST_LITERAL, // .a = root index into chunk->list_lit_nodes[] (grafted from the AST_LIST_LITERAL subtree at compile time -- see ListLitNode's own comment); pushes eval_build_list_literal_from_chunk(app, chunk, .a), built fresh each time this instruction runs, same as eval_expr's own AST_LIST_LITERAL case builds it fresh on every visit, not once.

    OP_LOCAL,        // .text = variable name; declares a same-named scope-local variable in the current call (0 if it's genuinely new, otherwise a no-op) -- no stack effect of its own; compile_call always follows this with OP_VOID_RESULT, same as OP_SET_VAR (MAKE)
    OP_ERASE,        // .text = procedure name; blanks that AST_PROC_DEF's own name so it can never be called/listed again (eval_erase_declare) -- same ".text carries a raw name, not an evaluated expression" shape as OP_LOCAL, and the same reason: ERASE's argument is ARG_QUOTED_WORD, never compiled as an ordinary expression
    OP_VOID_RESULT,  // pushes num_val(0) and clears last_call_produced_output -- the special-form equivalent of a void builtin's OP_CALL_BUILTIN return, for MAKE/LOCAL/ERASE/WHILE (constructs with their own dedicated opcodes/compiled shape that still need to honor the "every call leaves exactly one value, and OP_CHECK_OUTPUT can tell whether it was a real one" convention every ordinary call follows)

    OP_SEND,               // pop 3 (obj, message, arglist, in that order); resolves `message` through obj's prototype chain at RUNTIME (unlike every other call, the callee isn't known at compile time -- see vm.c's own exec_send), pushing a VM frame + jumping into the resolved procedure's own compiled body on success, or word_val("") with vm's own last_call_resolved cleared on any of SEND's own resolution/arity failures (each already prints its own specific message, mirroring do_send exactly -- never falls through to OP_CHECK_SEND_OUTPUT's generic one). No operands of its own: all 3 arguments were already compiled as ordinary expressions and are just popped.
    OP_CHECK_SEND_OUTPUT,  // only ever emitted right after OP_SEND used in expression position -- same job as OP_CHECK_OUTPUT, but reads the message name to report from vm->last_send_message (a runtime value OP_SEND itself just resolved) rather than from a compile-time .text field, since compile_call can't know SEND's own callee name ahead of time the way it can for an ordinary call

    // APPLY "name arglist -- pop 2 (name, arglist, in that order);
    // resolves `name` via find_proc_def (a plain, non-prototype-chain
    // lookup, unlike SEND's own eval_resolve_method) and, on success,
    // pushes a VM frame + jumps into the resolved procedure's own
    // compiled body exactly like OP_SEND's own success path. Every
    // failure path (unknown procedure, wrong arity, recursion too
    // deep) pushes a throwaway num_val(0) and advances past this
    // instruction instead of jumping, mirroring do_apply's own exact
    // messages -- unlike SEND, APPLY never forwards a resolved value:
    // interpreter.c's own APPLY documented as never handing back a
    // value at all (matching RUN), regardless of whether the applied
    // procedure itself called OUTPUT. OP_VOID_DISCARD, always emitted
    // immediately after (both on the eager-failure pc+1 landing spot
    // and as the resolved procedure's own frame->return_pc), is what
    // actually enforces that: it discards whatever's on top of the
    // stack (APPLY's own throwaway value, or the applied procedure's
    // real OUTPUT/STOP result) and replaces it with an ordinary void
    // result, the same "every call leaves exactly one value" contract
    // OP_VOID_RESULT keeps for MAKE/LOCAL/etc -- just POPping first,
    // since (unlike those) something is already guaranteed to be there.
    OP_APPLY,
    OP_VOID_DISCARD,

    // RUN thing / LOAD "path -- both re-lex/parse/compile a whole
    // statement sequence (not a single expression, unlike a template's
    // own body) into a genuinely fresh, independent BytecodeChunk, then
    // run it via a RECURSIVE vm_run call sharing this same Vm's own
    // stack/frames -- see vm.c's own exec_run/exec_load. Neither ever
    // produces a value, matching interpreter.c's own documented
    // behavior for both, so each is always followed by OP_VOID_RESULT.
    // OP_RUN pops its own already-evaluated argument; OP_LOAD's own
    // path is a compile-time-known literal (.text, same ARG_QUOTED_WORD
    // shape as OP_ERASE's own procedure name) -- LOAD's grammar can
    // only ever take a literal path, never a computed one. Neither
    // needs its own vm_run_depth guard the way WAIT/WAITKEY/INPUT/
    // PAUSE/ANIMATESPRITE do (RUN/LOAD themselves never suspend) -- but
    // a suspend point reached *inside* the RUN'd/LOADed code is still
    // correctly refused by those five opcodes' own existing checks,
    // since this recursive vm_run call increments vm_run_depth same as
    // a template's own does.
    OP_RUN,
    OP_LOAD,

    // THROW/CATCH's own cooperative unwind (see vm.c's own file comment
    // and compiler.c's own compile_block for the full mechanism): THROW
    // itself is an ordinary OP_CALL_BUILTIN (just sets
    // app->throw_requested/throw_tag, exactly like do_throw -- no
    // opcode of its own needed). Every OTHER opcode below exists only
    // to propagate that flag correctly once set, mirroring
    // ast_eval's own exec_block/do_while's cooperative
    // "if (stop_requested || throw_requested) break" checks --
    // reimplemented here as compile-time-inserted forward jumps instead
    // of a runtime recursive break, since this VM has no C call stack
    // to unwind through the way the tree-walker does.
    OP_CHECK_THROW,          // .a = jump target; if app->throw_requested, jump there (skipping every remaining statement in the CURRENT block only -- compile_block emits this after every statement, patching .a to that block's own end) -- otherwise fall through to the next instruction as normal. No stack effect.
    OP_CATCH_CHECK,          // pop 1 (the tag expression's own value, pushed by compile_call's own CATCH branch BEFORE compiling the block -- left sitting on the value stack, untouched, for the block's own entire duration, since compile_block is always stack-neutral overall; NOT stashed in a VM-level scratch field, since CATCH can nest and a nested CATCH's own tag would clobber an outer one's there); converts it to text and, if app->throw_requested and its own tag matches (case-insensitively, mirroring do_catch exactly), clears app->throw_requested -- absorbing the throw right here. A non-matching or absent throw is left completely untouched, so it keeps propagating via the OP_CHECK_THROW checks in whatever block encloses this CATCH statement. No jump of its own (CATCH's own block already handled its own internal propagation via its own OP_CHECK_THROW instructions before this ever runs).
    OP_CHECK_UNCAUGHT_THROW, // top-level-only (see compile_program's own top-level compilation, which uses this instead of OP_CHECK_THROW after each top-level statement): if app->throw_requested, reports "THROW: no CATCH found for <tag>" and clears the flag -- mirroring ast_eval_from's own per-top-level-statement recovery exactly, including that execution then CONTINUES to the next top-level statement rather than skipping the rest of the script (unlike an ordinary nested block, which skips to its own end on an uncaught throw). Always falls through; never jumps.

    OP_PEEK, // .a = depth below the current top (0 = the top itself); pushes a COPY of the value at that depth, leaving the original completely untouched. Used by REPEAT/FOREVER/FOR (see compiler.c's own compile_call/compile_for) to read a persistent loop-control value -- a remaining count, an iteration counter, a FOR loop's own limit/step/own internal counter -- that has to survive across however many transient values get pushed/popped computing each iteration's own condition, AND across a *recursive* call within the loop body itself. That second requirement is why these live on the value stack at all rather than in a hidden Logo variable: a hidden variable is genuinely global and would collide across nested/recursive invocations of the very same compiled loop (confirmed by direct analogy to CATCH's own tag-clobbering bug, see the THROW/CATCH batch's own notes), whereas a value's own fixed stack position is naturally protected once a recursive call pushes its own VmFrame -- that frame's own value_stack_base always sits strictly above this position, the same way exec_for's own plain C-local `limit`/`step` doubles are naturally protected by the C call stack itself.
    OP_POKE, // .a = depth below the top the value should end up at, AFTER popping; pops 1 value and overwrites the persistent slot at that (post-pop) depth with it -- the write-back half of OP_PEEK, used by FOR's own per-iteration counter update (see compiler.c's own compile_for). FOR needs this one specifically, unlike REPEAT/FOREVER's own single counter: FOR's own internal loop counter sits *underneath* its own persistent limit/step on the stack (not on top), so a plain "push 1; OP_ADD" can't update it in place the way it can for a lone top-of-stack counter -- confirmed necessary the hard way: an earlier version of FOR instead re-read the loop variable back from its own Logo-visible variable for control purposes, which (unlike exec_for's own genuinely separate C-local `i`) is global and got clobbered by a recursive call's own FOR loop over the same variable name, caught by test_vm.c's own recursion test.

    // MAP/FILTER/REDUCE/FOREACH's own "compiled once" fast path (see
    // docs/BYTECODE_VM_DESIGN.md's Progress log and compiler.c's own
    // compile_template_call) -- only used when the template argument is
    // a literal `[...]` visible at compile time; a runtime-computed
    // template (`MAP :tmpl [1 2 3]`) instead compiles as an ordinary
    // OP_CALL_BUILTIN, falling back to vm.c's own re-lex/re-parse-per-
    // element eval_map_value/eval_filter_value/eval_reduce_value/
    // eval_foreach_value (the same machinery do_map/do_filter/do_reduce/
    // do_foreach themselves call). For the fast path, the template's own
    // expression/condition/statement-block is compiled exactly ONCE,
    // inline in the same chunk (so any OP_CALL_PROC inside it resolves
    // against the program's own real procedures), ending in OP_HALT and
    // reached only via a RECURSIVE vm_run call, once per element -- see
    // vm.c's own exec_map_compiled and friends. `.a` = that compiled
    // template's own start pc; `.text` = the internal placeholder
    // variable's own compiler-generated, per-call-site-unique base name
    // (e.g. "__tmpl3__") that the template's own compiled code reads via
    // ordinary OP_PUSH_VAR (its "?"/"?1"/"?2" references were rewritten
    // to real `:name` varrefs at compile time) and that these opcodes'
    // own runtime handlers bind via ordinary set_var before each
    // element, then remove via eval_delete_var once the whole loop
    // finishes (so a script inspecting NAMES afterward sees nothing
    // extra, matching ast_eval's own template mechanism, which never
    // touches the variable namespace at all). All four pop 1 value (the
    // list argument, already evaluated as an ordinary expression).
    OP_MAP_COMPILED,     // collects the template's own result (an expression) for each element into a new list; pushes it
    OP_FILTER_COMPILED,  // keeps each element whose template (a condition) is truthy, copied as-is, into a new list; pushes it
    OP_REDUCE_COMPILED,  // folds left-to-right (".text" is the accumulator's own name; ".text" with "2" appended is the current element's) via the template (an expression); pushes the final accumulator, or num_val(0) if the list was empty
    OP_FOREACH_COMPILED, // runs the template (a statement block) once per element for side effects only, stopping early on OUTPUT/STOP/THROW exactly like do_foreach; pushes num_val(0) (void, matching do_foreach)

    // WAIT/WAITKEY/INPUT/PAUSE -- the real suspend points this VM has
    // (see docs/BYTECODE_VM_DESIGN.md's suspend/resume design). Unlike
    // every opcode above, these can make vm_run *return early*,
    // mid-chunk, handing control back to whatever's driving it (ui.c's
    // GTK event loop) instead of running to OP_HALT/completion --
    // vm_run's own return type reflects this (see vm.h's VmRunResult).
    // All four pop nothing themselves at the point they suspend except
    // what their own argument-compiling already pushed (OP_WAIT's lone
    // numeric arg, popped by its own handler before it decides whether
    // to suspend at all); the *value* a suspended call eventually
    // produces (or doesn't) is supplied by whichever vm_resume*
    // function resumes it, not by this opcode's own handler.
    OP_WAIT,    // pops the already-evaluated seconds argument; if > 0, suspends (VM_RUN_SUSPENDED_WAIT, vm->suspend_seconds set) with vm->pc pointing at the OP_VOID_RESULT that always immediately follows (WAIT never itself produces a value); if <= 0, falls through to it immediately, same as interpreter.c's own `if (seconds > 0)` guard
    OP_WAITKEY, // suspends unconditionally (VM_RUN_SUSPENDED_WAITKEY); the resuming vm_resume_with_key call pushes the pressed key's name as the value this instruction itself would otherwise have pushed, then continues at vm->pc
    OP_INPUT,   // suspends unconditionally (VM_RUN_SUSPENDED_INPUT); the resuming vm_resume_with_input call pushes the whole submitted line as the value this instruction itself would otherwise have pushed, then continues at vm->pc -- same shape as OP_WAITKEY, just gated by a different ui.c flag/event (Return-submits-the-buffer, not any keypress)
    OP_PAUSE,   // increments the shared app->pause_depth, captures the result as vm->pause_level, prints interpreter.c's own "Paused (level N)..." message, then suspends unconditionally (VM_RUN_SUSPENDED_PAUSE) with vm->pc pointing at the OP_VOID_RESULT that always immediately follows (PAUSE never itself produces a value, same as WAIT) -- resumed via the plain vm_resume, not a dedicated vm_resume_with_* function, once whatever's driving the VM (ui.c) decides app->pause_depth has dropped low enough (see CONTINUE/CO, ordinary builtins that just decrement it -- no opcode of their own needed)

    // OP_ANIMATESPRITE -- the one member of this group that can suspend
    // MORE THAN ONCE per call (once per remaining frame), unlike every
    // opcode above. Pops frames then delay (compiled in that push
    // order). If there's no sprite set, or frames <= 0, it's a
    // synchronous no-op/error, same as interpreter.c's own guard. If
    // delay <= 0, the ENTIRE frame loop runs synchronously with no
    // suspend at all -- a faithful port, not a fix: interpreter.c's own
    // loop only actually yields to GTK's main loop (making intermediate
    // frames visible) when its own busy-wait runs, which only happens
    // when delay > 0, so a delay <= 0 animation was never really
    // "animated" from the user's own visual perspective there either,
    // just an instant jump to the final frame. Otherwise: advances the
    // first frame, then suspends (VM_RUN_SUSPENDED_ANIMATESPRITE,
    // vm->suspend_seconds = delay, vm->suspend_frames_remaining =
    // frames - 1) with vm->pc pointing at the OP_VOID_RESULT that
    // always immediately follows (ANIMATESPRITE never itself produces a
    // value, same as WAIT/PAUSE) -- resumed via vm_resume_animatesprite,
    // which may itself return VM_RUN_SUSPENDED_ANIMATESPRITE again
    // (more frames left) before eventually falling through to vm->pc.
    OP_ANIMATESPRITE,

    // LAUNCH "procname / AWAIT / YIELD -- Phase 6's own first slice
    // (see docs/CONCURRENT_AGENTS_DESIGN.md). All three are void, same
    // as WAIT/PAUSE, and resumed via the plain vm_resume -- but by
    // agent.c's own synchronous scheduler, not ui.c's timer/keypress
    // callbacks: this first slice never needs a real GTK timer or
    // keypress at all, since WAIT/WAITKEY/INPUT/PAUSE/ANIMATESPRITE
    // used *inside* an agent are deliberately deferred (an explicit,
    // reported error, not a silent hang) rather than given a guessed-at
    // multi-agent semantic.
    //
    // OP_LAUNCH resolves its own procedure name and unpacks its own
    // arglist via find_proc_def/bytecode_find_proc, mirroring OP_APPLY's
    // own resolution and argument-unpacking exactly (a list unpacks
    // positionally into up to AST_MAX_PARAMS args, a bare scalar becomes
    // a single arg; same "no such procedure"/"wrong number of inputs"
    // eager-failure paths, pushing a throwaway value and falling
    // through rather than suspending on failure) -- but unlike APPLY,
    // a *successful* resolution doesn't push a VmFrame and jump: it
    // builds the resolved call's own bound Scope directly (via
    // eval_push_scope_for_call against a scratch one-slot ScopeStack,
    // stashed on vm->launch_scope) and suspends (VM_RUN_SUSPENDED_LAUNCH,
    // vm->launch_target_pc/vm->launch_scope both set) so agent.c's own
    // scheduler -- not this Vm -- is what actually starts a fresh Agent
    // running that procedure, installing vm->launch_scope as that
    // agent's own first scope before its first turn. `.text` carries
    // nothing of its own -- both the procedure name and its arglist are
    // ordinary ARG_EXPR arguments, same as APPLY's own two arguments
    // (not ERASE/LOAD's compile-time-literal ARG_QUOTED_WORD shape:
    // there's no AST to mutate and no compile-time backpatching
    // involved, so a computed name/arglist is fine), popped like any
    // other already-evaluated values (arglist first, name second,
    // matching APPLY's own pop order -- compiled in name-then-arglist
    // push order).
    OP_LAUNCH,
    // OP_AWAIT/OP_YIELD suspend unconditionally, no payload of their
    // own -- agent.c's own scheduler distinguishes them purely by which
    // VmRunResult came back. AWAIT means "don't give this agent another
    // turn until every other currently-tracked agent has finished";
    // YIELD means "this agent's own turn is over for now, but it's
    // still ready to run again on the very next scheduling pass" -- the
    // Logo-level, explicit-only cooperative timeslice this first slice
    // relies on instead of automatic per-loop-iteration yielding.
    OP_AWAIT,
    OP_YIELD,

    // SETSPEED's own throttle -- compile_call emits this right after
    // OP_CALL_BUILTIN for FD/FORWARD/BK/BACK/RT/RIGHT/LT/LEFT/SETXY/
    // SETHEADING/SETH/SETX/SETY/HOME/ARC (is_motion_command in
    // compiler.c), never for any other builtin. Unlike OP_WAIT there's
    // no companion OP_VOID_RESULT immediately after -- the call it
    // follows has already produced/discarded its own result via
    // finish_call, so this opcode touches the stack not at all. Pops
    // nothing; if app->turtle_speed_delay <= 0 (the default) or
    // vm_run_depth > 1 (inside a MAP/FILTER/REDUCE/FOREACH template,
    // RUN, or LOAD), it's a silent no-op -- deliberately NOT the
    // "not supported inside a template/RUN/LOAD" refusal WAIT/WAITKEY/
    // etc. give, since this is an automatic per-step throttle the
    // script never explicitly asked for at this call site, not a
    // command whose refusal the user needs reported. Otherwise suspends
    // (VM_RUN_SUSPENDED_MOTION_DELAY, vm->suspend_seconds =
    // app->turtle_speed_delay) with vm->pc pointing at the very next
    // instruction. ui.c resumes it exactly like WAIT (same timer, same
    // on_wait_timeout); agent.c's scheduler treats it like YIELD
    // (silently keeps the agent READY, no real delay -- see agent.c's
    // own comment) rather than the "not yet supported inside a
    // concurrent agent" teardown WAIT itself gets, for the same "this
    // wasn't an explicit ask" reasoning.
    OP_MOTION_DELAY,
} OpCode;

// One past the last OpCode member -- relies on the plain enum above
// being contiguous ints starting at 0 (never reassigned with explicit
// values), same assumption bytecode_opcode_name's own switch already
// makes. Used by bytecode_assemble's own name -> OpCode reverse lookup
// (a linear scan comparing bytecode_opcode_name(i) for i in
// [0, OPCODE_COUNT)) -- update this alongside the enum itself if a
// member is ever added after OP_MOTION_DELAY.
#define OPCODE_COUNT (OP_MOTION_DELAY + 1)

// AST_MAX_TEXT-sized would be wasteful here (512 bytes per instruction,
// most of which never carry text at all) -- a variable/procedure/
// builtin name only ever needs to fit what AST_MAX_TEXT itself already
// bounds identifiers to in practice; 64 bytes matches the same budget
// interpreter.c's own name buffers (Procedure.name, Variable.name-
// adjacent conventions) already use elsewhere.
#define INSTR_MAX_TEXT 64

typedef struct {
    OpCode op;
    int a, b;              // generic integer operands: jump targets, argc, a procedure's own target pc
    double number;           // OP_PUSH_NUMBER's literal
    char text[INSTR_MAX_TEXT]; // OP_PUSH_VAR/OP_SET_VAR/OP_CALL_BUILTIN/OP_CALL_PROC/OP_CHECK_OUTPUT's own name -- NOT OP_PUSH_WORD, see .a/word_literals[] below
} Instr;

// A fixed-size pool of instructions, same "fixed pool, loud error if
// exceeded" discipline as AstPool/list_pool/every other fixed table in
// this codebase, not a new malloc-heavy style. ~700KB (Instr's own
// text[64] dominates) -- heap-allocate this, never a stack local, same
// rule as every other multi-KB-or-larger struct in this project
// (LogoApp, ParseResult, AstPool).
#define MAX_INSTRUCTIONS 8192

// name -> start_pc for every procedure compile_program compiled --
// the same {name, start_pc} pairs compiler.c's own backpatching
// already computes internally while compiling (see compiler.c's own
// Compiler/ProcAddr), just kept on the chunk itself afterward instead
// of discarded once compile_program returns. compiler.c's OP_CALL_PROC
// backpatching still resolves its own targets *at compile time*
// (faster, and the callee is always statically known there) -- this
// table exists for the one case that genuinely can't be resolved until
// runtime: SEND, whose callee depends on a prototype-chain lookup
// against live app->plist_entries state that doesn't exist yet during
// compilation.
#define MAX_CHUNK_PROCS 50 // matches MAX_PROCEDURES in logo_types.h

// A procedure's own source text, from right after its name/params up
// to (not including) END -- copied at compile time from the AST_PROC_DEF
// node's own body_text/body_len (a pointer into the ORIGINAL SOURCE
// BUFFER, which a saved/loaded/assembled chunk can't assume outlives
// compilation the way an ordinary compile-then-run script's does).
// 8192 matches interpreter.c's own established Procedure.body budget
// for a procedure's body text, not a new number invented here.
#define MAX_PROC_SOURCE_TEXT 8192

typedef struct {
    char name[INSTR_MAX_TEXT];
    int start_pc;

    // Own copy of what used to be read from the AstPool at every call
    // (see docs/BYTECODE_VM_DESIGN.md's "Self-contained BytecodeChunk"
    // entry): eval_push_scope_for_call needs param_count/param_names to
    // bind arguments into scope, and TEXT/SAVE/SHOW need source_text --
    // none of it available at compile time to be inlined into an
    // OP_CALL_PROC/OP_SEND/OP_APPLY/OP_LAUNCH instruction directly (a
    // procedure's own identity is only known by name at each call site),
    // so it lives here instead, alongside the {name, start_pc} pair
    // that already made this table necessary for SEND.
    int param_count;
    char param_names[AST_MAX_PARAMS][32]; // matches AstNode's own param_names sizing exactly
    char source_text[MAX_PROC_SOURCE_TEXT];
} ProcAddr;

// OP_PUSH_WORD's own literal text, kept off Instr entirely (see
// INSTR_MAX_TEXT's own comment above): a literal word is unbounded in
// principle the same way any other word value is (AST_MAX_TEXT=512
// already budgets for this at the AST layer, matching
// EvalValue.word[512]/Variable.word[512] everywhere else in the
// runtime), so giving every one of MAX_INSTRUCTIONS instructions that
// budget just for the few that are OP_PUSH_WORD would waste ~3.5MB per
// chunk. 2048 entries * AST_MAX_TEXT costs ~1MB instead, and one entry
// per literal word *token* in the source is already generous --
// SETSPEED "medium and a "medium in another procedure are two
// separate entries, never deduplicated, same as ProcAddr not
// deduplicating anything either.
#define MAX_CHUNK_WORD_LITERALS 2048

// A [ ... ] list literal's own contents, rendered back into ordinary
// Logo source text at compile time (compiler.c's own
// render_list_literal_source, already trusted for exactly this job by
// MAP/FILTER/REDUCE/FOREACH's own compiled-template fast path -- see
// compile_template_call) instead of left as a pointer into the AstPool
// the way OP_PUSH_LIST_LITERAL originally worked (see
// docs/BYTECODE_VM_DESIGN.md's "Self-contained BytecodeChunk" entry).
// vm.c's own OP_PUSH_LIST_LITERAL handler re-lexes/parses this text at
// each visit and builds the value from that fresh parse -- the exact
// same "re-parse text at runtime" shape OP_RUN/OP_LOAD already use for
// arbitrary code, just for one list-literal expression instead of a
// whole program. 2048 bytes comfortably covers a real program's own
// literal lists (matches MAX_CHUNK_WORD_LITERALS' own entry-count
// scale, just repurposed as a per-entry byte budget here).
#define MAX_LIST_LITERAL_TEXT 2048
#define MAX_CHUNK_LIST_LITERALS 2048

typedef struct {
    Instr code[MAX_INSTRUCTIONS];
    int count;

    ProcAddr procs[MAX_CHUNK_PROCS];
    int proc_count;

    char word_literals[MAX_CHUNK_WORD_LITERALS][AST_MAX_TEXT];
    int word_literal_count;

    char list_literals[MAX_CHUNK_LIST_LITERALS][MAX_LIST_LITERAL_TEXT];
    int list_literal_count;
} BytecodeChunk;

// Appends `instr` to `chunk`, returning its own index (the position a
// jump/call target would need to record to point at it), or -1 if the
// chunk is full -- same "loud error, not silent truncation" policy as
// ast_alloc/list_alloc_node.
int bytecode_emit(BytecodeChunk *chunk, Instr instr);

// Looks up `name` in `chunk->procs`, returning its own start_pc, or -1
// if this chunk never compiled a procedure by that name. Used by both
// compiler.c's own backpatching (resolving an OP_CALL_PROC target
// that wasn't known yet when it was first emitted) and vm.c's own
// OP_SEND (resolving a message's target procedure at runtime, since
// SEND's callee is never known at compile time at all).
int bytecode_find_proc(const BytecodeChunk *chunk, const char *name);

// Same lookup as bytecode_find_proc, but returns the whole entry (NULL
// if not found) instead of just start_pc -- what OP_CALL_PROC/OP_SEND/
// OP_APPLY/OP_LAUNCH read param_count/param_names/source_text from at
// runtime instead of reaching back into the original AstPool the way
// they all used to (see docs/BYTECODE_VM_DESIGN.md's "Self-contained
// BytecodeChunk" entry).
const ProcAddr *bytecode_find_proc_entry(const BytecodeChunk *chunk, const char *name);

// Blanks the matching entry's own name in `chunk->procs` (a no-op if
// no entry matches), the same "blank the name so no lookup can ever
// match it again" mechanism eval_erase_declare already uses against an
// AstPool's own AST_PROC_DEF node -- OP_ERASE now has to do both:
// chunk->procs[] is this VM's own independent copy of every procedure's
// identity (see docs/BYTECODE_VM_DESIGN.md's "Self-contained
// BytecodeChunk" entry), so blanking only the AST side would leave
// OP_CALL_PROC/OP_SEND/OP_APPLY/OP_LAUNCH -- all chunk-only lookups now
// -- still able to find and call an "erased" procedure.
void bytecode_erase_proc(BytecodeChunk *chunk, const char *name);

// Appends `text` to `chunk->word_literals`, returning its own index
// (what OP_PUSH_WORD's own .a field stores), or -1 if the chunk's
// word-literal table is full -- same "loud error, not silent
// truncation" policy as bytecode_emit, and the whole reason this
// table exists instead of just inlining into Instr.text.
int bytecode_add_word_literal(BytecodeChunk *chunk, const char *text);

// Appends `text` (already bracket-wrapped Logo source, e.g. "[1 2 [3
// 4]]") to `chunk->list_literals`, returning its own index (what
// OP_PUSH_LIST_LITERAL's own .a field stores), or -1 if the chunk's
// list-literal table is full -- same "loud error, not silent
// truncation" policy as bytecode_add_word_literal.
int bytecode_add_list_literal(BytecodeChunk *chunk, const char *text);

// Returns `op`'s own C enum identifier as text (e.g. "OP_PUSH_NUMBER"),
// or "OP_UNKNOWN" for a value outside OpCode's own range (never
// crashes on bad input) -- what bytecode_disassemble's own CODE
// listing uses for each instruction's mnemonic, and what Stage C's
// eventual assembler will need the reverse of (name -> OpCode) to
// parse one back.
const char *bytecode_opcode_name(OpCode op);

// Renders `chunk` as human-readable text into `out` -- Stage B of the
// bytecode save/load/assembler initiative (docs/ROADMAP.md's own
// "Bytecode save/load/assembler" section), the chunk -> text half of
// what Stage C's eventual assembler will need the reverse of. Two
// sections: PROCS lists every still-defined entry in chunk->procs[]
// (name, start_pc, param_count/param_names, source_text) -- the one
// piece of chunk state an instruction's own operands don't always
// surface, since a procedure reachable only via SEND/APPLY/LAUNCH may
// have no static OP_CALL_PROC referencing it anywhere; CODE lists every
// instruction in order, each proc's own start_pc doubling as an inline
// "name:" label right before the instruction at that pc. word_literals[]/
// list_literals[] entries are resolved into their own literal text
// inline at each use site rather than a separate table a reader would
// have to cross-reference by index. Together this surfaces every field
// a self-contained BytecodeChunk carries (see
// docs/BYTECODE_VM_DESIGN.md's "Self-contained BytecodeChunk" entry).
void bytecode_disassemble(const BytecodeChunk *chunk, FILE *out);

// Stage C of the bytecode save/load/assembler initiative: parses
// `text` into `chunk`, appending instructions/procs/literals via the
// same bytecode_emit/bytecode_add_word_literal/bytecode_add_list_literal
// functions compiler.c itself uses, so a hand-assembled chunk is
// indistinguishable at runtime from a compiled one. `chunk` must
// already be zeroed (e.g. calloc'd) by the caller, same convention
// compile_program itself relies on. Accepts exactly what
// bytecode_disassemble produces (its own output round-trips
// unchanged), PLUS hand-written symbolic labels: a "name:" line
// (a proc's own entry-point label, or an ordinary bare one) anywhere
// in the CODE section defines a jump target that OP_JUMP/
// OP_JUMP_IF_FALSE/OP_CHECK_THROW/OP_CALL_PROC/OP_MAP_COMPILED-family
// operands may reference by name instead of (or as well as) the
// disassembler's own literal "@N" form -- a genuine two-pass assembly,
// so a label may be referenced before its own definition appears in
// the text. A procedure's own "start=" field in its PROCS entry is
// read but not trusted -- its real start_pc always comes from
// resolving its own "<name>:" label in CODE, so hand-editing
// instructions without keeping that field in sync by hand is safe.
// The leading "<pc>:" address column CODE instruction lines carry is
// also optional and, when present, not trusted either -- bytecode_
// assemble tracks its own running address, so inserting/removing
// lines never requires renumbering anything by hand.
//
// On success returns 1. On failure (malformed syntax, an unknown
// opcode mnemonic, an undefined or duplicate label, a proc whose own
// label is missing, or a fixed table filling up) returns 0 and writes
// a one-line description (including the offending line number where
// applicable) into `error` (size `error_size`) -- the same "loud
// error, not silent corruption" policy every other fixed-pool function
// in this file already follows.
int bytecode_assemble(const char *text, BytecodeChunk *chunk, char *error, size_t error_size);

#endif
