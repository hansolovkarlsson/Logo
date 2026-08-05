# Logo Language Reference

This documents the Logo dialect actually implemented in `src/main.c`, as of
the current build. It's a living document — update it in the same commit
whenever a language feature changes.

## Comments

```
; this whole line is a comment
FD 100 ; and so is everything after this semicolon
REPEAT 4 [
  FD 50 ; comments work inside blocks too
  RT 90
]
```

- `;` starts a comment that runs to the end of the line, recognized
  anywhere a token boundary falls — after a command, on its own line,
  inside a `REPEAT`/`IF`/`WHILE` block, between list elements.
- A `;` with no preceding whitespace (glued directly onto a word or
  `"`-literal, like `"hi;there`) is just an ordinary character, not a
  comment — a comment has to start at a token boundary, same as most
  languages.
- Because a comment consumes the rest of its line unconditionally, one
  placed *before* a list literal's closing `]` on the same line eats the
  `]` too (`PRINT [a b ; c]` never sees its closing bracket). Keep
  comments on their own line, or after a complete statement.

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
| `HIDETURTLE` | `HT` | — | Stop drawing the turtle marker (the trail still draws) |
| `SHOWTURTLE` | `ST` | — | Draw the turtle marker again |
| `WRAP` | — | — | Crossing the canvas edge wraps around to the opposite side |
| `FENCE` | — | — | Crossing the canvas edge stops the turtle right at it, with an error |
| `WINDOW` | — | — | No canvas boundary at all (the default) |

`SETXY`/`HOME` draw a connecting line if the pen is down, same as
`FORWARD` — they're a direct jump to a position, not a teleport. `CLEAR`
is the one that resets position without drawing, since it wipes the
canvas first.

```
WRAP
SETXY 600 250      (canvas is 500x500 -- wraps to 100 250, no line drawn)

FENCE
SETXY 600 250      (stops at 500 250, drawing up to the edge, and reports it)
```

- `WRAP`/`FENCE`/`WINDOW` choose what happens when a move (`FD`/`SETXY`/
  etc.) would cross the 500x500 canvas edge — a single canvas-wide
  setting, not per-turtle, and unaffected by `CLEAR` (same as pen color/
  width). `WINDOW` is the default: no boundary at all, exactly as if
  none of these three existed.
- `WRAP` wraps the crossing coordinate around to the opposite edge. The
  pen lifts for that one jump — no line is drawn from one edge to the
  other — so wrapping doesn't paint a stray diagonal line across the
  canvas.
- `FENCE` stops the turtle right at the edge (clamping the position),
  draws the line up to that point, and prints `FENCE: turtle stopped at
  the canvas edge`.

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

### Turtle state queries

```
FD 100
PRINT POS               -> 250 150
PRINT HEADING           -> 0

RT 45
MAKE "h HEADING
SETHEADING 200
SETHEADING :h
PRINT HEADING           -> 45
```

- `POS` and `HEADING` are operators (not commands) that read the current
  turtle's position and heading back into an expression — they work
  anywhere an argument is expected, same as `FIRST`/`COUNT`/etc. `SETXY`/
  `SETHEADING`/`RT`/`LT` can *set* these, but until now there was no way
  to *read* them back into a variable.
- `POS` outputs a 2-element list `[x y]`. `HEADING` outputs a plain
  number — the raw stored angle, not normalized to 0-360 (turning `RT 90`
  four times reads back as `360`, not `0`), matching how `RT`/`LT`/
  `SETHEADING` already accumulate it.
- Both always refer to whichever turtle `TELL` currently targets, same as
  `FD`/`RT`/etc.

## Expressions

