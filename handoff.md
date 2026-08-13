# Session Handoff

**Date:** 2026-08-13
**Repo:** `~/Projects/LogoMotive` (GitHub: `hansolovkarlsson/LogoMotive`)
**Branch:** `main` (all work committed and pushed directly, no feature
branch — the standing project convention)
**Range:** `56f3215..70004d6` (6 commits)

## Core Goal

Execute the **Linux port** that the previous session (2026-08-12) had
only scoped. That session filed it on `docs/ROADMAP.md`'s "Future /
unplanned" list and named its blocker explicitly: "this session's dev
environment is macOS and cannot itself validate Linux builds."

That blocker was gone — this session ran on **Fedora 42 (aarch64)**. So
the work was: actually build it, fix whatever broke, and get the docs to
match reality.

## Current Status & Progress

**Fully completed — Fedora 42 (aarch64) is a working, verified target:**

- Clean `make`, all **8/8** test binaries passing, `bin/logi` and
  `bin/vmrun` built and smoke-tested, `./build.sh` working.
- GUI runs under **both** the Wayland and X11 GDK backends.
- Menus, keyboard shortcuts (`Ctrl+S`/`Ctrl+Q`/`Ctrl+O`), text-size
  actions and an example script all **exercised by hand by the user** —
  not just introspected.
- macOS re-verified by the user after the flag changes: builds clean,
  all 8 suites pass, unchanged behaviour.

**Environment note for the next session:** `gtk4-devel` and `SDL2-devel`
are now installed on this Fedora box, and `git config user.name/email`
is set **repo-locally** (git had no identity on this machine; the values
were taken from existing commits, not guessed). `sudo` requires a
password here — the assistant cannot install packages unattended.

**Pending / not started:**

- **Ubuntu 24.04 LTS and Linux Mint (Cinnamon)** — the original
  pre-release validation targets, still completely unbuilt. This is the
  *only* thing left on the roadmap's Linux port item.
- `scripts/install_gtk.sh`'s `apt-get` and `pacman` package names
  (`libgtk-4-dev`, `libsdl2-dev`, etc.) are **from documentation, never
  from a real build**. Marked as such in the script's own comments.
- The roadmap item is deliberately still open (`- [ ]`) rather than
  moved to `docs/CHANGELOG.md`, because its own definition of done is
  "build clean on Fedora first, then confirm on both before calling the
  port done." Declaring it shipped now is a judgement call left to the
  user.

## Key Decisions Made

**Build fixes — three flags, one shared flag set, no per-OS branch.**
The 2026-08-12 prediction that this would be "mostly build-script work,
not a source port" held for the build: **`src/*.c` needed zero changes**
to compile and pass tests. Each flag is a real macOS-vs-Linux
difference, and each is documented in place in the `Makefile`:

- **`-std=c11` → `-std=gnu11`** (13 occurrences). Strict conformance
  leaves glibc's POSIX feature-test macros undefined, hiding
  `open_memstream`, `usleep` and `clock_gettime`. macOS headers don't
  gate them. It failed as a hard *error* because gcc 14 promoted
  `-Wimplicit-function-declaration` to an error.
- **`-lm` on all 8 link lines.** macOS folds libm into libSystem; glibc
  keeps it separate, so `eval.c`'s `atan2` failed to link.
- **`-fconserve-stack`**, gcc-only and therefore **probed, not
  hardcoded**. The probe uses `-Werror` deliberately: clang treats
  unknown `-f` optimization flags as a *warning*, so a naive probe would
  pass and inject a flag that breaks the Mac build. `CC = gcc` resolves
  to Apple clang on macOS, so the probe genuinely runs against clang
  there. Verified working by the user on their Mac.

**Why `-fconserve-stack` and not a different `-O` level.** `exec_call`
(`src/eval.c`) is one giant switch; gcc at `-O1` gives every branch's
locals its own stack slot. Measured with `-fstack-usage`, its frame is
**61,856 bytes** — each Logo recursion level costs one, so an 8 MB stack
dies near depth ~130 and `test_eval` segfaulted before
`MAX_SCOPE_DEPTH`'s 200-level cap could report the error it exists to
assert. The flag cuts the frame to **1,232 bytes** (50×). Measured
alternatives, so nobody re-derives this: `-O2` is *worse* (63,376), and
`-O2 -fconserve-stack` is far worse than `-O1 -fconserve-stack` (27,536
vs 1,232).

**Two real GUI bugs — the part the 2026-08-12 scoping missed.** Both in
`src/ui.c`, both `#ifdef`-gated so macOS is untouched by construction:

