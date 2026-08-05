# Logo Language Reference

This documents the Logo dialect actually implemented in `src/main.c`, as of
the current build. It's a living document — update it in the same commit
whenever a language feature changes.

## Turtle model

- Turtle 0 starts at position `(250, 250)` facing angle `0`, pen down.
- Angle `0` points up (north); `RIGHT`/`RT` increases the angle (clockwise),
  `LEFT`/`LT` decreases it.
- `CLEAR`/`CS` erases all drawn lines and resets *every* turtle's position
  and angle to the start (pen state, pen color, and background color are
  all left unchanged).
- There's one shared canvas, but multiple turtles — see "Multiple turtles"
  below.

## Multiple turtles

```
TELL 1
FD 100
TELL 0
RT 90 FD 50
```

- Every drawing command (`FD`, `RT`, `SETXY`, `SETPENCOLOR`, `PENUP`,
  `ARC`, ...) acts on the **current turtle**. `TELL n` switches which
  turtle is current, creating it (at the default position/heading/pen
  state) the first time it's addressed. Turtle indices run `0` to `9`.
- A script that never calls `TELL` behaves exactly as if there were only
  ever one turtle — turtle `0` is created automatically and is current by
  default.
- All existing turtles are drawn on the canvas at once, each as its own
  triangle marker (all the same fixed green, regardless of pen color).
- `TELL` with an index outside `0`-`9` prints an error and leaves the
  current turtle unchanged.

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
| `ARC` | — | `angle radius` | Draw a circle/arc of `radius` centered on the turtle; the turtle doesn't move |
| `TELL` | — | expr (0-9) | Switch which turtle subsequent commands control (see Multiple turtles) |
| `PENUP` | `PU` | — | Stop drawing while moving |
| `PENDOWN` | `PD` | — | Resume drawing while moving |
| `SETPENCOLOR` | `SETPC` | `r g b` (each 0-255) | Set the color new lines are drawn in |
| `SETPENWIDTH` | `SETPW` | expr (clamped 0.5-20) | Set the width new lines are drawn with |
| `SETBACKGROUND` | `SETBG` | `r g b` (each 0-255) | Set the canvas's background color |
| `CLEAR` | `CS` | — | Erase the canvas and reset the turtle's position/angle |

`SETXY`/`HOME` draw a connecting line if the pen is down, same as
`FORWARD` — they're a direct jump to a position, not a teleport. `CLEAR`
is the one that resets position without drawing, since it wipes the
canvas first.

```
ARC 360 80        (a full circle, radius 80)
RT 45 ARC 90 40    (a quarter arc, offset by the turtle's heading)
```

- `ARC angle radius` draws part (or all, at `angle` 360) of a circle
  centered *at the turtle's current position*, starting from its current
  heading and sweeping clockwise through `angle` degrees. Unlike every
  other drawing command, the turtle itself doesn't move or turn — only
  the arc is drawn, as if a phantom copy of the turtle traced it.
- There's no native arc primitive to draw with, so it's approximated as
  a fan of short straight segments (one per 5 degrees, at least 8 — so a
  360-degree circle is 72 segments). Each segment respects pen color/
  width/up-down exactly like any other drawn line.

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
- The turtle's own triangle marker is always the same fixed green/size,
  regardless of pen color or width.
- Default pen color is a dark gray, matching earlier versions of this app
  before `SETPENCOLOR` existed.
- `SETPENWIDTH`/`SETPW` works the same way as `SETPENCOLOR` — each line
  segment remembers the width it was drawn with, so a drawing can mix
  widths across calls. Default is `2`, matching earlier versions of this
  app before `SETPENWIDTH` existed.
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

- `MAKE "name expr` sets a variable to a number, creating it if it doesn't
  already exist.
- `:name` reads a variable's value inside any expression.
- Variables are either numbers or **words** — see "Words" below.

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

## Words

```
MAKE "name "World
PRINT :name
IF :name = "World [PRINT "matched]

MAKE "greeting [Hello there, friend!]
PRINT :greeting
IF :greeting = [Hello there, friend!] [PRINT "matched]
```

- `MAKE "name "word` sets a variable to a **word** — a single token, no
  spaces — instead of a number. `:name` still reads it back the same way.
- `MAKE "name [some words]` sets a variable to a multi-word string
  instead: the words inside the brackets, joined by single spaces
  regardless of the original whitespace between them (so `[hello   world]`
  and `[hello world]` produce the same value). This is the same bracketed
  syntax `PRINT [...]` uses (see Output below) — it's a value here instead
  of being printed immediately.
- Whether set via `"word` or `[a list]`, the result is the same kind of
  value — both are just a word (text) held in the variable. There's no
  separate "string" type, no concatenation, and no substring operations;
  a word/list literal is a single opaque piece of text you can set,
  print, and compare, not take apart — see `ROADMAP.md`.
- A word-typed variable used inside a numeric context (arithmetic, a
  `FORWARD`/`REPEAT`/etc. argument) just reads as `0` — words don't
  participate in arithmetic. `PRINT` and `=`/`<>` comparisons are word-aware
  (see Output and Conditionals below, and note `=`/`<>` also accept a
  `[bracketed list]` directly as an operand, not just a variable); every
  other numeric-expecting spot in the language is not.