Anywhere a command expects a number (`FD`, `RT`, a `REPEAT` count, a
procedure argument, `MAKE`'s value, ...), a full numeric expression is
accepted:

- Literals: `100`, `3.5`, `-2`
- Operators: `+ - * /`, standard precedence, unary `+`/`-`
- Parentheses: `(1 + 2) * 3`
- Variables: `:name` (see below)
- Prefix arithmetic operators, all one argument except `MOD`/`POWER`
  (two), binding like the list operators do (tighter than `* / + -`, so
  `MOD 7 3 + 1` is `(MOD 7 3) + 1`):
  - `MOD a b` — remainder, with the sign of `b` (so `MOD -7 3` is `2`,
    not `-1` the way C's own `%`/`fmod` would give).
  - `POWER a b` — `a` raised to the power `b`.
  - `SQRT a`, `ABS a`, `ROUND a` — square root, absolute value, and
    round-to-nearest-integer.
  - `SIN a`, `COS a`, `ARCTAN a` — trig functions; `a`/the result are in
    **degrees**, matching `RT`/`LT`/`HEADING`'s convention, not radians.
  - `RANDOM n` — a random integer in `[0, n)`.

Division by zero evaluates to `0` rather than crashing or erroring.
`SQRT` of a negative number returns C's own `NaN` rather than a special
error — it prints as `nan`, which is visibly broken rather than a
plausible-looking wrong number, so nothing special was added on top.

## Variables & scoping

```
MAKE "size 100
FD :size
MAKE "size :size + 10
```

- `MAKE "name expr` sets a variable to a number, creating it if it doesn't
  already exist.
- `:name` reads a variable's value inside any expression.
- Variables are either numbers or **words** — see "Words & lists" below.

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

```
TO uselocal
  LOCAL "x
  MAKE "x 1
  PRINT :x
END
```

- `LOCAL "name` declares a variable scoped to the current call, the same
  way a parameter is, but without being one — useful for a working
  variable a procedure needs internally that has nothing to do with its
  arguments. It reads as `0` until assigned, and is popped along with
  the rest of the call's scope when the procedure returns.
- Only valid inside an active procedure call — `LOCAL` at the top level
  (not inside any `TO`) prints `LOCAL: can only be used inside a
  procedure`.
- A call's locals share the same fixed-size slot table (`MAX_PARAMS` —
  8) that its parameters use, so a procedure with many parameters may
  not have room left for additional `LOCAL`s; going over prints `LOCAL:
  too many local variables`.

## Words & lists

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
- `MAKE "name [some words]` sets a variable to a **list**: the elements
  inside the brackets, in order, with whitespace between them normalized
  (so `[hello   world]` and `[hello world]` produce the same value). This
  is the same bracketed syntax `PRINT [...]` uses (see Output below) —
  it's a value here instead of being printed immediately.
- Lists nest: an element can itself be a bracketed sublist, e.g. `[a [b
  c] d]` is a 3-element list whose second element is the 2-element list
  `[b c]`. See "List operators" and "List construction" below for reading
  and building nested structure.
- A word and a list are two distinct kinds of value — `"word` (a single
  token, no spaces) versus `[a list]` (elements, possibly nested). `MAKE
  "a :b` copies whichever kind `:b` currently holds, unchanged.
- A word used inside a numeric context (arithmetic, a `FORWARD`/`REPEAT`/
  etc. argument) coerces by reading the number its text starts with —
  `FD FIRST [100 50]` moves forward `100` — falling back to `0` if it
  doesn't start with one at all (`PRINT "hello + 1` is `1`); a list
  coerces to `0`, having no meaningful numeric reading. This applies
  everywhere: every argument in the language (`PRINT`, `MAKE`, turtle
  commands, `REPEAT`'s count, procedure-call arguments, comparisons, ...)
  is parsed by the same word/list-aware expression evaluator.
- `MAKE "a :b` copies `:b`'s value as-is — word, number, or list (a bare
  `:name` only — `MAKE "a :b + 1` still evaluates numerically, same as any
  other compound expression). Copying a list this way is just copying a
  reference to its (immutable) elements, not a deep copy — cheap
  regardless of the list's size.

### List operators (and substrings)

```
MAKE "colors [red green blue]
PRINT FIRST :colors            -> red
PRINT LAST :colors             -> blue
PRINT BUTFIRST :colors         -> green blue
PRINT BUTLAST :colors          -> red green
PRINT COUNT :colors            -> 3
PRINT ITEM 2 :colors           -> green
IF FIRST :colors = "red [PRINT "yes]

PRINT COUNT [a [b c] d]        -> 3 (top-level elements only — [b c] counts as one)
PRINT FIRST [[1 2] 3]          -> 1 2 (the sublist [1 2], printed — see Output below
                                        for why a list PRINT'ing as its own top-level
                                        value never shows its own brackets)

PRINT FIRST "hello              -> h
PRINT LAST "hello               -> o
PRINT BUTFIRST "hello           -> ello
PRINT BUTLAST "hello            -> hell
PRINT COUNT "hello              -> 5
PRINT ITEM 3 "hello              -> l
```

- `FIRST`, `BUTFIRST`, `LAST`, and `BUTLAST` take one word/list-ish
  argument. On a list, they operate on its top-level elements only — an
  element that's itself a sublist is returned/skipped whole, not
  flattened into it. `FIRST`/`LAST` return that element as-is (a word, a
  number, or a list); `BUTFIRST` returns everything after the first
  element, `BUTLAST` everything except the last. `COUNT` takes the same
  kind of argument and returns a **number** — how many top-level elements
  it has (a sublist element counts as one, regardless of its own length).
- `ITEM index thing` is random access by position (1-indexed): the
  `index`th top-level element of a list, or the `index`th character of a
  word. An `index` less than 1 or past the end is a reported error (see
  Errors below) rather than a silent empty result or a crash.
- On a **word**, `FIRST`/`BUTFIRST`/`LAST`/`BUTLAST`/`COUNT`/`ITEM` all
  work the same way but one level deeper: a word is a sequence of
  *characters*, not one atomic element, so `FIRST`/`LAST`/`ITEM` return a
  single-character word, `BUTFIRST`/`BUTLAST` return the rest as a word,
  and `COUNT` returns its length. This is substrings — the only way to
  pull characters out of a word, no separate substring syntax needed. A
  bare **number**, unlike a word, stays atomic and does not decompose
  into digits (`COUNT 12345` is `1`, not `5`) — consistent with treating
  it as a one-element list, same as before nested lists existed.
- The argument to any of these can be a `"word`, a `[bracketed list]`, a
  `:variable` holding either, a nested list operator call (`FIRST BUTFIRST
  :colors`), or a bare number (treated as a one-element list, so `COUNT 5`
  is `1`, and only `ITEM 1` of one is valid). `FIRST`/`LAST`/`BUTFIRST`/
  `BUTLAST` of an empty list (`[]`) or an empty word (`""`) is empty;
  `COUNT` of either is `0`.
- All six work anywhere an argument is expected — `PRINT`, `MAKE "name`,
  both sides of `=`/`<>`, and plain arithmetic (`FD FIRST :colors`,
  `REPEAT COUNT :items [...]`) — coercing to a number the same way any
  other word does when arithmetic touches it (see above).

### List construction

```
PRINT WORD "hello "world        -> helloworld
PRINT SENTENCE "a "b            -> a b
PRINT SE [1 2] [3 4]            -> 1 2 3 4
PRINT LIST [1 2] [3 4]          -> [1 2] [3 4]
MAKE "colors [green blue]
PRINT FPUT "red :colors         -> red green blue
PRINT LPUT "purple :colors      -> green blue purple
PRINT FPUT [1 2] [3 4]          -> [1 2] 3 4
PRINT FPUT "a "bc                -> abc
```

- `WORD a b` concatenates two words directly with **no** space between
  them — pure string concatenation. It only accepts word/number
  arguments; passing it a list is an error (see Errors below) rather than
  silently flattening the list's structure into text.
- `SENTENCE a b` (or `SE`) **splices** two values into one flat list: a
  list argument contributes its own elements individually, a word/number
  argument contributes itself as one element.
- `LIST a b` **wraps** instead: each argument becomes exactly one element
  regardless of whether it's itself a list, so `LIST [1 2] [3 4]` is the
  2-element list `[[1 2] [3 4]]` (each element a sublist) — never
  splicing the way `SENTENCE` does. This is the actual difference between
  the two operators, now that lists nest; with only word/number
  arguments, they still behave identically (`LIST "x "y` is `x y`, same
  as `SENTENCE "x "y`).
- `FPUT thing list` prepends `thing` as a new first element; `LPUT thing
  list` appends it as a new last element. If `thing` is itself a list,
  this creates genuine nesting (`FPUT [1 2] [3 4]` is `[[1 2] 3 4]`,
  printing as `[1 2] 3 4`) rather than splicing it in.
- If `list` is itself a **word**, `FPUT`/`LPUT` build a new word by
  prepending/appending `thing`'s text as characters instead (`FPUT "a
  "bc` is `"abc"`, not a list) — consistent with a word being a sequence
  of characters (see "List operators (and substrings)" above). `thing`
  being a list has no character-level meaning to merge in, so that's a
  reported error instead (see Errors below) rather than silently
  flattening it into text.
- All four take exactly two arguments (nest calls for more: `WORD "a
  WORD "b "c`) and, like the list operators above, work anywhere an
  argument is expected — including plain arithmetic.

### Type & membership predicates

```
PRINT WORD? "hi           -> 1
PRINT LIST? [1 2]         -> 1
PRINT NUMBER? 5           -> 1
PRINT EMPTY? []           -> 1
PRINT EMPTY? [1]          -> 0
PRINT MEMBER? "b [a b c]  -> 1
PRINT MEMBER? "ell "hello -> 1

IF WORD? "hi [PRINT "yes]
```

- `WORD?`, `LIST?`, and `NUMBER?` each take one argument and report
  (`1`/`0`) whether it's that kind of value — the only way to ask "what
  kind of value is this?" from inside a running program.
- `EMPTY?` reports whether a list (`[]`) or word (`""`) is empty; a
  number is never "empty" (always `0`).
- `MEMBER? thing container` reports whether `thing` is one of
  `container`'s top-level elements (a list), or a substring of it (a
  word — so `MEMBER? "ell "hello` is true, not just single-character
  checks); a bare number `container` is treated as a one-element list,
  same convention as `FIRST`/`COUNT`/etc.