- **Accelerators were `<Meta>`** (Cmd on macOS, Super on Linux). Now
  `#ifdef __APPLE__`-gated, Linux getting `<Control>` **directly rather
  than `<Primary>`** — deliberate, because the original code comment
  records that `<Primary>` renders as Ctrl in the menu on macOS's
  Homebrew GTK build, which is why it was rejected the first time.
  Hardcoding `<Control>` on Linux avoids depending on `<Primary>`
  resolving correctly on either platform.
- **The menu bar didn't render on Linux at all** — no File, no View, so
  load/save, export and the text-size actions were unreachable except by
  shortcut. `gtk_application_set_menubar()` suffices on macOS (the
  backend hands the model to the global menu bar), but on Linux
  `GtkApplicationWindow` builds the widget itself and **GTK4 changed
  `show-menubar` to default FALSE** (TRUE in GTK3). Fixed with an
  `#ifndef __APPLE__`-gated
  `gtk_application_window_set_show_menubar(..., TRUE)` — gated so macOS
  can't draw an in-window bar duplicating its global one. Both window
  paths (`logo_activate`, `logo_open`) go through `build_main_window`,
  so one call covers both.

**One test was over-specified, not wrong.**
`test_setheading_towards_then_forward_reaches_the_point`
(`tests/test_eval.c`) asserted the exact string `"-1.52006e-13 100\n"`.
That x is mathematically 0; the digits are platform libm rounding
residue, and glibc/aarch64 leaves `-1.42109e-13`. Confirmed it fails on
the *unmodified* `-O1` build too (run with a raised `ulimit -s` to get
past the stack overflow first), so it is a genuine libm difference and
not a side effect of the flag changes. Now parses the numbers and uses
the suite's existing `CHECK_NEAR`. It was the only hardcoded
float-noise literal in the whole suite.

**Package-manager detection in one script, not a script per platform.**
`scripts/install_gtk.sh` now probes brew → dnf → apt-get → pacman rather
than a new `install_gtk_linux.sh` (which the previous handoff had
anticipated). Rationale: `Makefile` and `build.sh` are already
platform-neutral, so this is the only file in the repo that needs to
know what OS it's on. brew is probed first so a Mac with another package
manager still takes the Homebrew path.

