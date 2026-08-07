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
- `WHO` outputs the current turtle's index — the one thing `TELL` sets
  but has no way to read back on its own.

## Turtle commands

| Command | Abbreviation | Argument | Effect |
|---|---|---|---|
| `FORWARD` | `FD` | expr | Move forward by `expr`, drawing if pen is down |
| `BACK` | `BK` | expr | Move backward by `expr` |
| `RIGHT` | `RT` | expr | Turn clockwise by `expr` degrees |
| `LEFT` | `LT` | expr | Turn counter-clockwise by `expr` degrees |
| `SETXY` | — | `x y` | Jump directly to an absolute position, drawing if pen is down; heading unchanged |
| `SETX` | — | expr | Jump to a new x, leaving y unchanged; drawing if pen is down |
| `SETY` | — | expr | Jump to a new y, leaving x unchanged; drawing if pen is down |
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
| `CLEAN` | — | — | Erase the canvas only — unlike `CLEAR`/`CS`, the turtle's position/angle are untouched |
| `CLEARTEXT` | `CT` | — | Clear the history pane — separate from `CLEAR`/`CLEAN`'s canvas-only effect |
| `HIDETURTLE` | `HT` | — | Stop drawing the turtle marker (the trail still draws) |
| `SHOWTURTLE` | `ST` | — | Draw the turtle marker again |
| `WRAP` | — | — | Crossing the canvas edge wraps around to the opposite side |
| `FENCE` | — | — | Crossing the canvas edge stops the turtle right at it, with an error |
| `WINDOW` | — | — | No canvas boundary at all (the default) |
| `LABEL` | — | word/list | Draw text at the turtle's position, in its pen color |
| `FILL` | — | — | Flood-fill the region containing the turtle, bounded by drawn lines, with its pen color |
| `ERASERECT` | — | `width height` | Paint a rectangle centered on the turtle in the background color |
| `WAIT` | — | expr (seconds) | Pause before the next command, without freezing the window |

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
SETPENCOLOR 200 0 0
LABEL "hello

REPEAT 4 [FD 100 RT 90]
PENUP SETXY 300 200 PENDOWN
SETPENCOLOR 255 200 0
FILL
```

- `LABEL text` draws `text` (a word or list, same as `PRINT`) at the
  turtle's current position, in its current pen color. `CLEAR` erases
  labels along with lines.
- `FILL` flood-fills the region containing the turtle — bounded by
  whatever lines are currently drawn — with its current pen color, the
  same way a paint bucket tool works. `CLEAR` erases fills along with
  lines.
- `FILL`'s boundary is **frozen the instant `FILL` is called**: a line
  drawn afterward, even one that cuts straight through the filled
  region, never retroactively changes what got filled or splits it into
  separately-colored pieces the next time the canvas redraws.
- `ERASERECT width height` paints a `width`-by-`height` rectangle,
  centered on the turtle's current position, in the canvas's background
  color — a punch-out rather than a draw. `CLEAR` erases these along
  with fills and lines. Like `FILL`, it's frozen the instant it's
  called: a line drawn afterward through the erased area draws right
  through it rather than being retroactively erased itself, and doesn't
  bring back whatever the rectangle erased either.

```
REPEAT 4 [FD 100 RT 90]
WAIT 2
CLEAR
```

- `WAIT expr` pauses for `expr` seconds (this interpreter's own unit
  choice — real Logo counts 60ths of a second) before running the next
  command, without freezing the window: it queues a redraw of whatever's
  been drawn so far and keeps the GTK event loop responsive while it
  waits, instead of one long blocking sleep. This matters most for a
  script loaded all at once (`LOAD`, or pasting several lines together)
  — typing commands one at a time already shows each one immediately,
  but a whole file runs as a single unit, so without `WAIT`, a `CLEAR`
  partway through would wipe earlier drawing before it was ever visible.

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

```
FD 100
PRINT GETX              -> 250
PRINT GETY              -> 150

SETX 10
SETY 20
PRINT POS                -> 10 20
```

- `GETX`/`GETY` are single-axis complements to `POS`, outputting just one
  coordinate as a plain number instead of a 2-element list — handy when
  only one axis is needed and unpacking `POS` with `FIRST`/`LAST` would
  be more than the call site needs.
- `SETX`/`SETY` are single-axis complements to `SETXY`, jumping along
  just one coordinate while leaving the other (and the heading)
  unchanged — drawing a connecting line if the pen is down, same as
  `SETXY` itself.

```
PRINT DISTANCE [0 0] [3 4]      -> 5

