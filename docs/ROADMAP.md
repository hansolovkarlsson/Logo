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
for the first time. The 2026-08-14 Windows port then made it sharper
again: four supported platforms now (macOS, Linux, Windows x86-64,
Windows ARM64), with a downloadable build for two of them — macOS by
hand, Windows from CI. Same rule as the section above: pick up only on
explicit request.

- [x] **CI (GitHub Actions)** — done 2026-08-14,
  `.github/workflows/build.yml`. Four rows: `ubuntu-latest`,
  `macos-latest`, `windows-latest` (MSYS2 UCRT64) and `windows-11-arm`
  (MSYS2 CLANGARM64), each running `make` then `make test`, with
  `fail-fast` off so one platform's failure doesn't hide the others'
  results. The Windows rows also run `scripts/bundle_windows.sh` and
  upload the result, so every build produces a downloadable standalone
  Windows binary.

  Dependencies are installed by running the project's own
  `scripts/install_gtk.sh` rather than by listing packages in the
  workflow, so CI exercises the same script users are told to run and
  the two can't drift. That also finally exercises its `apt-get`
  branch, which had never actually been run.

  **The reasoning that put this first held up, twice.** The Linux
  port's `-std=c11` hiding `open_memstream`/`usleep`/`clock_gettime`
  behind glibc's feature-test macros, and its missing `-lm`, were
  immediate deterministic failures on any Linux box that sat
  undiscovered because the scoping session ran on macOS. The Windows
  port then repeated it exactly: `strcasestr` and `clock_gettime64`
  were instant deterministic failures on mingw-w64 that a careful
  static read of the sources did not predict. A red job either day
  would have said so in minutes.

  **The limit, stated plainly so CI isn't over-trusted**: it cannot see
  the GUI. The 2026-08-13 menu-bar bug compiled clean, emitted no
  warnings, passed all 8 suites and launched with zero stderr — a
  headless CI run would have been exactly as blind to it as the local
  suites were. The 2026-08-14 console-output bug is the same shape and
  worse: it would pass CI, because every way CI captures output is also
  a way that makes the bug disappear. CI covers the build and the test
  suites. Every GUI claim, and every claim about what a *terminal*
  shows, still needs a human or a screenshot.

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

  **One data point on the AppImage cost, from having now done the
  equivalent on Windows** (`scripts/bundle_windows.sh`, 2026-08-14):
  hand-bundling GTK4 turned out to be tractable rather than forbidding
  — a transitive DLL walk plus exactly the three filesystem-resolved
  pieces named above. Two caveats before reading that as encouragement.
  Windows has no graphics-driver problem, which is the item on that
  list most likely to be the genuinely hard one on Linux. And it still
  produced a ~50 MB artifact per architecture. So the estimate for
  AppImage should come down somewhat, but the ranking below Flatpak
  stands.

- [x] **Automated releases** — done 2026-08-14,
  `.github/workflows/release.yml`. Pushing a `v*` tag builds, tests and
  publishes three archives: Windows x86-64, Windows ARM64 and macOS
  ARM64. The Windows two are self-contained (`bundle_windows.sh`'s DLL
  closure); macOS carries the binary and examples and states its
  Homebrew requirement, matching the shape of the hand-built v0.1.0
  archive so anyone who downloaded that finds this familiar.

  Linux builds and tests in the release run but publishes no asset, for
  the glibc reason in the entry above — it gates the release rather
  than contributing to it, since cutting one from a tree that doesn't
  build on Linux would be worse than shipping no Linux asset.

  `workflow_dispatch` runs everything except the publish step, so the
  whole pipeline can be exercised without cutting a release.

  **A real bug this turned up**: the v0.1.0 archive's own README.txt
  told users to `brew install gtk4` and stopped there. The binary links
  SDL2 as well (joystick and audio), so following those instructions
  exactly gets a dyld error rather than a window. The generated README
  (`scripts/release_readme.sh`) names both.

- [ ] **macOS notarization** — split out from the automation above,
  which is now done. Apple requires a downloaded app to be signed and
  notarized or Gatekeeper refuses to open it. That needs a paid
  Developer ID, the certificate and an app-specific password in repo
  secrets, and a signing step in the release job — none of which can be
  set up from outside the account, which is why this is separate rather
  than folded in.

  Still **not verified either way** what Gatekeeper actually does with
  an unsigned zip on a machine that didn't build it. Worth checking
  before assuming urgency, since that determines whether this is a real
  barrier or a scary-looking dialog with a documented right-click
  workaround. The archives' README.txt describes that workaround today.

  Windows SmartScreen is the same shape of problem and is deliberately
  *not* being treated as work: an EV certificate is expensive and the
  warning is dismissible.

- [ ] **Bundle the macOS dylibs** — the Windows archives are
  self-contained but the macOS one isn't, so it's the only download
  that asks the user to install anything. Doing it means rewriting
  install names with `install_name_tool`, which is fiddly enough to
  want testing rather than assuming; it was left alone in the
  2026-08-14 release work specifically because no macOS machine was
  available to test it on, and shipping an untested bundler would be
  worse than a documented Homebrew line.

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