- Like every other operator here, all five return a plain number
  (`1`/`0`) rather than a special boolean type — the same convention
  `=`/`<`/etc. already use, so they work directly in `IF`/`WHILE` or
  combined with `AND`/`OR`/`NOT`.

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

### Deferred execution: RUN, APPLY

```
RUN [FD 100 RT 90]

MAKE "prog [FD 50 RT 90]
RUN :prog

TO add2 :a :b
  PRINT :a + :b
END
APPLY "add2 [3 4]        -> 7
```

- `RUN thing` executes a stored word or list as Logo source, exactly as
  if it had been typed directly — the thing that makes a list double as
  a deferred program instead of just data. A list containing a nested
  `[...]` block (like `RUN [REPEAT 4 [FD 10 RT 90]]`) round-trips
  correctly: a sublist always renders back out with its own brackets
  (see "Output" below), which happens to mean it's valid Logo source
  again.
- `APPLY "name arglist` calls procedure `name` with its arguments taken
  from `arglist`, instead of parsed positionally from the command line.
  The list's element count must exactly match the procedure's parameter
  count — `APPLY: wrong number of inputs for procedure "name` otherwise.
- Both are commands (not operators usable inside an expression like
  `FIRST`/`WORD`/etc. are) — that's tied to procedures not yet being
  able to return a value (see `ROADMAP.md`'s `OUTPUT`/`STOP` item), which
  is what an expression-usable `RUN`/`APPLY` would need to output.
- A self-referential `RUN` (e.g. `MAKE "x [RUN :x]` then `RUN :x`) is
  capped and reported as `RUN: too deeply nested, ignored` rather than
  crashing — this is a much lower, separate limit from ordinary
  recursion, since nested `RUN`s are considerably more expensive per
  level and a self-referential one is always a mistake in the program,
  never a legitimate technique.

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
- `PRINT [words...]` prints a bracketed list as its elements, joined by
  single spaces (any original whitespace inside the brackets is
  collapsed). This is direct, immediate printing — the list isn't
  evaluated as code (unlike a `REPEAT`/`IF`/`WHILE` block) and isn't
  stored anywhere.
- `PRINT <expr>` (anything else) evaluates and prints a numeric
  expression.
- A list's own outer brackets are never shown when it's the thing being
  printed directly (`PRINT [a b]` -> `a b`, not `[a b]`) — but a nested
  sublist **inside** that list does print with its brackets, since
  that's the only way to tell where one element ends and the next
  begins: `PRINT [a [b c] d]` -> `a [b c] d`. The same rule applies no
  matter how the list value was produced — `PRINT FIRST [[1 2] 3]`
  prints `1 2` (no brackets, since the sublist is PRINT's own top-level
  value here), while `PRINT LIST FIRST [[1 2] 3] "x` prints `[1 2] x`
  (bracketed, now that it's nested one level inside the 2-element list
  `LIST` just built).

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
  block ]` if the bracketed block is missing, unterminated (no matching
  `]`), or too long to fit the interpreter's internal buffer (4KB — see
  below). `IFELSE` additionally prints `IFELSE: expected two [ block ]s`
  if only one block is given (`IF` doesn't require a second block;
  `IFELSE` does). A bracketed list literal used as a value anywhere
  (`PRINT [...]`, `MAKE "name [...]`, an argument to `FIRST`/`FPUT`/etc.)
  prints `[ list ]: missing closing ] or too long` if it's unterminated
  or oversized, rather than silently treated as ending at the input's
  end.