SETXY 0 0
SETHEADING TOWARDS [120 90]
FORWARD DISTANCE POS [120 90]
PRINT POS                        -> 120 90
```

- `DISTANCE p1 p2` outputs the straight-line distance between two
  arbitrary `[x y]` points — it isn't tied to the turtle's own position
  (pass `POS` as one of the two points for "distance from here").
  `TOWARDS point` outputs the heading (same 0-360 convention as
  `HEADING`) to face directly from the turtle's *current* position
  toward `point`. Together they answer "turn to face a target, then
  walk exactly far enough to arrive there" in two lines, as shown above.
- Both report an error if either argument isn't a 2-element list:
  `DISTANCE: expected two 2-element lists` / `TOWARDS: expected a
  2-element list`.

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
  - `INT a` — truncates towards zero (`INT 2.9` is `2`, `INT -2.9` is
    `-2`), unlike `ROUND` (`ROUND -2.9` is `-3`).
  - `SIN a`, `COS a`, `ARCTAN a`, `TAN a`, `ASIN a`, `ACOS a` — trig
    functions; `a`/the result are in **degrees**, matching `RT`/`LT`/
    `HEADING`'s convention, not radians.
  - `LOG a` — base-10 logarithm. `LN a` — natural logarithm. `EXP a` —
    `e` raised to a power, `LN`'s inverse (so `LN EXP a` is `a`).
  - `RANDOM n` — a random integer in `[0, n)`.

Division by zero evaluates to `0` rather than crashing or erroring.
`SQRT` of a negative number, `ASIN`/`ACOS` outside `[-1, 1]`, and
`LOG`/`LN` of a non-positive number all return C's own `NaN` rather
than a special error — they print as `nan`, which is visibly broken
rather than a plausible-looking wrong number, so nothing special was
added on top.

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
- `THING name` also reads a variable back, but takes `name` as any
  expression evaluating to a word rather than a literal identifier —
  `:name` only ever takes a name written right there in the source,
  while `THING WORD "item :n` can compute which variable to read at run
  time. Otherwise identical to `:name`: same scope search, same `0` for
  an unbound name.

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

### Property lists

```
SETPROP "turtle1 "color "red
SETPROP "turtle1 "speed 5
PRINT GETPROP "turtle1 "color   ; red
PRINT GETPROP "turtle1 "nosuchkey   ; the empty list -- nothing stored there
PRINT PROPLIST "turtle1   ; color red speed 5
REMOVEPROP "turtle1 "color
```

A separate namespace from ordinary `MAKE`/`:name` variables — a way to
attach an open-ended set of named values ("properties") to a name,
without pre-declaring which properties exist. (Other Logo dialects call
these `PPROP`/`GPROP`/`REMPROP`/`PLIST` — renamed here since there's no
real cross-dialect standard to match and the abbreviations aren't
self-explanatory.)

- `SETPROP plistname propname value` stores `value` (any type — number,
  word, list, or array) under `propname` in the property list named
  `plistname`, creating the entry if it doesn't exist yet or overwriting
  it in place if it does.
- `GETPROP plistname propname` retrieves that value, or the empty list
  if nothing has been stored under that key — never an error, so it's
  safe to call speculatively (`IF NOT EMPTY? GETPROP "turtle1 "color
  [...]`).
- `REMOVEPROP plistname propname` deletes the entry; a silent no-op if
  it wasn't there.
- `PROPLIST plistname` outputs the whole record as a flat list,
  alternating property names and values (`[color red speed 5]`) —
  nothing at all (the empty list) for a `plistname` with no properties
  stored.
- `plistname` and `propname` are each any expression evaluating to a
  word, same convention as `THING` — `SETPROP WORD "turtle :n "color
  "red` works, not just a literal `"turtle1`.
- Every property list shares one global table regardless of scope —
  there's no `LOCAL`-style scoping for properties the way there is for
  variables.

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

```
PRINT PICK [10 20 30]     -> 10, 20, or 30
PRINT PICK "hello         -> a random single character of "hello
```

