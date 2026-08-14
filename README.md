<img src="website/assets/turtle-mark.svg" width="72" height="72" alt="">

# LogoMotive

LogoMotive is a Logo interpreter with turtle graphics, built in C with GTK4 —
a bytecode compiler and VM underneath, with concurrent multi-turtle agents,
sprites, and more on top of the classic language.

Type commands into the entry box and watch the turtle draw on the canvas — the
classic 80s Logo experience, running natively on macOS, Linux and Windows.

**[Tutorial & command reference →](https://hansolovkarlsson.github.io/LogoMotive/)**

## Features

- Turtle graphics canvas (Cairo-drawn) alongside a REPL/history pane
- Core turtle commands: `FORWARD`/`FD`, `BACK`/`BK`, `RIGHT`/`RT`, `LEFT`/`LT`,
  `PENUP`/`PU`, `PENDOWN`/`PD`, `CLEAR`/`CS`
- `REPEAT n [ ... ]` loops
- User-defined procedures: `TO name :param ... END`
- Variables: `MAKE "name expr` to set, `:name` to read
- Numeric expressions: `+ - * /` and parentheses, usable anywhere a number is
  expected (`FD :size * 2 + 10`)
- Conditionals: `IF <cond> [block]`, `IF <cond> [block] ELSE [block]`, and
  classic `IFELSE <cond> [block] [block]`, with `< > = <= >= <>` comparisons
- `PRINT`/`PR` to write text or expression results to the history pane
- A **View** menu to increase, decrease, or reset the text size of the REPL

## Requirements

- macOS, Linux, or Windows
- GTK4, SDL2 and pkg-config
- A C compiler and `make`
- `scripts/install_gtk.sh` installs all of the above on Linux and
  Windows, and the libraries on macOS (where the compiler comes from
  Xcode's Command Line Tools instead); it verifies the toolchain is
  present either way
- On macOS: [Homebrew](https://brew.sh)
- On Windows: [MSYS2](https://www.msys2.org) — see
  [Building on Windows](#building-on-windows) below

Linux is built and tested on Fedora and Ubuntu (see
[Platform support](#platform-support) below for exactly what's verified
where). There's a step-by-step Linux build guide, with per-distribution
package commands and troubleshooting, at
[`website/linux.html`](website/linux.html).

## Download

Pre-built archives for each tagged release are on the
[Releases page](https://github.com/hansolovkarlsson/LogoMotive/releases):
Windows x86-64, Windows ARM64 and macOS (Apple Silicon). The Windows
archives are self-contained; the macOS one needs `brew install gtk4
sdl2` first. Each holds the executable, the `examples/` scripts and a
`README.txt` with that platform's setup notes.

There is no Linux download — build from source below, which is three
commands and usually under a minute. A dynamically-linked Linux binary
can't be distributed usefully because glibc's symbol versioning is
forward-compatible only; a Flatpak is the intended fix (see
[`docs/ROADMAP.md`](docs/ROADMAP.md)).

None of the builds are code-signed, so macOS Gatekeeper and Windows
SmartScreen will both warn on first run; each archive's `README.txt`
says how to proceed.

## Build & Run

```sh
./scripts/install_brew.sh   # macOS only, and only if Homebrew isn't installed
./scripts/install_gtk.sh    # gtk4 + sdl2 + pkg-config + toolchain, via brew/dnf/apt/pacman/MSYS2
make run
```

On Windows the dependency step is different — see below — but `make`,
`make run` and `make test` are then exactly the same commands.

`make` alone builds `bin/logomotive` without launching it; `make clean` removes
build artifacts; `make test` runs the headless interpreter test suite
(no GTK window needed).

Pass a `.logo` file on the command line to load and run it immediately
on startup, instead of starting with a blank window:

```sh
bin/logomotive examples/concurrent_agents.logo
```

Pass `--speed <seconds>` to start every turtle-motion command already
throttled by that many seconds (same as calling `SETSPEED` as the
script's first line) -- useful for watching a drawing unfold step by
step:

```sh
bin/logomotive --speed 0.2 examples/multiple_turtles.logo
```

Pass `--headless` to run a script with no window at all, for
scripting/automation rather than interactive use -- prints whatever
the script `PRINT`s, exits 0 on success, and every suspend point
(`WAIT`, `SETSPEED`'s throttle, `ANIMATESPRITE`, `LAUNCH`'s concurrent
agents) resolves instantly rather than pausing for real time.
`WAITKEY`/`INPUT` read a real line from stdin instead of hanging:

```sh
bin/logomotive --headless examples/concurrent_agents.logo
echo world | bin/logomotive --headless a_script_that_uses_input.logo
```

`bin/logomotive -h` / `--help` prints the full option list above and exits.

On Windows the binary is `bin/logomotive.exe`; every command above works
unchanged otherwise, including `--headless` printing straight to a
console (see [Building on Windows](#building-on-windows)).

## Building on Windows

Everything builds from the same sources and the same `Makefile` as macOS
and Linux; the only Windows-specific pieces are `src/compat.h`'s shims
and the Makefile's own `STACK_FLAGS` (both explained in their own
comments). Build under [MSYS2](https://www.msys2.org), which supplies
the compiler, GTK4, SDL2, pkg-config, and the POSIX shell the Makefile
needs for `mkdir -p` / `rm -rf` and its `$(shell ...)` probes.

There's a step-by-step Windows build guide, with per-environment package
commands and troubleshooting, at
[`website/windows.html`](website/windows.html) — the counterpart to the
Linux one.

Pick the MSYS2 environment matching your CPU and open that shell:

| CPU | MSYS2 environment | Package prefix |
|---|---|---|
| ARM64 | **CLANGARM64** | `mingw-w64-clang-aarch64-` |
| x86-64 | **UCRT64** | `mingw-w64-ucrt-x86_64-` |

Then, from that shell:

```sh
./scripts/install_gtk.sh
make run
```

The script reads `$MSYSTEM` and installs the correctly-prefixed
packages for whichever environment you opened — no `sudo` needed, since
MSYS2's `pacman` runs as you. Open the plain **MSYS** shell by mistake
and it says so specifically, rather than failing partway through: that
environment builds against `msys-2.0.dll` and has no native GTK4 at all.

To install by hand instead (CLANGARM64 shown):

```sh
pacman -S --needed \
    mingw-w64-clang-aarch64-clang \
    mingw-w64-clang-aarch64-gcc-compat \
    mingw-w64-clang-aarch64-pkgconf \
    mingw-w64-clang-aarch64-gtk4 \
    mingw-w64-clang-aarch64-SDL2 \
    make
```

`gcc-compat` is what supplies the `gcc` the Makefile's `CC` asks for,
wrapping clang; on UCRT64 you get a real gcc instead and can install
`mingw-w64-ucrt-x86_64-gcc` in place of the `clang`/`gcc-compat` pair.

To hand the result to someone without MSYS2, `scripts/bundle_windows.sh`
collects the `.exe`, its ~60 GTK/SDL DLLs and the runtime data GTK looks
up by path into a self-contained folder under `dist/`. CI publishes one
of these for both architectures on every build.

Two things behave differently on Windows and are handled in-tree rather
than being anything you need to do:

- **Console output.** GTK4's own pkg-config libs link `-mwindows`, which
  makes the binary GUI-subsystem, and Windows withholds a console from
  such a process even when a console launched it. `main.c` calls
  `AttachConsole(ATTACH_PARENT_PROCESS)` at startup so `--help` and
  `--headless` print normally, while leaving redirection and pipes
  alone.
- **Line endings.** `.gitattributes` pins the working tree to LF, and
  `logo_normalize_newlines` (`src/lexer.h`) normalizes ingested source,
  so a `.logo` file written in a Windows editor with CRLF behaves
  exactly like an LF one.

## Platform support

| | Status |
|---|---|
| **macOS** (Apple Silicon) | The original target — developed and used here daily. |
| **Fedora** (42, aarch64) | Verified 2026-08-13: clean `make`, full `make test` suite green, GUI runs under both Wayland and X11, menus and keyboard shortcuts exercised by hand. |
| **Ubuntu** (24.04 LTS, aarch64) | Verified 2026-08-13: clean `make`, full `make test` suite green, `build.sh` and `--headless` working, File/View menu bar confirmed present on its older GTK 4.14.5 via accessibility introspection, and `Ctrl+O`/`Ctrl+S` confirmed firing by hand. `scripts/install_gtk.sh`'s `apt-get` package names check out. |
| **Ubuntu derivatives** (Linux Mint, Pop!_OS, …) | Expected to work, covered by the Ubuntu row rather than tested separately — Mint's main edition is built on the Ubuntu LTS base and installs the same GTK4/SDL2 packages. Mint ships Cinnamon rather than GNOME, which could in principle grab a keyboard shortcut differently; menu-bar rendering is GTK-internal and unaffected. LMDE is Debian-based, so it's outside this. |
| **Windows 11** (ARM64, MSYS2 CLANGARM64) | Verified 2026-08-14 on clang 22.1.8 / GTK 4.22.4 / SDL2 2.32.10: clean `make` with zero warnings, full `make test` suite green, GUI runs and draws with the File/View menu bar present, `--headless` correct to a console, to a file redirect and through a pipe, and `INPUT` reading a piped stdin. |
| **Windows** (x86-64, MSYS2 UCRT64) | Verified 2026-08-14 by CI, not by hand — no x86-64 machine was available here, which is exactly the gap CI was added to cover: clean `make` and a full green `make test`, plus `scripts/install_gtk.sh`'s own UCRT64 branch and `scripts/bundle_windows.sh` both exercised. Genuinely a different configuration from the ARM64 row rather than a second copy of it, since UCRT64 has a real gcc: `-fconserve-stack` applies there, so `STACK_FLAGS` carries less of the load. Not GUI-tested — CI can't see a window. |

The build is a single shared flag set for all three platforms rather
than a per-OS branch — see the `Makefile`'s own comments for why
`-std=gnu11`, `-lm` and `-fconserve-stack` are each load-bearing on
Linux specifically, and why `STACK_FLAGS` is on Windows.

Windows needs three small pieces of its own, each isolated and
commented where it lives: `src/compat.h` supplies `strcasestr` and
`localtime_r` (absent from mingw-w64) and compiles to nothing
elsewhere; `bytecode_disassemble_to_string` (`src/bytecode.c`) backs
Windows with a temp file where POSIX uses `open_memstream`; and
`STACK_FLAGS` raises the stack from the 1 MB a PE image gets by default
to the 8 MB macOS and Linux already give the main thread, which
`MAX_SCOPE_DEPTH`'s 200-level recursion cap needs.

Both `#ifdef __APPLE__` sites in `src/ui.c` already had the right
answer for Windows without changes — it takes the same `#else` branch
Linux does, giving `<Control>` accelerators and an in-window menu bar.

Two Linux-only menu issues have been found and fixed (`src/ui.c`, both
`#ifdef`-gated so macOS behaviour is untouched): the accelerators were
bound to `<Meta>`, which is Cmd on macOS but the Super key on Linux, and
the File/View menu bar didn't render at all, because GTK4 defaults
`GtkApplicationWindow:show-menubar` to FALSE and macOS never needed it
(its backend routes the menu model to the system menu bar instead).
Both are fixed and confirmed working on Fedora and Ubuntu; see
[`docs/CHANGELOG.md`](docs/CHANGELOG.md)'s Linux port entry for the full
write-up.

## Example

```
MAKE "size 50
REPEAT 4 [FD :size RT 90]
IF :size > 10 [PRINT "big] ELSE [PRINT "small]
```

## Documentation

- **[Learning materials site](https://hansolovkarlsson.github.io/LogoMotive/)**
  — the tutorial and command reference below, as a browsable website
- [`docs/TUTORIAL.md`](docs/TUTORIAL.md) — a from-scratch tutorial, for
  learning Logo with no prior programming experience
- [`docs/TUTORIAL_II.md`](docs/TUTORIAL_II.md) — the sequel: objects,
  concurrent agents, sprites, file I/O, and event-driven programs
- [`docs/TUTORIAL_III.md`](docs/TUTORIAL_III.md) — precise navigation,
  trig-driven drawing, bitwise operators, and saving compiled programs
- [`docs/COMMAND_REFERENCE.md`](docs/COMMAND_REFERENCE.md) — every
  command `bin/logomotive` actually supports, as a lookup table
- [`docs/LANGUAGE.md`](docs/LANGUAGE.md) — full language reference for the
  Logo dialect as implemented
- [`docs/BYTECODE_REFERENCE.md`](docs/BYTECODE_REFERENCE.md) — the VM's
  own instruction set, for anyone working on the interpreter itself
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — planned features and known gaps
- [`docs/CHANGELOG.md`](docs/CHANGELOG.md) — history of completed work

## Project structure

```
src/logo_types.h    shared structs (Turtle, Procedure, Variable, LogoApp)
src/lexer.h/c       tokenizes Logo source text
src/parser.h/c      builds an AST from tokens; the real command grammar
                     lives here (BUILTIN_SIGNATURES)
src/ast.h/c         fixed-pool AST node storage
src/compiler.h/c    compiles the AST into bytecode
src/bytecode.h/c    bytecode instruction format + fixed-pool storage
src/vm.h/c          the bytecode VM -- bin/logomotive's actual runtime
src/agent.h/c       LAUNCH/AWAIT/YIELD: concurrent multi-turtle agents
src/eval.h/c        a Stage 1 tree-walking evaluator, kept only to
                     shadow-diff test the VM against (not the runtime)
src/interpreter.h/c the original tree-walker (eval_logo) -- frozen,
                     pre-VM, kept only for its own tests
src/ui.h/c          the GTK window: canvas, REPL entry, View menu
src/headless.h/c    --headless: runs a script with no GTK window at all
src/compat.h        Windows-only shims for the libc functions mingw-w64
                     lacks (strcasestr, localtime_r) -- empty elsewhere
src/main.c          entry point (and, on Windows, the parent-console
                     attach that lets --help/--headless print)
tests/              headless tests for the interpreter core (make test)
docs/               language reference and roadmap
Makefile            make / make run / make test / make clean
build.sh            one-shot alternative build script
scripts/            dependency setup (brew/dnf/apt/pacman/MSYS2) and
                     bundle_windows.sh, which packages a standalone
                     Windows build
```
