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

- [ ] **Linux port** — **the dev target now works**; what's left is
  validation on the other two distros and one UX fix. LogoMotive builds
  and runs on Fedora 42 (aarch64) as of 2026-08-13: clean `make`, all
  eight `make test` binaries green, `bin/logi`/`bin/vmrun` working, and
  the GUI running under both the Wayland and X11 GDK backends. Full
  write-up in `docs/CHANGELOG.md`; the short version is that the
  2026-08-12 prediction held — it was build configuration, not a source
  port. Three `Makefile` flags (`-std=gnu11`, `-lm`, `-fconserve-stack`)
  and one test that had hardcoded a platform-specific float, no changes
  to `src/*.c` at all.

  **Remaining before the port is done:**

  - **Ubuntu 24.04 LTS and Linux Mint (Cinnamon)** — the original
    pre-release validation targets, still unbuilt. Both lag Fedora's
    GTK4 version (Mint tracks Ubuntu LTS directly), which is why they
    were never the dev target, but they're the representative
    real-world ones: Ubuntu LTS as the most common Linux desktop
    baseline, Mint as a proxy for less technical users on older/modest
    hardware. `scripts/install_gtk.sh` already has an `apt-get` branch,
    but its package names (`libgtk-4-dev`, `libsdl2-dev`) come from
    documentation rather than a real build — verify them there first.
  - **Menu accelerators use `<Meta>`** (`src/ui.c`, all ten of them:
    open, save, load/save bytecode, export PNG, the three text-size
    actions, toggle input window, quit). `<Meta>` is Cmd on macOS but
    the Super key on Linux, where the convention is Ctrl — and GNOME
    reserves many Super combinations for the shell, so several are
    likely swallowed before the app ever sees them. Found by reading
    `src/ui.c`, not by using the app — the Fedora verification covered
    the build, the headless suites and a clean GUI launch, but nobody
    has click-tested the menus or tried a shortcut there yet, so treat
    the severity as predicted rather than observed. The code comment
    there justifies the choice with "this app is macOS-only anyway",
    which is precisely the assumption that just stopped being true.
    Wants a platform conditional, not a global swap to `<Primary>` —
    that same comment records that `<Primary>` renders as Ctrl in the
    menu on macOS's GTK build, which is why it was avoided originally.

  Separate from pre-built binaries via GitHub Releases (not yet on
  this list) — that idea is blocked on CI + macOS notarization for the
  *existing* macOS build, and now would want a Linux artifact too.

- [ ] **Inline VM assembly blocks (`{...}`)** — write raw bytecode
  instructions directly in Logo source, delimited like `[...]` is for
  lists but containing assembler text instead. Scoped 2026-08-12, no
  blocker found — architecturally cleaner than `.MACRO` below, because
  it splices straight into the chunk already being compiled at that
  point rather than running a separate scratch chunk through a nested
  `vm_run` call, so it never hits the `VmFrame`-has-no-chunk-pointer
  issue that blocks `.MACRO`/`RUN`/`LOAD`/`EXECTIME`.

  **Shape of a v1 slice**: `{`/`}` aren't literally free today —
  `is_bareword_char()` (`src/lexer.c`) currently swallows them into
  ordinary barewords — but nothing depends on that; needs two new
  token types and excluding `{`/`}` from the bareword charset, same
  as `[`/`]` already work. `src/bytecode.c`'s existing hand-rolled
  text assembler (`bytecode_assemble` plus its internal
  `asm_parse_operands`/`asm_lookup_opcode`/`asm_is_label_line`/
  `asm_resolve_target` helpers, built for the save/load-bytecode
  feature) does most of the real work already — needs a
  `bytecode_assemble_fragment()` variant that appends onto the chunk
  currently being compiled at its current pc instead of building a
  whole standalone `START:`/`PROCS:`/`CODE:` file; constant-pool
  interning for word/list literals already takes `chunk` as a
  parameter, so literals used inside a block land in the same pool
  for free. Two real gaps: `OP_CALL_PROC` bakes an absolute target pc
  at compile time and `asm_resolve_target` doesn't fall back to
  `chunk->procs[]` by name, so calling an existing Logo procedure from
  inside a block needs the same lookup `compile_call` already does via
  `find_proc_def`; and jump targets inside a block should be
  restricted to labels defined within that same block (reject bare
  `@N`, since the real pc is only known once spliced). Recommend
  starting statement-only, not usable in expression position — that
  needs `OP_CHECK_OUTPUT`, which reads flags (
  `last_call_produced_output`/`last_call_resolved`) set by specific
  call opcodes, not "is there a value on the stack," so raw assembly
  wouldn't set them correctly without its own convention.

  **Safety, precisely**: no memory-safety/ASan risk — `push`/`pop`
  (`vm.c`) are bounds-checked and silently no-op on stack over/
  underflow, and `chunk->code` is an array of fixed-size tagged
  `Instr` structs, so jumping to any in-chunk pc is well-defined
  dispatch, never a misaligned decode. The real risk is logical, not
  memory-unsafe: nothing statically verifies a block's net stack
  effect, so an unbalanced block silently desyncs the value stack for
  the rest of that procedure call (`VmFrame.value_stack_base` is just
  a stack offset). Accepted nature of an escape hatch, same category
  as inline asm in C — document plainly rather than try to eliminate.