- `MAKE`, `ERASE`, `LOAD`, and `SAVE` each print `<COMMAND>: expected a
  "name`/`"path` if the required quoted word is missing. `ERASE` also
  prints `ERASE: no such procedure "<name>` if the name isn't defined.
- `TO <name>: procedure body too long, not defined` if a procedure's body
  exceeds the interpreter's internal buffer (8KB).
- `TO <name>: too many parameters, extra parameters ignored` if more than
  8 (`MAX_PARAMS`) are declared — the procedure is still defined, using
  only the first 8.
- `MAKE: too many variables defined, not set` if the global variable
  table (100 entries) is full and `MAKE "name` would need to create a
  new one — an existing variable can still be updated either way.
- Recursion past 200 nested calls prints `Recursion too deep, call
  ignored` (see Variables & scoping above); a `WHILE` past 1,000,000
  iterations prints `WHILE: stopped after too many iterations`.
- `WORD` prints `WORD: expected words, not a list` if given a list
  argument, rather than silently flattening its structure into text (see
  "List construction" above). `FPUT`/`LPUT` print the analogous `FPUT:
  can't add a list to a word` / `LPUT: can't add a list to a word` if
  `thing` is a list and `list` is a word.
- `ITEM` prints `ITEM: index out of range` if given an index less than 1
  or past the end of its list/word argument, rather than an empty result
  or an unchecked out-of-bounds read.
