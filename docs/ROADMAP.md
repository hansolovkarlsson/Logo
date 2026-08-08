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