- `PICK thing` outputs a random element — a random top-level element of
  a list, a random character of a word, or a random slot of an array
  (see "Arrays" below) — trivial given `RANDOM`/`ITEM`/`COUNT` already
  exist. Reports `PICK: empty list` / `PICK: empty word` for an empty
  list/word (an array's size is always at least 1, so it can't be
  empty).

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

### FLATTEN, PARSE, SUBST

```
PRINT FLATTEN [1 [2 3] [4 [5 6]] 7]   -> 1 2 3 4 5 6 7
PRINT FLATTEN [a b c]                 -> a b c (already flat -- unchanged)

PRINT PARSE "hello                    -> hello (a one-element list)
PRINT PARSE [red green blue]          -> red green blue

PRINT SUBST "b "x [a b c b]           -> a x c x
PRINT SUBST "b "x [a [b c] [d [b e]]] -> a [x c] [d [x e]]
PRINT SUBST [1 2] "x [[1 2] 3 4]      -> x 3 4
```

- `FLATTEN thing` collapses every level of nesting into one flat list:
  a sublist's own brackets disappear, and its leaf elements take their
  place at the top level, in order — only meaningful now that lists
  really nest. A bare word/number is treated as a one-element list, same
  as `COUNT`/`FIRST`/etc.
- `PARSE thing` tokenizes `thing`'s printed text (the same rendering
  `PRINT` shows) by whitespace into a list of words — the reverse of
  what `PRINT` already does for a plain word. It's a pure text split,
  not bracket-aware: parsing a list that itself contains a sublist
  yields literal `"[..."`/`"...]"`-shaped word tokens back, not a real
  nested list, since a bracket is just another non-space character here.
- `SUBST old new thing` rebuilds `thing`, replacing every element equal
  to `old` with `new` — including a whole matching sublist as one unit,
  not just a leaf value (`SUBST [1 2] "x [[1 2] 3 4]` replaces the whole
  `[1 2]` sublist). A non-matching sublist is recursed into, so a nested
  occurrence substitutes without disturbing the rest of that sublist. A
  bare (non-list) `thing` just checks itself directly against `old`.

### Arrays

```
MAKE "a ARRAY 3
SETITEM 1 :a "red
SETITEM 2 :a "green
SETITEM 3 :a "blue
PRINT :a               -> {red green blue}
PRINT ITEM 2 :a        -> green
PRINT COUNT :a         -> 3
```

- `ARRAY size` creates a new array of `size` slots (`size` must be at
  least 1), each defaulting to an empty list, matching real Logo.
  Unlike a list, an array is stored as directly-indexed slots rather
  than a linked chain — reaching slot `n` is one step, not a walk past
  the first `n - 1` elements, which is the whole reason to reach for an
  array over a list.
- `ITEM index array` reads a slot (1-indexed, same convention as
  `ITEM` on a list/word) and `SETITEM index array value` overwrites one
  in place. `SETITEM`'s `value` can be a number, word, or list — but not
  another array (see below) — and prints `SETITEM: index out of range`
  for an out-of-bounds `index`, or `SETITEM: expected an array` if given
  something other than an array.
- `FILLARRAY array value` overwrites every slot with the same `value` in
  one call, rather than looping `SETITEM` manually. Same `value`/error
  rules as `SETITEM` (`FILLARRAY: expected an array` /
  `FILLARRAY: can't store an array inside an array`), just applied to
  every index at once.
- `ARRAY?` reports (`TRUE`/`FALSE`) whether a value is an array, and
  `COUNT` reports its length — same conventions as the other type
  predicates and `COUNT` on a list/word.
- **Arrays are the one mutable, reference-like value in this
  language.** Every other value here is copy-on-build — `MAKE "b :a`
  for a list just aliases the same (never-mutated) underlying nodes, so
  the aliasing is invisible. An array is different: `MAKE "b :a` shares
  the *same* underlying slots, so `SETITEM` through either `:a` or `:b`
  is visible from both — assigning an array doesn't copy it.
- An array always prints inside `{ }`, even as `PRINT`'s own top-level
  value — unlike a list, whose own outermost brackets are never shown
  that way (see "Output" below). This matches real Logo's own
  convention of printing arrays and lists distinctly.