**Verification without eyes.** Screenshot APIs are blocked in this
environment (GNOME's DBus screenshot returns `AccessDenied`; `import`
can't reach the Wayland compositor). Two techniques worked instead and
are worth reusing: a **standalone GTK probe** that walks the widget tree
(`gtk_widget_get_first_child`/`get_next_sibling`) to assert a
`GtkPopoverMenuBar` exists, and **AT-SPI introspection of the running
binary** via `gi.repository.Atspi`, which showed the live app going from
no menu to `menu bar: 'Menu bar'` with `File`/`View`.

## Files & Paths Touched

Commits, oldest first:

| Commit | Summary |
|---|---|
| `a492f11` | Make LogoMotive build and run on Linux |
| `7e00113` | Remove handoff.md (the previous session's, now superseded) |
| `7ea4e43` | Fix menu accelerators to use Ctrl on Linux instead of Meta *(authored on the user's Mac)* |
| `58c2e31` | Record Fedora verification of the Ctrl accelerator fix |
| `0d963c7` | Show the File/View menu bar on Linux |
| `70004d6` | Record hand-verification of the Linux GUI on Fedora |

**Modified:**

- **`Makefile`** — the three flags above, each with a full explanatory
  comment block. `CONSERVE_STACK` probe variable added near the top.
- **`src/ui.c`** — the two `#ifdef`-gated GUI fixes (accelerator block
  around the `*_accels[]` declarations; `show_menubar` call just after
  `gtk_application_set_menubar`, before `gtk_window_present`).
- **`tests/test_eval.c`** — the `CHECK_NEAR` fix described above.
- **`build.sh`** — passed **neither `sdl2` nor `-lm`**, so the "one-shot
  alternative to `make`" had been failing at link on *every* platform
  since Phase 4 added SDL2. Now mirrors the Makefile's flags (including
  its own `-fconserve-stack` probe), since `-O0` would also produce a
  binary that blows the stack on deep recursion.
- **`scripts/install_gtk.sh`** — rewritten as described. Also now
  installs **SDL2, which it had never installed even on macOS** despite
  the Makefile requiring it since Phase 4. A stale commented-out
  `gtk+3` line was dropped.
- **`README.md`** — "macOS and Linux" throughout, Homebrew marked
  macOS-only, new **Platform support** table, `scripts/` no longer
  described as Homebrew-specific, "native macOS menu bar" dropped from
  the View-menu feature line.
- **`docs/ROADMAP.md`** — Linux port entry rewritten around what
  remains; both GUI bugs recorded with their reasoning.
- **`docs/CHANGELOG.md`** — dated write-up of the build-side port.
- **`website/index.html`** — download card now says GTK4 **and** SDL2
  (it under-stated deps for macOS too) and points Linux users at
  building from source.

**Deleted:** `handoff.md` (the 2026-08-12 one) — superseded, at the
user's request. Recoverable via `git show 56f3215:handoff.md`.

**Not touched, deliberately:**

- `website/reference.html` — generated by `website/build_reference.py`;
  hand-editing it would be wrong.
- The "Download for macOS" hero button in `website/index.html` — the
  pre-built binaries genuinely are macOS-only, and Releases artifacts
  are a separate blocked roadmap item.
- The "native macOS menu bar" phrasing still in `docs/LANGUAGE.md`
  (~L1731/L1737) and `docs/COMMAND_REFERENCE.md` (~L784). Now
  understated on Linux, where an in-window menu bar renders instead.
  Left alone because `COMMAND_REFERENCE.md` drives the generated
  website page and this wasn't in scope. **Small, real doc debt.**

## Failed/Rejected Approaches

- **Don't reach for `-O2` to shrink stack frames.** Measured, it makes
  `exec_call` *worse* than `-O1` (63,376 vs 61,856 bytes), and
  `-O2 -fconserve-stack` is 22× worse than `-O1 -fconserve-stack`.
- **Don't drop `-Werror` from the `CONSERVE_STACK` probe.** It is not
  decoration. clang warns rather than errors on unknown `-f`
  optimization flags, so without it the probe passes on macOS and
  injects a flag clang rejects.
- **Don't assume a clean build + green test suite means the GUI works.**
  This is the session's main lesson. The menu bar bug survived three
  commits: it compiled clean, emitted no warnings, passed all 8 suites,
  and launched with zero stderr. The headless suites cannot see the GUI,
  and "it starts without errors" was never evidence the UI was correct.
- **Don't try to screenshot in this environment.** GNOME's DBus
  screenshot method returns `AccessDenied`; ImageMagick `import` fails
  against the Wayland compositor; no `grim`, `gnome-screenshot`, `xwd`
  or `gdb` installed. Use the widget-tree probe or AT-SPI instead.
- **Don't use `pkill -f "bin/logomotive"`.** The pattern matches the
  invoking shell's own command line and kills it. Use `pkill -x
  logomotive` or a bracketed pattern (`pgrep "logomotiv[e]"`).
- **`timeout N ./bin/logomotive` does not kill the app.** It installs
  its own signal handler for Ctrl+C interrupts (`request_interrupt`, see
  `src/main.c`) and survives SIGTERM. Kill it explicitly afterwards.
- **Don't assume `strcasecmp` needs a feature-test macro.** It was the
  obvious suspect at ~430 uses, but glibc declares it unguarded — it was
  never the problem.
- **From the previous session, still valid:** `{`/`}` are *not* free
  tokens (`is_bareword_char()` swallows them); `asm_resolve_target`
  can't resolve procedure names; in-block jump targets should be labels
  only, not bare `@N`.

## Immediate Next Step

**No implementation is queued, and nothing is broken.** The tree is
clean, `main` is pushed, and Fedora is fully verified. Wait for the user
to pick something rather than starting work unprompted.

The single obvious continuation, if they want the Linux port closed out:

1. Get an **Ubuntu 24.04 LTS** machine or VM (Mint after it).
2. `./scripts/install_gtk.sh` — this is the **first real test of the
   `apt-get` branch**; confirm `libgtk-4-dev` and `libsdl2-dev` are the
   right package names and fix the script if not.
3. `make clean && make && make test` — expect 8/8.
4. **Then actually open the File and View menus and press `Ctrl+S`.**
   Do not skip this. Ubuntu and Mint ship *older* GTK4 than Fedora, and
   the menu-bar bug was a GTK-version behaviour difference — precisely
   the class of thing that can differ again, and precisely what a clean
   build and green suite will not tell you.
5. If both pass, the roadmap item's own done-criteria are met: move the
   Linux port write-up from `docs/ROADMAP.md` into `docs/CHANGELOG.md`
   per that file's stated convention.

Otherwise the roadmap's "Future / unplanned" section still holds its
other 7 items (inline `{...}` VM assembly blocks, patch grid, breeds,
`ERRACT`, `MDARRAY`, live-bound widgets, live data plots), all
pick-up-on-explicit-request only.
