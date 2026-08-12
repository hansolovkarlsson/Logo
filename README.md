<img src="website/assets/turtle-mark.svg" width="72" height="72" alt="">

# LogoMotive

LogoMotive is a small Logo interpreter with turtle graphics, built in C with
GTK4.

Type commands into the entry box and watch the turtle draw on the canvas — the
classic 80s Logo experience, running natively on macOS.

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
- A **View** menu (native macOS menu bar) to increase, decrease, or reset the
  text size of the REPL

## Requirements

- macOS
- [Homebrew](https://brew.sh)
- GTK4 and pkg-config (`scripts/install_gtk.sh` installs both)

## Build & Run

```sh
./scripts/install_brew.sh   # only if Homebrew isn't installed yet
./scripts/install_gtk.sh    # installs gtk4 + pkg-config
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
src/interpreter.h/c the Logo language core: eval_logo, expression parser
src/ui.h/c          the GTK window: canvas, REPL entry, View menu
src/main.c          entry point
tests/              headless tests for the interpreter core (make test)
docs/               language reference and roadmap
Makefile            make / make run / make test / make clean
build.sh            one-shot alternative build script
scripts/            Homebrew/GTK setup helpers
```