- Arrays are more limited than lists: `FIRST`/`BUTFIRST`/`LAST`/
  `BUTLAST`/`MEMBER?`/`WORD`/`SENTENCE`/`LIST`/`FPUT`/`LPUT`/`MAP`/
  `FILTER`/`REDUCE`/`FOREACH` don't support them — only `ARRAY`/`ITEM`/
  `SETITEM`/`ARRAY?`/`COUNT` do. An array also can't be nested inside
  another array's slot (`SETITEM` prints `SETITEM: can't store an array
  inside an array`) or passed as a user-defined procedure's argument —
  like every value, an argument is coerced to a plain number at the
  call boundary (see `ROADMAP.md`'s Robustness section).

### DOT, CROSS

```
PRINT DOT [1 2 3] [4 5 6]     -> 32   (1*4 + 2*5 + 3*6)
PRINT CROSS [1 0 0] [0 1 0]   -> 0 0 1
```

- `DOT a b` computes the dot product of two numeric lists of the same
  length — the sum of each pair of elements multiplied together.
  Prints `DOT: expected two lists` if either argument isn't a list, or
  `DOT: lists must be the same length` if their lengths differ.
- `CROSS a b` computes the 3D cross product of two numeric lists,
  returning a new 3-element list. Only defined for 3-element vectors,
  so it prints `CROSS: expected two 3-element lists` for anything else
  (including a length that isn't exactly 3).
- Both coerce each element the same way any other numeric context does
  (see "Words & lists" above) — a word element reads as the number its
  text starts with, falling back to `0`.

### TRUE, FALSE

```
PRINT TRUE
PRINT FALSE
MAKE "flag TRUE
IF :flag [PRINT "yes]
```

- `TRUE` and `FALSE` are real values — printable, storable in a
  variable with `MAKE`, and passable around like any other word. They're
  words rather than a distinct value type (real Logo doesn't have one
  either): `TRUE`/`FALSE` are just special-cased, case-insensitive
  keyword literals, same spelling either way (`PRINT true` also prints
  `TRUE`).
- `IF`/`WHILE` (and the comparisons/`AND`/`OR`/`NOT` inside their
  condition) treat the word `FALSE` as false, and everything else
  (including `TRUE`, any other non-empty word, or a nonzero number) as
  true — the same non-empty-word-is-true convention as before, with one
  added special case for the literal word `FALSE`.
- Comparisons (`=`, `<`, etc.) and `AND`/`OR`/`NOT` themselves are
  unchanged — they only exist inside `IF`/`WHILE`'s own condition slot
  (see Conditionals below), not as general expression values. There's no
  `PRINT :a = :b` or `MAKE "bigger :a > :b` — only `IF :a = :b [...]`.

### Type & membership predicates

```
PRINT WORD? "hi           -> TRUE
PRINT LIST? [1 2]         -> TRUE
PRINT NUMBER? 5           -> TRUE
PRINT EMPTY? []           -> TRUE
PRINT EMPTY? [1]          -> FALSE
PRINT MEMBER? "b [a b c]  -> TRUE
PRINT MEMBER? "ell "hello -> TRUE

IF WORD? "hi [PRINT "yes]
```

- `WORD?`, `LIST?`, and `NUMBER?` each take one argument and report
  (`TRUE`/`FALSE`) whether it's that kind of value — the only way to ask
  "what kind of value is this?" from inside a running program.
- `EMPTY?` reports whether a list (`[]`) or word (`""`) is empty; a
  number is never "empty" (always `FALSE`).
- `MEMBER? thing container` reports whether `thing` is one of
  `container`'s top-level elements (a list), or a substring of it (a
  word — so `MEMBER? "ell "hello` is true, not just single-character
  checks); a bare number `container` is treated as a one-element list,
  same convention as `FIRST`/`COUNT`/etc.
- All five return `TRUE`/`FALSE` (see "TRUE, FALSE" above) rather than a
  special boolean type, so they work directly in `IF`/`WHILE` or
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
- `SHOW "name` prints a procedure's own definition back out, exactly as
  `SAVE` would write it to a file — `TO name :params`, its body, then
  `END`. Prints `SHOW: no such procedure "name` if it isn't defined.
- `TEXT "name` is the read-as-data complement to `SHOW`: instead of
  printing the definition, it outputs the procedure's raw body text
  tokenized into a flat list of words — the same whitespace tokenizing
  `PARSE` does (see "FLATTEN, PARSE, SUBST" above), not a full re-parse
  of Logo syntax. A `"quoted word or a `[bracketed block]` in the body
  keeps its punctuation as literal characters within a token, rather
  than being reinterpreted the way `eval_logo` itself would — so `TEXT`
  of a body containing `PRINT "hi` gives a two-word list whose second
  word is the literal text `"hi`, quote character included, not the
  word `hi`. Prints `TEXT: no such procedure "name` if it isn't defined.
