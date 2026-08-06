# Feature Atlas

A survey of Berkeley Logo's own reference manual against this project's current
command set, plus a look at the handful of dialects that pushed Logo somewhere
genuinely different — objects, concurrency, mass simulation. Written 2026-08-05;
see `ROADMAP.md` for what's actually queued up to build.

Legend: 🟢 standard Logo, missing here — 🟣 rare, one or two dialects ever did this.

## 🟢 The common repertoire

**Status: all shipped.** This section is a point-in-time survey from
2026-08-05 — every item below has since been implemented (working
through them one by one is what most of `ROADMAP.md`'s history is);
see `docs/LANGUAGE.md` for how each one actually works today. Left
as-written below for the historical record of what this survey found
missing at the time, not as a current gap list.

Everything below is unremarkable in Logo terms — it's in the Berkeley Logo manual,
the closest thing the language has to a spec. None of it is exotic; it's just not
built yet.

### Procedures & scope

- **`OUTPUT` / `STOP`** — Right now `TO … END` only runs commands — there's no way
  for a procedure to hand back a value, so it can never appear inside an
  expression the way `FIRST` or `WORD` can. This is the single biggest thing
  separating "commands you define" from "functions you define."
  _Effort: medium–large — changes the call convention._
- **`LOCAL "name`** — Declares a variable scoped to the current call without
  making it a parameter. Cheap to add on top of the scope stack that parameters
  already use.
  _Effort: small._

### Arithmetic

- **`MOD`, `POWER`, `SQRT`** — Arithmetic currently stops at `+ − * /`. Real Logo
  (and every teaching use of it involving geometry) leans on modulo and power
  constantly.
  _Effort: small._
- **`SIN`, `COS`, `ARCTAN`, `RANDOM`, `ROUND`** — Trig functions, a random-number
  generator, and rounding — the other load-bearing primitives behind spirals,
  circles, and anything randomized.
  _Effort: small._

### Words & lists

- **`ITEM`, `BUTLAST`** — The natural other half of `FIRST`/`BUTFIRST`/`LAST`:
  `ITEM` is random access by position, `BUTLAST` is "everything except the last
  element." Both slot straight into the substring/list machinery already built.
  _Effort: small._
- **`MEMBER?`, `EMPTY?`, `WORD?`, `LIST?`, `NUMBER?`** — Type and membership
  predicates. There's no way right now to ask "is this a list?" or "is X in this
  list?" from inside a running program.
  _Effort: small._
- **`MAP`, `FOREACH`, `FILTER`, `REDUCE`** — Higher-order iteration over a list,
  in place of hand-walking it with `FIRST`/`BUTFIRST` inside a `WHILE`. Needs
  `RUN` (below) as groundwork.
  _Effort: medium._
- **`RUN`, `APPLY`** — `RUN` executes a stored list as if it were typed code —
  the thing that makes a list double as a deferred program, not just data.
  Nothing in this interpreter can do that today.
  _Effort: medium._

### Turtle & robustness

- **`POS`, `HEADING`** — The turtle's position and heading can be *set* but never
  *read back* from a running program — there's no way to write `MAKE "h HEADING`
  and restore it later. Probably the cheapest, highest-value gap on this whole
  list.
  _Effort: small._
- **`HIDETURTLE` / `SHOWTURTLE`, `WRAP` / `FENCE` / `WINDOW`** — Toggling turtle
  visibility, and what happens when a turtle drives off the edge of the canvas
  (wrap around, stop dead, or keep going off-screen).
  _Effort: small–medium._
- **`LABEL`, `FILL`** — Draw text at the turtle's position, and flood-fill a
  closed region — both need new Cairo drawing calls in `ui.c`, not just
  interpreter logic.
  _Effort: medium._
- **`CATCH` / `THROW`** — Structured error recovery — right now an error just
  prints a message and the program keeps going or stops; there's no way for a
  procedure to catch and handle a failure from something it called.
  _Effort: medium–large._

## 🟣 Uncharted territory

These aren't gaps in the usual sense — they're directions only a handful of Logo
dialects ever took, each one closer to a different language than a missing
command. Worth knowing about; not a quick add.

**Object Logo** — *Coral Software, Macintosh, 1986*
Bolted real object-oriented programming onto Logo — classes, inheritance, and
message-passing via an `ask` construct, sitting on top of the same procedural
core. One of the only Logo dialects to take OOP seriously.
> Would mean a second object model living alongside procedures and variables —
> a language redesign, not a feature.

**MultiLogo** — *Mitchel Resnick, MIT Media Lab, 1990*
Introduced an `agent` construct: multiple independent Logo processes running
*concurrently*, each with its own program counter, originally built to control
several LEGO robots from one program. This is a different thing from this
project's `TELL` — `TELL` just switches which turtle a single sequential program
is currently steering. MultiLogo's agents are genuinely interleaved, with real
synchronization bugs to match.
> True concurrency needs cooperative scheduling inside `eval_logo` — a
> fundamentally different execution model from the current single-threaded
> interpreter loop.

**StarLogo / NetLogo** — *Resnick → Wilensky, MIT / Northwestern, 1990s–*
MultiLogo's idea taken to its limit: thousands of turtles plus a grid of
stateful *patches* — the ground itself is programmable — all stepping through
their own procedures once per simulation "tick," driven by broadcast commands
like `ask turtles [...]`. Built for agent-based modeling (ecosystems, traffic,
social dynamics), not drawing.
> A different programming model entirely — population-scale, tick-based,
> declarative — not an extension of turtle graphics as this project has it.

**Logo3D / turtleSpaces** — *Various, 2000s–*
The turtle gains a third axis to move through, plus pitch and roll alongside
heading, real 3D shapes, and in turtleSpaces' case, sound and simple game
interactivity layered on top.
> Means replacing the 2D Cairo canvas with a 3D renderer — a UI rewrite more
> than a language one.

**LEGO TC Logo / robotics Logo** — *MIT, 1980s (precursor to Mindstorms)*
The same turtle commands, but driving a real LEGO robot's motors and sensors
instead of an on-screen turtle — "embodied" programming with real-world timing
and I/O in place of a canvas.
> Interesting mainly as a reminder that FD/RT were always meant to be
> hardware-agnostic — not something this desktop app needs.

## Closing note

"The common repertoire" is done (see the status note at the top of that
section) — it slotted into `ROADMAP.md` one item at a time, the same way
nested lists and substrings did before this survey even existed.

"Uncharted territory" is here for context, not a backlog — each one is closer to
a research project than a pull request.

### Sources

- [Berkeley Logo 6.1 User Manual](https://people.eecs.berkeley.edu/~bh/docs/html/usermanual.html)
- [MultiLogo: A Study of Children and Concurrent Programming](https://www.media.mit.edu/publications/multilogo-a-study-of-children-and-concurrent-programming-2/), Resnick 1990
- [StarLogo](https://en.wikipedia.org/wiki/StarLogo), [NetLogo](https://en.wikipedia.org/wiki/NetLogo)
- [Object Logo (Macintosh Repository)](https://www.macintoshrepository.org/25985-object-logo-student-edition)
- [turtleSpaces](https://turtlespaces.org/)