- `MAKE "a :b` where `:b` is a word does **not** copy the word — it falls
  back to that same numeric-context behavior (copies `0`). To copy a word,
  assign it again as a literal (`MAKE "a "sameword` or `MAKE "a [some
  words]`).

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
  present, the expression's truthiness is used (non-zero number, or
  non-empty word = true).
- `=` and `<>` also work between words (`:name = "Alice`, or
  `:greeting = [hello there]`), comparing text case-insensitively.
  `< > <= >=` are numeric-only — comparing a word with one of those
  always reports false.

```
IF :x > 0 AND :x < 10 [PRINT "in-range]
IF :x < 0 OR :x > 100 [PRINT "out-of-range]
IF NOT :x = 0 [PRINT "nonzero]
```

- `AND`, `OR`, and `NOT` combine comparisons into a larger condition, same
  as in `IF`/`IFELSE`/`WHILE`. `NOT` binds tightest, then `AND`, then `OR`
  — standard boolean precedence — and clauses combine strictly left to
  right within each level.
- There are **no parentheses for grouping** boolean clauses (parens are
  only for arithmetic — see Expressions above). `:a AND :b OR :c` always
  parses as `(:a AND :b) OR :c`; if that's not what you mean, restructure
  the condition (e.g. two separate `IF`s) rather than trying to force
  grouping with parens.

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
PRINT :name
PRINT [hello there, this prints as one line]
```

- `PRINT`/`PR` writes to the history pane (auto-scrolling into view).
- `PRINT "word` prints a single literal word (no spaces — it stops at the
  first whitespace character).
- `PRINT :name` prints a word-typed variable as its text, or a
  numeric-typed variable as a number — whichever it holds.
- `PRINT [words...]` prints a bracketed list as its words, joined by
  single spaces (any original whitespace inside the brackets is
  collapsed). This is direct, immediate printing — the list isn't
  evaluated as code (unlike a `REPEAT`/`IF`/`WHILE` block) and isn't
  stored anywhere.
- `PRINT <expr>` (anything else) evaluates and prints a numeric
  expression.

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

## Errors

Malformed input reports an error message to the history pane rather than
silently doing nothing (or, in one case that's now fixed, crashing):

- An unrecognized command prints `I don't know how to <NAME>`.
- `TO` without a matching `END` prints `TO <name>: missing END` and stops
  processing the rest of that input, rather than running the dangling body
  as top-level commands.
- `REPEAT`, `WHILE`, `IF`, and `IFELSE` each print `<COMMAND>: expected [
  block ]` if the bracketed block is missing or malformed. `IFELSE`
  additionally prints `IFELSE: expected two [ block ]s` if only one block
  is given (`IF` doesn't require a second block; `IFELSE` does).
- `MAKE`, `ERASE`, `LOAD`, and `SAVE` each print `<COMMAND>: expected a
  "name`/`"path` if the required quoted word is missing. `ERASE` also
  prints `ERASE: no such procedure "<name>` if the name isn't defined.
- Recursion past 200 nested calls prints `Recursion too deep, call
  ignored` (see Variables & scoping above); a `WHILE` past 1,000,000
  iterations prints `WHILE: stopped after too many iterations`.

What's still silent: passing the wrong *kind* of value somewhere (a word
where a number is expected reads as `0`; see Words above) and any parse
error inside a numeric expression itself (an unparseable expression just
evaluates to `0`, same as always) — see `ROADMAP.md`.

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
- **Bracket matching**: whenever the cursor touches a `[` or `]` (on
  either side of it), that bracket and its match are highlighted. A
  bracket with no match — e.g. `REPEAT 4 [FD 10` with no closing `]` — is
  flagged in a different color instead. This is in the entry box only,
  not the history pane.
- The **File** menu (native macOS menu bar) has **Open…** (⌘O), which picks
  a file via a native dialog and runs it the same way `LOAD` does;
  **Save…** (⌘S), which does the same for `SAVE`; and **Export as PNG…**
  (⌘E), which renders the current canvas (background, drawn lines, and the
  turtle) to an image file at the canvas's actual pixel size.
- The **View** menu (native macOS menu bar) has Increase/Decrease/Reset
  Text Size, applied to both the history pane and the entry box.

## Known limitations (intentional, tracked in `ROADMAP.md`)

- No general string type (concatenation, substrings, multi-word text in a
  variable) — only single-token words, and only `PRINT`/`=`/`<>` are
  word-aware. See "Words" above.
- No lists/arrays as a storable, manipulable value — `PRINT [...]` prints
  a bracketed list's words directly but there's no `FIRST`/`BUTFIRST`/
  `LAST`/`COUNT`, and a list can't be assigned to a variable.
- No multiple turtles.
- Passing a word where a number is expected (or vice versa) silently
  coerces rather than erroring — see "Errors" above.
- The multi-line input's completeness check only balances `[`/`]` and counts
  `TO`/`END` — it doesn't validate the syntax inside, so e.g. a stray `]`
  can make an otherwise-valid input submit early.