- Procedures can call other procedures, including recursively.
- `PROCEDURES` outputs a list of every currently-defined procedure's
  name, in definition order — workspace introspection alongside `SHOW`.
- `NAMES` outputs a list of every currently-defined **global**
  variable's name — a procedure's own parameters and any `LOCAL`s don't
  appear, only whatever `MAKE` has set outside of (or before) any call.
  Reassigning an existing name with `MAKE` doesn't add a second entry.

### OUTPUT, STOP

```
TO double :n
  OUTPUT :n * 2
END
PRINT double 5        -> 10

TO fact :n
  IF :n = 0 [OUTPUT 1]
  OUTPUT :n * fact :n - 1
END
PRINT fact 5           -> 120
```

- `OUTPUT expr` (or `OP expr`) ends the current procedure call and hands
  `expr` back as its value — like a `return` statement. That's what lets
  a procedure call be used as a value anywhere an argument is expected
  (`PRINT double 5`, `MAKE "x double 5`), the same way `FIRST`/`WORD`/
  etc. already can. A same-named built-in always wins over a
  user-defined procedure at this "used as a value" position, same
  precedence rule the ordinary command dispatch already has for command
  names.
- `STOP` ends the current call the same way but with no value at all —
  for a procedure that's only ever called for its side effects (like a
  turtle-drawing procedure), used to bail out early (e.g. `IF :n < 0
  [STOP]`).
- Calling a procedure that never actually reaches an `OUTPUT` (it either
  runs to completion or hits a bare `STOP` first) as if it *were* a
  value is a reported error — `<name>: didn't output a value` — rather
  than a silent `0` or empty word.
- Called as a plain statement instead (not inside another expression),
  a procedure's `OUTPUT` value is simply discarded, no error either way
  — `double 5` alone on a line just runs it.
- `OUTPUT`/`STOP` can appear anywhere inside the procedure, including
  nested inside any number of `REPEAT`/`IF`/`WHILE`/`FOR`/`FOREVER`
  blocks, and still end the *whole call* — not just the block they're
  textually inside.
- `OUTPUT` needs an active procedure call to hand its value back to, so
  it's an error at the top level: `OUTPUT: can only be used inside a
  procedure`. `STOP` has no such requirement — it's also the only way to
  escape a `FOREVER` typed directly at the top level (there's no
  condition to fall false there), so a bare top-level `STOP` is legal:
  it just quietly ends the rest of that run, the same way it ends the
  rest of a procedure call, with nothing printed either way.

### CATCH, THROW

```
CATCH "err [
  PRINT "before
  THROW "err
  PRINT "unreachable
]
PRINT "after            -> before
                            after

TO inner
  THROW "boom
END
TO outer
  inner
  PRINT "unreachable
END
CATCH "boom [outer]
PRINT "after-boom        -> after-boom (nothing from inner/outer prints)
```

- `CATCH "tag [block]` runs `block`; if a `THROW "tag` (same tag, matched
  case-insensitively like everything else) happens anywhere inside it —
  including inside procedures it calls, however deeply nested — execution
  jumps straight back to right after `CATCH`, skipping the rest of
  `block` and every call frame in between. If `block` never throws,
  `CATCH` just runs it normally and produces no other effect.
- `THROW "tag` unwinds up through as many call frames (procedure calls,
  `REPEAT`/`IF`/`WHILE` blocks) as it takes to reach the nearest enclosing
  `CATCH` whose tag matches — skipping any `CATCH`es with a different tag
  along the way, the way an exception skips a `catch` block for the wrong
  type in other languages.
- A `THROW` with no matching `CATCH` anywhere prints `THROW: no CATCH
  found for "tag` and execution then resumes with whatever top-level
  command comes next — the same recovery an unrecognized command already
  gets, not a crash or a silently bricked interpreter.
- `CATCH`/`THROW` are independent of `OUTPUT`/`STOP`: a `STOP` or
  `OUTPUT` inside a `CATCH`'s block still only unwinds to its own
  enclosing procedure call, same as if the `CATCH` weren't there; `CATCH`
  doesn't intercept those.
