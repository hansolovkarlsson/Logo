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

- [ ] Fuller arithmetic: `MOD`, `POWER`, `SQRT`, `SIN`, `COS`, `ARCTAN`,
  `RANDOM`, `ROUND`, `ABS` — arithmetic currently stops at `+ − * /`.
- [ ] Type/membership predicates: `MEMBER?`, `EMPTY?`, `WORD?`, `LIST?`,
  `NUMBER?` — no way right now to ask "is this a list?" or "is X in this
  list?" from inside a running program.
- [ ] `LOCAL "name` — a variable scoped to the current call without being a
  parameter; cheap on top of the scope stack parameters already use.
- [ ] `HIDETURTLE`/`SHOWTURTLE`, `WRAP`/`FENCE`/`WINDOW` — toggle turtle
  visibility, and choose what happens when a turtle drives off the canvas
  edge (wrap around, stop, or keep going off-screen).
- [ ] `RUN`, `APPLY` — execute a stored list as code, or call a
  procedure/template with a list of arguments. The thing that makes a list
  double as a deferred program, not just data.
- [ ] `MAP`, `FOREACH`, `FILTER`, `REDUCE` — higher-order iteration over a
  list, instead of hand-walking it with `FIRST`/`BUTFIRST` inside a `WHILE`.
  Builds on `RUN`/`APPLY` above.
- [ ] `LABEL`, `FILL` — draw text at the turtle's position, and flood-fill a
  closed region. Needs new Cairo drawing calls in `ui.c`, not just
  interpreter logic.
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
