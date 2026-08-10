# MultiLogo-style concurrent turtle agents

Status: **scoped, not started.** Four design decisions resolved with the
user 2026-08-10 (see "Decisions" and "Proposed first slice" below); no
code written yet.

## Why

`docs/FEATURE_ATLAS.md`'s own survey of "uncharted territory" named this
directly and explained why it was out of reach at the time:

> **MultiLogo** — introduced an `agent` construct: multiple independent
> Logo processes running *concurrently*, each with its own program
> counter... True concurrency needs cooperative scheduling inside
> `eval_logo` — a fundamentally different execution model from the
> current single-threaded interpreter loop.

That was true of `eval_logo` (and of `ast_eval`, Stage 1's tree-walker —
both recurse the C stack directly, with no explicit, save-able program
counter). It stopped being true once the bytecode VM shipped: `Vm` already
has exactly what cooperative scheduling needs — an explicit `pc`, explicit
call frames in an array instead of C recursion, and (as of the
suspend/resume batches) a real, working mechanism for a `Vm` to stop mid-
script and be resumed later from precisely where it left off. This was
also one of the four original motivations for building the VM at all
(`docs/BYTECODE_VM_DESIGN.md`'s own "Why", #4: "closures, generators,
concurrent turtle agents... wants a real, structured execution model
underneath it, not string-rescanning") — concurrency is the thing that
finally makes that motivation concrete rather than aspirational.

This project's own `TELL` is a different, much smaller thing: it switches
which turtle a single *sequential* program is currently steering, one
statement at a time. MultiLogo's agents are genuinely interleaved — each
with its own call stack, pausing and resuming independently of the
others, not a single program's attention moving between targets.

## What's already there vs. what's missing

The VM's own frame-array design (`vm.h`) already delivers per-agent
*call*-stack independence for free: `Vm.stack[]`/`frames[]`/`frame_count`
are already a self-contained, per-`Vm` unit — nothing about running two
independent `Vm`s side by side conflicts with that design.

What's missing is everything *outside* the `Vm` that Logo-level code
still depends on, which today lives as single, shared, mutable fields
directly on `LogoApp` — confirmed by reading the actual code, not assumed:

- **Variable scope storage.** `find_var` (`interpreter.c`) walks
  `app->scopes[0..scope_depth-1]` — one array, one cursor, shared by
  every call regardless of which "script" is running:
  ```c
  Variable* find_var(LogoApp *app, const char *name) {
      for (int s = app->scope_depth - 1; s >= 0; s--) { ... }
  ```
  If two agents are cooperatively interleaved and both are mid-procedure-
  call (`scope_depth > 0`) when a context switch happens, they're pushing
  and popping the *same* stack — agent B's own locals land on top of
  agent A's, and if A resumes before B has fully unwound, A's own `MAKE`s
  land in B's scope instead. This is the same `app->scopes[]`/
  `scope_depth` the Stage 2 vertical slice deliberately left shared
  between `ast_eval` and the VM ("so a VM-compiled procedure's own
  `AST_VARREF`/`OP_PUSH_VAR` reads and an interpreter-run procedure's
  variable reads can't drift") — a real, deliberate choice at the time,
  now the one piece of that decision concurrency actually breaks.
- **`app->current_turtle`.** `TELL` just sets this one shared `int`. If
  Agent A's own `TELL 0` is followed (before its next motion command
  runs) by Agent B interleaving with `TELL 1`, Agent A's own `FD 10`
  moves the wrong turtle.
- **`app->throw_requested`/`throw_tag`.** `OP_CHECK_THROW`/`OP_CATCH_CHECK`
  read these directly. If Agent A throws and hasn't been caught yet when
  the scheduler switches to Agent B, B's own `OP_CHECK_THROW` sees A's
  still-pending throw and incorrectly starts unwinding B's own code.
- **`app->run_depth`.** `RUN`'s own recursion cap (`exec_run`, added in
  the builtin-coverage-audit batch). Shared, so one agent's own `RUN`
  nesting count would count against a completely unrelated agent's cap.
