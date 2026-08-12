# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. When an item ships, move its write-up into `docs/CHANGELOG.md`
(dated, with the full rationale/detail — see that file's own intro) instead
of marking it done in place here. Keeps this file trimmed to what's
genuinely still ahead; see `docs/CHANGELOG.md` for the full history of
everything that's already landed.

Every item previously listed here has shipped (see `docs/CHANGELOG.md`
for the full history). Joystick support (both event triggers and plain
polling — see `docs/COMMAND_REFERENCE.md`'s own appendix) was dropped
entirely, not deferred: it'd need a real new dependency (GNOME's
libmanette, or direct IOKit/HID access) not worth taking on for a
feature the user doesn't consider a priority.

## Future / unplanned

Real ideas, not currently prioritized — pick up only on explicit
request. Sourced from a 2026-08-12 research pass into other Logo
dialects' distinctive features (UCBLogo/Berkeley Logo, NetLogo,
StarLogo/StarLogo Nova, MicroWorlds EX, FMSLogo), filtered to things
LogoMotive genuinely doesn't have — cross-checked against
`docs/COMMAND_REFERENCE.md` command by command, not assumed from
"classic Logo."

- [ ] **Patch grid + `DIFFUSE`** (NetLogo/StarLogo) — a stationary grid
  of cells, each with its own named variables, addressable by grid
  coordinate (not turtle-owned, not shared globally); `DIFFUSE` spreads
  a patch variable's value to its 8 neighbors each tick, conserving the
  total. A genuinely new data model, not another drawing command —
  makes pheromone trails, heat maps, and cellular automata trivial to
  write. LogoMotive's canvas has pixel-level ops (`PLOT`/`ERASERECT`/
  `FILL`) but nothing addressable-and-stateful at cell granularity.
  Would sit naturally next to the existing `ARRAY`/`CANVASSIZE`
  machinery.

- [ ] **Breeds + `ASK <agentset> [...]`** (NetLogo) — turtles
  partitioned into named breeds (`sheep`, `wolves`) with breed-specific
  owned variables, and any breed/agentset commandable in bulk (`ask
  wolves [...]` runs a block once per wolf, each with its own
  bindings). A different concurrency idiom than `LAUNCH`/`AWAIT`/
  `YIELD` (call-oriented, one agent per explicit launch): breeds give
  you typed groups you can query and command collectively instead of
  tracking individually launched agents by hand. Same problem space as
  `docs/CONCURRENT_AGENTS_DESIGN.md`, worth comparing directly against
  it before designing.

- [ ] **`.MACRO`** (UCBLogo) — a user-defined procedure whose *output*
  (a list of Logo instructions) gets spliced back in and evaluated in
  the caller's own context, rather than returned as a value — lets
  users invent their own control structures (`REPEAT`/`IF`-like) that
  can `STOP`/`OUTPUT`/`LOCAL` correctly in the caller's frame, which an
  ordinary wrapper procedure can't do. LogoMotive already has the
  building blocks (`RUN`/`EVAL`/`APPLY`, a real bytecode compiler) —
  the missing piece is compile-time recognition of a macro name plus a
  "run this list as if inlined" step. Genuinely new idiom (code that
  writes code), not currently expressible at all.

- [ ] **`ERRACT`-style error pause** (UCBLogo) — an uncaught error
  drops into an interactive pause at the failure point (locals still
  inspectable) instead of just printing a message and unwinding.
  Extends existing pieces rather than inventing from scratch:
  `PAUSE`/`CONTINUE` (suspend/resume), `BACKTRACE` (call-stack print),
  `THROW`/`CATCH` are all already there — right now an uncaught `THROW`
  just prints and resumes at the next top-level command, with no way to
  actually stop and inspect state. A debugging-capability gap, not a
  new command category. Probably the cheapest of this whole list, since
  it's mostly wiring pieces that already exist.

- [ ] **Multi-dimensional arrays (`MDARRAY`)** (UCBLogo) — LogoMotive's
  `ARRAY` is explicitly 1-D (a flat slot vector); `MDARRAY` takes a
  list of dimension sizes and gives a real N-dimensional structure
  addressed by an index list. Concrete, non-filler gap: grids/matrices
  (game boards, cellular automata, simple linear algebra) currently
  need hand-simulated index arithmetic over a flat array.

- [ ] **Live-bound GUI widgets** (MicroWorlds EX/FMSLogo) — a
  persistent, always-visible slider/text box/checkbox bound to a
  variable, so dragging it changes the variable continuously with no
  polling or callback needed. Different idiom than the existing
  `ONCLICK`/`ONKEY`/`ONMOUSEMOVE` (one-shot event callbacks) and
  `WAITKEY`/`INPUT` (blocking reads) — a live, always-on reactive
  binding instead. Plausible fit given the app is already a real GTK4
  window (a slider is just another GTK widget next to the canvas);
  would open up parameter-tuning-style scripts (adjust a live variable
  via a slider while a simulation runs).

- [ ] **Live data plots** (NetLogo) — a running chart (line plot,
  histogram) as a first-class output alongside the turtle canvas: call
  a charting primitive each tick and a separate graph window updates
  live — note NetLogo's own name for this, `plot`, collides with
  LogoMotive's existing `PLOT` (a single dot at the turtle's position,
  shipped 2026-08-12), so this would need its own name, not that one.
  A different output modality than turtle-drawn graphics (data
  visualization vs. spatial drawing). Pairs naturally with the
  patch-grid or breeds ideas above (plot average patch value, or
  population size, over time). Lower priority than the others — more
  "new widget" than "new capability."
