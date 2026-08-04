# Logo Language Reference

This documents the Logo dialect actually implemented in `src/main.c`, as of
the current build. It's a living document — update it in the same commit
whenever a language feature changes.

## Turtle model

- The turtle starts at position `(250, 250)` facing angle `0`, pen down.
- Angle `0` points up (north); `RIGHT`/`RT` increases the angle (clockwise),
  `LEFT`/`LT` decreases it.
- `CLEAR`/`CS` erases all drawn lines and resets the turtle's position and
  angle to the start (pen state, pen color, and background color are all
  left unchanged).
- There is currently one turtle and one canvas — no multiple turtles yet
  (see `ROADMAP.md`).

## Turtle commands

| Command | Abbreviation | Argument | Effect |
|---|---|---|---|
| `FORWARD` | `FD` | expr | Move forward by `expr`, drawing if pen is down |
| `BACK` | `BK` | expr | Move backward by `expr` |
| `RIGHT` | `RT` | expr | Turn clockwise by `expr` degrees |
| `LEFT` | `LT` | expr | Turn counter-clockwise by `expr` degrees |
| `SETXY` | — | `x y` | Jump directly to an absolute position, drawing if pen is down; heading unchanged |
| `SETHEADING` | `SETH` | expr | Set the turtle's absolute heading (no movement) |
| `HOME` | — | — | `SETXY` back to the start position and `SETHEADING 0`, drawing if pen is down |
| `PENUP` | `PU` | — | Stop drawing while moving |
| `PENDOWN` | `PD` | — | Resume drawing while moving |
| `SETPENCOLOR` | `SETPC` | `r g b` (each 0-255) | Set the color new lines are drawn in |
| `SETBACKGROUND` | `SETBG` | `r g b` (each 0-255) | Set the canvas's background color |
| `CLEAR` | `CS` | — | Erase the canvas and reset the turtle's position/angle |

`SETXY`/`HOME` draw a connecting line if the pen is down, same as
`FORWARD` — they're a direct jump to a position, not a teleport. `CLEAR`
is the one that resets position without drawing, since it wipes the
canvas first.

All commands are case-insensitive (`fd 100` and `FD 100` are equivalent).

```
SETPENCOLOR 255 0 0
FD 80 RT 90
SETPC 0 100 255
FD 80
```

- `SETPENCOLOR`/`SETPC` takes three expressions, each clamped to 0-255,
  and becomes the color used by every line drawn from then on.
- Each drawn line segment remembers the pen color it was drawn with, so a
  single drawing can freely mix colors across `SETPENCOLOR` calls — it's
  not a single canvas-wide color.
- The turtle's own triangle marker is always the same fixed green,
  regardless of pen color.
- Default pen color is a dark gray, matching earlier versions of this app
  before `SETPENCOLOR` existed.
- `SETBACKGROUND`/`SETBG` works the same way but sets the canvas's
  background — unlike pen color, this is a single canvas-wide value (not
  remembered per line), so changing it recolors the whole background
  immediately, past drawing included. Default is white.

## Expressions

