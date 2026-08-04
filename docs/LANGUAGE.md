# Logo Language Reference

This documents the Logo dialect actually implemented in `src/main.c`, as of
the current build. It's a living document — update it in the same commit
whenever a language feature changes.

## Turtle model

- The turtle starts at position `(250, 250)` facing angle `0`, pen down.
- Angle `0` points up (north); `RIGHT`/`RT` increases the angle (clockwise),
  `LEFT`/`LT` decreases it.
- `CLEAR`/`CS` erases all drawn lines and resets the turtle to its start
  position and angle (pen state is left unchanged).
- There is currently one turtle, one pen color (fixed, dark gray), and one
  canvas — no color, multiple turtles, or SETXY/HOME/SETHEADING yet (see
  `ROADMAP.md`).

## Turtle commands

| Command | Abbreviation | Argument | Effect |
|---|---|---|---|
| `FORWARD` | `FD` | expr | Move forward by `expr`, drawing if pen is down |
| `BACK` | `BK` | expr | Move backward by `expr` |
| `RIGHT` | `RT` | expr | Turn clockwise by `expr` degrees |
| `LEFT` | `LT` | expr | Turn counter-clockwise by `expr` degrees |
| `PENUP` | `PU` | — | Stop drawing while moving |
| `PENDOWN` | `PD` | — | Resume drawing while moving |
| `CLEAR` | `CS` | — | Erase the canvas and reset the turtle |

All commands are case-insensitive (`fd 100` and `FD 100` are equivalent).

## Expressions

Anywhere a command expects a number (`FD`, `RT`, a `REPEAT` count, a
procedure argument, `MAKE`'s value, ...), a full numeric expression is
accepted:

- Literals: `100`, `3.5`, `-2`
- Operators: `+ - * /`, standard precedence, unary `+`/`-`
- Parentheses: `(1 + 2) * 3`
- Variables: `:name` (see below)

Division by zero evaluates to `0` rather than crashing or erroring.

## Variables

```
MAKE "size 100
FD :size
MAKE "size :size + 10
```

- `MAKE "name expr` sets a variable (creates it if it doesn't exist yet).
- `:name` reads a variable's value inside any expression.
- Variables are **global** — there is no per-procedure local scope. A
  variable set inside a procedure is visible everywhere afterward.
- All variables are numeric (`double`). There is no string/word variable
  type yet.

## Procedures

```
TO square :size
  REPEAT 4 [FD :size RT 90]
END

square 50
```

- Defined with `TO name [:param] ... END`. `END` ends the definition —
  don't use the literal word `END` elsewhere in a procedure body.
- At most **one** parameter is supported per procedure.
- Parameter binding is implemented as literal text substitution: the
  procedure body has every occurrence of `:param` replaced with the
  argument's value (as text) before the body is evaluated. This means a
  parameter name can't currently be dynamically scoped/shadowed the way a
  true call-by-value binding would be — see `ROADMAP.md`.
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
  `REPEAT`/`IF`/procedure calls.
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

## Interface

- Commands are typed into the entry box at the bottom of the REPL pane and
  run on Enter.
- The **View** menu (native macOS menu bar) has Increase/Decrease/Reset
  Text Size, applied to both the history pane and the entry box.

## Known limitations (intentional, tracked in `ROADMAP.md`)

- No string/word data type — everything numeric except `PRINT "word` and
  `MAKE "name` literals.
- No lists/arrays.
- Variables are global; no local scoping in procedures.
- Procedures take at most one parameter.
- No `WHILE`, no boolean `AND`/`OR`/`NOT`.
- No pen color, background color, or multiple turtles.
- Malformed input generally fails silently (no error messages surfaced to
  the user) rather than reporting a parse/runtime error.