- `CATCH` prints `CATCH: expected [ block ]` if the bracketed block is
  missing, unterminated, or too long, same as `REPEAT`/`IF`/`WHILE`.

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
- Both are commands, not operators usable inside an expression the way
  `FIRST`/`WORD`/etc. are — there's no `PRINT RUN [...]` or
  `MAKE "x APPLY "name [...]` that captures a value back out of them.
- A self-referential `RUN` (e.g. `MAKE "x [RUN :x]` then `RUN :x`) is
  capped and reported as `RUN: too deeply nested, ignored` rather than
  crashing — this is a much lower, separate limit from ordinary
  recursion, since nested `RUN`s are considerably more expensive per
  level and a self-referential one is always a mistake in the program,
  never a legitimate technique.

### Higher-order iteration: MAP, FOREACH, FILTER, REDUCE

```
PRINT MAP [? * 2] [1 2 3]         -> 2 4 6
PRINT FILTER [? > 2] [1 2 3 4]    -> 3 4
PRINT REDUCE [?1 + ?2] [1 2 3 4]  -> 10
FOREACH [PRINT ?] [1 2 3]
```

- All four take a **template** — a `[bracketed expression]` with `?`
  standing in for the current element (`?1`/`?2` for `REDUCE`'s
  accumulator-so-far and current-element, since it needs two slots) —
  and a list (a bare word/number is treated as a one-element list, same
  convention as `FIRST`/`COUNT`/etc.). There's no procedure-name template
  form (`MAP "double :list`) — only the bracketed-expression form above.
- `MAP` builds a new list by substituting `?` and evaluating the
  template as an **expression** once per element. `FILTER` substitutes
  and evaluates the template as a **condition** (so comparisons like
  `? > 2` work, not just arithmetic) once per element, keeping the
  *original* element — not the template's true/false result — whenever
  it's true. `REDUCE` folds left to right, seeding the accumulator with
  the list's own first element (no separate start-value argument).
  `FOREACH` is the odd one out: it's a command, not an operator — it
  *runs* the substituted template once per element for side effects
  (like `PRINT`), producing no list of its own.
- If an element is itself a list, it's substituted in *with* its
  brackets (`MAP [COUNT ?] [[1 2] [3 4 5]]` is `2 3`) — otherwise its own
  elements would spill out as separate tokens into the template instead
  of staying one value.

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

```
FOR [i 1 5] [PRINT :i]
FOR [i 5 1] [PRINT :i]
FOR [i 0 10 5] [PRINT :i]
```

- `FOR [var start limit step] [block]` is a counted loop that exposes the
  loop variable directly as `:var`, unlike `REPEAT` (count only, no
  variable) or `WHILE` (a condition you update by hand). `step` is
  optional: it defaults to `1`, or `-1` when `limit` is less than `start`,
  same as Berkeley Logo. `start`/`limit`/`step` can be any expression, not
  just literals. A `step` of `0` is an error rather than an infinite loop.
- Same 1,000,000-iteration safety net as `WHILE`, for a range that never
  reaches its limit.

```
TO countdown
  MAKE "i 5
  FOREVER [
    PRINT :i
    MAKE "i :i - 1
    IF :i < 1 [STOP]
  ]
END
```

