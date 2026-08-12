# Changelog

Dated write-ups of finished work, moved out of `docs/ROADMAP.md` once
shipped (see that file's own note at the top) — this is where the
detailed rationale/history lives; `docs/ROADMAP.md` itself stays
trimmed to what's actually still ahead. Originally organized as
`docs/ROADMAP.md`'s own "Future directions" section (ideas from a
2026-08-05 brainstorm, grouped into rough phases by complexity/risk
rather than a strict priority order) — every phase below has since
shipped in full.

## Phase 4 — Interactive input

`WAITKEY`/`INPUT`/`PAUSE` (see `docs/LANGUAGE.md`) share one underlying
mechanism for pausing `eval_logo` (which otherwise runs synchronously
start-to-finish) to wait on a live event, solved once rather than three
separate times. Mouse and joystick input turned out not to need it after
all — `MOUSEPOS`/`BUTTON?`/`JOYSTICK?`/etc. are passive state queries,
continuously updated in the background, rather than something that
waits on an event.

`TONE`/`PLAYSOUND`/`STOPSOUND` (see `docs/LANGUAGE.md`'s "Sound"
section) round out this phase — fire-and-forget sound effects through
SDL2's own audio device, reusing the joystick's SDL2 dependency rather
than adding a second audio library. Phase 4 is now complete.

## Phase 5 — Large architectural bets (need a design discussion first, not just scoping)

Prototype-style objects (`NEW`/`SEND` — see `docs/LANGUAGE.md`'s
"Prototype-style objects" section) turned out not to need the language
redesign originally scoped here: an object is just a property list
(already shipped in an earlier phase) with a `"prototype` link, and
`SEND` reuses the exact command/operator split ordinary procedure
calls already have. No new value type, no changes to variable scoping
or `call_procedure` at all.

Compile to a bytecode VM instead of tree-walking the AST Stage 1 built
(see `docs/BYTECODE_VM_DESIGN.md`) — an order of magnitude bigger than
everything else on this page: a real compiler (AST → bytecode) and a
from-scratch execution core (explicit instruction pointer, explicit
call frames in an array instead of C recursion), not a rewrite of
Stage 1's own frontend. Motivation: real suspend/resume instead of
every busy-wait `WAITKEY`/`PAUSE`/`WAIT`/etc. currently uses,
decoupling recursion depth from a C-stack footprint entirely, and
growing Logo past "toy language" scope generally (closures, generators,
concurrent turtle agents). Prioritized into the checklist below —
resolved 2026-08-09, after Stage 1 shipped in full — rather than a
single undifferentiated bullet; check off a group as it lands and
delete it once `docs/BYTECODE_VM_DESIGN.md`'s own Progress log has the
detail, same convention Stage 1's own checklist used.

## Phase 5, Stage 1 — new evaluator built-in coverage

The real lexer/parser/AST/tree-walking evaluator itself (see
`docs/BYTECODE_VM_DESIGN.md`) is done and proven — the mechanism works,
confirmed by a shadow-diff harness that runs the same script through both
engines and checks they agree. What's left is coverage: growing
`BUILTIN_SIGNATURES`/`eval.c` toward the ~150 operators
`interpreter.c` already has, one `do_*` function at a time (each gets its
own function — see `eval.c`'s file comment — plus shadow-diff tests, and
the ground-truth-against-the-real-interpreter verification habit this
project has already caught three real fidelity bugs with). Grouped by natural
work batches, each roughly a "commit and push" unit; check off a group as it
lands and delete it once `docs/BYTECODE_VM_DESIGN.md`'s own Progress log has
the detail, per this file's usual convention.

