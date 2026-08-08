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

### Phase 4 — Interactive input (one shared architectural blocker)

`eval_logo` runs synchronously start-to-finish, so anything that needs to
*pause* a running script and wait on a live event shares the same
underlying problem — worth solving once rather than four separate times:

- [ ] A "wait for a keypress" pause, as an alternative to `WAIT`'s
  time-based one (carried over from the original roadmap) — the entry
  box would need to feed a keypress back into a running `eval_logo`
  call.
- [ ] `INPUT` (or similar) — read a line of live user input into a
  variable mid-script, Berkeley Logo's `READLIST`/`READWORD` territory.
- [ ] Joystick/game-controller input — a genuinely new dependency (no
  game-controller library linked today) on top of the same
  pause-and-resume problem.
- [ ] Mouse position/click input (Terrapin's `MOUSE`/`BUTTON`) — shares
  the same pause-and-resume problem as the rest of this phase, and
  arguably more useful than a joystick for a desktop app.

Sound doesn't share the pausing problem above, but is the other genuinely
new dependency (no audio library linked today) in this same "game engine"
direction:

- [ ] Sound effects/playback.

### Phase 5 — Large architectural bets (need a design discussion first, not just scoping)

- [ ] Prototype-style object orientation, no classes — everything is an
  object (`Integer.New`-style construction), message-passing closer to
  Smalltalk/Self than Berkeley Logo's own (class-based) Object Logo
  dialect (see `docs/FEATURE_ATLAS.md`'s "Uncharted territory"). A real
  object value type plus message dispatch is a language redesign, not a
  feature — needs its own design conversation before scoping.
- [ ] Compile to a bytecode VM instead of directly tree-walking
  `eval_logo` over raw text. An order of magnitude bigger than
  everything else on this page — the current interpreter is
  deliberately one direct-execution function with no AST; a bytecode VM
  means a real compile step and a from-scratch execution core,
  obsoleting most of `interpreter.c`'s current design. Motivation
  (performance? something else?) needs to be clear before this is ever
  scoped for real.

## Robustness

- [ ] Grow `tests/test_interpreter.c`'s coverage as new language features
  land (it currently covers turtle motion, procedures/scoping, `IF`/
  `WHILE`/booleans, words, and the error-message paths — see `make test`).
- [ ] User-defined procedure parameters coerce every argument to a plain
  number, even a list/word/array — found 2026-08-06 while adding arrays
  (the "new data types" phase above). `TO test :x PRINT :x END` then
  `test [1 2 3]` prints `0`, not `1 2 3`: `call_procedure` binds every
  parameter as `VALUE_NUMBER`
  (`arg_vals` is a plain `double[MAX_PARAMS]`), so a list/word/array
  argument is silently number-coerced at the call boundary — the same
  way a bare word or list evaluates to `0` in any other numeric context,
  just not somewhere a user would expect it applied. Never surfaced
  before since every existing example/test only ever passes numeric
  arguments. Fixing this means threading a real `Value` (not a `double`)
  through `arg_vals`/`Scope.vars`/`APPLY`'s argument list — a real but
  contained change, not attempted here since it's orthogonal to arrays
  themselves.