Anywhere a command expects a number (`FD`, `RT`, a `REPEAT` count, a
procedure argument, `MAKE`'s value, ...), a full numeric expression is
accepted:

- Literals: `100`, `3.5`, `-2`
- Operators: `+ - * /`, standard precedence, unary `+`/`-`
- Parentheses: `(1 + 2) * 3`
- Variables: `:name` (see below)

Division by zero evaluates to `0` rather than crashing or erroring.

## Variables & scoping

```
MAKE "size 100
FD :size
MAKE "size :size + 10
```

- `MAKE "name expr` sets a variable, creating it if it doesn't already
  exist.
- `:name` reads a variable's value inside any expression.
- All variables are numeric (`double`). There is no string/word variable
  type yet.

Procedure parameters are **local, dynamically-scoped** variables. Calling a
procedure pushes a fresh scope binding its parameters to the (already
evaluated) argument values; that scope is popped when the call returns.
`:name` and `MAKE "name` search the active call's scope first, then any
outer calls on the stack, then the globals — so:

- A parameter shadows a same-named global or outer-call variable for the
  duration of that call.
- `MAKE "n ...` inside a procedure with a parameter `:n` reassigns that
  parameter (it's a real variable now, not a text substitution) — it does
  not create an unrelated global.
- Recursive calls each get their own independent scope, so `:n` in one
  call of a recursive procedure never sees another call's `:n`.
- This is *dynamic* scoping, matching classic Logo: a procedure with no
  parameter of its own named `x` can still see and, via `MAKE`, mutate an
  outer call's local `x` if one is active. Only when no matching binding
  exists anywhere on the call stack does `MAKE` fall back to creating a
  global.
- Recursion is capped at 200 nested calls; going deeper prints "Recursion
  too deep, call ignored" instead of crashing.

## Procedures

```
TO rect :w :h
  REPEAT 2 [FD :w RT 90 FD :h RT 90]
END

rect 100 40
```

- Defined with `TO name [:param ...] ... END`. `END` ends the definition —
  don't use the literal word `END` elsewhere in a procedure body.
- Up to 8 parameters are supported per procedure, bound positionally: the
  first argument at the call site fills the first declared parameter, and
  so on. Arguments are evaluated in the *caller's* scope, before the
  callee's own scope is pushed.
- Defining a `TO` with a name that already exists **overwrites** the
  existing procedure in place — handy for fixing a typo and re-running the
  definition. `ERASE "name` removes a procedure entirely.
- Procedures can call other procedures, including recursively.

## Conditionals

```
IF :size > 10 [PRINT "big]
IF :size > 10 [PRINT "big] ELSE [PRINT "small]
IFELSE :size > 10 [PRINT "big] [PRINT "small]
```

- `IF <condition> [block]` runs `block` only if `condition` is true.
- `IF <condition> [block] ELSE [block]` and `IFELSE <condition> [block]
  [block]` are equivalent — both run the first block when true, the second
  when false. (`IF` accepts the second block with or without the literal
  `ELSE` keyword in between.)
- Conditions are expressions optionally followed by a relational operator
  and a second expression: `< > = <= >= <>`. If no relational operator is
  present, the expression's truthiness is used (non-zero = true).
- There is no `AND`/`OR`/`NOT` yet — see `ROADMAP.md`.

## Loops

```
REPEAT 4 [FD 100 RT 90]
```

- `REPEAT <expr> [block]` evaluates `block` `expr` times (truncated to an
  integer). Blocks can nest and contain any commands, including further
  `REPEAT`/`IF`/`WHILE`/procedure calls.

```
MAKE "i 0
WHILE :i < 4 [FD 80 RT 90 MAKE "i :i + 1]
```

- `WHILE <condition> [block]` re-checks `condition` before each pass and
  runs `block` for as long as it's true. Unlike `REPEAT`, the condition is
  a live expression — remember to update whatever it depends on inside the
  block, or the loop won't terminate.
- As a safety net against runaway loops freezing the app, `WHILE` stops
  itself after 1,000,000 iterations and prints a warning to the history
  pane rather than hanging.

## Output

```
PRINT "hello
PRINT 2 + 2
PRINT :size
```

- `PRINT`/`PR` writes to the history pane (auto-scrolling into view).
- `PRINT "word` prints a single literal word (no spaces — it stops at the
  first whitespace character). There is no support yet for printing a
  bracketed list of words or a quoted string containing spaces.
- `PRINT <expr>` evaluates and prints a numeric expression.

## Files

```
LOAD "/Users/you/scripts/star.logo
SAVE "/Users/you/scripts/star.logo
```

- `LOAD "path` reads a file and runs its contents as Logo source, exactly
  as if it had been typed into the REPL — including `TO` definitions,
  which is the main use case (write/edit a script externally, `LOAD` it,
  test, repeat).
- `SAVE "path` writes every currently-defined procedure out as `TO ... END`
  Logo source, readable back in by `LOAD`. It only saves procedure
  definitions, not variables, turtle state, or drawing — loading a saved
  file gives you back the same reusable procedures, not a replay of
  everything you typed.
- Both take the path as a single whitespace-delimited word, same as
  `PRINT "word` and `MAKE "name` — a path containing spaces won't parse
  correctly typed this way. Use **File → Open…**/**Save…** in the menu bar
  instead for paths with spaces, since those go through a native file
  picker rather than this text syntax.
- If the file can't be read or written, prints "LOAD: could not read
  file" / "SAVE: could not write file" rather than crashing.

## Interface

- Commands are typed into the entry box at the bottom of the REPL pane.
- Enter runs the input **once it's complete** — brackets are balanced and
  every `TO` has a matching `END`. A single-line command like `FD 100`
  always runs immediately. A `TO ... END` procedure (or anything with an
  unclosed `[`) keeps accepting new lines, growing the box, until it
  balances out — then the whole thing runs at once. This means you can
  type a multi-line procedure definition one line at a time, the same way
  most Logo REPLs handle it.
- Shift+Enter always inserts a newline without submitting, regardless of
  whether the input is complete.
- **Up/Down arrow** recall previously-submitted history (each submission —
  even a multi-line `TO ... END` — is one entry), same as a shell. They
  only trigger recall when the cursor is on the entry's first line (Up) or
  last line (Down); otherwise they move the cursor normally, so browsing
  within a recalled multi-line block still works. Whatever you'd typed but
  not yet submitted is preserved and restored once you browse back down
  past the newest entry.
- The **File** menu (native macOS menu bar) has **Open…** (⌘O), which picks
  a file via a native dialog and runs it the same way `LOAD` does;
  **Save…** (⌘S), which does the same for `SAVE`; and **Export as PNG…**
  (⌘E), which renders the current canvas (background, drawn lines, and the
  turtle) to an image file at the canvas's actual pixel size.
- The **View** menu (native macOS menu bar) has Increase/Decrease/Reset
  Text Size, applied to both the history pane and the entry box.

## Known limitations (intentional, tracked in `ROADMAP.md`)

- No string/word data type — everything numeric except `PRINT "word`,
  `MAKE "name`, and `LOAD "path` literals.
- No lists/arrays.
- No boolean `AND`/`OR`/`NOT`.
- No pen color, background color, or multiple turtles.
- Malformed input generally fails silently (no error messages surfaced to
  the user) rather than reporting a parse/runtime error.
- The multi-line input's completeness check only balances `[`/`]` and counts
  `TO`/`END` — it doesn't validate the syntax inside, so e.g. a stray `]`
  can make an otherwise-valid input submit early.
