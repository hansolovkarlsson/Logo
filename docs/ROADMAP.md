# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. Update this alongside `LANGUAGE.md` as items land — move a
finished item's description into the language reference and delete it here
rather than marking it "done" in place.

## Future directions

Ideas from a 2026-08-05 brainstorm, grouped into rough phases by
complexity/risk rather than a strict priority order. Later phases don't
strictly depend on earlier ones, but roughly track "how big a bet is this"
— treat this as a menu to pick from, not a committed queue.

### Phase 4 — Interactive input

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

### Phase 5 — Large architectural bets (need a design discussion first, not just scoping)

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

### Phase 5, Stage 1 — new evaluator built-in coverage

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

### Phase 5, Stage 2 — bytecode VM

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

### Phase 6 — MultiLogo-style concurrent turtle agents

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

### CLI ergonomics

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

## Robustness

- [ ] Grow `tests/test_interpreter.c`'s coverage as new language features
  land (it currently covers turtle motion, procedures/scoping, `IF`/
  `WHILE`/booleans, words, and the error-message paths — see `make test`).