- `FOREVER [block]` runs `block` forever — the only way out is `STOP` (or
  a procedure's `OUTPUT`, or `THROW`) inside it, same as any other loop
  body. Useful for a game-loop-style script that runs until some
  in-script condition decides to stop it. Unlike `OUTPUT`, `STOP` works
  directly at the top level too (see "OUTPUT, STOP" below) specifically
  so a bare top-level `FOREVER` has a real way to end, rather than only
  the same 1,000,000-iteration safety net as `WHILE`/`FOR`.

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
- `TYPE <expr>` works exactly like `PRINT`, but **without** the trailing
  newline — several `TYPE`s (or a `TYPE` followed by a `PRINT`) build up
  one line of output piece by piece: `TYPE "a` then `TYPE "b` then
  `PRINT "c` prints `abc` on one line, not three.

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

## Background image

```
LOADPIC "backyard.png
SAVEPIC "with_turtle.png
```

- `LOADPIC "path` loads an image file (any format `gdk-pixbuf` supports —
  PNG, JPEG, GIF, BMP, and more) as the canvas background, scaled to
  fill it exactly. Drawing (lines, `FILL`, `ERASERECT`, turtles) is
  painted on top of it, same as it would be over the flat
  `SETBACKGROUND` color. It persists across `CLEAR`/`CLEAN` (they only
  erase drawing, not the background) — load a different image, or a
  blank-colored one, to replace it.
- `ERASERECT` always erases back to the flat `SETBACKGROUND` color
  within the rectangle it covers, not back to the loaded image — a
  known, accepted simplification rather than re-compositing that patch
  of the image.
- `SAVEPIC "path` writes the current canvas — background image (if any),
  every line, and every turtle — out as a PNG, regardless of the
  extension given. The same rendering **File → Export as PNG…** uses,
  just reachable from a script instead of the menu.
- Both take the path as a single whitespace-delimited word, same
  convention as `LOAD`/`SAVE` (see above) — use the file-picker menu
  actions instead for a path containing spaces.
- If the file can't be decoded/read or written, prints "LOADPIC: could
  not load "path" / "SAVEPIC: could not save "path" rather than
  crashing.

## Turtle sprites

```
LOADSPRITE "ant "ant.png
SETSPRITE "ant
REPEAT 12 [STAMPSPRITE FD 40 RT 30]
SETSPRITE "NONE
```

- `LOADSPRITE "name "path` loads an image file (any format `gdk-pixbuf`
  supports, same as `LOADPIC`) and registers it under `name`, scaled to
  a fixed 40x40 box. Loading a second image under an existing name
  replaces it.
- `SETSPRITE "name` assigns a previously loaded shape to the current
  turtle, replacing its default triangle everywhere it's drawn — live on
  the canvas and in any later `STAMPSPRITE`. `SETSPRITE "NONE` resets it
  back to the default triangle. Prints `SETSPRITE: no such sprite
  "name` if that name was never `LOADSPRITE`d.
- `STAMPSPRITE` bakes a **permanent** copy of the turtle's current shape
  (its `SETSPRITE` image, or the default triangle) onto the canvas at
  its current position and heading — the turtle keeps moving
  afterward, but the stamped copy stays exactly where it was stamped.
  Same call-time-frozen treatment as `FILL`/`ERASERECT`: a line drawn
  after a `STAMPSPRITE` can't retroactively cover it, and vice versa.
- If `LOADSPRITE` fails (bad path, unrecognized format, or the sprite
  table — 20 entries — is already full), prints `LOADSPRITE: could not
  load "path` rather than crashing.

### Sprite-sheet blit

```
LOADSPRITESHEET "walker "walker.png 4 2
SETSPRITE "walker
FOR [i 0 23] [SETSPRITEFRAME MOD :i 8 STAMPSPRITE FD 20 RT 15]
SETSPRITE "NONE
```

- `LOADSPRITESHEET "name "path cols rows` is `LOADSPRITE`'s sibling for
  loading one image packed with several frames in an even grid — a
  walk-cycle strip, a tile set, anything where only one cell should be
  drawn at a time. It slices the image into `cols` columns by `rows`
  rows of equal-size cells and registers all of them under `name`,
  0-indexed row-major (frame 0 is the top-left cell, reading left to
  right then top to bottom). Both `cols` and `rows` must be at least 1;
  otherwise prints `LOADSPRITESHEET: cols and rows must be at least 1`
  without touching any existing sprite under that name.
- `SETSPRITE` (see above) works the same way for a sheet as for a plain
  image — it assigns the whole grid to the turtle and resets its active
  frame back to 0.
- `SETSPRITEFRAME n` picks which cell of the current turtle's grid is
  shown — live on the canvas and in any later `STAMPSPRITE` — until the
  next `SETSPRITEFRAME` or `SETSPRITE` call. Requires a sprite already
  assigned via `SETSPRITE` (prints `SETSPRITEFRAME: no sprite set (use
  SETSPRITE first)` otherwise) and `n` within that sprite's grid (prints
  `SETSPRITEFRAME: frame out of range` otherwise, `0` to `cols * rows -
  1`). A plain (non-sheet) `LOADSPRITE` image is just a 1x1 grid, so
  `SETSPRITEFRAME 0` is its only valid frame.