- **`app->pause_depth`** — not just a technical hazard but a genuine
  *semantic* question with no obvious answer: does `PAUSE` inside one
  agent pause that agent alone, or the whole swarm? The current design
  (one shared counter, the REPL keeps accepting other commands while
  paused) was never built with multiple independent call stacks in mind.
  Resolved below: deferred, not guessed at.

`eval_logo`/`ast_eval` are unaffected by any of this — neither will ever
run concurrently with itself, so their own direct use of these `LogoApp`
fields stays exactly as it is. This is a VM-only feature, matching the
established pattern for every suspend/resume-era batch (`WAIT`/`WAITKEY`/
`PAUSE`/sprites/`RUN`/`LOAD`) — new machinery lives in `vm.c`/`ui.c`,
`eval.c`/`interpreter.c` stay untouched.

## Decisions (resolved 2026-08-10)

1. **Full rework, not a narrower first pass.** A narrower option was
   considered and rejected: restrict a context switch to only ever happen
   when the outgoing agent's own `scope_depth` is `0` (nothing of its own
   left on the shared stack), sidestepping the scope-storage rework
   entirely. This technically works, but its real cost is severe: `WAIT`/
   `PAUSE`/any suspend point become effectively unusable from *inside* an
   agent's own procedure calls (using one there would violate the
   restriction), directly undermining the whole reason those suspend
   points were built recursion-depth-independent in the first place. The
   user chose the full rework instead — agents can suspend from
   arbitrarily deep inside their own procedure calls, the same as a
   single script already can, no surprising restriction to learn.
2. **A spawned agent auto-gets a fresh turtle**, fixed at spawn time (an
   implicit `TELL` to a newly created index) — matching MultiLogo's own
   model of an agent naturally paired with the thing it controls, and
   sidestepping any "two agents both explicitly `TELL` the same index on
   purpose" ambiguity.
