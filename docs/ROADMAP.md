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

- [ ] **A small, real, end-to-end vertical slice first** — not full
  `BUILTIN_SIGNATURES` parity. Just enough instruction set (literals,
  arithmetic, `PRINT`, `IF`/`WHILE`, procedure calls with
  `OUTPUT`/`STOP`) to prove the compiler + explicit-frame-array VM
  mechanism actually works, shadow-diffed against `ast_eval` itself
  (not `eval_logo` directly — Stage 1's frontend is already proven
  byte-identical to that, so this isolates "does the compiler+VM match
  the tree-walker's semantics" as its own mechanically-checkable
  question). Frame-array sizing (how large a fixed cap replaces
  `MAX_SCOPE_DEPTH`'s 200) gets decided here, once, not revisited per
  batch — each frame's cost is small and uniform now, unlike
  `eval_logo`'s own variable per-branch C-stack usage, so this can
  likely afford a much larger cap.
- [ ] Grow instruction coverage the same way Stage 1 grew
  `BUILTIN_SIGNATURES` — incremental batches (lists/arrays, `MAKE`,
  property lists, turtle/drawing commands, ...), each shadow-diffed
  against `ast_eval` before moving to the next.
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
