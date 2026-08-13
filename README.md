<img src="website/assets/turtle-mark.svg" width="72" height="72" alt="">

# LogoMotive

LogoMotive is a Logo interpreter with turtle graphics, built in C with GTK4 —
a bytecode compiler and VM underneath, with concurrent multi-turtle agents,
sprites, and more on top of the classic language.

Type commands into the entry box and watch the turtle draw on the canvas — the
classic 80s Logo experience, running natively on macOS and Linux.

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

- macOS, or Linux
- GTK4, SDL2 and pkg-config (`scripts/install_gtk.sh` installs all three)
- On macOS: [Homebrew](https://brew.sh)

Linux is built and tested on Fedora (see [Platform support](#platform-support)
below for exactly what's verified where).

## Build & Run

```sh
./scripts/install_brew.sh   # macOS only, and only if Homebrew isn't installed
./scripts/install_gtk.sh    # gtk4 + sdl2 + pkg-config, via brew/dnf/apt/pacman
make run
```

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

## Platform support

| | Status |
|---|---|
| **macOS** (Apple Silicon) | The original target — developed and used here daily. |
| **Fedora** (42, aarch64) | Verified 2026-08-13: clean `make`, full `make test` suite green, GUI runs under both Wayland and X11, menus and keyboard shortcuts exercised by hand. |
| **Ubuntu 24.04 LTS / Linux Mint** | Not yet built. `scripts/install_gtk.sh` has an `apt-get` branch, but the package names in it come from documentation rather than a real build. |

The build is a single shared flag set for both platforms rather than a
per-OS branch — see the `Makefile`'s own comments for why `-std=gnu11`,
`-lm` and `-fconserve-stack` are each load-bearing on Linux specifically.

Two Linux-only menu issues have been found and fixed (`src/ui.c`, both
`#ifdef`-gated so macOS behaviour is untouched): the accelerators were
bound to `<Meta>`, which is Cmd on macOS but the Super key on Linux, and
the File/View menu bar didn't render at all, because GTK4 defaults
`GtkApplicationWindow:show-menubar` to FALSE and macOS never needed it
(its backend routes the menu model to the system menu bar instead).
Both are fixed and confirmed working on Fedora; see
[`docs/ROADMAP.md`](docs/ROADMAP.md) for the remaining distro
validation.

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
src/main.c          entry point
tests/              headless tests for the interpreter core (make test)
docs/               language reference and roadmap
Makefile            make / make run / make test / make clean
build.sh            one-shot alternative build script
scripts/            dependency setup helpers (brew/dnf/apt/pacman)
```