3. **`PAUSE` inside an agent is refused, not given a guessed-at
   semantic.** Prints a clear message and continues, the same
   "documented gap, not silent corruption" spirit already established
   for `WAIT`/`WAITKEY`/`INPUT`/`PAUSE`/`ANIMATESPRITE` used inside a
   `MAP`/`FILTER`/`REDUCE`/`FOREACH` template or `RUN`/`LOAD`'d code
   (`vm_run_depth > 1`) — except this is a *design* gap (no agreed
   meaning yet), not a *mechanical* one (the suspend can't propagate).
   `WAIT`/`WAITKEY`/`INPUT`/`ANIMATESPRITE` all still work fine inside
   an agent — **except in the first slice specifically**, where all
   five are deferred together (see decision #4 and "Proposed first
   slice" below): this slice never needs a real GTK timer/keypress at
   all, a real simplification, not just PAUSE's own open semantic
   question.
4. **`YIELD` is explicit only, not automatic per loop iteration**
   (resolved while scoping the first slice specifically, 2026-08-10).
   The Logo programmer calls it themselves at chosen points, same model
   Lua coroutines use. Automatic yielding after every `REPEAT`/`WHILE`/
   `FOR`/`FOREVER` iteration would give real fairness against a runaway
   loop, but touches `compile_call`'s own loop-compiling code in four
   places — real risk to already-shipped, tested code, set aside for
   a later follow-up rather than folded into an already-large first
   slice. A script that never `YIELD`s inside a long loop will starve
   its sibling agents until that loop finishes — a known, narrow
   limitation to document, matching this project's own tolerance for
   the comparable `WHILE`/`FOR` iteration-cap gap already accepted
   elsewhere.

## Mechanism

**A new `Agent` struct** bundles everything that needs to be per-agent
instead of shared:
```c
typedef enum { AGENT_READY, AGENT_WAITING_JOIN, AGENT_FINISHED } AgentState;

typedef struct {
    Vm vm;                      // already fully self-contained (stack/frames/pc)
    int turtle_index;           // fixed at spawn, per decision #2
    Scope scopes[MAX_SCOPE_DEPTH];
    int scope_depth;
    gboolean throw_requested;
    char throw_tag[64];
    int run_depth;
    AgentState state;           // just READY/WAITING_JOIN/FINISHED in the first
                                 // slice -- WAITING_TIMER/WAITING_KEY are a later
                                 // follow-up, once WAIT/WAITKEY-in-an-agent lands
} Agent;
```
`pool`/`chunk` (the compiled program) are **not** duplicated — every
agent runs against the same, already-compiled `AstPool`/`BytecodeChunk`
the top-level script itself compiled from, exactly like `MAP`/`RUN`'s own
recursive `vm_run` calls already share chunks/pools with their caller
where appropriate. (`ERASE` mutating a shared `AST_PROC_DEF` while another
agent is mid-call against it is a known, narrow, accepted edge case —
same tolerance this project already extends to comparably rare
cross-cutting interactions, not designed around preemptively.)

**Context switch is a save/restore swap, not a rewrite of `find_var` and
friends.** Since `app->scopes`/`scope_depth`/`throw_requested`/
`throw_tag`/`run_depth` are the exact fields every existing, already-
tested opcode handler already reads and writes, the scheduler doesn't
need to change any of them: before resuming Agent X, it copies X's own
saved `scopes[0..scope_depth)` (proportional to actual depth, not the
full `MAX_SCOPE_DEPTH`) and the other four fields into `app`'s own
fields, and sets `app->current_turtle = X.turtle_index`; when X suspends
(any `VmRunResult` other than continuing immediately), it copies `app`'s
current values back into X's own storage before switching to whichever
agent runs next. Zero changes needed to `eval.c`/`interpreter.c`, and
`vm.c`'s existing opcodes need no changes either — they're already
reading/writing exactly the fields the swap targets.

**The scheduler reuses suspend/resume as-is.** A cooperative round-robin
over a list of `Agent`s, living in `ui.c` alongside the existing
`g_suspended_run`/`g_paused_runs` machinery: "this agent's turn is over"
is just another `VmRunResult` — `WAIT`/`WAITKEY`/`INPUT`/`ANIMATESPRITE`
already hand control back cleanly (a real GLib timer/keypress resumes
later); a new voluntary `YIELD` opcode is the same shape, just resumed on
the *next scheduling tick* instead of an external event. The top-level
script itself only becomes "one more agent" once the first `LAUNCH`
happens — before that, it runs exactly as it does today (a single
top-level `run_logo_script` call, zero scheduler overhead), so this adds
no cost to the common, non-concurrent case at all.

**Proposed syntax** (concrete, open to revision once it's actually used):
`LAUNCH "walker` spawns a fresh-turtle agent running procedure `walker`
(no arguments — matching `APPLY`'s own arity-checked call convention
would be a natural follow-up, not needed for a first slice) to
completion, cooperatively interleaved with everything else already
running. `AWAIT` blocks the calling agent (or the top-level script, once
it's implicitly agent 0) until every currently-launched agent has
finished — the natural "join" needed before a top-level script can look
at the finished drawing.

## Proposed first slice (scoped 2026-08-10)

**A fourth decision, resolved while scoping this slice specifically**:
`YIELD` is **explicit only** — the Logo programmer calls it themselves at
chosen points in their own agent script, the same model Lua coroutines
use. The alternative (automatic yield after every loop iteration, for
real fairness against a runaway `REPEAT`/`WHILE`/`FOR`/`FOREVER`) was
considered and set aside: it touches `compile_call`'s own loop-compiling
code in four places, real risk to already-shipped, tested code, on top
of everything else this slice already needs. A script that never calls
`YIELD` inside a long loop will starve its sibling agents until that
loop finishes — a known, narrow limitation to document, matching this
project's own tolerance for the comparable `WHILE`/`FOR` iteration-cap
gap already accepted elsewhere. Automatic, granular yielding is a real,
separate follow-up, not attempted here.

**A crucial simplification this forces, worth its own callout**: this
slice deliberately does *not* support `WAIT`/`WAITKEY`/`INPUT`/`PAUSE`/
`ANIMATESPRITE` inside an agent yet (their own multi-agent semantics —
does `WAIT` in one agent block just that agent, or the whole scheduler
tick? — are a real follow-up design question, not resolved here). That
means **nothing in this slice ever needs a real GLib timer or keypress**
— every suspend reason the scheduler has to handle (`LAUNCH`/`AWAIT`/
`YIELD`, plus ordinary completion) is resolved by the scheduler itself,
on its own next loop iteration, never by GTK's event loop. So the whole
round-robin can be a **plain, synchronous C loop** — no new `ui.c`
scheduler state, no GTK re-entry, completely headless-testable, the same
style `vm.c`'s own existing tests already use. If an agent hits `WAIT`/
`WAITKEY`/`PAUSE`/`INPUT`/`ANIMATESPRITE`, the scheduler treats it as an
explicit, reported error (that agent is torn down with a clear message,
not silently mishandled or hung) rather than attempting semantics nobody
has agreed on yet.

**Concrete pieces**:
- Three new opcodes (`OP_LAUNCH`/`OP_AWAIT`/`OP_YIELD`, `bytecode.h`) and
  three new `VmRunResult` variants (`vm.h`), gated by the same
  `vm_run_depth > 1` refusal every other suspend opcode already has (a
  `LAUNCH`/`AWAIT`/`YIELD` reached from inside a `MAP`/`FOREACH`
  template or `RUN`/`LOAD`'d code has the identical wrong-chunk hazard).
  `OP_LAUNCH` resolves its own procedure name via `find_proc_def`/
  `bytecode_find_proc` (mirroring `APPLY`'s own resolution, including
  its own "no such procedure" and "must take no inputs" — no argument-
  passing to a launched agent in this slice) and hands the resolved
  target `pc` back via a new `Vm.launch_target_pc` field, the same
  "suspend, carry a payload, let the driver act on it" shape
  `suspend_seconds`/`pause_level` already established.
- A new `Agent` struct and scheduler (own new files, `agent.h`/`agent.c`
  — not `ui.c`, since nothing here touches GTK): `Vm` plus the per-agent
  copies of `scopes[]`/`scope_depth`/`throw_requested`/`throw_tag`/
  `run_depth`, a fixed `turtle_index` (auto-assigned at spawn from
  `app->turtles[]`, refusing with a clear message if all `MAX_TURTLES`
  (10 — a real, fairly low bound worth remembering) are already taken),
  and a small state enum (`READY`/`WAITING_JOIN`/`FINISHED` — no
  `WAITING_TIMER`/`WAITING_KEY` needed yet, per the simplification
  above). The context switch is the save/restore swap already designed:
  before running an agent's own turn, copy its saved state into `app`'s
  shared fields (`scopes`/`scope_depth`/`throw_requested`/`throw_tag`/
  `run_depth`/`current_turtle`); copy back out once its turn ends.
- `ui.c`'s own `run_logo_script` needs exactly one new branch: if its
  initial `vm_run` call returns `VM_RUN_SUSPENDED_LAUNCH` (instead of
  any case `handle_vm_result` already knows), hand off to a new
  `scheduler_run(...)` — a blocking call that owns the round-robin until
  every agent is `FINISHED`, then returns control to `handle_vm_result`
  exactly as if the whole thing had been one ordinary script. Everything
  else in `ui.c` (`g_suspended_run`/`g_paused_runs`, `on_entry_key_pressed`,
  ...) stays completely untouched — a concurrent-agent run and an
  ordinary suspend/resume run can never overlap in this slice, since
  agents can't reach any of `ui.c`'s own suspend paths at all yet.
- Testing: fully headless, no shadow-diff (no `ast_eval` equivalent will
  ever exist) — direct `vm_run`/scheduler calls asserting on final
  variable/turtle state, the same style already established for
  suspend/resume/sprites. Proof case: two agents, each looping and
  touching their own turtle/variables, `YIELD`ing mid-loop, confirming
  neither's state leaks into the other — the actual point of decision #1
  (scope-depth-independent correctness), demonstrated without yet
  needing `WAIT`/`WAITKEY` themselves inside an agent to prove it.

**Deliberately deferred past this slice, not silently dropped**:
`WAIT`/`WAITKEY`/`INPUT`/`PAUSE`/`ANIMATESPRITE` inside an agent (needs
its own follow-up design — likely `Agent` gaining real `WAITING_TIMER`/
`WAITING_KEY` states and `ui.c` finally getting involved, once this
slice's own mechanism is proven); automatic per-loop-iteration yielding;
passing arguments to a launched agent; any way to inspect a running
agent's own state from outside it (introspection, `WHO`-style).

## Progress

**First slice shipped 2026-08-10**, same day as the scoping above (user
said "commit and push, then start building the first slice" right
after). `OP_LAUNCH`/`OP_AWAIT`/`OP_YIELD` (`bytecode.h`/`vm.c`), three
new `VmRunResult` variants and `Vm.launch_target_pc` (`vm.h`), `LAUNCH`/
`AWAIT`/`YIELD` in `parser.c`'s `BUILTIN_SIGNATURES` and their
`compiler.c` special-form branches, and the new `Agent` struct +
synchronous scheduler in `src/agent.h`/`src/agent.c` (a new module, not
`ui.c`, per the first slice's own "no GTK needed at all" simplification)
— all built exactly as scoped above, with one real design gap found and
fixed along the way, not assumed away:

**A genuine bug caught by the first test run, not guessed**: the very
first `LAUNCH` (the one whose own suspend is what gets a top-level
script into multi-agent mode at all) was never actually spawning its
own target agent — only *later* launches, reached from inside
`scheduler_run`'s own loop, were. `OP_LAUNCH`'s own suspend only
resolves and carries the target `pc`; something has to actually *act*
on that by creating an `Agent` for it, and the very first one has no
"inside the loop" to be created from. Fixed by factoring a shared
`spawn_agent` helper (same turtle-assignment/exhaustion-check logic
either way) and calling it once, explicitly, right before
`scheduler_run`'s own loop starts, treating the initial agent's own
still-live `launch_target_pc` as a "pending spawn" exactly like any
other. Caught immediately by `test_launch_runs_a_procedure_to_completion`
— the launched procedure's own output was silently missing entirely,
not a crash, which is exactly the kind of failure a real test (not just
code review) is for.

**A second, smaller gap found while wiring `ui.c`, from a compiler
warning, not a test**: `handle_vm_result`'s own `switch` didn't handle
the three new `VmRunResult` variants (`-Wswitch`), because a top-level
script that hits an *ordinary* suspend point (`WAIT`/`WAITKEY`/`PAUSE`/
`ANIMATESPRITE`) and only calls `LAUNCH` *after* resuming would reach
`handle_vm_result` directly — `run_logo_script`'s own new branch only
catches a script's very first `vm_run` call. Rather than silently
mis-handle or crash on that combination, added an explicit `default:`
case reporting it as not yet supported (mixing ordinary top-level
suspend/resume with a *later* `LAUNCH` is a real, if narrow, gap this
first slice doesn't attempt to solve) and cleaning up normally.

7 new headless `tests/test_vm.c`-style cases in a new `tests/test_agent.c`
(new `make test-agent` target, folded into `make test`) — the two that
matter most directly prove decision #1's own point: two agents, each
suspending via `YIELD` from *inside* a nested procedure call with a
real local variable (not a global) still live on its own scope stack,
confirmed neither's value leaks into the other; and two agents each
moving their own turtle a known, distinct distance, confirmed via
`hypot` against `home_x`/`home_y` that neither's motion leaks into the
other's turtle. Also: `LAUNCH` unknown-procedure/wrong-arity errors
(matching `APPLY`'s own wording pattern), `AWAIT` genuinely blocking
until every launched agent finishes (an output-ordering assertion, not
just "it returns"), and `WAIT` used inside an agent correctly reporting
the deferred-support error and being torn down rather than hanging —
confirming `AWAIT` still resolves even when a sibling agent was torn
down abnormally, not stuck waiting on it forever. Confirmed clean under
AddressSanitizer (`test_agent` fully clean; `test_vm` the same one
pre-existing, unrelated crash already documented for every earlier
batch); all 7 `make test` suites pass; `bin/logo`/`bin/logi` build
warning-free and `bin/logo` was confirmed to launch/run without
crashing. The actual interactive multi-agent run (watching two turtles
move independently on screen) still needs the user to confirm manually.

Deliberately deferred, not silently dropped, matching the first slice's
own scope exactly: `WAIT`/`WAITKEY`/`INPUT`/`PAUSE`/`ANIMATESPRITE`
inside an agent; automatic per-loop-iteration yielding; passing
arguments to a launched agent; mixing ordinary top-level suspend/resume
with a later `LAUNCH`; any way to inspect a running agent's own state
from outside it.
