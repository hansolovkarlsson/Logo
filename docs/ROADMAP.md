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

### Phase 2 — New data types, background/sprite images

- [ ] Arrays — fixed-size, O(1) random-access storage, as a real
  alternative to lists for this (today's linked-list `ITEM` is an O(n)
  walk). Matters most once sprite tables/tile grids exist (Phase 2/3
  below).
- [ ] Property lists (key/value records) — Berkeley Logo's `PLIST`;
  also a natural stepping stone toward the OOP idea in Phase 4, since a
  prototype "object" is essentially a plist with a type tag.
- [ ] Load a background image onto the canvas.
- [ ] Load part of an image onto part of the canvas (a sprite-sheet
  style blit) — `gdk-pixbuf`/Cairo are already linked (used for PNG
  export today), so this slots in next to existing drawing code rather
  than needing a new dependency.
- [ ] Sprites controlled by turtles — swap a turtle's drawn triangle
  for a loaded image; a fairly direct extension of the existing
  per-turtle drawing (see "Multiple turtles" in `LANGUAGE.md`).
- [ ] Animated sprites (frame-cycling) — needs some notion of a timer/
  tick driving redraws, similar in spirit to `WAIT`'s existing
  redraw-and-drain-events technique but recurring rather than one-shot.

### Phase 3 — Interactive input (one shared architectural blocker)

`eval_logo` runs synchronously start-to-finish, so anything that needs to
*pause* a running script and wait on a live event shares the same
underlying problem — worth solving once rather than three separate times:

- [ ] A "wait for a keypress" pause, as an alternative to `WAIT`'s
  time-based one (carried over from the original roadmap) — the entry
  box would need to feed a keypress back into a running `eval_logo`
  call.
- [ ] `INPUT` (or similar) — read a line of live user input into a
  variable mid-script, Berkeley Logo's `READLIST`/`READWORD` territory.
- [ ] Joystick/game-controller input — a genuinely new dependency (no
  game-controller library linked today) on top of the same
  pause-and-resume problem.

Sound doesn't share the pausing problem above, but is the other genuinely
new dependency (no audio library linked today) in this same "game engine"
direction:

- [ ] Sound effects/playback.

### Phase 4 — Large architectural bets (need a design discussion first, not just scoping)

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
- [ ] `eval_logo`'s 200-call recursion cap (`MAX_SCOPE_DEPTH`) is close to,
  but not confirmed over, a stack-safety edge under AddressSanitizer —
  found 2026-08-05 while stress-testing `RUN`/`APPLY`. `eval_logo` is one
  giant function with every command as a branch, compiled at `-O0`, so
  each new feature's locals add to its per-call stack frame even though
  only one branch runs per call; 200 levels of recursion now overflows
  under ASan's inflated overhead, though every plain (non-ASan) run has
  passed cleanly. Left as-is for now rather than lowering the documented
  cap or splitting `eval_logo` into smaller per-command functions — revisit
  if a real (non-ASan) crash from deep recursion is ever reported.
