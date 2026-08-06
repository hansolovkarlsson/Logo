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

### Phase 2 — Small, self-contained additions

Ideas from a 2026-08-06 review of
[Terrapin Logo's command reference](https://resources.terrapinlogo.com/logo/commands/)
against this interpreter's existing command set — same "small,
self-contained" character as the original Phase 1, just found later:

- [ ] `CLEAN` — erases drawing but leaves the turtle's position/heading
  alone, unlike `CLEAR`/`CS` (which also homes it) — a real Berkeley
  Logo distinction this interpreter doesn't have yet.
- [ ] `CLEARTEXT` — clears the history pane specifically, separate from
  `CLEAR`'s canvas-only effect.
- [ ] `WHO` — reports which turtle `TELL` currently has selected.
- [ ] `PICK list` — a random element from a list; trivial given
  `RANDOM`/`ITEM`/`COUNT` already exist.
- [ ] `FLATTEN`, `PARSE`, `SUBST` — flatten nested lists into one flat
  list, tokenize a word into a list of words, and substitute occurrences
  of one thing for another within a list. `FLATTEN` specifically is only
  meaningful now that lists really nest.
- [ ] `FILLARRAY array value` — fill every slot of an array with one
  value in a single call, rather than looping `SETITEM` manually.
- [ ] `PROCEDURES`/`NAMES` — list every currently-defined procedure/
  variable name; workspace introspection alongside Phase 1's `SHOW`.
- [ ] `TEXT "name` — like `SHOW`, but returns a procedure's body as
  *data* (a list) instead of printing it — the complementary
  read-as-data half of what `SHOW` already prints.
- [ ] `THING word` — reads a variable by a name that's itself a
  computed word (`THING WORD "item :n`), unlike `:name` which only
  takes a literal name — a genuine reflective capability `:name` alone
  can't provide.

### Phase 3 — New data types, background/sprite images

- [ ] Property lists (key/value records) — Berkeley Logo's `PLIST`
  (real command names: `GPROP`/`PPROP`/`REMPROP`); also a natural
  stepping stone toward the OOP idea in Phase 5, since a prototype
  "object" is essentially a plist with a type tag.
- [ ] Load a background image onto the canvas (Terrapin's `LOADPIC`;
  `SAVEPIC` for the reverse).
- [ ] Load part of an image onto part of the canvas (a sprite-sheet
  style blit) — `gdk-pixbuf`/Cairo are already linked (used for PNG
  export today), so this slots in next to existing drawing code rather
  than needing a new dependency.
- [ ] Sprites controlled by turtles (Terrapin's `STAMP`) — swap a
  turtle's drawn triangle for a loaded image; a fairly direct extension
  of the existing per-turtle drawing (see "Multiple turtles" in
  `LANGUAGE.md`).
- [ ] Animated sprites (frame-cycling) — needs some notion of a timer/
  tick driving redraws, similar in spirit to `WAIT`'s existing
  redraw-and-drain-events technique but recurring rather than one-shot.
- [ ] A real debugger — breakpoints/step execution, a call-stack trace,
  and per-call timing (Terrapin's `PAUSE`/`CONTINUE`/`BACKTRACE`/
  `EXECTIME`) — a substantial standalone feature area on its own, not a
  quick add alongside the rest of this phase.
- [ ] General file I/O beyond today's procedure-only `LOAD`/`SAVE` —
  reading/writing arbitrary files and listing directory contents
  (Terrapin's `OPEN`/`CLOSE`/`CREATE`/`DELETE`/`DIRECTORY`).
- [ ] A resizable canvas (Terrapin's `SETEXTENT`/`EXTENT`) — needs
  `CANVAS_WIDTH`/`CANVAS_HEIGHT` (currently `#define`d constants baked
  into turtle-position math, `WRAP`/`FENCE` boundary checks, and the
  raster surfaces built for `FILL`/`ERASERECT`) to become a
  runtime-configurable size instead — a bigger lift than it looks at
  first.

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