- Each cell is scaled independently into the same fixed 40x40 turtle box
  every other sprite uses, so cells don't need to be square — a
  `LOADSPRITESHEET "name "path 4 2` on a 200x100 image slices out
  50x50 cells and scales each one up to 40x40, same as `LOADSPRITE`
  scales a whole image.

### Animated sprites

```
LOADSPRITESHEET "walker "walker.png 4 2
SETSPRITE "walker
ANIMATESPRITE 0.15 24
SETSPRITE "NONE
```

- `ANIMATESPRITE delay frames` plays the current turtle's sprite-sheet
  frames in place, over real time: it advances the active frame by 1
  (wrapping back to 0 at the end of the grid), pauses `delay` seconds,
  and requests a redraw, `frames` times in a row — so each intermediate
  frame is actually visible on screen as it plays, not just the final
  one once the command returns. The turtle doesn't move; combine with
  `FD`/`RT` (and `SETSPRITEFRAME` directly, as in the sprite-sheet blit
  example above) for a walking animation instead of an in-place one.
  `delay` of `0` (or less) skips the pause but still advances every
  frame and redraws, same as `WAIT`'s own zero-or-negative handling.
  Requires a sprite already assigned via `SETSPRITE` — same requirement
  and error message (`ANIMATESPRITE: no sprite set (use SETSPRITE
  first)`) as `SETSPRITEFRAME`.
- Doesn't touch the canvas raster or interact with `STAMPSPRITE` at
  all — it's a live-only turtle-state change over time, same category
  as `SETSPRITEFRAME`. Call `STAMPSPRITE` yourself in between if you
  also want a permanent copy of some frame.

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
- `MAKE`, `ERASE`, `SHOW`, `LOAD`, `SAVE`, `LOADPIC`, `SAVEPIC`,
  `LOADSPRITE`, `LOADSPRITESHEET`, and `SETSPRITE` each print
  `<COMMAND>: expected a "name`/`"path` if the required quoted word is
  missing. `ERASE`/`SHOW` also print `<COMMAND>: no such procedure
  "<name>` if the name isn't defined; `SETSPRITE` similarly prints
  `SETSPRITE: no such sprite "<name>` if it wasn't `LOADSPRITE`d/
  `LOADSPRITESHEET`d. `SETSPRITEFRAME` prints `SETSPRITEFRAME: no sprite
  set (use SETSPRITE first)` with no sprite assigned, or
  `SETSPRITEFRAME: frame out of range` outside its sprite's grid.
  `ANIMATESPRITE` prints the same "no sprite set" message (as
  `ANIMATESPRITE: no sprite set (use SETSPRITE first)`) under the same
  condition.
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
- `CATCH` prints `CATCH: expected [ block ]` if its bracketed block is
  missing, unterminated, or too long. `THROW` with no matching `CATCH`
  anywhere prints `THROW: no CATCH found for "tag` and execution recovers
  at the next top-level command, rather than aborting the rest of the
  running script (see "CATCH, THROW" above).
- `DOT` prints `DOT: expected two lists` if either argument isn't a
  list, or `DOT: lists must be the same length` if their lengths differ.
  `CROSS` prints `CROSS: expected two 3-element lists` if either
  argument isn't a list with exactly 3 elements (see "DOT, CROSS"
  above).
- `ARRAY` prints `ARRAY: size must be at least 1` for a size less than
  1. `SETITEM` prints `SETITEM: expected an array` if not given an
  array, `SETITEM: index out of range` for an out-of-bounds index, or
  `SETITEM: can't store an array inside an array` if `value` is itself
  an array (see "Arrays" above).
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
"Known limitations" below.

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
  **Save…** (⌘S), which does the same for `SAVE`; **Export as PNG…**
  (⌘E), which renders the current canvas (background, drawn lines, and the
  turtle) to an image file at the canvas's actual pixel size; and **Quit**
  (⌘Q), which closes the app.
- The **View** menu (native macOS menu bar) has Increase/Decrease/Reset
  Text Size, applied to both the history pane and the entry box.

## Known limitations (intentional, permanent design choices)

- A word that doesn't start with a number, used where a number is
  expected, silently coerces to `0` rather than erroring — see "Errors"
  above.
- The multi-line input's completeness check only balances `[`/`]` and counts
  `TO`/`END` — it doesn't validate the syntax inside, so e.g. a stray `]`
  can make an otherwise-valid input submit early.
