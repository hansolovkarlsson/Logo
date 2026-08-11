# Bytecode VM Reference

A complete, per-opcode reference for every `OpCode` `compiler.c` emits
and `vm.c` executes — the 55 members of `bytecode.h`'s own `OpCode`
enum, in the order they're declared there, cross-checked directly
against that enum (not copied from prose without verification, the
same discipline `docs/COMMAND_REFERENCE.md` uses against
`parser.c`'s `BUILTIN_SIGNATURES`).

This is a different altitude from the other two bytecode-adjacent
docs:
- `docs/BYTECODE_VM_DESIGN.md` is the design *narrative* — why the VM
  exists, the two-stage plan, and a chronological progress log of what
  shipped when and why. Read it for history and rationale.
- `docs/COMMAND_REFERENCE.md` is the *Logo-language* reference — what
  a script author can type.
- This document is the *instruction-set* reference — what
  `compile_program` actually turns a script into, and what `vm_run`
  actually executes. Read it when touching `compiler.c`/`vm.c`
  directly, or tracing what a piece of Logo source compiles to.

Every opcode's authoritative definition is its own comment in
`bytecode.h` (right next to the enum member) — this document is a
scannable summary of those comments, grouped by role, not a
replacement for reading them when working on the VM itself.

**Keep this in sync**: whenever a new `OpCode` is added to
`bytecode.h`, or an existing one's operands/stack effect/semantics
change, add/update its row here in the same batch — and check whether
the change also touches `docs/COMMAND_REFERENCE.md` (a new or changed
Logo-level command that compiles to it).

## Contents

- [Core shapes](#core-shapes)
- [Push values & variables](#push-values--variables)
- [Arithmetic](#arithmetic)
- [Comparison & logic](#comparison--logic)
- [Control flow](#control-flow)
- [Calls](#calls)
- [Procedure return & halt](#procedure-return--halt)
- [List literals](#list-literals)
- [LOCAL / ERASE / void result](#local--erase--void-result)
- [SEND](#send)
- [APPLY](#apply)
- [RUN / LOAD](#run--load)
- [THROW / CATCH](#throw--catch)
- [Loop-control stack slots](#loop-control-stack-slots)
- [Compiled MAP/FILTER/REDUCE/FOREACH templates](#compiled-mapfilterreduceforeach-templates)
- [Suspend points](#suspend-points)
- [Concurrent agents](#concurrent-agents)
- [Turtle speed throttle](#turtle-speed-throttle)

---

## Core shapes

**`Instr`** (`bytecode.h`) — one instruction: `op` (the `OpCode`),
generic integer operands `a`/`b` (jump targets, argc, a procedure's
target pc — meaning depends on the opcode), `number` (`OP_PUSH_NUMBER`'s
literal), and `text[INSTR_MAX_TEXT]` (64 bytes — an identifier: a
variable/builtin/procedure name. **Not** used for `OP_PUSH_WORD`'s own
literal word data — see below).

**`BytecodeChunk`** (`bytecode.h`) — a compiled program, and (since the
"Self-contained BytecodeChunk" batch, `docs/BYTECODE_VM_DESIGN.md`)
genuinely self-contained: nothing at runtime needs to reach back into
the original `AstPool` for `OP_CALL_PROC`/`OP_SEND`/`OP_APPLY`/
`OP_LAUNCH`/`OP_PUSH_LIST_LITERAL` to work. A fixed `code[MAX_INSTRUCTIONS]`
array (8192 instructions) plus three side tables keyed by index instead
of inlined into `Instr` itself:
- `procs[MAX_CHUNK_PROCS]` (`ProcAddr`: name → start_pc, plus its own
  copy of `param_count`/`param_names`/`source_text` — everything
  `eval_push_scope_for_call` and `TEXT`/`SAVE`/`SHOW` need, so no call
  opcode has to read an `AstNode` at runtime anymore)
- `word_literals[MAX_CHUNK_WORD_LITERALS][AST_MAX_TEXT]` (2048 entries
  × 512 bytes — `OP_PUSH_WORD`'s own literal text, sized to the
  language's real word budget instead of `INSTR_MAX_TEXT`'s 64-byte
  identifier budget)
- `list_literals[MAX_CHUNK_LIST_LITERALS][MAX_LIST_LITERAL_TEXT]` (2048
  entries × 2048 bytes — a list literal's own contents, rendered back
  into bracket-wrapped Logo source text at compile time instead of left
  as an `AstPool` node index)

`Instr.a` is an index into the relevant table for each of these opcodes.

**`Vm`** (`vm.h`) — one running (or suspended) VM: a value stack
(`stack[MAX_VM_STACK]`/`stack_top`), a call-frame stack
(`frames[MAX_VM_FRAMES]`/`frame_count`, each frame just `return_pc` +
`value_stack_base`), and its own scope storage
(`scopes[MAX_VM_SCOPE_DEPTH]`/`scope_depth` — decoupled from the old
shared `app->scopes`, see `docs/BYTECODE_VM_DESIGN.md`'s "VM-owned
scope storage" entry). Plus a handful of scalar fields several opcodes
below share: `last_call_produced_output`/`last_call_resolved` (read by
`OP_CHECK_OUTPUT`), `last_send_message` (read by
`OP_CHECK_SEND_OUTPUT`), and the suspend/resume fields (`pc`,
`suspend_seconds`, `pause_level`, `suspend_frames_remaining`,
`launch_target_pc`, `launch_scope`) valid only right after the
matching `VM_RUN_SUSPENDED_*` result.

**`VmRunResult`** (`vm.h`) — what `vm_run`/`vm_resume*` return:
`VM_RUN_HALTED`, or one `VM_RUN_SUSPENDED_*` per suspend point
(`WAIT`, `WAITKEY`, `INPUT`, `PAUSE`, `ANIMATESPRITE`, `LAUNCH`,
`AWAIT`, `YIELD`, `MOTION_DELAY`) — see "Suspend points" and
"Concurrent agents" below for which opcode produces which, and which
`vm_resume*` function continues it.

**Stack-effect convention**: every call (builtin or user procedure)
always leaves exactly one value on the value stack, mirroring
`eval.c`'s own `exec_call` — a statement-position call is followed by
a plain `OP_POP`; an expression-position one by `OP_CHECK_OUTPUT`.
Conditions are ordinary values, not a separate sub-language — this
language's `AND`/`OR` never short-circuit, so `OP_AND`/`OP_OR`/`OP_NOT`/
the `OP_CMP_*` family are plain pop-N-push-1 operators, consumed by
`OP_JUMP_IF_FALSE` via the same `eval_is_truthy` every truthiness
check in this codebase already uses.

---

## Push values & variables

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_PUSH_NUMBER` | `.number` | push 1 | Push `num_val(.number)` |
| `OP_PUSH_WORD` | `.a` (index into `chunk->word_literals[]`) | push 1 | Push `word_val(...)` — the literal's own text, not inlined in `Instr` (see "Core shapes") |
| `OP_PUSH_VAR` | `.text` (variable name) | push 1 | Push the variable's current value, or `num_val(0)` if unbound — matches `eval_expr`'s `AST_VARREF` default |
| `OP_SET_VAR` | `.text` (variable name) | pop 1 | `MAKE`-equivalent assignment |
| `OP_POP` | — | pop 1 | Discard the top of the value stack (what a statement-position call compiles to, after its own result is pushed) |

## Arithmetic

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_ADD` / `OP_SUB` / `OP_MUL` / `OP_DIV` | — | pop 2, push 1 | Binary arithmetic |
| `OP_NEG` | — | pop 1, push 1 | Unary negate |

## Comparison & logic

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_CMP_LT` / `OP_CMP_GT` / `OP_CMP_EQ` / `OP_CMP_LE` / `OP_CMP_GE` / `OP_CMP_NE` | — | pop 2, push `num_val(0/1)` | Comparison operators |
| `OP_NOT` | — | pop 1, push `num_val(0/1)` | Logical not |
| `OP_AND` / `OP_OR` | — | pop 2, push `num_val(0/1)` | Logical and/or — **no short-circuiting** (matches this language's own `AND`/`OR`) |

## Control flow

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_JUMP` | `.a` = target instruction index | — | Unconditional jump |
| `OP_JUMP_IF_FALSE` | `.a` = target instruction index | pop 1 | Pop (via `eval_is_truthy`); jump if falsy |

## Calls

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_CALL_BUILTIN` | `.text` = builtin name, `.a` = argc | pop `.a`, push 1 | Calls a builtin; pushes its real result, or a dummy `num_val(0)` for a void builtin (`vm.c`'s `call_builtin` dispatch tracks which, for `OP_CHECK_OUTPUT`'s sake) |
| `OP_CALL_PROC` | `.a` = target pc (backpatched at compile time), `.b` = argc, `.text` = procedure name | pop `.a` args, push 1 | Pushes a new `VmFrame`; pushes the eventual result once the call returns |
| `OP_CHECK_OUTPUT` | `.text` = the call's own name | — (may rewrite the top of stack) | Only emitted right after an `OP_CALL_BUILTIN`/`OP_CALL_PROC`/`OP_VOID_RESULT` used in *expression* position — if no real value was produced (`vm->last_call_produced_output`), reports `"<name>: didn't output a value"` and replaces the top with `word_val("")` |

## Procedure return & halt

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_OUTPUT` | — | pop 1 | Return from the current procedure with that value |
| `OP_STOP` | — | — | Return with `num_val(0)`, no real output — also what the compiler appends after every procedure body's own last statement (falling off the end) |
| `OP_HALT` | — | — | Stop the whole run — the very last instruction of the top-level program |

## List literals

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_PUSH_LIST_LITERAL` | `.a` = index into `chunk->list_literals[]` (bracket-wrapped Logo source text, e.g. `"[1 2 [3 4]]"`, rendered from the original `AST_LIST_LITERAL` at compile time) | push 1 | Builds `eval_build_list_literal_from_text(app, chunk->list_literals[.a])` fresh on every visit — lexes/parses the stored text into a scratch `AstPool`, same "re-parse text at runtime" shape `OP_RUN`/`OP_LOAD` use for arbitrary code, chosen so the chunk never needs the original `AstPool` back (see `docs/BYTECODE_VM_DESIGN.md`'s "Self-contained BytecodeChunk" entry) |

## LOCAL / ERASE / void result

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_LOCAL` | `.text` = variable name | — | Declares a scope-local variable in the current call (no-op if it already exists); always followed by `OP_VOID_RESULT` |
| `OP_ERASE` | `.text` = procedure name | — | Blanks that `AST_PROC_DEF`'s own name so it can never be called/listed again |
| `OP_VOID_RESULT` | — | push 1 | Pushes `num_val(0)` and clears `last_call_produced_output` — the special-form equivalent of a void builtin's return, for `MAKE`/`LOCAL`/`ERASE`/`WHILE` |

## SEND

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_SEND` | — (3 already-evaluated args) | pop 3 (obj, message, arglist) | Resolves `message` through `obj`'s prototype chain **at runtime** (the one call whose callee isn't known at compile time); pushes a frame + jumps in on success, or `word_val("")` with `vm->last_call_resolved` cleared on any resolution/arity failure |
| `OP_CHECK_SEND_OUTPUT` | — (reads `vm->last_send_message`) | — | `SEND`'s own `OP_CHECK_OUTPUT` — reads the message name from runtime state instead of a compile-time `.text`, since the callee name isn't known at compile time |

## APPLY

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_APPLY` | — (2 already-evaluated args) | pop 2 (name, arglist) | Resolves `name` via `bytecode_find_proc_entry` (a plain name lookup against `chunk->procs[]`, non-prototype-chain, unlike `SEND`); pushes a frame + jumps in on success. Every failure path (unknown procedure, wrong arity, recursion too deep) pushes a throwaway `num_val(0)` and falls through instead of jumping |
| `OP_VOID_DISCARD` | — | pop 1, push 1 | Always emitted right after `OP_APPLY` (both the eager-failure landing spot and the resolved procedure's own return): discards whatever's on top and replaces it with an ordinary void result — `APPLY` never hands back a value at all, matching `RUN` |

## RUN / LOAD

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_RUN` | — (1 already-evaluated arg) | pop 1 | `RUN thing` — re-lexes/parses/compiles a whole statement sequence into a fresh `BytecodeChunk`, runs it via a **recursive** `vm_run` sharing this `Vm`'s own stack/frames. Always followed by `OP_VOID_RESULT` |
| `OP_LOAD` | `.text` = path (compile-time literal only) | — | `LOAD "path` — same mechanism as `OP_RUN`, but the path is always a literal, never computed. Always followed by `OP_VOID_RESULT` |

Neither needs its own `vm_run_depth` suspend guard (they never suspend
themselves), but a suspend point reached *inside* the run/loaded code
is still correctly refused by those opcodes' own checks, since the
recursive `vm_run` call increments `vm_run_depth` the same way a
template's own does.

## THROW / CATCH

`THROW` itself is an ordinary `OP_CALL_BUILTIN` (just sets
`app->throw_requested`/`throw_tag` — no opcode of its own needed).
Every opcode below exists only to propagate that flag correctly once
set, reimplemented as compile-time-inserted forward jumps instead of a
runtime recursive break (this VM has no C call stack to unwind through
the way `ast_eval`'s tree-walker does).

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_CHECK_THROW` | `.a` = jump target (patched to the enclosing block's own end) | — | Emitted after every statement in a block; if `app->throw_requested`, jump there, skipping the rest of the current block only |
| `OP_CATCH_CHECK` | — (reads the tag value already left on the stack by `CATCH`'s own compiled preamble) | pop 1 | If `app->throw_requested` and the tag matches (case-insensitively), clears the flag — absorbing the throw right here. A non-matching or absent throw is left untouched |
| `OP_CHECK_UNCAUGHT_THROW` | — | — | Top-level only: if `app->throw_requested`, reports `"THROW: no CATCH found for <tag>"` and clears the flag, then execution continues to the next top-level statement (unlike a nested block, which skips to its own end) |

## Loop-control stack slots

`REPEAT`/`FOREVER`/`FOR` need a persistent loop-control value (a
remaining count, an iteration counter, `FOR`'s own limit/step) that
survives across however many transient values get pushed/popped
computing each iteration's condition, *and* across a recursive call
within the loop body — which is why these live on the value stack
itself rather than a hidden Logo variable (a hidden variable would be
genuinely global and collide across nested/recursive invocations of
the same compiled loop).

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_PEEK` | `.a` = depth below the current top (0 = the top itself) | push 1 | Pushes a **copy** of the value at that depth, leaving the original untouched |
| `OP_POKE` | `.a` = depth below the top the value should end up at, after popping | pop 1 | Overwrites the persistent slot at that (post-pop) depth — the write-back half of `OP_PEEK`, needed by `FOR`'s own counter, which sits underneath its own persistent limit/step rather than alone on top |

## Compiled MAP/FILTER/REDUCE/FOREACH templates

`MAP`/`FILTER`/`REDUCE`/`FOREACH`'s own "compiled once" fast path —
used only when the template argument is a literal `[...]` visible at
compile time (a runtime-computed template instead compiles as an
ordinary `OP_CALL_BUILTIN`, falling back to `vm.c`'s re-lex/re-parse-
per-element `eval_*_value` machinery). The template's own body is
compiled exactly once, inline in the same chunk, ending in `OP_HALT`
and reached only via a recursive `vm_run` call, once per element.

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_MAP_COMPILED` | `.a` = template start pc, `.text` = placeholder variable's compiler-generated base name | pop 1 (list) | Collects the template's own result for each element into a new list; pushes it |
| `OP_FILTER_COMPILED` | same | pop 1 (list) | Keeps each element whose template (a condition) is truthy; pushes the new list |
| `OP_REDUCE_COMPILED` | same (`.text` = accumulator name, `.text`+`"2"` = current element) | pop 1 (list) | Folds left-to-right via the template; pushes the final accumulator, or `num_val(0)` if the list was empty |
| `OP_FOREACH_COMPILED` | same | pop 1 (list) | Runs the template once per element for side effects only, stopping early on `OUTPUT`/`STOP`/`THROW`; pushes `num_val(0)` (void) |

## Suspend points

The real suspend points this VM has (see `docs/BYTECODE_VM_DESIGN.md`'s
suspend/resume design). Unlike every opcode above, these can make
`vm_run` **return early**, mid-chunk, handing control back to whatever
is driving it (`ui.c`'s GTK event loop) instead of running to
`OP_HALT`. All refuse outright (a clear runtime message, not a silent
hang) when `vm_run_depth > 1` — inside a template, `RUN`, or `LOAD` —
since a suspended return from an inner recursive `vm_run` call would
only unwind that one C frame, silently losing the suspend.

| Opcode | Result | Resumed via | Description |
|---|---|---|---|
| `OP_WAIT` | `VM_RUN_SUSPENDED_WAIT` | `vm_resume` | Pops the seconds argument; suspends only if `> 0` (`vm->suspend_seconds` set), else falls through immediately |
| `OP_WAITKEY` | `VM_RUN_SUSPENDED_WAITKEY` | `vm_resume_with_key` | Suspends unconditionally; the resume call pushes the pressed key's name as this instruction's own value |
| `OP_INPUT` | `VM_RUN_SUSPENDED_INPUT` | `vm_resume_with_input` | Suspends unconditionally; the resume call pushes the whole submitted line |
| `OP_PAUSE` | `VM_RUN_SUSPENDED_PAUSE` | `vm_resume` | Increments `app->pause_depth`, prints `"Paused (level N)..."`, suspends unconditionally; produces no value (same as `WAIT`) |
| `OP_ANIMATESPRITE` | `VM_RUN_SUSPENDED_ANIMATESPRITE` (possibly repeatedly) | `vm_resume_animatesprite` | Pops frames, then delay. No sprite set, or `frames <= 0`: synchronous no-op/error. `delay <= 0`: the whole frame loop runs synchronously, no suspend (matches `interpreter.c`'s own behavior). Otherwise advances one frame and suspends, once per remaining frame |

## Concurrent agents

Phase 6's own first slice (see `docs/CONCURRENT_AGENTS_DESIGN.md`).
All three are void and resumed via the plain `vm_resume` — but by
`agent.c`'s own synchronous scheduler, not `ui.c`'s timer/keypress
callbacks. `WAIT`/`WAITKEY`/`INPUT`/`PAUSE`/`ANIMATESPRITE` used
*inside* an agent are deliberately deferred with an explicit reported
error rather than a guessed-at multi-agent semantic.

| Opcode | Result | Description |
|---|---|---|
| `OP_LAUNCH` | `VM_RUN_SUSPENDED_LAUNCH` | Pops 2 already-evaluated args (arglist, then name — compiled name-then-arglist). Resolves the procedure name and unpacks the arglist exactly like `OP_APPLY` (positional list unpacking, bare scalar = one arg; same "no such procedure"/wrong-arity eager-failure paths). Unlike `APPLY`, a successful resolution doesn't jump in directly — it builds the call's own bound `Scope` (via `eval_push_scope_for_call`, stashed on `vm->launch_scope`) and suspends so `agent.c`'s scheduler (not this `Vm`) starts a fresh `Agent` running that procedure, installing `vm->launch_scope` as that agent's first scope |
| `OP_AWAIT` | `VM_RUN_SUSPENDED_AWAIT` | No payload. Means "don't give this agent another turn until every other currently-tracked agent has finished" |
| `OP_YIELD` | `VM_RUN_SUSPENDED_YIELD` | No payload. Means "this agent's turn is over for now, but it's still ready to run again on the very next scheduling pass" |

## Turtle speed throttle

| Opcode | Operands | Stack effect | Description |
|---|---|---|---|
| `OP_MOTION_DELAY` | — (reads `app->turtle_speed_delay`) | — | Emitted right after `OP_CALL_BUILTIN` for any motion command (`FD`/`BK`/`RT`/`LT`/`SETXY`/`SETHEADING`/`SETX`/`SETY`/`HOME`/`ARC`), never any other builtin. If the throttle is off (`<= 0`) or `vm_run_depth > 1` (inside a template/`RUN`/`LOAD`), it's a **silent** no-op — deliberately not the "not supported inside a template" refusal the suspend points above give, since this is an automatic per-step throttle the script never explicitly asked for at this call site. Otherwise suspends (`VM_RUN_SUSPENDED_MOTION_DELAY`), resumed exactly like `WAIT` (same timer). Inside a concurrent agent, `agent.c`'s scheduler treats it like `YIELD` (silently keeps the agent `READY`, no real delay) rather than the deferred-error treatment `WAIT` itself gets |
