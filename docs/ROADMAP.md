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
  Real recursion-depth independence from `MAX_SCOPE_DEPTH` needs
  VM-owned scope storage, its own deliberate follow-up (not started).
- [ ] Grow instruction coverage the same way Stage 1 grew
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
- [ ] `ANIMATESPRITE` — **blocked, not skipped by oversight**: scoping
  it (2026-08-10) found it operates on `Turtle.sprite_index`, which
  only `SETSPRITE` ever sets away from its default `-1` — and
  `SETSPRITE`/`LOADSPRITE`/`STAMPSPRITE`/`SETSPRITEFRAME` don't exist
  anywhere in `parser.c`/`eval.c` at all (sprites were deliberately out
  of Stage 1's own scope entirely, grouped with sound/`WINDOW` as "a
  large GTK/SDL-state surface disproportionate to a research
  evaluator's needs"). So `ANIMATESPRITE` can't do anything real
  through this pipeline yet regardless of its own suspend/resume
  design — it would always hit the `"no sprite set"` error path, since
  nothing can set a sprite first. Porting the whole sprite subsystem
  first would be its own separate, larger scoping question, not a
  quick add-on to this batch; the user chose to leave it blocked rather
  than take that on now. `PAUSE` remains the only actual open
  suspend/resume item.

## Robustness

- [ ] Grow `tests/test_interpreter.c`'s coverage as new language features
  land (it currently covers turtle motion, procedures/scoping, `IF`/
  `WHILE`/booleans, words, and the error-message paths — see `make test`).