## Not being considered yet — real complications found while scoping

Unlike the list above, these aren't just unprioritized — scoping them
out (2026-08-12) turned up a genuine architectural blocker, not just
"nobody's gotten to it yet." Don't pick these up without discussing the
complication first.

- [ ] **`.MACRO`** (UCBLogo) — a user-defined procedure whose *output*
  (a list of Logo instructions) gets spliced back in and evaluated in
  the caller's own context, rather than returned as a value — lets
  users invent their own control structures (`REPEAT`/`IF`-like) that
  can `STOP`/`OUTPUT`/`LOCAL` correctly in the caller's frame, which an
  ordinary wrapper procedure can't do.

  **The complication**: that "`STOP`/`OUTPUT` reaches back into the
  caller's frame" behavior is exactly the one thing `RUN`/`LOAD`/
  `EXECTIME` already document as broken — `vm.c`'s own `exec_run`
  comment says outright: `"RUN: OUTPUT/STOP escaping the RUN'd
  snippet's own top level is not fully supported"`. Traced why: those
  three all run a freshly-compiled scratch `BytecodeChunk` via a
  recursive `vm_run` call that *shares the caller's own frame stack*
  (`vm.h`'s `VmFrame` — deliberately, per `docs/BYTECODE_VM_DESIGN.md`)
  — but a `VmFrame` only stores `{return_pc, value_stack_base}`, with
  no record of *which chunk* that `return_pc` belongs to. If `STOP`/
  `OUTPUT` inside the scratch code pops an ancestor frame that was
  pushed against the *original* chunk, the recursive `vm_run` call
  keeps interpreting that `return_pc` against its own *scratch* chunk
  instead — a real correctness hazard (wrong bytecode array), not a
  missing nice-to-have. A "shallow" `.MACRO` that just auto-`RUN`s a
  macro's output would inherit this exact gap, which would kill the
  entire point of macros: correct escape is what a macro-based custom
  control structure needs to actually work.

  A real port means fixing the underlying gap first — giving each
  `VmFrame` its own chunk reference so `vm_run`'s dispatch loop can
  correctly resume in the right chunk after a cross-boundary `STOP`/
  `OUTPUT`. That's a change to the VM's core dispatch loop, not a
  mechanical command port like everything shipped so far — bigger and
  riskier than any single feature this project has shipped to date. One
  real upside: fixing it would *also* retroactively fix `RUN`/`LOAD`/
  `EXECTIME`'s own long-standing limitation, not just enable macros.
  Deliberately not started (2026-08-12, at the user's request) —
  discuss the frame/chunk redesign explicitly before picking this back
  up.