Every batch on this checklist has now landed (see
`docs/BYTECODE_VM_DESIGN.md`'s Progress log for the full history),
including the `LOAD` cross-boundary-call gap this section used to flag
here — fixed for real (eager parse-time `LOAD` following), not worked
around; see `docs/BYTECODE_VM_DESIGN.md`'s own LOAD-cross-boundary-
call-fix milestone.

Deliberately **not** planned for Stage 1, and not just "not done yet":
- `WAITKEY`/`INPUT`/`PAUSE`/`WAIT`/`BUTTON?`/`JOYSTICK?`/`JOYSTICKAXIS`/
  `JOYSTICKBUTTON?`/`MOUSEPOS`/`MOUSEX`/`MOUSEY` — every one of these is
  either the busy-wait suspend/resume mechanism Phase 4 built for
  `eval_logo`, or a live hardware/event query with no meaning for a
  headless test-driven evaluator. The bytecode VM (the other Phase 5
  bullet above) is the actual fix for the underlying suspend/resume
  problem; porting a busy-wait shim into the new engine first would be
  solving it twice, the second time throwaway. (`EOF?`/`BUTTON?`/
  `JOYSTICK?`-family predicates were already flagged out of scope for
  this same reason when type predicates landed — see
  `docs/BYTECODE_VM_DESIGN.md`.)
- `TONE`/`PLAYSOUND`/`STOPSOUND` and sprites/animation (`LOADSPRITE`/
  `LOADSPRITESHEET`/`SETSPRITE`/`SETSPRITEFRAME`/`STAMPSPRITE`/
  `ANIMATESPRITE`/`LOADPIC`/`SAVEPIC`) — real features, but large,
  GTK/SDL-state-heavy surface area disproportionate to what a
  research/learning evaluator needs; revisit only if Stage 1 coverage
  becomes the thing actually driving the real app (`bin/logo` still runs
  exclusively on `eval_logo` today — see `docs/BYTECODE_VM_DESIGN.md`).
  (`WINDOW`, the Logo language command that turns off the canvas edge
  boundary, used to be miscategorized here too — it's not GTK/SDL-heavy
  at all, just a one-line `edge_mode` setter alongside `WRAP`/`FENCE`,
  and shipped with the rest of the drawing/canvas primitives batch.)
- `PAUSE`/`CONTINUE`/`BACKTRACE`/`EXECTIME` — the debugger commands are
  intrinsically tied to `eval_logo`'s own live-pause mechanism (see
  `docs/LANGUAGE.md`'s "Debugger" section); no equivalent concept exists
  in a tree-walking evaluator with no suspend point yet.

## Phase 5, Stage 2 — bytecode VM

Priority order, each roughly its own design-then-build unit; later
items depend on earlier ones landing first (unlike Stage 1's batches,
which were mostly independent of each other). See
`docs/BYTECODE_VM_DESIGN.md`'s own "Stage 2 sketch" section for the
instruction-set/frame-layout detail behind each of these.

- [x] **A small, real, end-to-end vertical slice first** — not full
  `BUILTIN_SIGNATURES` parity. Literals, arithmetic, comparisons/
  `AND`/`OR`/`NOT`, `PRINT`, `MAKE`, `IF`/`IFELSE`/`WHILE`, and
  procedure calls (including recursive/mutually-recursive/forward-
  referenced) with `OUTPUT`/`STOP` — `bytecode.h`/`bytecode.c`
  (instruction format), `compiler.h`/`compiler.c` (AST → bytecode, one
  pass + backpatch list for forward-referenced procedure calls),
  `vm.h`/`vm.c` (the explicit-pc/explicit-frame-stack dispatch loop),
  shadow-diffed against `ast_eval` in `tests/test_vm.c` (`make
  test-vm`). Confirmed the mechanism itself works: Logo-level
  recursion (`test_recursive_procedure`,
  `test_mutually_recursive_procedures`) runs as VM-frame pushes/pops
  and `pc` jumps, not new C stack frames.
  **Frame-array sizing was deliberately deferred, not decided here**:
  variable *bindings* still go through the unchanged
  `app->scopes[]`/`MAX_SCOPE_DEPTH` (200) mechanism via a new shared
  `eval_push_scope_for_call` (factored out of `call_ast_procedure`,
  used by both engines) — only the VM's own value stack and call
  frames (`{return_pc, value_stack_base}`) are new, VM-owned arrays.
  Real recursion-depth independence from `MAX_SCOPE_DEPTH` needed
  VM-owned scope storage, its own deliberate follow-up -- **done
  2026-08-10**, see `docs/BYTECODE_VM_DESIGN.md`'s own Progress log
  entry ("VM-owned scope storage — real recursion-depth independence")
  for the full writeup: a new `ScopeStack` abstraction threaded through
  the five functions that touch scope storage, a new `Vm.scopes[2000]`
  array (chosen after asking the user directly, over a dynamic-realloc
  alternative), and a required fix to `Agent`'s own Phase 6 save/restore
  mechanism (scope copying removed entirely, now free/structural since
  each `Agent`'s own `Vm` already isolates it). The VM's real recursion
  depth is now ~10x the old shared ceiling, proven via a new 1000-level
  test, not just claimed.
- [x] **`INSTR_MAX_TEXT` word-literal truncation fix** (scoped
  2026-08-10, built 2026-08-11 — see `docs/BYTECODE_VM_DESIGN.md`'s own
  entry of the same name for the full writeup): a word literal (e.g. a
  long `OPENWRITE` path, or a `'raw text'` sentence) longer than 63
  characters used to be silently truncated at compile time —
  `Instr.text` (64 bytes) was shared by both short identifiers (already
  generous — real identifiers cap at 32 bytes elsewhere in this
  codebase) and unbounded literal word data, which the AST layer itself
  already budgets 512 bytes for. Fix: a new `BytecodeChunk`-level
  `word_literals[][512]` side table (mirroring the existing
  `proc_table`) for `OP_PUSH_WORD` specifically, leaving `Instr.text`
  untouched for every other opcode — chosen over uniformly raising
  `INSTR_MAX_TEXT` to 512, which would have cost ~3.5MB of waste per
  chunk (every one of `MAX_INSTRUCTIONS` = 8192 instructions, not just
  the ones that need it) instead of this approach's ~1MB. New
  regression test in `test_vm.c` proves a 100-byte literal, past the
  old 63-byte ceiling, now round-trips exactly; verified clean under a
  standalone ASan run isolated from an unrelated pre-existing
  `eval_logo` deep-recursion overflow already present in `test_vm.c`
  before this fix.
- [x] Grow instruction coverage the same way Stage 1 grew
  `BUILTIN_SIGNATURES` — incremental batches, each shadow-diffed
  against `ast_eval` before moving to the next.
  - [x] **Lists/arrays/`MAKE`-adjacent ops** (2026-08-09): list literals
    (a new `OP_PUSH_LIST_LITERAL`, since a literal's contents are
    recursive/variable-arity raw AST data, not a flat value any `Instr`
    field could hold — the VM keeps a live `AstPool` reference and
    builds it fresh each visit via a shared `eval_build_list_literal`,
    same as `eval_expr` already did), `FIRST`/`BUTFIRST`/`LAST`/
    `BUTLAST`/`COUNT`/`EMPTY?`/`FPUT`/`LPUT`/`WORD`/`SENTENCE`/`SE`/
    `LIST`, `ARRAY`/`ITEM`/`SETITEM`/`FILLARRAY`, `THING`/`LOCAL`/
    `NAMES`. **`compile_call`'s builtin dispatch is now a real lookup,
    not an if-chain**: `find_proc_def` (moved from `eval.c` to
    `ast.h`/`ast.c` — it never touched `LogoApp`, so it belongs with
    the AST-only module and lets `compiler.c` call it without pulling
    in `interpreter.h`) tells `compile_call` whether a name is a user
    procedure; everything else is assumed a builtin and routed through
    a generalized `OP_CALL_BUILTIN`, so growing coverage further only
    touches `vm.c`'s own `call_builtin` dispatch and `eval.c`'s
    value-taking cores, never `compiler.c` again. Also closed a real
    fidelity gap this generalization exposed: a void builtin/special
    form (`MAKE`/`LOCAL`/`WHILE`/`SETITEM`/`FILLARRAY`) used in
    *expression* position needs to report "didn't output a value" too,
    not just a procedure that never calls `OUTPUT` — a new
    `OP_VOID_RESULT` opcode plus generalizing `OP_CHECK_OUTPUT`'s own
    `last_call_produced_output` flag to every call form (not just
    `OP_CALL_PROC`) handles it uniformly. 18 new `tests/test_vm.c`
    cases (ported from already-confirmed `test_eval.c` scripts),
    confirmed clean under AddressSanitizer.
  - [x] **Property lists / `NEW`** (2026-08-09): `SETPROP`/`GETPROP`/
    `REMOVEPROP`/`PROPLIST`/`NEW`. Zero `compiler.c` changes needed —
    exactly the payoff the previous batch's dispatch-generalization was
    for: just exposing five already-value-shaped `eval.c` functions
    (`eval_getprop`/`eval_setprop`/`eval_removeprop`/`eval_proplist`/ a
    new `eval_new_declare`, the last centralizing the `"prototype"` key
    string `NEW` writes under so it isn't duplicated between `do_new`
    and `vm.c`) and adding them to `vm.c`'s `call_builtin` dispatch. 6
    new `tests/test_vm.c` cases, confirmed clean under AddressSanitizer.
    **`SEND` deliberately deferred, not part of this batch**: unlike
    these, it dynamically resolves which procedure to call at runtime
    (through the prototype chain) and has its own resolved/produced
    error-suppression shape (see `do_send`'s own comment) — closer to a
    new opcode than an ordinary builtin call, worth its own dedicated
    design rather than folding into a batch about property-list
    plumbing.
  - [x] **Turtle/drawing commands, plus `ERASE`/`PROCEDURES`**
    (2026-08-09): `FD`/`BK`/`RT`/`LT`/`SETXY`/`SETHEADING`/`SETX`/
    `SETY`/`GETX`/`GETY`/`HEADING`/`POS`/`CANVASSIZE`/`DISTANCE`/
    `TOWARDS`/`PENUP`/`PENDOWN`/`HOME`/`TELL`/`WHO`/`CLEAR`/`ARC`/
    `CLEAN`/`HIDETURTLE`/`SHOWTURTLE`/`WRAP`/`FENCE`/`WINDOW`/
    `SETPENCOLOR`/`SETPENWIDTH`/`SETBACKGROUND`/`SETCANVASSIZE`/
    `LABEL`/`FILL`/`ERASERECT`, plus `ERASE`/`PROCEDURES` (pulled in
    alongside since a real `ERASE` test needs `PROCEDURES` to observe
    it). One new opcode, `OP_ERASE` (`ERASE`'s argument is
    `ARG_QUOTED_WORD`, a raw name, same shape as `MAKE`/`LOCAL`, so it
    can't go through the ordinary `OP_CALL_BUILTIN` path).
    **A real, unplanned gap this surfaced and fixed, not left open**:
    `ERASE` mutates the AST it's given (blanking an `AST_PROC_DEF`'s
    own name) as its whole mechanism, so a call compiled as
    `OP_CALL_PROC` (because the name resolved to a real procedure *at
    compile time*) can still fail *at runtime* if `ERASE` ran first —
    `exec_call_proc`'s own "unknown procedure" path didn't print
    anything before this batch, unlike `do_user_procedure_call`'s own
    `"I don't know how to X"`, and didn't suppress `OP_CHECK_OUTPUT`'s
    generic message the way `do_user_procedure_call`'s own
    `*resolved=0` does either — both fixed via a new `last_call_resolved`
    flag on `Vm`, mirroring `eval_expr`'s own `resolved && !produced`
    check exactly (including reproducing a genuine *double* message
    ast_eval itself prints for recursion-too-deep in expression
    position, which does NOT clear `resolved`).
    **A second, harder bug this surfaced: the test harness itself, not
    the VM.** `tests/test_vm.c`'s `shadow_diff_vm` used to parse the
    source once and hand the *same* `AstPool` to both `ast_eval` and
    `compile_program`/`vm_run` — fine for every prior batch (nothing
    mutated the AST), but `ERASE` breaks that assumption: whichever
    engine ran first would blank the procedure's name before the
    second engine's own run ever saw it, producing a false failure
    that looked like a VM bug but was actually stale test-harness
    design. Fixed by parsing the source independently per engine (two
    full lex+parse passes, two separate `ParseResult`s) — the same
    "never let one run's side effects leak into another's" discipline
    `test_shadow_diff.c` already followed for its own two engines, now
    actually required here for a specific operator's sake, not just as
    a precaution. 27 new `tests/test_vm.c` cases — turtle motion/
    queries, `DISTANCE`/`TOWARDS`, `HOME`/`CLEAR`, `WHO`/`TELL`
    (including out-of-range), canvas size (including out-of-range),
    pen/canvas-appearance and drawing-primitive smoke tests (`text`-
    diff-only coverage — `shadow_diff_vm` doesn't snapshot full turtle
    state the way `test_shadow_diff.c` does), and the three `ERASE`
    cases above. Confirmed clean under AddressSanitizer; all 6 test
    suites pass via `make test`.
  - [x] **`SEND`** (2026-08-09): the piece deferred twice before this
    for a real reason — `SEND`'s own callee isn't known until runtime
    (resolved through the object's prototype chain against live
    `app->plist_entries` state), so it can't be backpatched into a
    static `OP_CALL_PROC` target the way every other call in this VM
    is. Solved by giving `BytecodeChunk` itself a small persistent
    `{name, start_pc}` table (`bytecode_find_proc`) — the same data
    `compiler.c`'s own backpatching already computes while compiling,
    just kept around afterward instead of discarded — so a new
    `OP_SEND` can resolve the message *at runtime* and look up its
    target's compiled address the same way. Two new opcodes:
    `OP_SEND` (pops obj/message/arglist, resolves, pushes a `VmFrame`
    on success exactly like `OP_CALL_PROC`'s own success path, or
    prints its own specific error and `word_val("")` on any failure —
    every case ported directly from `do_send`'s own exact wording and
    control flow, including the one place its error-suppression
    genuinely differs from an ordinary call: `do_send` clears its own
    `resolved` flag on *any* "didn't produce a value" outcome,
    including recursion-too-deep, unlike an ordinary call which only
    clears it for a truly unknown name) and `OP_CHECK_SEND_OUTPUT`
    (`OP_CHECK_OUTPUT`'s own job, but reading the message name to
    report from a new `Vm.last_send_message` runtime field instead of
    a compile-time `.text`, since `compile_call` can't know `SEND`'s
    own callee name ahead of time). `eval_resolve_method` and a new
    `eval_send_unpack_args` (extracted from `do_send`'s own argument-
    unpacking, since the VM's call mechanism differs from
    `call_ast_procedure`'s tree-walking one but the unpacking itself
    doesn't) are shared between both engines. 11 new `tests/test_vm.c`
    cases, ported directly from `test_eval.c`'s own already-confirmed
    `SEND` corpus — direct method call, prototype-chain lookup,
    inherited methods vs. non-inherited data fields, extra message
    arguments, operator-form output capture (including the "never
    outputs" diagnostic), every one of `do_send`'s own error messages
    (unknown message, data property, missing `:self`, wrong arg
    count), and a cyclic prototype chain staying bounded. All passed
    on the first run; confirmed clean under AddressSanitizer.
- [x] **`THROW`/`CATCH`** (2026-08-09) — turned out to need less new
  machinery than the checklist's own original framing ("an explicit
  unwind-target stack the VM consults") anticipated, once the
  tree-walker's own mechanism was actually re-read: `ast_eval` itself
  doesn't use real stack unwinding either — `THROW` just sets a shared
  `app->throw_requested`/`throw_tag` flag, and every loop/block
  construct (`exec_block`, `do_while`, ...) cooperatively checks it
  after each statement/iteration and breaks early, letting it cascade
  up through however many nested C calls are on the stack. `STOP`/
  `OUTPUT` already work identically in `ast_eval` (both just set
  `stop_requested`, checked the same way) — this VM's own `OP_STOP`/
  `OP_OUTPUT` already handle that case correctly via a *direct* frame
  pop + `pc` jump (a more direct implementation of the same semantics,
  not a different one), so only `THROW`'s own multi-frame cooperative
  propagation needed new work. Reimplemented as compile-time-inserted
  forward jumps standing in for the tree-walker's own recursive
  breaks: `compile_block` now emits a new `OP_CHECK_THROW` after
  *every* statement (not just loop iterations), patched to jump to
  that block's own end — composing correctly across nested blocks,
  `WHILE`'s own loop (one extra check before its loop-back jump, since
  `OP_STOP` can never fall through to it but `THROW` can), and a
  procedure body's already-existing auto-appended `OP_STOP` (which
  turns out to double as the correct "throw propagated to the end of
  this procedure, return now" landing pad, with no extra code needed).
  A new `OP_CATCH_CHECK` (after `CATCH`'s own block, itself compiled
  with the same per-statement checks) absorbs a matching throw exactly
  like `do_catch`; a new `OP_CHECK_UNCAUGHT_THROW`, used only for
  `compile_program`'s own top-level statements (via a new
  `compile_block` parameter, `is_top_level`), reports `"THROW: no
  CATCH found for ..."` and *keeps running* the rest of the script,
  mirroring `ast_eval_from`'s own top-level recovery loop exactly
  (unlike a nested block, which skips to its own end instead).
  **A real bug caught by the test corpus, not guessed**: `CATCH`'s own
  evaluated tag was first stashed in a `Vm`-level scratch field
  (`vm->pending_catch_tag`) between evaluating it and checking it —
  broke immediately on *nested* `CATCH` (an inner `CATCH`'s own tag
  overwrote the outer one's before the outer ever got to check it).
  Fixed by leaving the tag value on the VM's own value stack instead
  (which naturally supports nesting via ordinary push/pop) and popping
  it only once, right when `OP_CATCH_CHECK` actually needs it — no
  scratch field at all. `THROW` itself needed no new opcode (an
  ordinary `OP_CALL_BUILTIN`, sharing a newly exposed
  `eval_throw_value` with `do_throw`); `eval_catch_check`/
  `eval_report_uncaught_throw` are similarly shared with
  `do_catch`/`ast_eval_from` rather than duplicated. 9 new
  `tests/test_vm.c` cases (3 ported from `test_eval.c`'s own corpus,
  6 new — specifically targeting cross-frame/cross-construct
  propagation the existing tree-walker corpus didn't happen to
  exercise: a throw through two nested procedure calls, breaking a
  `WHILE` loop early, an `IF` branch, nested `CATCH` with a
  non-matching inner tag, and a procedure that throws in expression
  position — confirming the "didn't output a value" diagnostic and the
  uncaught-throw report both fire, in the right order). All 9 new
  cases passed on the first run after the nested-CATCH fix; confirmed
  clean under AddressSanitizer. (`REPEAT`/`FOREVER`/`FOR` were left as
  a known, separate gap from this batch — see below, now closed.)
- [x] **`REPEAT`/`FOREVER`/`FOR`** (2026-08-09) — the loop-construct
  gap this VM's own `THROW`/`CATCH` batch had explicitly deferred.
  All three needed persistent loop-control state (a remaining count,
  an iteration counter, `FOR`'s own limit/step/internal counter) that
  survives a *recursive* call within the loop body — a hidden Logo
  variable would collide across nested/recursive invocations of the
  same compiled loop (exactly the class of bug `CATCH`'s own tag
  clobbering already surfaced), so a new opcode, `OP_PEEK` (read a
  copy of a value at a fixed depth without popping it), keeps this
  state on the VM's own value stack instead, naturally protected by
  whatever `VmFrame` a recursive call pushes. `REPEAT`'s own count is
  truncated once via a small newly-exposed `eval_int_value` (`INT`'s
  own core, added to `vm.c`'s builtin dispatch too as a side effect —
  not a wholesale math-operators port, just the one operator `REPEAT`
  itself needs); `FOREVER` gets the `MAX_WHILE_ITERATIONS`-capped
  iteration counter its own tree-walker equivalent has (a real
  necessity, unlike `WHILE`'s still-open gap — an uncapped `FOREVER`
  can hang the VM outright). **A second, harder bug this surfaced,
  caught by `tests/test_vm.c`'s own recursion-safety test, not
  guessed**: `FOR`'s first working version re-read its own loop
  variable back from the *Logo-visible* variable for its condition/
  increment — `exec_for`'s own tree-walker equivalent never does this
  (its loop control is a genuinely separate, C-local `double i`,
  naturally recursion-safe; only the *Logo-visible* copy, written via
  a plain `set_var` purely for the block's own benefit, is
  deliberately not scoped) — so a recursive call whose own `FOR` loop
  shared the same variable name corrupted the outer loop's next
  comparison once it returned. Fixed by giving `FOR` a third,
  genuinely separate persistent stack slot for its own internal
  counter (never read back from Logo state at all), needing one more
  new opcode, `OP_POKE` (the write-back half of `OP_PEEK`, since this
  counter sits underneath `limit`/`step` on the stack, not on top,
  so a plain "push 1; add" can't update it in place the way it can for
  `REPEAT`/`FOREVER`'s own lone top-of-stack counter). `FOR`'s own
  loop is compiled as two near-duplicate variants (ascending/
  descending), the direction picked once via a runtime branch on
  step's sign rather than re-derived every iteration. No
  `MAX_WHILE_ITERATIONS` cap for `FOR` itself in this batch — left
  alongside `WHILE`'s own still-open gap, since `FOR`'s termination is
  normally guaranteed by its own arithmetic, a smaller risk than
  `FOREVER`'s. 11 new `tests/test_vm.c` cases — 6 ported from
  `test_eval.c`'s own confirmed corpus, plus a fractional-count
  truncation case, `THROW` breaking a `FOR` loop early, and (the two
  cases that actually matter most for this design) `REPEAT` and `FOR`
  each recursing into a call containing the exact same loop construct,
  confirming the outer invocation's own state survives intact.
  Confirmed clean under AddressSanitizer; all 6 test suites pass via
  `make test`.
- [x] **`MAP`/`FILTER`/`REDUCE`/`FOREACH` templates compiled once**
  (2026-08-09) — the genuine semantic upgrade flagged above, not just a
  port. When the template argument is a literal `[...]` visible at
  compile time, `compile_template_call` (`compiler.c`) rewrites its own
  "?"/"?1"/"?2" placeholders directly in the AST into real,
  compiler-generated, per-call-site-unique Logo variable references
  (`:__tmplN__`, via a new `Compiler.template_counter`), renders the
  result back to source text (`render_list_literal_source`, walking the
  list literal's own `AST_WORD`/nested-`AST_LIST_LITERAL` children —
  they were never operator-precedence-parsed to begin with), and
  lexes/parses it *once*, right then — not once per element the way
  both `eval_logo` and `ast_eval` still do. The freshly parsed body
  can't be compiled straight out of its own scratch `AstPool` though:
  `OP_PUSH_LIST_LITERAL`'s stored node index and `OP_CALL_PROC`'s own
  `find_proc_def` lookup are only meaningful against the *one* `AstPool`
  the rest of the compiled program uses, so a new `ast_graft` deep-copies
  the parsed subtree, node for node, into that same pool first — this
  also makes nesting free (a nested template's own grafted body grafts
  into the very same pool again). Four new opcodes,
  `OP_MAP_COMPILED`/`OP_FILTER_COMPILED`/`OP_REDUCE_COMPILED`/
  `OP_FOREACH_COMPILED` (`.a` = the template's own compiled start pc,
  `.text` = its placeholder's base name), reached only via a
  jump-around at compile time and a *recursive* `vm_run` call at
  runtime, once per element — `vm_run`'s own `OP_HALT` handler is a
  plain C `return;`, correctly scoped to just that one recursive
  invocation since `pc` is a local variable each call. At runtime, each
  iteration just binds the placeholder to the current element's actual
  `EvalValue` and re-executes the same already-compiled bytecode — a
  real value binding, not a text round-trip, so it handles list-valued
  elements and (with the fix below) recursion for free, without
  `eval_value_to_source_text`'s own quote-escaping dance at all. A
  runtime-computed template (not a literal `[...]`), or one that's
  syntactically broken (discovered at compile time, when the rendered
  text fails to parse), falls through to the *exact same*
  `OP_CALL_BUILTIN` path a dynamic template already needs — newly
  exposed `eval_map_value`/`eval_filter_value`/`eval_reduce_value`/
  `eval_foreach_value` (refactored from `do_map`/`do_filter`/
  `do_reduce`/`do_foreach`, parameterized by already-evaluated
  `EvalValue`s instead of AST indices), so a broken template still gets
  `ast_eval`'s own exact defensive per-element behavior, not a
  reimplementation of it.
  Two real bugs found and fixed after the mechanism first ran, neither
  guessed — both caught by `tests/test_vm.c`:
  - **Numeric-looking list elements silently failed every ordering
    comparison** (`FILTER [? > 2] [1 2 3 4]` kept nothing). A list
    literal's own elements are *always* internally `VALUE_WORD` (see
    `node_to_value`/`eval_build_list_literal` — never
    `LIST_ELEM_NUMBER`, even for `"1"`); the *old* runtime engines never
    noticed, because substituting an element's own text into the
    template and re-lexing it from scratch makes a numeric-looking
    token lex as a genuine number, regardless of the original list
    element's own internal tag — `exec_compare`'s own non-numeric
    fallback for `>`/`<`/`<=`/`>=` is unconditionally false for
    anything that isn't *already* `VALUE_NUMBER`. This VM's own
    direct-value binding has no re-lex step to do that promotion
    implicitly, so a new `coerce_template_element` (`vm.c`) replicates
    it explicitly wherever an element gets bound to a placeholder.
  - **The placeholder variable isn't safe against reentrancy of the
    *same* compiled call site** — the harder one, and a genuine
    correction to the design's own earlier assumption that a
    per-call-site-unique name was sufficient. It rules out two
    *different* `MAP`/`FILTER`/`REDUCE`/`FOREACH` calls ever colliding,
    but not the *same* call site being reached again through recursion
    while the outer activation's own loop is still mid-iteration
    (paused inside its own template's evaluation, about to read `?`
    *after* the recursive call returns) — unconditionally deleting the
    placeholder once the whole loop finished (the first version of this
    code) let the *inner* invocation's own cleanup delete it out from
    under the still-in-flight outer one. Fixed the same way the `FOR`
    loop bug from the previous batch was: state that must survive a
    recursive reentry of the same compiled construct can't be a shared
    slot that's merely written-then-read. Here, unlike `FOR`, the fix
    needed no new opcode or stack-depth tracking at all — `save_var`/
    `restore_var` capture whatever the placeholder was bound to (if
    anything) once, before the loop starts, and restore exactly that
    afterward instead of deleting; since each `exec_*_compiled`
    invocation's own `saved` is a C-local, save/restore naturally nest
    correctly across however many recursive reentries happen, riding on
    the C call stack's own natural nesting instead of needing a
    VM-level mechanism for it.
  A separate, narrower, *documented-not-fixed* limitation: `OUTPUT`/
  `STOP` executed directly inside a template body (not through a
  further nested real procedure call) can pop a frame that belongs to
  whatever real procedure encloses the whole `MAP`/`FILTER`/`REDUCE`/
  `FOREACH` call, but C's own call/return means control still returns
  to the `exec_*_compiled` loop, not further up where that frame
  actually belonged — a `frame_count` "floor," recorded before the loop
  and checked after every recursive `vm_run` call, stops the loop early
  as a fail-safe the moment that's detected, but doesn't fully unwind
  correctly; a `STOP` with *no* enclosing procedure at all (e.g. a
  top-level `FOREACH`) doesn't stop the loop early either, since
  `exec_return`'s own top-level fallback never touches
  `app->stop_requested` (this VM's `OP_STOP` is a hard frame-pop, not
  the tree-walker's own cooperative flag). Both are pre-existing
  architectural consequences of a hard-frame-based `STOP`, not new to
  this batch, and disproportionately invasive to fully solve here — left
  as a known gap rather than tested as if it worked. `eval_delete_var`
  (swap-with-last removal from `app->variables[]`, mirroring
  `REMOVEPROP`'s own pattern) closes the one other real gap: without
  it, an internal placeholder variable would leak into a later `NAMES`
  call, unlike `ast_eval`'s own pure-text-substitution mechanism, which
  never creates one. 20 new `tests/test_vm.c` cases — 14 ported from
  `test_eval.c`'s own confirmed corpus (minus one, `STOP` directly
  inside a `FOREACH` template, deliberately not ported: it exercises
  exactly the documented gap above), plus 6 new — a template calling a
  real user procedure (exercising `ast_graft`'s whole reason for
  existing), a runtime-computed template and a compile-time-malformed
  literal one (both confirming the dynamic-fallback path), nested
  `MAP`/`REDUCE` templates sharing one chunk without colliding, and the
  recursion-reentrancy case above. Confirmed clean under
  AddressSanitizer (aside from one pre-existing, unrelated ASan-only
  stack-overflow in `test_recursion_depth_cap_reports_error_not_a_crash`,
  confirmed to reproduce identically on the pre-batch codebase too, so
  not a regression from this work); all 6 `make test` suites pass.
- [x] **Suspend/resume's actual GTK integration** (2026-08-10) — the
  real point of this whole stage, scoped narrow on purpose: `WAIT`/
  `WAITKEY` only (real suspend triggers via a timer and a keypress,
  respectively, the two structurally different cases), with `INPUT`/
  `PAUSE`/`ANIMATESPRITE` left for later incremental batches, same
  "grow coverage one group at a time" pattern as every other Stage 2
  batch. `bin/logo` itself now runs scripts through the compiler+VM,
  not `eval_logo` — the first real cutover; `eval_logo` stays in the
  tree, still used by `tests/test_interpreter.c`, just no longer this
  app's own execution path.
  **Scoping surfaced two things the checklist's own wording had
  undersold, confirmed by grep, not assumed**: `WAIT`/`WAITKEY`/
  `INPUT`/`PAUSE`/`ANIMATESPRITE` existed *only* in `eval_logo` before
  this batch (a deliberate prior decision, see above — porting a
  busy-wait shim into `ast_eval` first would have been throwaway), and
  `vm_run` had *never* been called from `ui.c` at all — `bin/logo` ran
  exclusively on `eval_logo` until this batch. So this was never just
  "wire an already-integrated VM into GTK"; it was building the
  suspend mechanism, porting two commands into the new pipeline, and
  cutting the live app over, together.
  **The mechanism** (`vm.h`/`vm.c`): `vm_run`'s return type changed
  from `void` to a new `VmRunResult` (`VM_RUN_HALTED`/
  `VM_RUN_SUSPENDED_WAIT`/`VM_RUN_SUSPENDED_WAITKEY`). Two new opcodes,
  `OP_WAIT`/`OP_WAITKEY` (`bytecode.h`) — `OP_WAIT` pops its own
  already-evaluated seconds argument and, if positive, stores it in a
  new `Vm.suspend_seconds` and *returns* `VM_RUN_SUSPENDED_WAIT`
  mid-chunk instead of running to `OP_HALT`; `OP_WAITKEY` returns
  `VM_RUN_SUSPENDED_WAITKEY` unconditionally. A new `Vm.pc` field
  (distinct from every other opcode's plain C-local `pc`, which can't
  survive a `return`) records exactly where to continue. Two new entry
  points, `vm_resume`/`vm_resume_with_key`, restart `vm_run` from
  `vm->pc` — the latter pushing the pressed key's name first, exactly
  the value `OP_WAITKEY`'s own handler would have pushed had it not
  suspended, so the immediately-following `OP_CHECK_OUTPUT`/`OP_POP`
  (whichever `finish_call` emitted) sees the same shape either way.
  Deliberately **not** a GTK-aware design: `vm.c` never touches a
  clock or an event loop itself — it just reports "suspended, here's
  why, here's the payload" (a `double` for `WAIT`, nothing for
  `WAITKEY`), leaving *how* to actually wait entirely to whichever
  GTK-aware caller is driving it (`ui.c`), the same seam discipline
  `request_redraw` already established for `interpreter.c` reaching
  back into GTK. Because a suspended `Vm`'s `stack[]`/`frames[]`/
  `frame_count` are left fully intact (nothing freed, nothing
  truncated), resuming correctly unwinds back through however many
  *ordinary* nested procedure calls (`VmFrame`-based) were in progress
  when the suspend happened — the actual payoff of the frame-array
  design, confirmed directly by a test suspending 3 real procedure
  calls deep and resuming back out through all 3 `OUTPUT`s correctly,
  not just asserted.
  **The one deliberately-NOT-fixed gap, by design decision, not
  oversight**: `MAP`/`FILTER`/`REDUCE`/`FOREACH` templates already
  recurse into `vm_run` via plain C recursion (see the batch above,
  `exec_map_compiled` and friends) — a `SUSPENDED` return from that
  *inner*, recursive `vm_run` call would only unwind that one C frame,
  landing back in `exec_map_compiled`'s own C loop, not out to
  whatever's driving the *outermost* `vm_run` (`ui.c`), silently
  losing the suspend rather than delivering it. Rather than redesign
  templates off C recursion first (a real, separate refactor of
  already-shipped, tested code), a new `Vm.vm_run_depth` counter
  (incremented/decremented at every `vm_run` entry/exit) lets
  `OP_WAIT`/`OP_WAITKEY` detect "I'm inside a template body" and
  refuse outright with a clear runtime message instead of attempting
  a suspend that can't actually propagate — the same "documented gap,
  not silent corruption" spirit as the template batch's own
  `frame_floor` mitigation for `OUTPUT`/`STOP`-inside-a-template.
  **`ui.c` wiring**: a new `run_logo_script` replaces both of the
  app's own former `eval_logo(app, text)` call sites (the REPL's
  Enter-submit and `LOAD`'s file-open completion) — lex/parse/compile/
  `vm_run`, then a shared `handle_vm_result` acts on the returned
  status. A file-scope `SuspendedRun{result, chunk, vm}` (not
  anything owned by `LogoApp` itself — only `ui.c` ever drives
  resumption, one script "thread" at a time) keeps a paused run alive
  across however many GTK callbacks it takes to resume. `WAIT` gets a
  real one-shot `g_timeout_add` (not `eval_logo`'s own `g_usleep`
  spin); `WAITKEY` reuses the existing `waiting_for_key` gate in
  `on_entry_key_pressed`, but instead of setting a `key_ready` flag for
  a busy-wait loop to notice, the handler calls `vm_resume_with_key`
  directly the moment a key arrives — real event-driven resume, not
  polling. `eval_logo`'s own `key_ready`/`pending_key`/`waiting_for_input`
  busy-wait fields are untouched and still used by `eval_logo` itself
  (`tests/test_interpreter.c`'s own coverage), just no longer read by
  `ui.c`'s live wiring.
  **Testing**: can't shadow-diff against `ast_eval` the way every
  other Stage 2 batch's tests do — that engine has no suspend concept
  at all, and `WAIT`/`WAITKEY` don't even exist there. 6 new
  `tests/test_vm.c` cases call `vm_run`/`vm_resume`/`vm_resume_with_key`
  directly instead, asserting on the returned `VmRunResult` and
  captured output — entirely headless, no GTK needed, which is exactly
  the payoff of keeping `vm.c` itself GTK-free: `WAIT` suspending with
  the right duration then resuming to completion, `WAIT 0` never
  suspending at all (matches `interpreter.c`'s own `seconds > 0`
  guard), `WAITKEY` suspending then resuming with a supplied key,
  `WAITKEY` suspending and resuming correctly through 3 nested real
  procedure calls, and `WAITKEY`/`WAIT` used directly inside a `MAP`/
  `FOREACH` template reporting the documented refusal instead of
  hanging or corrupting state. Confirmed clean under AddressSanitizer
  (aside from the same pre-existing, unrelated
  `test_recursion_depth_cap_reports_error_not_a_crash` stack-overflow
  noted in the batch above); all 6 `make test` suites pass; `bin/logo`
  builds warning-free and was confirmed to launch and run without
  crashing. The actual interactive click-test (typing `WAIT 2`/
  `WAITKEY` into the running app, confirming the window stays
  responsive during the wait) needs a human — this environment has no
  Accessibility permission for scripted GTK UI control, and full-screen
  automation isn't an appropriate substitute. **Confirmed working by
  the user directly, both commands.**
- [x] **`INPUT`** (2026-08-10) — the second of the three commands the
  `WAIT`/`WAITKEY` batch above deliberately deferred, and the easiest
  of the three: mechanically identical to `WAITKEY` (suspends
  unconditionally, produces a value, gated by the same
  `vm_run_depth`-based template refusal), just resumed with a whole
  submitted line instead of a single key name. A new
  `VM_RUN_SUSPENDED_INPUT` (`VmRunResult`) and `OP_INPUT` (`bytecode.h`)
  mirror `OP_WAITKEY` exactly; a new `vm_resume_with_input` mirrors
  `vm_resume_with_key`'s own push-then-continue shape — kept as its own
  function rather than a shared/parameterized one, since `ui.c` gates
  the two on genuinely different flags/events (`waiting_for_key`, any
  keypress, vs. `waiting_for_input`, only Return/KP_Enter after
  ordinary typing) and they're conceptually distinct resumption events,
  matching this codebase's own convention of dedicated functions per
  concept (`eval_first_value`/`eval_last_value`, etc.) over one
  parameterized helper. `ui.c`'s existing `waiting_for_input` branch in
  `on_entry_key_pressed` (already there for `eval_logo`'s own busy-wait,
  now unused by the live app) needed the same treatment as `WAITKEY`'s
  own branch: capture the submitted line, then call
  `vm_resume_with_input` directly instead of setting `input_ready` for
  a busy-wait loop to notice. 2 new `tests/test_vm.c` cases (suspend
  with a submitted line then resume to completion; `INPUT` directly
  inside a `MAP` template reporting the same documented refusal as
  `WAIT`/`WAITKEY`), confirmed clean under AddressSanitizer (same one
  pre-existing, unrelated crash); all 6 `make test` suites pass;
  `bin/logo` builds warning-free and confirmed to launch/run without
  crashing. `PAUSE`/`ANIMATESPRITE` remain as the last two follow-up
  batches — `PAUSE` is the harder of the two, since `eval_logo`'s own
  version relies on reentrant nested `eval_logo` calls (an ordinary
  REPL command typed while paused) rather than a single suspend/resume
  point, and has no VM-level design sketched yet.
  **A real concurrency bug in the suspend mechanism itself, found while
  scoping `ANIMATESPRITE` (2026-08-10), fixed before going further**:
  unlike `WAITKEY`/`INPUT`, `WAIT` suspends with neither
  `waiting_for_key` nor `waiting_for_input` set, so nothing stopped
  `ui.c`'s ordinary Enter-submit path (or `LOAD`) from starting a
  *second* script while the first was still counting down — the new
  run would silently overwrite the single `g_suspended_run` slot the
  first script's own eventual timer callback depends on, so that timer
  would go on to resume the wrong `Vm` (or dereference a freed one) once
  it fired. Fixed by moving the check into `run_logo_script` itself
  (the single entry point both call sites already share): if
  `g_suspended_run != NULL`, print `"A script is still running --
  please wait for it to finish"` and refuse the new submission outright
  rather than racing it against the first — matches how `WAITKEY`/
  `INPUT` already behave (one script "thread" at a time), just closing
  the one case (`WAIT`) that had no dedicated gating flag to piggyback
  on. Confirmed via `make test`/ASan (unaffected — `ui.c` isn't part of
  either) and a clean `bin/logo`/`bin/logi` build; the interactive
  repro itself (start a `WAIT`, try to submit another command before it
  finishes, confirm it's rejected and the original still resumes
  correctly) needs the user to confirm directly, same reason as every
  other interactive check in this stage.
- [x] **`ANIMATESPRITE` + the sprite subsystem** (2026-08-10) —
  originally found blocked (see the earlier note, preserved below) and
  scoped as its own real project rather than folded into the
  suspend/resume batch; picked up separately once `PAUSE` closed out
  every other named suspend/resume item.
  Ported the five ordinary (non-suspending) sprite commands —
  `SETSPRITE`/`LOADSPRITE`/`LOADSPRITESHEET`/`SETSPRITEFRAME`/
  `STAMPSPRITE` — plus `ANIMATESPRITE` itself, **`vm.c`-only, at the
  user's own call**: matching the WAIT/WAITKEY/INPUT/PAUSE precedent
  exactly, since `bin/logo` no longer runs on `ast_eval` at all, adding
  these to `eval.c` too would only buy extra shadow-diff test
  infrastructure (extending `TurtleSnapshot` again) for a subsystem
  that only ever executes through the live VM now. All five ordinary
  commands needed **zero special `compiler.c` treatment** — confirmed
  directly that `ARG_QUOTED_WORD` arguments already compile through the
  generic `OP_CALL_BUILTIN` fallback path exactly like any other
  expression (an `AST_WORD` node just becomes an ordinary
  `OP_PUSH_WORD`), the same reason `DELETEFILE`/`TEXT`/`SHOW` never
  needed a special branch either — direct ports living as new static
  helpers in `vm.c` (`exec_loadsprite`/`exec_loadspritesheet`/
  `exec_setsprite`/`exec_setspriteframe`/`exec_stampsprite`), reusing
  already-public `LogoApp`/`Turtle` fields and the
  `app->load_sprite_image` callback-pointer seam (same shape as
  `request_redraw` — silently a no-op when `NULL`, e.g. headless
  tests). Deliberately dropped `interpreter.c`'s own `"expected a
  \"name"`/`"expected a \"path"` sscanf-parse-failure errors: this
  pipeline's real grammar-based parser already guarantees a
  syntactically valid name at parse time, the same reason `ERASE`/
  `MAKE`/`DELETEFILE` never emit that class of error here either.
  **`ANIMATESPRITE` itself needed a genuinely new mechanism, the one
  real new piece of this batch**: unlike every suspend point so far
  (each suspends exactly once), `ANIMATESPRITE` can suspend *multiple
  times* per call — once per remaining frame. A new
  `Vm.suspend_frames_remaining` (alongside the already-existing
  `suspend_seconds`, reused here for the per-frame delay) tracks this;
  `OP_ANIMATESPRITE` advances the first frame and suspends
  (`VM_RUN_SUSPENDED_ANIMATESPRITE`) if `delay > 0`, and a new
  `vm_resume_animatesprite` advances one more frame per call, either
  suspending again (frames remain) or finally falling through to a real
  `vm_run(vm->pc)` continuation once done — `ui.c` just re-arms its
  timer each time via the existing `handle_vm_result` dispatch (a new
  `on_animatesprite_timeout`, mirroring `on_wait_timeout`), needing no
  new orchestration logic there beyond one more `VmRunResult` case.
  Uses the same "block concurrent submission" `g_suspended_run` slot as
  `WAIT` (not `PAUSE`'s reentrant stack) — animating doesn't want other
  commands interleaving mid-sequence any more than `WAIT` does. If
  `delay <= 0`, the whole frame loop runs synchronously with no suspend
  at all — a faithful port of `interpreter.c`'s own real behavior, not
  a "fix": its own busy-wait loop is the only thing that ever gives GTK
  a chance to actually repaint an intermediate frame, so a `delay <= 0`
  animation was never really animated from the user's own visual
  perspective there either, just an instant jump to the final frame.
  Same `vm_run_depth`-based template refusal as every other suspend
  opcode. 11 new headless `tests/test_vm.c` cases — the ordinary
  commands' error/no-op paths mirror `tests/test_interpreter.c`'s own
  sprite corpus exactly (`load_sprite_image` is `NULL` in test apps
  there too, so no sprite is ever really registered via `LOADSPRITE`);
  the two that matter most directly poke `app`'s own sprite fields
  (`sprite_count`/`sprite_names`/`sprite_frame_cols`/`rows`,
  `Turtle.sprite_index`/`sprite_frame`) to set up "a sprite exists"
  without the GUI-only load path — confirming a 3-frame animation
  suspends exactly 3 times with the right frame advancing each time
  before finally completing, and that a zero-delay animation advances
  all 5 frames (wrapping correctly mod 4) synchronously in one shot.
  Confirmed clean under AddressSanitizer (same one pre-existing,
  unrelated crash); all 6 `make test` suites pass; `bin/logo`/`bin/logi`
  build warning-free and run without crashing. The real image-decoding
  half (an actual `LOADSPRITE`d image drawing correctly, sprite-sheet
  grid slicing) still needs the user to confirm manually against the
  running app, same limitation `test_interpreter.c`'s own sprite corpus
  already accepted for `eval_logo`.
  **A separate, previously-unnoticed gap found while scoping this
  (2026-08-10), not fixed here**: `TEXT`/`SHOW`/`DELETEFILE`/`LOAD`/
  `SAVE` and the whole file-I/O family (`OPENREAD`/`OPENWRITE`/
  `OPENAPPEND`/`READLINE`/`EOF?`/`DIRECTORY`/`CLOSE`/`FILEPRINT`) are
  all in `parser.c`'s own `BUILTIN_SIGNATURES` (from Stage 1's own
  batches) but were **never wired into `vm.c`'s `call_builtin`** —
  confirmed directly, zero hits. They parse fine but silently no-op
  through the VM today, falling through to the same defensive fallback
  that swallows a genuinely unknown builtin. Flagged clearly rather
  than silently left for someone to discover the hard way; the user
  chose to keep it as its own separate, similarly-sized follow-up batch
  rather than fold it into this one.
  <details><summary>Original "blocked" note (2026-08-10, superseded above)</summary>
  Scoping it first found it operates on `Turtle.sprite_index`, which
  only `SETSPRITE` ever sets away from its default `-1` — and
  `SETSPRITE`/`LOADSPRITE`/`STAMPSPRITE`/`SETSPRITEFRAME` didn't exist
  anywhere in `parser.c`/`eval.c` at all (sprites were deliberately out
  of Stage 1's own scope entirely, grouped with sound/`WINDOW` as "a
  large GTK/SDL-state surface disproportionate to a research
  evaluator's needs"). So `ANIMATESPRITE` couldn't do anything real
  through this pipeline at the time regardless of its own suspend/
  resume design. Porting the whole sprite subsystem was its own
  separate, larger scoping question; the user chose to leave it blocked
  rather than take that on as part of the suspend/resume batch — then
  picked it up as its own project right after, resolved above.
  </details>
- [x] **`PAUSE`/`CONTINUE`/`CO`** (2026-08-10) — the last actual open
  suspend/resume item, and it turned out to compose cleanly with
  `WAIT`'s own design rather than needing anything fundamentally new:
  mechanically, `PAUSE` suspends and resumes exactly like `WAIT` (no
  value produced, resumed via the plain `vm_resume` — no dedicated
  `vm_resume_with_*` needed), reusing the already-shared
  `app->pause_depth` field and printing `interpreter.c`'s own exact
  `"Paused (level N). Type CONTINUE to resume."` message. `CONTINUE`/
  `CO` needed no opcode or suspend machinery at all — they're ordinary
  zero-arg builtins that just decrement `app->pause_depth` (a new
  `exec_continue` in `vm.c`'s `call_builtin`), printing `"CONTINUE:
  nothing is paused"` if already `0`, matching `do_continue` exactly.
  **The one genuinely new piece was reconciling this with the previous
  batch's own concurrency fix**: `WAIT`/`WAITKEY`/`INPUT` all correctly
  *block* a second concurrent script submission (`run_logo_script`'s
  `g_suspended_run != NULL` check), but `PAUSE`'s entire point is the
  opposite — the REPL must keep accepting and running ordinary commands
  while paused, so the user can inspect/modify the paused call's own
  live variables (which falls out for free: variable storage is already
  shared global state via `app->scopes[]`, not per-`Vm`, matching
  `eval_logo`'s own reentrant-`eval_logo`-call design exactly). Solved
  with a **separate** stack (`ui.c`'s new `g_paused_runs`, deliberately
  not folded into the single-slot `g_suspended_run`), so a non-empty
  pause stack never trips `run_logo_script`'s own concurrency guard.
  Nesting (a command run while paused can itself hit `PAUSE` again) is
  fully supported, at the user's own call after weighing it against a
  narrower single-level-only first pass — `app->pause_depth` already
  naturally supports it (each nested `PAUSE` captures a strictly higher
  level), so a small stack instead of one slot wasn't much more code.
  A new `maybe_resume_paused_runs` (`ui.c`), called at the tail of
  `handle_vm_result` after every single script action, checks the
  innermost (highest-level) paused run's own captured level against the
  current `app->pause_depth` and resumes it once `CONTINUE` has dropped
  it low enough — recursing back through `handle_vm_result` so a chain
  of nested pauses unwinds one `CONTINUE` at a time, matching
  `interpreter.c`'s own busy-wait condition (`pause_depth >= my_level`)
  exactly, just checked on each transition instead of polled. Ctrl+C-
  based force-unpause (`interpreter.c`'s own `g_interrupt_requested`
  handling) is deliberately left out, matching the already-documented,
  broader gap that interrupt-checking isn't wired into this pipeline at
  all yet. 5 new `tests/test_vm.c` cases — suspend/resume with the
  right captured level, `CONTINUE` with nothing paused, the same
  template-refusal guard as `WAIT`/`WAITKEY`/`INPUT`, and (the one that
  actually matters most for this design) two independently compiled/run
  scripts sharing one `LogoApp`, confirming `PAUSE` nesting captures
  strictly increasing levels and a single `CONTINUE` resumes exactly
  the innermost one, in the right order — all headless, no GTK needed.
  Confirmed clean under AddressSanitizer (same one pre-existing,
  unrelated crash); all 6 `make test` suites pass; `bin/logo`/`bin/logi`
  build warning-free and `bin/logo` was confirmed to launch/run without
  crashing. The interactive reentrant-nesting check itself (pausing,
  typing another command that inspects/modifies a live variable, typing
  `CONTINUE`, confirming it resumes correctly) needs the user to
  confirm directly, same reason as every other interactive check in
  this stage.
  **This closes out every named suspend/resume item on Stage 2's own
  checklist.** `ANIMATESPRITE` itself was picked up as its own separate
  project right after (see below) — it landed too.
- [x] **`TEXT`/`SHOW`/`SAVE`/`DELETEFILE` + general file I/O** (2026-08-10)
  — the gap flagged above, fixed. `OPENREAD`/`OPENWRITE`/`OPENAPPEND`/
  `READLINE`/`EOF?`/`DIRECTORY`/`CLOSE`/`FILEPRINT`/`DELETEFILE`/
  `TEXT`/`SHOW`/`SAVE` (12 of the 13 originally flagged; `LOAD` excluded
  — see below) now all work through the VM. Unlike sprites/`PAUSE`,
  these already existed in `eval.c` for `ast_eval` — the fix was the
  established `eval_X_value`-core split (same pattern as lists/
  property-lists/turtle-drawing): each `do_X` in `eval.c` split into a
  thin wrapper (unchanged, still evaluates its own arguments from raw
  AST indices for `ast_eval`'s sake) plus a new `eval_X_value` core
  taking already-evaluated `EvalValue`s, exposed via `eval.h` for
  `vm.c`'s `call_builtin` to call directly — zero new opcodes, zero
  `compiler.c` changes, confirmed via a full `make test` run (all 6
  suites) that the refactor changed nothing about `ast_eval`'s own
  existing behavior. 21 new headless `tests/test_vm.c` cases, direct
  single-engine VM tests rather than `shadow_diff_vm`: real file I/O
  against `build/` means running the identical script through both
  engines back-to-back (`shadow_diff_vm`'s own convention) would double
  up non-idempotent side effects (a second `DELETEFILE`/`OPENAPPEND` on
  the same real file behaves differently the second time), so these
  mirror `tests/test_eval.c`'s own file-I/O corpus almost verbatim,
  just pointed at the VM. Confirmed clean under AddressSanitizer (same
  one pre-existing, unrelated crash); all 6 `make test` suites pass;
  `bin/logo`/`bin/logi` build warning-free and run without crashing.
  **`LOAD` deliberately excluded, a bigger piece than a value-taking-core
  refactor**: `do_load` runs a loaded file's own top-level statements via
  `exec_block` (this tree-walker) — the VM has no equivalent hook for
  that without its own dedicated opcode and a runtime nested-compile-
  and-run mechanism (mirroring how `MAP`/`FILTER`/`REDUCE`/`FOREACH`
  templates and the parser's own eager-`LOAD`-following pre-pass each
  solve a similar problem), a real design question of its own, not
  implemented as part of this batch.
  **A much bigger version of the same gap, found via this batch's own
  testing, not gone looking for**: while writing a test for `DIRECTORY`,
  `LIST?` turned out to be silently broken through the VM too — a
  scripted audit (diffing every name in `parser.c`'s own
  `BUILTIN_SIGNATURES` against every name `vm.c`'s `call_builtin` and
  `compiler.c`'s special-form branches actually recognize) found **35**
  such names total, not just the ones already known: math operators
  (`ABS`/`ACOS`/`ARCTAN`/`ASIN`/`COS`/`EXP`/`LN`/`LOG`/`MOD`/`POWER`/
  `RANDOM`/`ROUND`/`SIN`/`SQRT`/`TAN`), list/word operators (`CROSS`/
  `DOT`/`FLATTEN`/`MEMBER?`/`PARSE`/`PICK`/`SUBST`), the type
  predicates (`ARRAY?`/`LIST?`/`NUMBER?`/`WORD?`), deferred execution
  (`APPLY`/`RUN`), a handful of turtle-command short aliases whose full
  names already work (`HT`/`SETBG`/`SETH`/`SETPC`/`SETPW`/`ST` —
  `HIDETURTLE`/`SETBACKGROUND`/`SETHEADING`/`SETPENCOLOR`/
  `SETPENWIDTH`/`SHOWTURTLE` are all fine, only their short forms
  aren't), and `LOAD` itself (already known, see above). All silently
  no-op through the VM today the same way `TEXT`/`SHOW`/file-I/O did.
  Reported to the user in full rather than fixed ad hoc as each one
  happened to surface in a test; scope/priority for the remaining 34 is
  the user's own call, not yet decided.
- [x] **Math operators — the first, highest-value slice of the 35-name
  audit** (2026-08-10): `ABS`/`SQRT`/`POWER`/`RANDOM`/`ROUND`/`MOD`/
  `SIN`/`COS`/`TAN`/`ASIN`/`ACOS`/`ARCTAN`/`LN`/`LOG`/`EXP` all now work
  through the VM. Even simpler than the file-I/O batch: every one of
  these is a pure function needing no `app`/`pool` at all (not even
  `RANDOM`, which calls the process-global `random_below` directly) —
  the same shape `eval_int_value` (`REPEAT`'s own truncation helper)
  already established, so each new `eval_X_value` core is a one-line
  `EvalValue -> EvalValue` (or two-arg) wrapper around the existing
  `fabs`/`sqrt`/`pow`/… call, with the `do_X` wrapper in `eval.c`
  reduced to "evaluate my own argument(s), forward to the core." Zero
  new opcodes, zero `compiler.c` changes, confirmed via a full
  `make test` run that `ast_eval`'s own behavior didn't change. 5 new
  `tests/test_vm.c` cases, all ordinary `shadow_diff_vm` calls (no
  filesystem side effects to worry about, unlike file-I/O) except
  `RANDOM` — both engines draw from the same process-global RNG stream,
  so running one script through both back-to-back would consume
  different draws and spuriously "diverge"; tested separately as a
  single-engine bounds check instead. Confirmed clean under
  AddressSanitizer (same one pre-existing, unrelated crash); all 6
  `make test` suites pass; `bin/logo`/`bin/logi` build warning-free and
  run without crashing. 20 names remain from the original 35-name
  audit: list/word operators (`CROSS`/`DOT`/`FLATTEN`/`MEMBER?`/
  `PARSE`/`PICK`/`SUBST`), the type predicates (`ARRAY?`/`LIST?`/
  `NUMBER?`/`WORD?`), `APPLY`/`RUN`, the turtle-command short aliases
  (`HT`/`SETBG`/`SETH`/`SETPC`/`SETPW`/`ST`), and `LOAD` itself.
- [x] **The remaining 20 names — closing out the 35-name audit
  entirely** (2026-08-10): type predicates, list/word operators, and
  the turtle short aliases were more of the same mechanical
  `eval_X_value`-core work (zero new opcodes) — `ARRAY?`/`LIST?`/
  `NUMBER?`/`WORD?` are one-line `ValueType` tag checks, same shape as
  every prior batch; `CROSS`/`DOT`/`FLATTEN`/`MEMBER?`/`PARSE`/`PICK`/
  `SUBST` needed only `app` (list_pool/`random_below`), never `pool`;
  `HT`/`ST`/`SETH`/`SETPC`/`SETPW`/`SETBG` were just `||` additions to
  their already-working full-name branches. `APPLY`/`RUN`/`LOAD` were
  the genuinely new pieces.
  **`APPLY`** resolves its callee dynamically (by name, via
  `find_proc_def`, not a prototype chain), so it needed its own opcode
  — mechanically almost identical to `OP_SEND`'s own success path (push
  a `VmFrame`, jump into the resolved procedure), but with a real
  twist: `APPLY` never hands back a value at all, confirmed in
  `docs/LANGUAGE.md`, even when the applied procedure itself calls
  `OUTPUT` — a new `OP_VOID_DISCARD` (pop whatever's there, whether
  `APPLY`'s own failure-path placeholder or the callee's real result,
  push void) enforces that uniformly, on every path.
  **`RUN`/`LOAD` needed a genuinely new mechanism**, the one piece of
  this whole 35-name project requiring real new architecture: both
  execute a *whole statement sequence*, not a single expression, via
  `interpreter.c`'s own `exec_block` (the tree-walker) — no VM
  equivalent existed. New `OP_RUN`/`OP_LOAD` re-lex/parse/`compile_program`
  the given source into a fresh, independent `BytecodeChunk` at
  runtime, then run it via a *recursive* `vm_run` call sharing this
  same `Vm`'s own stack/frames — the same trick `MAP`/`FILTER`/`REDUCE`/
  `FOREACH` templates already established, just against a genuinely
  separate chunk instead of a shared one. `RUN` keeps `do_run`'s own
  `app->run_depth`/`MAX_RUN_DEPTH` cap (a self-referential `RUN` would
  otherwise blow the C call stack); `LOAD` deliberately has none,
  matching `do_load`'s own documented asymmetry. `LOAD`'s own path
  stays a compile-time literal (`.text`, same `ARG_QUOTED_WORD` shape
  as `OP_ERASE`'s procedure name) — the parser's own eager-`LOAD`-
  following pre-pass had *already* solved making a loaded file's own
  procedures callable at compile time (the original `LOAD` cross-
  boundary-call fix); this recursive call's only remaining job is the
  loaded file's own top-level statements.
  **A real, if narrow, correctness wrinkle found while designing this,
  not discovered by accident**: because `RUN`/`LOAD`'s own recursive
  `vm_run` call uses a *different* chunk than the outer one (unlike a
  template's shared-chunk body), a bare `OUTPUT`/`STOP` at the
  run/loaded snippet's own top level (not inside its own `TO...END`)
  would pop a frame belonging to the enclosing real procedure and set
  `pc` to an index only meaningful in the *outer* chunk — the same
  `frame_floor` mitigation `MAP`/`FILTER`/`REDUCE`/`FOREACH` templates
  already accept as a documented gap applies here too, detecting (not
  fully preventing) it after the fact. Also generalized the 5 existing
  suspend-refusal messages (`WAIT`/`WAITKEY`/`INPUT`/`PAUSE`/
  `ANIMATESPRITE`) to mention `RUN`/`LOAD` alongside templates, since
  `vm_run_depth > 1` is now reachable through either path, and updated
  `vm.h`'s own `vm_run_depth` comment to match.
  15 new `tests/test_vm.c` cases. Unlike the earlier file-I/O batch,
  ordinary `shadow_diff_vm` was safe for every one of these, including
  `LOAD` — it only ever reads its file, never writes or deletes, so
  running the same script through both engines back-to-back never
  double-mutates anything on disk (`APPLY`/`RUN` never touch the
  filesystem at all). Confirmed clean under AddressSanitizer (same one
  pre-existing crash, confirmed via the same "which test printed last"
  isolation as every earlier batch — not assumed); all 6 `make test`
  suites pass; `bin/logo`/`bin/logi` build warning-free and run without
  crashing.
  **This closes the 35-name audit entirely — a scripted re-run confirms
  zero `parser.c`-declared builtins remain unreachable from `vm.c`.**

## Phase 6 — MultiLogo-style concurrent turtle agents

Scoped 2026-08-10 (see `docs/CONCURRENT_AGENTS_DESIGN.md` for the full
design record — this entry just points at it, same convention Phase 5's
own bytecode-VM entry used for `docs/BYTECODE_VM_DESIGN.md`). Flagged
directly in `docs/FEATURE_ATLAS.md`'s own survey as needing "cooperative
scheduling inside `eval_logo` — a fundamentally different execution
model" — no longer true now that the bytecode VM's explicit `pc`/frame
array and real suspend/resume mechanism exist. `TELL` is a different,
smaller thing (switches which turtle a single sequential program steers,
one statement at a time); MultiLogo's own agents are genuinely
interleaved, each with its own independent call stack.

Three resolved decisions: (1) a full rework — per-agent scope storage
plus fixing the other confirmed shared-state hazards (`current_turtle`,
`throw_requested`/`throw_tag`, `run_depth`) — rather than a narrower
"agents can only yield at their own top level" pass, which would have
made `WAIT`/`PAUSE` effectively unusable inside an agent's own procedure
calls; (2) a spawned agent auto-gets a fresh turtle at spawn time; (3)
`PAUSE` inside an agent is refused with a clear message for now (no
agreed per-agent semantic yet — pause just that agent, or the whole
swarm?), the same "documented gap, not silent corruption" spirit as
every other `vm_run_depth`-gated refusal.

- [x] **First slice** (scoped 2026-08-10, shipped 2026-08-10, see
  `docs/CONCURRENT_AGENTS_DESIGN.md`'s own "Progress" section for the
  full writeup): three new opcodes `OP_LAUNCH`/`OP_AWAIT`/`OP_YIELD`
  (`bytecode.h`/`vm.c`/`parser.c`/`compiler.c`) plus three new
  `VmRunResult` variants and `Vm.launch_target_pc` (`vm.h`); a new
  `Agent` struct and a synchronous, headless round-robin `scheduler_run`
  in new files `src/agent.h`/`src/agent.c` — not `ui.c`, since decision
  #4 (`YIELD` is explicit-only, not automatic per loop iteration) means
  this slice never needs a real GTK timer/keypress at all; one new
  branch in `ui.c`'s `run_logo_script`. The context switch is a plain
  save/restore swap of `app->scopes`/`scope_depth`/`throw_requested`/
  `throw_tag`/`run_depth`/`current_turtle` — `find_var` and every
  existing opcode needed zero changes. `WAIT`/`WAITKEY`/`INPUT`/`PAUSE`/
  `ANIMATESPRITE` inside an agent are deliberately deferred together,
  reported as an explicit error rather than a silent hang. 7 new tests
  in `tests/test_agent.c` (`make test-agent`, folded into `make test`),
  including two isolation proofs (nested-call local variables and
  turtle selection both provably don't leak between two interleaved
  agents). Two real bugs found and fixed during implementation, not
  assumed away: the very first `LAUNCH` wasn't spawning its own target
  agent (caught by the first test run — fixed by factoring a shared
  `spawn_agent` helper called once up front, not just from inside the
  round-robin); `handle_vm_result` didn't handle the three new
  `VmRunResult` variants when reached via a *later* resume rather than
  a script's initial run (caught by `-Wswitch` — fixed with an explicit
  "not yet supported" `default:` case). Verified via `make test` (7
  suites), standalone ASan builds of `test_vm`/`test_agent` (the latter
  fully clean; the former's one failure is the same pre-existing,
  unrelated recursion-depth crash documented for every earlier batch),
  and a warning-free full build. Still deferred, not silently dropped:
  passing arguments to a launched agent, automatic per-loop yielding,
  and mixing an ordinary top-level suspend/resume with a later `LAUNCH`.
- [x] **Agent arguments** (scoped and shipped 2026-08-10 — see
  `docs/CONCURRENT_AGENTS_DESIGN.md`'s own "Agent arguments" section
  for the full writeup): `LAUNCH "procname arglist` (breaking change,
  matching `APPLY`'s own convention exactly, `[]` for none), built by
  reusing `eval_push_scope_for_call` to construct the launched agent's
  own first scope directly in `exec_launch` rather than teaching
  `agent.c` any new binding logic. Also fixes a real bug found while
  scoping this: `LOCAL` used directly inside a launched procedure's own
  top level used to silently fall back to a global (no scope was ever
  pushed for a zero-arg launch before this) — closed for every launch,
  not just argument-taking ones, since every launch now gets a real
  scope pushed. `examples/concurrent_agents.logo` redesigned (not just
  migrated) to actually demonstrate argument passing. 6 new
  `tests/test_agent.c` cases, including a direct test of the `LOCAL`
  fix. Verified via `make test` (7 suites), standalone ASan builds of
  `test_vm`/`test_agent` (both fully clean), a warning-free full build,
  and a live `bin/logo` launch of the redesigned demo.
- [x] **Mixing top-level suspend/resume with a later LAUNCH** (scoped
  and shipped 2026-08-10 — see `docs/CONCURRENT_AGENTS_DESIGN.md`'s own
  section of the same name): moved the `LAUNCH`-to-`scheduler_run`
  hand-off from a special early-return branch in `run_logo_script`
  (which only ever saw a script's very first `vm_run` call) into
  `handle_vm_result` itself — the one dispatch point every resume path
  (`on_wait_timeout`, `on_entry_key_pressed`, `on_animatesprite_timeout`,
  `maybe_resume_paused_runs`) already funnels through, so `LAUNCH`
  reached after an earlier `WAIT`/`WAITKEY`/`INPUT`/`PAUSE`/
  `ANIMATESPRITE` resumes is now handled identically to `LAUNCH` as a
  script's first suspend, not refused. `ui.c`-only change; `vm.c`/
  `agent.c` untouched. Verified against the real `bin/logo` app (this
  path is GTK-timer-driven, so no headless test can reach it) via a
  file-I/O progress trace (`OPENWRITE`/`FILEPRINT`/`CLOSE`) proving the
  full `WAIT` → resume → `LAUNCH` → `AWAIT` → continue sequence lands
  correctly, and confirming a bare `AWAIT` with no `LAUNCH` still
  reports its own (reworded) explicit error rather than hanging or
  crashing. A real, separate, pre-existing bug was found and reported
  (not fixed, out of scope) while debugging this: any word literal over
  63 characters in source text is silently truncated by `INSTR_MAX_TEXT`
  (`bytecode.h`, 64 bytes) — unrelated to `LAUNCH` or agents, present
  since `OP_PUSH_WORD` was first written.

## Bytecode save/load/assembler

Staged the way Stage 1/2 and Phase 6 were (user: "something that would
be cool now though is the ability to save the bytecode to file, load
bytecode, and even have an assembler for it", 2026-08-11): A, the
architectural precondition both later stages need, is done; B/C/D are
scoped but not yet built.

- [x] **Stage A — self-contained `BytecodeChunk`** (shipped 2026-08-11;
  full writeup in `docs/BYTECODE_VM_DESIGN.md`'s own entry of this
  name). `OP_CALL_PROC`/`OP_SEND`/`OP_APPLY`/`OP_LAUNCH` no longer read
  `pool` at runtime at all — `chunk->procs[]` (`ProcAddr`) now carries
  its own copy of `param_count`/`param_names`/`source_text`, and
  `OP_PUSH_LIST_LITERAL` reads a rendered-to-text list literal off a new
  `chunk->list_literals[]` table instead of an AST node index. Purely
  internal: no new syntax, no new builtins, `make`/`make test` output
  byte-for-byte unchanged. `TEXT`/`SAVE`/`SHOW` deliberately stay
  `pool`-dependent (out of scope for this stage, still fully
  functional) — flagged as a real, separate follow-up if a future stage
  wants loaded-only bytecode to support them too.
- [x] **Stage B — disassembler** (chunk → text; shipped 2026-08-11).
  `bytecode_disassemble(chunk, FILE *out)` (bytecode.h/.c, no
  VM/LogoApp dependency, same standalone shape as the rest of
  bytecode.c) prints a PROCS section (every still-defined entry's
  name/start_pc/param_names/source_text) followed by a labeled CODE
  listing — each instruction's own `word_literals[]`/`list_literals[]`
  index resolved into its literal text inline, jump targets rendered as
  `@N`, and every proc's own `start_pc` doubling as a `name:` label in
  the listing. `bytecode_opcode_name(OpCode)` (a plain name table, one
  case per enum member, no `default:` so a missing case is a compiler
  warning) backs the CODE listing's own mnemonics and is exposed for
  Stage C's eventual reverse lookup. Covered by a new, GTK-free
  `tests/test_bytecode.c` (`make test-bytecode`) that lexes/parses/
  compiles real snippets and asserts on the disassembly text.
- [x] **Stage C — assembler** (text → chunk, including label resolution
  for jump targets; shipped 2026-08-11). `bytecode_assemble(text,
  chunk, error, error_size)` (bytecode.h/.c) accepts exactly what
  `bytecode_disassemble` produces — its own output round-trips
  unchanged — plus hand-written symbolic labels: any `name:` line in
  `CODE:` (a proc's own entry point, or an ordinary bare one) can be
  referenced by name from a jump-target operand instead of (or as well
  as) the disassembler's own literal `@N` form, resolved via a genuine
  two-pass assembly so a label may be referenced before its own
  definition. A proc's `start=` field in its `PROCS:` entry is read but
  never trusted — its real `start_pc` always comes from resolving its
  own `<name>:` label — and the `CODE:` section's own `<pc>:` address
  column is optional and, when present, likewise not trusted; both
  choices mean hand-inserting/removing instructions never requires
  renumbering anything by hand. Malformed input (unknown opcode,
  undefined/duplicate label, a proc with no matching label, a params-
  list/argc mismatch, a full fixed table) fails loudly with a one-line,
  line-numbered message rather than silently producing a broken chunk.
  Verified via structural round-trip tests (disassemble → assemble →
  compare instructions/proc metadata) for arithmetic, nested list
  literals, `IF`, recursion, multiple procedures, and a compiled `MAP`
  template, hand-written-label and error-path tests (all in
  `tests/test_bytecode.c`), and — the one check that genuinely needs
  the full VM — a new `test_vm.c` test that disassembles a compiled
  `FACT` recursion, reassembles it, and runs the reassembled chunk
  against a completely empty `AstPool` (no original AST at all),
  confirming its output matches the original run exactly: proof a
  hand-assembled or reloaded-from-disk program can actually execute
  standalone.
- [x] **Stage D — `SAVEBYTECODE`/`LOADBYTECODE`** builtins wiring B/C
  together, with round-trip tests (shipped 2026-08-11). Ordinary
  `ARG_QUOTED_WORD` builtins (same shape as `SAVE`/`LOAD`/`DELETEFILE`)
  — no dedicated opcode needed, and no `INSTR_MAX_TEXT`-style
  truncation risk on the path argument either, since an ordinary
  builtin's arguments always compile through the generic
  `compile_expr`-per-argument path into `OP_PUSH_WORD`'s own
  `word_literals[]` table, unlike `LOAD`/`ERASE`'s own dedicated-opcode
  `Instr.text[64]` fields. `SAVEBYTECODE "path` disassembles the
  currently-executing chunk (`call_builtin` gained a `BytecodeChunk *`
  parameter to reach it) to `path`. `LOADBYTECODE "path` reads the
  file, assembles it, and runs it via a recursive `vm_run` call against
  an empty `AstPool` — the assembled chunk needs nothing else. Closed a
  real gap found while building this: `compile_program`'s own top-level
  entry point isn't `0` in general (procedure bodies compile first) and
  wasn't recoverable from disassembled text at all — fixed by adding
  `BytecodeChunk.start_pc` plus a `START:` line to the Stage B/C text
  format (required, resolved via the same label mechanism as any jump
  target). **Real, documented limitation**: unlike `LOAD` (whose own
  procedures the parser eagerly hoists into the *caller's* AstPool at
  compile time), a procedure inside a `LOADBYTECODE`d file is not
  callable from the rest of the script that loaded it — there's no
  Logo source to hoist from, just a `LOADBYTECODE`d file always runs as
  its own self-contained unit. VM-only — `eval_logo`/`ast_eval` have no
  bytecode chunk to save at all, and gracefully report "I don't know
  how to SAVEBYTECODE" via already-existing machinery if either engine
  is ever asked (neither is reachable from `bin/logo`). Verified: a
  live end-to-end run (compile a real multi-procedure recursive
  program, `SAVEBYTECODE` it, then in a completely separate process-
  like session with zero compile step, `LOADBYTECODE` it and confirm
  identical output) plus dedicated `test_vm.c` tests for the real-file
  round trip and every error path (missing file, malformed file,
  unwritable path) — all 8 `make test` suites pass, warning-free
  rebuild, ASan-clean, live `bin/logo` launch confirmed.

This closes the initiative the user asked for: bytecode can now be
saved to file, loaded back, and hand-assembled.

- [x] **GUI menu equivalents** (shipped 2026-08-11, added after Stage
  D): `ui.c`'s native macOS **File** menu gained *Save Bytecode…* and
  *Load Bytecode…*, same `GtkFileDialog` pattern as the existing
  Open/Save/Export-as-PNG items (`.lgb`-filtered, `⌘⇧S`/`⌘⇧O`). *Save
  Bytecode* compiles (but deliberately does not run) the entry box's
  own current text and saves the disassembled result — no side effects
  from a menu click, unlike the builtin, which only ever runs
  mid-script. *Load Bytecode* runs a chosen file through the same
  suspend/resume machinery every other script in this app already
  uses. See `docs/BYTECODE_VM_DESIGN.md`'s own progress-log entry of
  this name for the full design writeup.

## CLI ergonomics

- [x] **Run a `.logo` file straight from the command line** (shipped
  2026-08-10): `bin/logo script.logo` loads and runs it immediately on
  startup instead of always opening a blank window. Uses GApplication's
  own `"open"` signal (`G_APPLICATION_HANDLES_OPEN`) rather than parsing
  argv by hand, so multi-file-open semantics and GTK's own arg handling
  stay correct for free; a new `build_main_window` helper factors the
  window/widget construction out of `logo_activate` so the new
  `logo_open` handler (`ui.c`) can share it, then reuses the exact same
  load+run logic `File > Open…` already had (`on_file_open_response`).
  No argument still opens a blank window as before. Verified live (both
  launch modes checked via process liveness, not GUI screenshot, per
  this project's no-GUI-automation constraint) with `examples/
  concurrent_agents.logo` (the synchronous scheduler path) and
  `examples/animate_sprite.logo` (the real-GTK-timer suspend/resume
  path) — both loaded, ran to completion, and stayed alive with no
  crash.

- [x] **`SETSPEED`/`SPEED` + `--speed`** (shipped 2026-08-10): a
  turtle-motion throttle for watching a drawing unfold step by step, or
  slowing things down to debug — `SETSPEED seconds` pauses the turtle
  that long after every motion command (`FD`/`FORWARD`/`BK`/`BACK`/
  `RT`/`RIGHT`/`LT`/`LEFT`/`SETXY`/`SETX`/`SETY`/`SETHEADING`/`SETH`/
  `HOME`/`ARC`) from then on; `SPEED` reads it back; `0` (the default)
  is instant, unchanged from every prior release. `--speed <seconds>`
  on the command line sets the same value before a script's first
  instruction runs (works with both launch modes above).
  Reuses `WAIT`'s own real-timer suspend/resume mechanism exactly (a new
  `OP_MOTION_DELAY` opcode + `VM_RUN_SUSPENDED_MOTION_DELAY`, compiled
  in by `compiler.c` right after every motion-command call, resumed by
  `ui.c`'s existing `on_wait_timeout`) — a VM-only feature, same as
  Phase 6's `LAUNCH`/`AWAIT`/`YIELD`, since only the bytecode VM
  (`bin/logo`'s own live engine) has real suspend/resume at all.
  Deliberately differs from `WAIT` in one respect: inside a
  `MAP`/`FILTER`/`REDUCE`/`FOREACH` template, `RUN`, `LOAD`, or a
  concurrent agent, the delay is silently skipped rather than reported
  as unsupported — an automatic per-step throttle the script never
  explicitly asked for at that exact call site doesn't deserve the same
  reported refusal an explicit `WAIT`-like call gets; agent.c's own
  scheduler treats it like `YIELD` (stays READY) rather than tearing the
  agent down, so a global `SETSPEED` can't turn an ordinary `FD` inside
  an agent into a silent killer. 5 new `test_vm.c` cases + 1 new
  `test_agent.c` case; a new `examples/setspeed.logo`; verified via
  `make test` (still 7 suites, all pass), standalone ASan builds of
  `test_vm`/`test_agent` (both fully clean this run — the previously-
  noted pre-existing recursion-depth crash didn't reproduce this time,
  a known flaky/borderline case, not something this change introduced),
  a headless harness confirming the exact suspend count/timing/output
  for `examples/setspeed.logo`, and a live `bin/logo --speed 1.0`
  launch showing CPU dropping back to idle right around the expected
  ~8-second mark for an 8-motion-command script (proxy confirmation,
  not pixel-level proof — GUI screenshot verification is off-limits per
  this project's own established constraint).

## Mouse/keyboard event triggers

- [x] **`ONKEY`/`OFFKEY`/`ONCLICK`/`OFFCLICK`** (shipped 2026-08-11):
  push-based event handlers, registered by procedure name and fired as
  a fresh background invocation whenever a key is pressed (entry box
  focused) or the canvas is clicked — unlike `WAITKEY`, the main script
  never blocks waiting for them. `ONKEY "procname` requires `procname`
  to take exactly one input (`:KEY`, same `gdk_keyval_name` convention
  `WAITKEY`'s own output already uses); `ONCLICK "procname` requires
  exactly three (`:X :Y :BUTTON`, canvas-relative pixels — the same
  coordinate space `SETXY`/`POS` already use — and GDK's own 1/2/3
  left/middle/right button numbering). A missing procedure or an arity
  mismatch is a reported error at *registration* time, leaving any
  previously-registered handler untouched rather than clearing it.
  `OFFKEY`/`OFFCLICK` clear the respective slot; each slot holds at
  most one procedure, a new registration replacing the old (same
  single-slot shape as `SETSPEED`).

  Implementation ended up simpler than the original design sketch (see
  `docs/ROADMAP.md`'s own history) in two ways: it does NOT reuse
  `LAUNCH`'s agent scheduler (that scheduler is synchronous and runs to
  completion in one call, wrong shape for a GTK-event-driven invocation
  that might itself suspend on `WAIT`/etc) — instead each fire is a
  brand-new top-level run through `ui.c`'s existing `handle_vm_result`
  suspend/resume machinery, same as an ordinary REPL submission. And
  the "debounce a busy handler" concern from the original design didn't
  need a dedicated per-handler flag: firing already goes through the
  same `g_suspended_run != NULL` check every ordinary submission uses,
  so a fire during an already-running/suspended script or handler
  invocation is just silently missed for free, no separate bookkeeping
  needed.

  The real new problem this needed solving: an ordinary script run's
  own compiled chunk is normally freed the instant that run finishes
  (`free_suspended_run`), but `ONKEY`/`ONCLICK`'s handler procedure
  still needs to be findable/callable in it *after* the registering
  script has finished (the whole point — register once, keep firing
  indefinitely). Fixed with a `SuspendedRun.owns_chunk` flag (`FALSE`
  only for a fired-handler's own run, whose chunk/result are borrowed,
  not owned) plus two independent `RetainedChunk` slots in `ui.c`
  (`g_onkey_owner`/`g_onclick_owner`, deliberately not one shared slot —
  `ONKEY` and `ONCLICK` can be registered by two different scripts at
  different times, each needing its own chunk kept alive) that
  `handle_vm_result`'s `VM_RUN_HALTED` case adopts a finishing run's
  chunk into (via a `bytecode_find_proc_entry` check against whichever
  handler name is currently set) instead of freeing it, and releases
  (with a pointer-equality check to avoid a double free when both slots
  happen to share one chunk) the moment the corresponding handler is
  cleared. One known, narrow gap left in place rather than chased down:
  a handler registered by a script that also calls `LAUNCH` isn't
  retained, since `scheduler_run`'s own call site frees its chunk
  unconditionally — register `ONKEY`/`ONCLICK` from a script that
  doesn't also `LAUNCH`, if both are needed.

  No new opcodes: both go through the existing generic `OP_CALL_BUILTIN`
  dispatch with `ARG_QUOTED_WORD` procedure-name arguments, the exact
  same shape Stage D's `SAVEBYTECODE`/`LOADBYTECODE` used (see the
  bytecode save/load/assembler entry above) — `src/parser.c`'s
  `BUILTIN_SIGNATURES` table plus two new `vm.c` helpers
  (`exec_onkey`/`exec_onclick`) were the entire compiler-side change.
  7 new `test_vm.c` cases covering registration/arity/missing-procedure/
  clearing (the actual GTK firing path, `ui.c`'s `fire_onkey`/
  `fire_onclick`, is untestable headlessly, same as `WAIT`'s real timer
  or `WAITKEY`'s real keypress already are); a new
  `examples/onkey_onclick.logo`; verified via `make test` (all 8 suites
  pass), a standalone ASan build of `test_vm` (clean with a raised
  `ulimit -s` — the pre-existing recursion-depth crash this project
  already tracks needs more headroom than this environment's default
  stack size to get past, not a new issue), and two live `bin/logo`
  process-liveness launches (the example script, and a smaller smoke
  test covering registration + `OFFKEY`/`OFFCLICK` clearing) confirming
  no crash on startup.

- [x] **`ONMOUSEMOVE`/`OFFMOUSEMOVE`** (shipped shortly after the entry
  above): the third event trigger, firing on every pointer motion over
  the canvas with `:X :Y` (same coordinate space `ONCLICK` uses).
  Exactly the same shape as `ONKEY`/`ONCLICK` throughout —
  `exec_onmousemove` in `vm.c` validates a 2-param procedure at
  registration time, `ui.c` gets a third retained-chunk slot
  (`g_onmousemove_owner`) and `fire_onmousemove`, wired into the
  existing `on_canvas_motion` controller right alongside `MOUSEPOS`'s
  own `mouse_x`/`mouse_y` update.

  The one real question this raised: motion events can fire dozens of
  times a second, so would firing a full background invocation per
  event let them pile up faster than they finish? Turned out not to
  need any new debounce mechanism — `fire_handler`'s existing
  `g_suspended_run != NULL` idle check (added for `ONKEY`/`ONCLICK`,
  see the entry above) already drops a fire outright whenever the
  interpreter isn't idle, which is exactly the behavior a busy handler
  needs; confirmed with a live `examples/onmousemove.logo` run (the
  turtle follows the pointer smoothly, pen down, no runaway pileup or
  lag) rather than only reasoned about.

  `release_retained_chunk` (shared by all three handlers' retained
  chunks) needed a real generalization here, not just a third call
  site: it used to take a single "other" slot to check against for the
  shared-chunk case (two handlers registered by the same script,
  sharing one chunk) -- with three slots now, a chunk shared by all
  three needed checking against BOTH siblings, not one, so it now scans
  a fixed array of all three owners instead of taking an explicit
  "other" parameter. 3 new `test_vm.c` cases (registration, wrong
  arity, `OFFMOUSEMOVE` clearing — mirroring `ONCLICK`'s own three);
  a new `examples/onmousemove.logo`; verified via `make test` (all 8
  suites), a standalone ASan build of `test_vm` (clean, same raised
  `ulimit -s` workaround as before for the pre-existing recursion-depth
  finding), and a live `bin/logo` process-liveness launch of the new
  example.

- [x] **`ONKEYUP`/`OFFKEYUP`/`ONRELEASE`/`OFFRELEASE`** (shipped
  shortly after the entry above): key/button *release* mirrors of
  `ONKEY`/`ONCLICK`, same shape throughout — `exec_onkeyup` requires a
  1-param procedure (`:KEY`), `exec_onrelease` a 3-param one (`:X :Y
  :BUTTON`), both validated at registration time exactly like their
  press-side counterparts. Two more `RetainedChunk` slots in `ui.c`
  (`g_onkeyup_owner`/`g_onrelease_owner`), `fire_onkeyup`/
  `fire_onrelease` wired into a new `on_entry_key_released` callback
  (connected to the entry box's existing `GtkEventControllerKey` via
  its `key-released` signal, alongside `on_entry_key_pressed`'s own
  `key-pressed`) and the existing `on_canvas_button_released` handler
  respectively.

  With five handlers now, `handle_vm_result`'s own "reconcile cleared
  handlers, then adopt a finishing run's chunk for whichever handlers
  it registered" logic was a real repetition smell (five hand-
  duplicated near-identical blocks) rather than something worth
  tolerating for a sixth time — refactored into a small `EventHandlerSlot
  {handler_name, owner}` array plus two short loops, so a future sixth
  handler (a joystick trigger, say) won't need a sixth copy-paste.
  `release_retained_chunk` similarly scans a fixed
  `NUM_EVENT_HANDLER_OWNERS`-sized array of every owner to detect a
  shared chunk, rather than the old two-slot "check one specific other
  owner" shape.

  6 new `test_vm.c` cases (registration/arity/clearing for both, three
  each, mirroring `ONKEY`/`ONCLICK`'s own tests exactly); a new
  `examples/onkeyup_onrelease.logo`; verified via `make test` (all 8
  suites), a standalone ASan build of `test_vm` (clean, same raised
  `ulimit -s` workaround), and a live `bin/logo` process-liveness launch
  of the new example. This closes out the mouse/keyboard side of event
  triggers entirely — only joystick support remains open, and it's a
  separate dependency decision, not more of the same shape.

## Language completeness

Cross-checked every command on
[Terrapin Logo's command reference](https://resources.terrapinlogo.com/logo/commands/)
against `src/parser.c`'s `BUILTIN_SIGNATURES` table and every command
documented in `docs/COMMAND_REFERENCE.md`. Terrapin documents ~470 names;
~341 don't exist here — but most of that gap isn't real. Terrapin is a full
IDE with its own text editor, robot hardware drivers, and native
menu-editing API, none of which apply to this project:

- Workspace/editor commands (`EDIT`/`EDALL`/`EDP`/..., `BURY*`/`UNBURY*`,
  `POT`/`POPS`/`PRIMITIVES`/`COPYDEF`/`DEFINE`) — no in-app text editor to
  bury/unbury/print from
- Menu & window chrome (`APPENDMENU*`, `ONCOMMAND`, `SELECT.FILE`,
  `SETWINSIZE`/`WPOS`, `FULLSCREEN`/`SPLITSCREEN`) — Terrapin's own native
  menu-editing API, doesn't map to a fixed GTK menu
- Hardware (`BLUEBOT.*`, `PROBOT.*`, `OPEN.PORT`) — physical robot drivers
- Bitmap-era graphics (`SHAPE`/`LOADSHAPE`/`STAMP`/`SNAP`/`FONT`/`PLAY`
  (sound)/`BYTEARRAY`/`GRID`) — this app already has a different
  (sprite-based) turtle-image system
- Byte-level serial I/O (`GETBYTE`/`PUTBYTE`/`PEEKBYTE`)

Some more are naming differences, not real gaps: `XCOR`/`YCOR` = our
`GETX`/`GETY`; `AGET`/`ASET` = our `ITEM`/`SETITEM`; `ARRAYP` = our
`ARRAY?`; `MODULO` = our `MOD`.

What's left — every genuinely-missing general-purpose primitive the
comparison actually surfaced — shipped across four batches, 2026-08-11
through 2026-08-12:

- [x] **The easy tier of the 2026-08-11 Terrapin comparison** (shipped
  2026-08-11): 24 small general-purpose builtins, all VM-only
  (`src/parser.c`'s `BUILTIN_SIGNATURES` + a matching `vm.c` dispatch
  branch/helper each — no new opcodes, same generic `OP_CALL_BUILTIN`
  path every ordinary math operator already uses), grouped by category:
  - Math/RNG: `PI` (the constant), `RERANDOM` (reseed `RANDOM` to a
    fixed, reproducible sequence instead of the clock).
  - Char/case: `ASCII`/`CHAR` (single-character <-> code point, plain C
    `char`, not full Unicode — this project's word/text handling is
    byte-oriented throughout), `UPPERCASE`/`LOWERCASE`.
  - Bitwise: `BITAND`/`BITOR`/`BITXOR`/`BITNOT`/`LSHIFT`/`RSHIFT`
    (named for clarity rather than Terrapin's own cryptic `LOGAND`/
    `LOGOR`/`LOGXOR`/`LOGNOT`/`LSH` — a deliberate user preference, not
    a compatibility choice like the rest of this batch; `LSHIFT`/
    `RSHIFT` are two separate, explicitly-directional operators rather
    than Terrapin's single sign-overloaded `LSH`), truncating through a
    64-bit integer (wide enough for real bit patterns, unlike `INT`'s
    own fmod-precision-oriented trunc).
  - Trig: `ARCTAN2`, `SEC`/`CSC`/`COT`/`ASEC`/`ACSC`/`ACOT` — degrees
    in/out, matching `SIN`/`COS`/`TAN`/`ASIN`/`ACOS`/`ARCTAN`'s own
    existing convention exactly.
  - Clock: `TIME`/`DATE`/`MILLISECONDS` (`localtime`/`strftime`/
    `clock_gettime` wrappers).
  - Introspection: `DEFINED?` (looks the word up in the currently
    executing chunk's own `procs[]`, same lookup `ONKEY`/`ONCLICK`
    already use to validate a handler at registration), `TURTLES`
    (`app->turtle_count`).
  - List generation: `RANGE from to` (integers, counting by ±1),
    `SPACEDRANGE from to count` (count equally-spaced numbers) — named
    `RANGE`/`SPACEDRANGE` rather than Terrapin's own `ISEQ`/`RSEQ`, same
    naming-clarity preference as the bitwise ops above. Both build real
    `list_pool` chains via the existing `value_to_node` helper.

  `RERANDOM` needed one real shared-state change: `random_below`'s own
  "seeded?" flag was a function-local static in `interpreter.c`,
  invisible to `vm.c`. Promoted to file scope with a new
  `logo_rerandom()` setter, so a `RERANDOM` called *before* `RANDOM`'s
  own first (lazy, clock-seeded) use doesn't get silently undone the
  first time `RANDOM` finally seeds itself.

  A real, pre-existing language quirk surfaced while testing the shift
  operators, not a bug in this batch: an unparenthesized negative
  literal in a *later* argument position (`RSHIFT 16 -4`) greedily
  parses as binary subtraction continuing the *previous* argument
  (`16 - 4 = 12`, then a missing/defaulted second argument), rather
  than starting a fresh argument — parenthesizing (`RSHIFT 16 (-4)`) is
  required, and is now called out explicitly in
  `docs/COMMAND_REFERENCE.md`. (`LSHIFT`/`RSHIFT` do each still
  tolerate a negative shift count by flipping direction, purely as a
  fallback against undefined C shift behavior — not the intended way to
  ask for the other direction.)

  13 new `test_vm.c` cases (one per builtin or small logical group,
  e.g. all six new trig functions share one test); a new
  `examples/language_completeness.logo`; verified via `make test` (all
  8 suites), a standalone ASan build of `test_vm` (clean, same raised
  `ulimit -s` workaround as prior rounds), and a live `bin/logo`
  process-liveness launch of the new example. `REPCOUNT`/`READWORD`/
  `READCHAR`/`EVAL` remain open in `docs/ROADMAP.md` — each needs real
  design work (VM loop-counter exposure, a stdin protocol, list-running
  semantics respectively), not just a wrapper like this batch.

- [x] **`REPCOUNT`** (shipped shortly after the batch above): the
  innermost currently-running `REPEAT`'s 1-indexed pass number, `-1`
  outside any `REPEAT` — the one item from that batch flagged as
  needing real VM support rather than a plain wrapper, since `REPEAT`'s
  own existing loop-control counter (see this file's "Bytecode VM
  Stage 1+2" background, and `docs/BYTECODE_REFERENCE.md`'s "Loop-
  control stack slots") is a descending, anonymous value-stack slot —
  wrong direction (counts down, not up) and wrong visibility (only the
  exact compiled loop that pushed it can address it by stack depth; a
  procedure called from inside the loop body has no way to know that
  depth at all).

  Solved with three new opcodes (`OP_REPCOUNT_PUSH`/`_INCR`/`_POP`, see
  `docs/BYTECODE_REFERENCE.md`'s new "REPCOUNT bookkeeping" section)
  manipulating a genuinely separate per-`Vm` stack
  (`vm->repcount_stack`/`repcount_depth`, sized like `scopes`/
  `scope_depth`) rather than the value stack — `REPEAT`'s own compiled
  loop now brackets itself with a push/pop pair (even for a zero-pass
  `REPEAT 0 [...]`) and an increment once per completed pass;
  `REPCOUNT` itself is an ordinary 0-arg `OP_CALL_BUILTIN` reading
  `Vm` state directly (unlike every other builtin in `vm.c`, which
  only ever needs `LogoApp`). Being a real per-`Vm` stack rather than a
  value-stack slot is exactly what makes it work correctly from inside
  a procedure called from the loop body (Terrapin's own documented
  dynamic-scope behavior — the callee sees the caller's `REPCOUNT`) and
  through nested `REPEAT`s (each gets its own stack entry, innermost on
  top) without any compile-time depth tracking.

  7 new `test_vm.c` cases (outside any `REPEAT` reports `-1`; counts up
  correctly; reverts to `-1` after the loop ends; a zero-pass `REPEAT`
  never pushes; nested `REPEAT`s each report their own innermost count;
  propagates dynamically into a called procedure; an uncaught `THROW`
  exiting the loop early still balances the push/pop, confirmed by a
  following unrelated `REPEAT` reporting the right count rather than a
  leaked one) plus 1 new `test_bytecode.c` disassemble/assemble round-
  trip case; a new `examples/repcount.logo`; verified via `make test`
  (all 8 suites), standalone ASan builds of both `test_vm` (same raised
  `ulimit -s` workaround as prior rounds) and `test_bytecode` (clean),
  and a live `bin/logo` process-liveness launch of the new example.
  Also caught and fixed a stale doc count while here:
  `docs/BYTECODE_REFERENCE.md`'s own "N members of OpCode" claim was
  cross-checked against `bytecode_opcode_name`'s own exhaustive switch
  (compiler-enforced via `-Wall`'s `-Wswitch`, the only fully reliable
  way to count — a naive grep over the enum's own declaration lines
  undercounts, missing entries with an unusual comment layout) and
  updated from 55 to 58.

- [x] **`READWORD`/`READCHAR`** (shipped shortly after `REPCOUNT`):
  finer-grained file-channel reads than the existing `READLINE`, same
  channel argument and EOF/bad-channel sentinel (an empty word, checked
  via `EOF?` separately). No local precedent existed for word/char
  boundary semantics anywhere in this codebase (not even in the old
  tree-walking engine) — a fresh, deliberately simple design: `READWORD`
  skips leading whitespace (space/tab/CR/LF) then reads to the next
  whitespace or EOF, leaving trailing whitespace for the next call to
  skip; `READCHAR` returns the single next raw byte, whitespace
  included, no skipping. Both VM-only, like everything else added this
  session — no existing `eval.c`/tree-walker wiring touched.

  3 new `test_vm.c` cases (word-by-word across a multi-line file;
  character-by-character including a mid-stream newline check via
  `CHAR 10`; an invalid channel reporting empty for both); a new
  `examples/readword_readchar.logo`; verified via `make test` (all 8
  suites), a standalone ASan build of `test_vm` (clean, same raised
  `ulimit -s` workaround), and a live `bin/logo` process-liveness
  launch confirming the example's own scratch file gets created and
  cleaned up correctly (proof every read succeeded without halting
  early). Only `EVAL` remains open in `docs/ROADMAP.md`.

- [x] **`EVAL`** (shipped shortly after `READWORD`/`READCHAR`): the
  last item from the 2026-08-11 Terrapin comparison, and the one
  genuinely needing its own design, not just a wrapper — Terrapin's
  own description is "runs list and collects outputs." Each element of
  `EVAL`'s own list argument is either a `LIST` (treated as one
  standalone piece of Logo code — the same "list = deferred code"
  convention `RUN` itself uses) or a plain value (passed straight
  through unchanged, since there's no code to run); results collect
  into a new list, always the same length as the input.

  Lives in `eval.c`, not `vm.c` (unlike every other builtin added this
  session) — `EVAL` needs `eval_expr`, `eval.c`'s own private
  tree-walking expression evaluator, to actually run each re-parsed
  code element, the same reason `eval_map_value`/`eval_reduce_value`
  (MAP/REDUCE/FILTER's own runtime-computed-template fallback path)
  already live there and get called directly from `vm.c`'s own
  `call_builtin`. No VM/bytecode involvement at all: each `LIST`
  element is rendered to text via `eval_value_to_text`, re-lexed, and
  parsed as ONE standalone expression via `logo_parse_expr` (the same
  public entry point `logo_parse_expr`/`logo_parse_condition` MAP/
  FILTER/REDUCE's own runtime-template fallback already uses for
  exactly this "re-parse a snippet at runtime" need), then evaluated
  directly via `eval_expr` — reusing `eval_apply_template_expr`'s own
  shape (shared scratch `ParseResult`, `MAX_TEMPLATE_TOKENS` budget)
  minus the "?" substitution step, since there's no template here,
  only code.

  A design question resolved by testing rather than guessing: what
  happens to a code element that's a *command* (e.g. `PRINT`), not an
  operator, and so doesn't itself produce a value? Turns out
  `logo_parse_expr` doesn't reject it at parse time (any known callable
  name parses), it runs (including its own side effect), and then
  `eval_expr`'s own existing "didn't output a value" error path fires —
  the *same* error any command misused in expression position already
  reports elsewhere in this language, not something `EVAL` needed to
  invent. That element's own slot in the result list just comes back
  empty, keeping `EVAL`'s own "one output per input element" guarantee
  intact rather than shortening the list.

  This closes out the entire 2026-08-11 Terrapin comparison — every
  genuinely-missing general-purpose primitive it surfaced has now
  shipped. The comparison's own out-of-scope survey (workspace/editor
  commands, menu/window chrome, hardware, bitmap-era graphics, byte-
  level serial I/O — none of which apply to this project) and naming-
  difference notes (`XCOR`/`YCOR` = `GETX`/`GETY`, etc.) have moved
  here from `docs/ROADMAP.md` along with this entry, now that nothing
  from the comparison remains open there.

  5 new `test_vm.c` cases (code elements collected correctly; a plain
  value passing through unchanged; an empty list; a non-list argument;
  output length matching input length even when a code element is a
  command with no value of its own); a new `examples/eval.logo`;
  verified via `make test` (all 8 suites), standalone ASan builds of
  both `test_vm` (clean, same raised `ulimit -s` workaround) and
  `test_eval` (`eval.c`'s own suite, exercised since this shipped
  there — also clean), and a live `bin/logo` process-liveness launch of
  the new example.

## TYPE/PR — closing a real, previously-undocumented VM grammar gap

- [x] **`TYPE` and `PR`** (shipped 2026-08-12): both implemented since
  Phase 1 (`interpreter.c`, the old tree-walking engine, long before
  the bytecode VM existed) but never ported to `src/parser.c`'s own
  grammar (`BUILTIN_SIGNATURES`) — `bin/logo` silently reported
  `unknown word: TYPE`/`unknown word: PR` for either one.
  `docs/COMMAND_REFERENCE.md`'s own "Appendix: documented elsewhere,
  not available in `bin/logo`" section had already listed both as
  known gaps, but nobody had gone back to actually close them.

  Found the hard way: `examples/readword_readchar.logo` (shipped a few
  turns earlier, this same session) uses `TYPE` five times and was
  silently broken the whole time — a live `bin/logo` process-liveness
  launch (the only verification method available in this background
  session, with no GUI screenshot access) can't distinguish "ran
  successfully" from "failed to parse and never ran at all," since
  both look identical from outside: no crash, no leftover scratch
  file either way (since nothing after the parse error ever runs). A
  user noticing the example didn't work is what actually surfaced this.
  `examples/type_show.logo` (an actual Phase 1 relic) turned out to
  have been silently broken by the exact same gap this whole time too,
  never caught because nothing had ever loaded and run the real file
  against the VM specifically.

  `TYPE` is `PRINT` without the trailing newline; `PR` is a plain
  `PRINT` alias (same shape as `FD`/`FORWARD`, `BK`/`BACK`, etc.) — both
  trivial, well-precedented additions: `eval_type_value` (new) sits
  right next to `eval_print_value` in `eval.c`, and both are wired into
  all three places `PRINT` already is (`src/parser.c`'s
  `BUILTIN_SIGNATURES`, `vm.c`'s `call_builtin`, and `eval.c`'s own
  tree-walker dispatch — the last one specifically so `TYPE` could be
  shadow-diff tested against the tree-walker too, not just run
  standalone against the VM).

  6 new `test_vm.c` cases (a `TYPE`/`PR` shadow-diff test each, an
  exact-output check confirming no trailing newline, and — the real
  point of this whole fix — two tests that load and run the *actual*
  example files from disk rather than a copy of their text embedded in
  the test, catching exactly the class of bug that motivated this fix:
  an example silently failing to parse against `bin/logo` while still
  looking fine from a live-launch check). Moved `TYPE`/`PR` out of
  `docs/COMMAND_REFERENCE.md`'s appendix into its real "Output" section
  alongside `PRINT`. Verified via `make test` (all 8 suites), standalone
  ASan builds of `test_vm`/`test_eval`/`test_shadow_diff` (all clean,
  `test_vm` with the same raised `ulimit -s` workaround as prior
  rounds), and live `bin/logo` process-liveness launches of both
  now-fixed example files.

## examples/objects.logo — fixed a stale SEND call convention, not a VM bug

- [x] **`examples/objects.logo` "unknown word: TO"** (shipped 2026-08-12):
  flagged as an open finding during the TYPE/PR fix above (a proactive
  audit of every `examples/*.logo` file for silent parse failures) and
  investigated separately. Root cause: the file used the OLD
  tree-walking engine's `SEND` calling convention (`SEND obj "message`,
  trailing positional args) — `bin/logo`'s own `SEND` is deliberately
  fixed at 3 arguments (`obj`, `message`, `arglist`; see
  `src/parser.c`'s own `BUILTIN_SIGNATURES` comment on `SEND`) since the
  bytecode compiler builds the whole AST before any value exists to
  look a dynamically-resolved method's arity up with, so it can't know
  at parse time how many trailing tokens a `SEND` call owns — same
  wall Smalltalk's `perform:withArguments:` hits, solved the same way.

  The mismatch didn't surface as a `SEND`-shaped error at all: since
  `SEND` is expression-capable (used as an operator, e.g.
  `PRINT SEND "dog "getname`), the parser treated `SEND`'s missing 3rd
  argument as "parse one more expression," which greedily swallowed
  every following statement as a nested expression until it hit a
  token that couldn't start one — a `TO` two blocks later, a genuinely
  misleading error far from the real cause.

  Fixed by rewriting the file to the current 3-argument syntax (`[]`
  for a zero-argument message, e.g. `SEND "dog "speak []`; `[alice]`
  for `animal_greet`'s one extra arg) rather than reverting `SEND`'s
  own deliberate design — confirmed correct against the file's own
  inline comments describing expected output. New permanent regression
  test `test_objects_example_runs_correctly` in `tests/test_vm.c`
  (same real-file-loading pattern as the TYPE/PR fix above). Verified
  via `make test` (all 8 suites), a standalone ASan build of `test_vm`
  (clean), and a live `bin/logo` process-liveness launch.

## Nine more old-engine builtins ported to the VM

- [x] **`CLEARTEXT`/`CT`, `LOADPIC`, `SAVEPIC`, `MOUSEPOS`/`MOUSEX`/
  `MOUSEY`/`BUTTON?`, `BACKTRACE`/`BT`, `EXECTIME`** (shipped
  2026-08-12): the rest of `docs/COMMAND_REFERENCE.md`'s own appendix
  (commands `docs/LANGUAGE.md` documents but the VM's grammar didn't
  recognize) — 17 in total, TYPE/PR already closed above, joystick
  input and sound left deliberately open (see below). Same root cause
  as TYPE/PR: real, working code in the old `eval_logo` engine
  (`src/interpreter.c`) that was simply never carried over when the
  bytecode VM took over as `bin/logo`'s only engine.

  Mostly wiring, not new functionality — `app->clear_history`,
  `app->load_background_image`, `app->save_canvas_image`, and
  `app->mouse_x`/`mouse_y`/`mouse_button_down` were all already
  populated/wired up in `ui.c` from the old engine's own use of them
  (the mouse fields specifically already existed for `ONMOUSEMOVE`/
  `ONCLICK`'s benefit), so most of these were a `parser.c` grammar row
  plus a `vm.c` dispatch case reusing existing state, following the
  exact `LOADSPRITE`/`CANVASSIZE` precedent for each shape (0-arg
  command, 1-arg quoted-word command, 0-arg operator).

  Two needed real new work. `BACKTRACE` reads `vm->scope_depth`/
  `vm->scopes[].proc_name` (VM-owned scope storage, see
  `docs/BYTECODE_VM_DESIGN.md`'s own entry on that) rather than
  `app->scope_depth`/`app->scopes[]` the old engine uses — the two are
  separate arrays, so the old implementation couldn't be reused
  unchanged. `EXECTIME` (times a `RUN`-like call, in microseconds)
  needed a genuinely new dedicated opcode, `OP_EXECTIME` — not because
  `EXECTIME` is exotic, but because `RUN`/`LOAD` themselves already
  established that "recursively `vm_run` a freshly-compiled scratch
  chunk" needs its own opcode rather than living in the generic
  `call_builtin` string-dispatch table (see `bytecode.h`'s own `OP_RUN`
  comment: neither ever produces a value at the *compiler's* own
  compile-time-known level, so each gets paired with `OP_VOID_RESULT`
  rather than the runtime `*produced`-flag mechanism ordinary
  `call_builtin` commands use). `EXECTIME` is the one member of that
  family that *does* produce a value, so `OP_EXECTIME`'s own VM handler
  pushes it directly and is never followed by `OP_VOID_RESULT` — caught
  a real bug here mid-implementation: `OP_CHECK_OUTPUT` (the "did this
  expression-position call actually produce a value" check `finish_call`
  emits) reported a spurious `EXECTIME: didn't output a value` at first,
  because `vm->last_call_resolved`/`last_call_produced_output` are only
  set generically by `OP_CALL_BUILTIN` itself — a dedicated opcode has
  to set them explicitly, the same way `OP_APPLY`/`OP_VOID_RESULT`
  already do, which the first draft had missed.

  9 new `tests/test_vm.c` cases (`start_vm_session`-based, not
  `shadow_diff_vm` — confirmed none of these ever existed in `eval.c`
  either, only in the oldest `interpreter.c`, so there's no tree-walker
  to diff against): headless-default/no-op checks for the six pure
  wiring cases, a two-level and a top-level `BACKTRACE` check, and
  three `EXECTIME` cases (plausible-value-in-expression-position,
  works-fine-as-a-command-too, and the same self-referential-capped-
  not-a-crash check `RUN` itself already has). `docs/COMMAND_REFERENCE.md`
  gained three new sections (`Debugger`; `Background image & canvas
  export`; `Mouse state`) and lost `CLEARTEXT`'s own old "not currently
  reachable" footnote; the appendix table shrank from 17 rows to 2
  (`JOYSTICK?`-family, deliberately deprioritized separately; `TONE`/
  `PLAYSOUND`/`STOPSOUND`, no porting effort scoped yet). Verified via
  `make test` (all 8 suites, including `test_bytecode` — confirms the
  new `OP_EXECTIME` opcode didn't break the disassembler/assembler's
  own exhaustive-switch invariant) and a clean full rebuild of
  `bin/logomotive`.

## PLOT — a single dot, closing a real gap DOT left behind

2026-08-12. The user noticed there was no command to mark a single
point: some other Logo dialects call this `DOT`, but that name is
already the vector dot-product operator here (and a user-defined
procedure can't be named `DOT` either — it collides with the builtin
and fails to parse, rather than shadowing it). `PLOT` was picked over
`POINT`/`DOTMARK` after asking the user directly.

The obvious cheap implementation — draw a zero-length line — doesn't
work: `ui.c` never calls `cairo_set_line_cap`, so lines use Cairo's
default `CAIRO_LINE_CAP_BUTT` (square, not round), and a degenerate
line renders nothing at all. (Tutorial II/III's capstones had worked
around exactly this with an `FD 1 BK 1` "DOTMARK" hack before this was
understood architecturally — that workaround is now obsolete but was
left as-is rather than retroactively edited.) `PLOT` instead became a
fourth `RasterOpKind`, `RASTER_OP_DOT`, following the same
"instant, order-baked" shape `FILL`/`ERASERECT`/`STAMP` already use:
`do_plot(LogoApp *app)` in `eval.c` records a `RasterOp` at the
turtle's current position, in its current pen color, with radius set
to half the current pen width (reusing the `RasterOp` struct's `w`
field, since `DOT` has no width/height of its own) — same shared-core
pattern as `do_fill`, dispatched from both `vm.c`'s `call_builtin` and
`eval.c`'s own `exec_call` so it stays shadow-diff-testable. `ui.c`'s
raster-op baking loop gained a `RASTER_OP_DOT` branch that draws it
with `cairo_arc` + `cairo_fill`.

One new `tests/test_vm.c` case, `start_vm_session`-based rather than
`shadow_diff_vm` — captured text output alone can't confirm a filled
dot actually got recorded correctly, so the test reads `app->raster_ops[0]`
directly (`kind`/position/color/radius) after `SETXY`/`SETPENCOLOR`/
`SETPENWIDTH`/`PLOT`. (Caught two unrelated test-writing mistakes
along the way, both previously-seen patterns in this codebase: a new
`TEST()` block needs its own explicit `RUN(...)` line in `main` or it
silently never executes despite `make test` reporting success, and
`SETXY 12 -34` parses as the single arithmetic expression `12 - 34`
rather than two arguments — same class of surprise as the
parens-are-arithmetic-only rule, just without parens involved.) Added
to `docs/COMMAND_REFERENCE.md`'s "Pen, color & canvas" table and
worked example, and regenerated `website/reference.html`. Verified via
`make test` (all 8 suites) and a clean full rebuild of `bin/logomotive`.

## LOADSPRITE/LOADSPRITESHEET's "could not load" error path — the last gap from the 2026-08-12 error-path audit

2026-08-12. Closes the one item the earlier error-path audit (see this
file's "Nine more old-engine builtins ported to the VM" entry, and the
`tests/test_vm.c` gap-fill entry before it) had left open: every other
sprite test in `tests/test_vm.c` runs with `app->load_sprite_image ==
NULL` (`new_app()`'s default), which makes `LOADSPRITE`/
`LOADSPRITESHEET`'s failure branch — `"could not load"` — structurally
unreachable, since the `NULL`-callback check short-circuits before it.

Closed with a new `fake_load_sprite_image` test helper in
`tests/test_vm.c`, not a mock: it calls the real `gdk_pixbuf_new_from_file`
(available in the test binary because `gtk/gtk.h` is already pulled in
transitively via `logo_types.h` and every test binary links against
GTK/gdk-pixbuf, even though `ui.c` itself — and its own `static`
`load_named_sprite_image` — isn't compiled into `test_vm`), then
registers into `app->sprite_names`/`sprite_frame_cols`/
`sprite_frame_rows`/`sprite_count` on success, mirroring
`load_named_sprite_image` closely enough for VM-level tests (it skips
storing an actual `cairo_surface_t`, since nothing at the VM level ever
reads `sprite_images` — confirmed by the existing `ANIMATESPRITE` test
comments this same file already carries).

Four new tests, using real files from `examples/` rather than
synthetic ones: `examples/ant.png` (already documented as a plain
`LOADSPRITE` example) and `examples/walker.png` (already documented as
a 4-column, 2-row, 8-frame `LOADSPRITESHEET` grid) each get a
success-path test — confirms silent output and the correct
name/cols/rows landing in `app`'s sprite tables — plus a matching
failure-path test against a deliberately nonexistent path, confirming
both the exact `"LOADSPRITE: could not load ..."` / `"LOADSPRITESHEET:
could not load ..."` output and that nothing gets registered
(`sprite_count` stays 0) when the load genuinely fails. Removes the
last open item from `docs/ROADMAP.md`'s "Robustness" section, which
had no more entries after this and was removed. Verified via `make
test` (all 8 suites, run from the repo root so the `examples/*.png`
relative paths resolve) and a clean full rebuild of `bin/logomotive`.

## View > Show Input Window — a presentation/demo mode toggle

2026-08-12. Ships the last "Future / unplanned" `docs/ROADMAP.md` item
scoped alongside the `--headless` CLI flag idea: a checkbox menu item
that hides the history pane/entry box, leaving just the canvas
visible, for presentation or demo use.

`src/logo_types.h` gained one new `GtkWidget *repl_box` field on
`LogoApp` — a pointer to the paned's own end child (the vertical box
holding the history pane, the keyboard-hint label, and the entry box;
everything except the canvas), stored by `build_main_window` the same
way `paned`/`text_view`/`entry` already are. The toggle itself,
`action_toggle_input_window` in `src/ui.c`, is a `GSimpleAction` with
boolean state (declared via `GActionEntry`'s own `.state = "true"`
field, which is what makes `GMenu` render it with a checkmark
automatically, no separate menu-building work needed) — activating it
flips the state and calls `gtk_widget_set_visible(app->repl_box, ...)`
directly. Nothing else changes: `drawing_area` and `paned` are left
alone, since GTK4's own `GtkPaned` already accounts for an invisible
child while laying out (an invisible end child stops claiming space,
and the canvas — the paned's non-resizable start child — simply gets
the freed width), so no manual resize/position bookkeeping was needed
the way `resize_canvas_widget` needs for `SETCANVASSIZE`.

Bound to ⌘⇧I (`<Meta><Shift>i`, alongside the other View-menu/File-menu
accelerators, all already `<Meta>`-based rather than `<Primary>` since
this app is macOS-only). `docs/LANGUAGE.md`'s own View-menu bullet
(under "Interface" — the same one documenting Increase/Decrease/
Reset Text Size) now mentions it too. No `tests/test_vm.c` coverage —
this is GTK widget-visibility wiring with no VM-observable behavior,
the same reason the View menu's existing text-size actions have never
had test coverage either (the new `LogoApp.repl_box` field itself is
visible to every test binary, since `logo_types.h` is pulled in
transitively through `interpreter.h`, but stays an untouched `NULL` in
headless tests — the actual wiring lives in `ui.c`, which isn't linked
into any test binary). Verified via a clean full rebuild of
`bin/logomotive` and `make test` (all 8 suites, unaffected).

While testing the toggle above, the user hit a real, pre-existing bug
unrelated to it: quitting (Cmd-Q or the window's own close button)
sometimes printed `Gdk-CRITICAL **: gdk_surface_request_layout:
assertion 'frame_clock' failed` to the terminal. Root cause: `WAIT`,
`SETSPEED`'s own motion-delay throttle, and `ANIMATESPRITE` all
suspend via a real `g_timeout_add` timer (`on_wait_timeout`/
`on_animatesprite_timeout`, `ui.c`) that nothing ever cancelled — quit
while one was still pending, and it fired anyway sometime during (or
just after) window teardown, and `handle_vm_result`'s unconditional
`gtk_widget_queue_draw(app->drawing_area)` hit a canvas surface whose
frame clock was already gone. Fixed by tracking the pending timer's id
in a new file-scope `g_suspend_timeout_id` (set at each of the three
`g_timeout_add` call sites, cleared at the top of each callback) and
cancelling it — plus freeing its `SuspendedRun` — from a new
`on_window_destroy` handler connected to the window's own `"destroy"`
signal (chosen over `"close-request"` since it fires for every
teardown path, not just the close button, and runs before
`drawing_area`'s own resources go away). `WAITKEY`/`INPUT` (resumed by
a real GTK key event, not a timer) and `PAUSE` (resumed only by an
explicit `RESUME` command) were never at risk the same way, so neither
needed this. No new test coverage — same reasoning as the toggle
itself, this is GTK teardown-ordering behavior with no VM-observable
effect, and reproducing the exact timing race outside a real windowed
run isn't something the headless test harness can do. Verified with a
clean full rebuild and `make test` (all 8 suites).

## `bin/logomotive --headless` — closes the last item on the roadmap that wasn't joystick support

2026-08-12. Ships the other "Future / unplanned" `docs/ROADMAP.md`
item: `bin/logomotive --headless script.logo` runs one script with no
GTK window/event loop at all, prints its output, and exits — for
scripting/automation use, not interactive use. Resolves the roadmap
entry's own open design question (real timing vs. instant resolution)
in favor of instant: every suspend point resolves with no real delay,
since headless implies batch use, not watching a drawing unfold.

New `src/headless.c`/`.h` (picked up automatically by the `bin/logomotive`
build — its `Makefile` target already globs every `src/*.c`, so no
`Makefile` change was needed), promoting `tools/vmrun_cli.c`'s same
headless-VM-driver shape (built 2026-08-12 for doc verification) into
the real app, but more complete in three ways `vmrun_cli.c` never
needed to be, being just a doc-checking tool:

- `WAITKEY`/`INPUT` read a genuine line from stdin instead of a canned
  value — `INPUT` gets the line's own text; `WAITKEY` gets the same
  line used as its "key name" (piping in `space` satisfies `WAITKEY =
  "space`), a deliberate, documented stand-in for real single-keypress
  capture (which would need raw terminal mode plus GDK-style key-name
  mapping — see `ui.c`'s own `gdk_keyval_name` calls — that headless
  mode's own batch-use case doesn't warrant). EOF resolves to `""`
  rather than hanging.
- `ANIMATESPRITE` resolves every remaining frame in a tight loop via
  repeated `vm_resume_animatesprite` calls, rather than stopping at
  "unsupported suspension" the way `vmrun_cli.c` deliberately does.
- `LAUNCH` hands off to `agent.c`'s own `scheduler_run`, exactly the
  construction `ui.c`'s `handle_vm_result` already uses for its own
  `VM_RUN_SUSPENDED_LAUNCH` case (copy `vm` into a fresh `Agent`, run
  the scheduler, minus the GTK redraw at the end since there's no
  canvas here) — concurrent agents needed no changes at all to become
  headless-safe, since `scheduler_run` was already a plain synchronous
  loop with zero GTK dependency (confirmed directly: no
  `gtk_`/`GTK_`/`g_timeout_add` call anywhere in `agent.c`).

`main.c` gained `consume_headless_flag` (mirrors `consume_speed_flag`'s
own argv-surgery approach exactly, just simpler — a bare flag, no
value) and now branches before ever calling `gtk_application_new`:
`--headless` skips `GtkApplication`/`g_application_run` entirely rather
than opening and hiding a window, since `GApplication`'s own startup
(session-bus registration, etc.) is overhead a script-only run
shouldn't pay for. `LOADSPRITE`/`LOADSPRITESHEET` stay silent no-ops in
headless mode, same as every other headless/test entry point already
in this codebase (`request_redraw`/`load_sprite_image`/etc. all stay
`NULL`) — so `SETSPRITE`/`ANIMATESPRITE` report their ordinary "no such
sprite"/"no sprite set" errors if a script tries to use a sprite it
never actually loaded.

`README.md` gained a `--headless` usage example (next to the existing
`--speed` one) and a `src/headless.h/c` line in the project-structure
table; `docs/LANGUAGE.md`'s own "Interface" section gained a bullet
covering the exact suspend-resolution rules above. No `tests/test_vm.c`
coverage — this is process-level CLI/stdin/exit-code behavior with no
VM-observable effect the existing harness's `output_sink`-capture
pattern can check; verified instead by hand against several real
scripts (a plain drawing script, `WAIT`/`SETSPEED` timing, `WAITKEY`/
`INPUT` piped via stdin, `examples/concurrent_agents.logo`'s own
`LAUNCH`/`AWAIT`/`YIELD`, `ANIMATESPRITE` with no sprite set, the
missing-file and missing-argument error paths) and a clean full
`make`/`make test` (all 8 suites, unaffected).

## `bin/logomotive -h`/`--help` — a real one, not GApplication's generic stub

2026-08-12. Found by the user trying `bin/logomotive --help` right
after `--headless` shipped: it printed GLib's own generic
`GApplication` "Help Options" stub (`-h, --help`, `--help-all`,
`--help-gapplication`) rather than anything about this program's own
CLI surface, since no `GOptionEntry` table is registered here. Actively
misleading, not just unhelpful -- it doesn't mention `--speed` or
`--headless` at all, both of which `main.c` already strips out of
`argv` (via `consume_speed_flag`/`consume_headless_flag`) before
`GApplication` ever gets a look at it, so those two flags were
invisible to GLib's own help text by construction, not by omission.

Fixed with a `wants_help`/`print_usage` pair in `main.c`, checked
before anything else in `main` -- before `consume_speed_flag`/
`consume_headless_flag`, before `signal(SIGINT, ...)`, before
`gtk_application_new` -- so `-h`/`--help` prints real usage (the
optional `script.logo` argument, `--speed <seconds>`, `--headless`,
and `-h`/`--help` itself) and exits 0 immediately, with no GTK
involvement at all. No new test coverage (same reasoning as
`--headless` itself: process-level CLI output, nothing for
`tests/test_vm.c`'s `output_sink`-capture pattern to check) --
verified by hand (`bin/logomotive -h`, `bin/logomotive --help`) and
confirmed `--speed`/`--headless` both still parse correctly afterward.
`README.md` and `docs/LANGUAGE.md`'s own "Interface" section both
gained a one-line mention. Verified with a clean full rebuild and
`make test` (all 8 suites, unaffected).

## Documentation audit — five real issues, after the joystick mistake exposed the same failure mode elsewhere

2026-08-12. Prompted by fixing the JOYSTICK?-family error in
`docs/ROADMAP.md` (see this file's own "Fix a factual error..." entry)
— asked whether more docs claimed something as shipped/available that
doesn't actually exist in `src/parser.c`'s `BUILTIN_SIGNATURES`. An
audit of every doc file plus `website/*.html` against the real source
found five real issues (verified directly, not taken on faith) and
fixed all of them in one batch:

- **The exact same false-claim bug, for sound.** `TONE`/`PLAYSOUND`/
  `STOPSOUND` were advertised as an already-working feature in
  `README.md`, `website/index.html` (three spots — the intro
  paragraph, a feature chip, and the reference card), `docs/TUTORIAL.md`,
  and `website/tutorial.html` — but `grep -n '"TONE"\|"PLAYSOUND"\|
  "STOPSOUND"' src/parser.c src/vm.c` returns nothing; they only exist
  in the frozen `interpreter.c` engine, exactly like `JOYSTICK?` was.
  `docs/COMMAND_REFERENCE.md`'s own appendix already correctly listed
  Sound as unported — the other five docs just hadn't caught up. Fixed
  by dropping the false "sound" mentions from all five (not by
  claiming it's planned — it's simply not there yet).
- **Two dead cross-references** in `docs/COMMAND_REFERENCE.md`,
  pointing at `docs/ROADMAP.md` subsections ("Mouse/keyboard event
  triggers", "Future / unplanned") that no longer exist now that
  `ROADMAP.md` is trimmed to a single paragraph (see the entries just
  above this one). Fixed: the event-triggers pointer now points at
  `docs/CHANGELOG.md`'s own "Mouse/keyboard event triggers" entries
  (confirmed that heading exists); the joystick one now points at
  `docs/ROADMAP.md` generically rather than quoting a specific
  subsection, so it can't go stale the same way again.
- **`docs/BYTECODE_REFERENCE.md` was out of sync with its own subject**,
  violating the "keep in sync" rule its own header states: claimed
  "the 58 members of `bytecode.h`'s own `OpCode` enum," but a real
  count (stripping comments, deduplicating) is 59 — missing
  `OP_EXECTIME`, added when `EXECTIME` was ported to the VM 2026-08-12
  and apparently missed in that same batch. Added its row (in a new
  "RUN / LOAD / EXECTIME" section, alongside its two closest relatives)
  and corrected the count. Cross-checked afterward: all 59 opcodes now
  appear somewhere in the doc.
- **`README.md`'s "Project structure" table described the pre-Stage-2
  architecture** — listed `interpreter.h/c` as "the Logo language
  core: eval_logo, expression parser," with no mention anywhere of
  `lexer.c`/`parser.c`/`ast.c`/`compiler.c`/`vm.c`/`bytecode.c`/
  `agent.c`, i.e. every file the real running app actually uses.
  Rewritten to list the real pipeline, with `interpreter.c`/`eval.c`
  both clearly labeled for what they now are (frozen legacy engine;
  shadow-diff-only Stage 1 evaluator, respectively).
- **`docs/COMMAND_REFERENCE.md`'s own Output table didn't list `PR`**
  as a `PRINT` alias, even though `PR` shipped 2026-08-12 (confirmed in
  `parser.c`/`vm.c`) and the appendix already said so. Added an
  Aliases column.

`website/reference.html` regenerated afterward (`website/build_reference.py`)
to pick up the `COMMAND_REFERENCE.md` changes. One item from the audit
was deliberately left as-is: a leftover `bin/logo` (pre-rename binary
name) in `docs/CONCURRENT_AGENTS_DESIGN.md` — that file, like
`docs/CHANGELOG.md`/`docs/BYTECODE_VM_DESIGN.md`, was explicitly
exempted from the 2026-08-12 `bin/logo` → `bin/logomotive` rename pass
as a dated historical record, and this is the same kind of entry.
Verified with a clean full rebuild and `make test` (all 8 suites,
unaffected — this batch never touched `src/`).
