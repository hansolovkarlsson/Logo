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
- [ ] `THROW`/`CATCH` as a real unwind mechanism, not just another
  opcode — non-local exit to an arbitrary ancestor frame needs its own
  design (an explicit unwind-target stack the VM consults), flagged
  separately since it's one of the two genuinely hard pieces (with
  suspend/resume itself) rather than a mechanical port like most of the
  rest of this list.
- [ ] `MAP`/`FILTER`/`REDUCE`/`FOREACH` templates compiled once instead
  of re-lexed/re-parsed per element (today's real cost, in both
  `eval_logo` and Stage 1's own `ast_eval`) — a genuine semantic
  upgrade this stage enables, not just a port: closer to a real lambda
  than Stage 1's own re-entrant lex/parse machinery, and a concrete
  first step toward the "growing past toy language" motivation
  (closures) rather than just suspend/resume.
- [ ] Suspend/resume's actual GTK integration — the real point of this
  whole stage, deliberately last: a VM-internal detail alone doesn't
  deliver it. Needs its own design for how the VM's "run until suspend
  point or completion" loop hooks into `ui.c`'s event loop — a keypress
  triggering resume for `WAITKEY`/`INPUT`, a timer for `WAIT`/`PAUSE` —
  not assumed to fall out of the frame-array mechanism for free.

## Robustness

- [ ] Grow `tests/test_interpreter.c`'s coverage as new language features
  land (it currently covers turtle motion, procedures/scoping, `IF`/
  `WHILE`/booleans, words, and the error-message paths — see `make test`).