- `APPLY` prints `APPLY: no such procedure "name` if given an undefined
  procedure name, or `APPLY: wrong number of inputs for procedure "name`
  if its list's element count doesn't match the procedure's parameter
  count. `RUN` prints `RUN: too deeply nested, ignored` for a
  self-referential `RUN` (see "Deferred execution" above).
- Every internal text buffer (procedure bodies, block bodies, words,
  variable values, file paths, REPL history entries) is fixed-size but
  generously sized for normal use; input that would overflow one is
  rejected with an error above rather than silently truncated. List
  storage works the same way: every list element in the program (across
  every list, including sublists) comes from one fixed-size pool
  (8192 elements); building a list once it's full prints `list storage
  full, list operation ignored` instead of overflowing.

What's still silent: a word that doesn't start with a number, used where
a number is expected, reads as `0` rather than erroring (see Words
above), and any parse error inside a numeric expression itself (an
unparseable expression just evaluates to `0`, same as always) — see
`ROADMAP.md`.

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
- **Syntax highlighting**: as you type, commands (built-in ones like
  `REPEAT`/`FD`/`MAKE`, and calls to your own `TO`/`END` procedures) are
  colored one way, numbers another, `:variables` another, and `"word`
  literals another. Re-evaluated on every keystroke against the current
  set of defined procedures — this is a separate, lightweight scan, not
  the real interpreter, so it only classifies text for coloring and
  doesn't validate it. Also entry-box only.
- The **File** menu (native macOS menu bar) has **Open…** (⌘O), which picks
  a file via a native dialog and runs it the same way `LOAD` does;
  **Save…** (⌘S), which does the same for `SAVE`; and **Export as PNG…**
  (⌘E), which renders the current canvas (background, drawn lines, and the
  turtle) to an image file at the canvas's actual pixel size.
- The **View** menu (native macOS menu bar) has Increase/Decrease/Reset
  Text Size, applied to both the history pane and the entry box.

## Known limitations (intentional, tracked in `ROADMAP.md`)

- A word that doesn't start with a number, used where a number is
  expected, silently coerces to `0` rather than erroring — see "Errors"
  above.
- The multi-line input's completeness check only balances `[`/`]` and counts
  `TO`/`END` — it doesn't validate the syntax inside, so e.g. a stray `]`
  can make an otherwise-valid input submit early.
