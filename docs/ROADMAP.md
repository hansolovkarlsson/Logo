# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. Update this alongside `LANGUAGE.md` as items land — move a
finished item's description into the language reference and delete it here
rather than marking it "done" in place.

## Language

Standard Berkeley Logo features this interpreter doesn't have yet — see
`docs/FEATURE_ATLAS.md` for the full survey this list is drawn from. Ordered
smallest-to-largest lift, so working top to bottom tackles the cheap, clearly-
scoped items first.

- [ ] `WAIT expr` — pause execution for `expr` before continuing, keeping
  the GTK event loop responsive (so the canvas actually redraws mid-script
  instead of visibly freezing until the whole program finishes). Directly
  motivated by manually testing `LABEL`/`FILL`: a multi-step drawing that
  `CLEAR`s partway through is otherwise impossible to see the intermediate
  state of. A "wait for a keypress" variant would also solve this, but has
  no real Logo precedent and needs actual interactive input handling
  mid-script — start with the simpler time-based `WAIT`.
- [ ] `OUTPUT`/`STOP` — let a `TO ... END` procedure return a value and be
  used inside an expression, the way `FIRST`/`WORD`/etc. already can.
  Changes the procedure-call convention — the biggest item on this list.
- [ ] `CATCH`/`THROW` — structured error recovery, so a procedure can catch
  and handle a failure from something it called instead of an error just
  printing a message and unwinding.

## Robustness

- [ ] Grow `tests/test_interpreter.c`'s coverage as new language features
  land (it currently covers turtle motion, procedures/scoping, `IF`/
  `WHILE`/booleans, words, and the error-message paths — see `make test`).
- [ ] `FILL` recomputes its flood-fill from whatever lines currently exist
  on every redraw, rather than freezing a snapshot at the moment `FILL`
  was called (see `LANGUAGE.md`'s Turtle commands section) — a deliberate
  scope decision (the alternative needs a persisted raster layer in
  `LogoApp`, freed/reset on `CLEAR`), not a bug, but worth fixing if it
  ever causes a real drawing to look wrong after further lines are added
  past an existing fill.
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
