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

## Distribution & infrastructure

Not language features, and not from the dialect research pass above.
These came out of the 2026-08-13 Linux port, which turned "how does
someone who isn't the author actually get this" into a real question
for the first time — there are now three supported platforms and a
pre-built binary for exactly one of them. Same rule as the section
above: pick up only on explicit request. They're listed in dependency
order; the first unblocks the other two.

- [ ] **CI (GitHub Actions)** — build the project and run `make test`
  automatically on every push, on machines this project doesn't own. A
  YAML file in `.github/workflows/`; free for public repos.

  **Why it's first**: the Linux port's build failures are exactly what
  CI exists to catch. `-std=c11` hiding `open_memstream`/`usleep`/
  `clock_gettime` behind glibc's feature-test macros, and the missing
  `-lm`, were both *immediate, deterministic* failures on any Linux
  box — but they sat undiscovered until 2026-08-13, because the
  2026-08-12 session that scoped the port ran on macOS and said so
  outright ("this session's dev environment is macOS and cannot itself
  validate Linux builds"). A Linux job would have gone red the day the
  code was written. The same applies going forward: `#ifdef
  __APPLE__`-gated code in `src/ui.c` means the two platforms now
  compile *different source*, and only one of them gets compiled on any
  given developer machine.

  **Shape of a v1 slice**: one workflow, `make && make test` on
  `ubuntu-latest` and `macos-latest`, on push and PR. Roughly 20 lines.
  Runners are available in both x86_64 and aarch64, which matters
  because every machine this project has been developed on is aarch64
  while most Linux desktop users are x86_64 — CI is the cheapest way to
  compile for hardware nobody here owns.

  **The limit, stated plainly so CI isn't over-trusted**: it cannot see
  the GUI. The 2026-08-13 menu-bar bug compiled clean, emitted no
  warnings, passed all 8 suites and launched with zero stderr — a
  headless CI run would have been exactly as blind to it as the local
  suites were. CI covers the build and the test suites. Every GUI claim
  still needs introspection or a human.

- [ ] **A Linux package** — there's no Linux download, only build-from-
  source. Worth fixing eventually, but **not by uploading a binary to
  Releases**, which is the obvious move and the wrong one.

  **Why a plain binary doesn't work on Linux**: `bin/logomotive`
  dynamically links glibc, GTK4 and SDL2. glibc's symbol versioning is
  forward-compatible only, so a binary built on Fedora 42 won't start
  on Ubuntu 24.04's older glibc — you must build on the *oldest* system
  you intend to support, not the newest. GTK compounds it: the verified
  targets are 4.18.6 (Fedora) and 4.14.5 (Ubuntu). macOS has no
  equivalent problem, which is why the existing single-binary approach
  works there and doesn't transfer.

  **Preferred answer: Flatpak, published on Flathub.** GTK4 comes from
  the `org.gnome.Platform` runtime rather than the host, so the version
  skew disappears by construction; Flathub's build bots produce x86_64
  and aarch64 from one manifest, which also solves the "all dev
  machines here are aarch64" problem and doubles as CI for the packaged
  build. Cost is learning a manifest file. **Unverified**: whether SDL2
  comes from the GNOME runtime or has to be a bundled module in the
  manifest — check before committing to an estimate.

  Alternatives, and why they rank lower: **AppImage** is a single file
  with no install, which is genuinely appealing, but bundling GTK4 is
  notoriously fiddly (gdk-pixbuf loaders, GSettings schemas, icon
  themes and graphics drivers all have to be handled by hand) — more
  work than Flatpak for strictly less compatibility. **`.deb`/`.rpm`**
  are the most native and the most maintenance: one build plus a
  dependency declaration per distro per release.

- [ ] **Automated, notarized releases** — the current release is
  `v0.1.0` (2026-08-12), a single hand-built asset
  (`logomotive-v0.1.0-macos-arm64.zip`). Built and uploaded by hand,
  which doesn't scale to three platforms and doesn't repeat reliably.

  Two separate things are missing. **Automation** — building the
  release artifacts from a tag rather than from whatever was on the
  author's machine that day — is blocked on CI above and is mostly
  mechanical once it exists. **macOS notarization** is the real work:
  Apple requires a downloaded app to be signed and notarized or
  Gatekeeper refuses to open it, which needs a paid Developer ID and a
  signing step in the release job. Not verified either way what
  Gatekeeper currently does with the v0.1.0 zip on a machine that
  didn't build it — worth actually checking before assuming it's a
  problem, since that determines whether this is urgent or cosmetic.

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
