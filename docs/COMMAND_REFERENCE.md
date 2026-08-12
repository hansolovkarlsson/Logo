# Command Reference

A complete, per-command reference for every instruction the live app
(`bin/logo`, running entirely on the bytecode compiler+VM — see
`docs/BYTECODE_VM_DESIGN.md`) actually recognizes, as of this build.
Every command below was cross-checked directly against `src/parser.c`'s
own `BUILTIN_SIGNATURES` table (the VM's grammar) — not copied from
`docs/LANGUAGE.md` without verification, precisely because that
cross-check surfaced real gaps (see "Appendix" at the bottom). Where
`docs/LANGUAGE.md` already documents a command's full behavior in
prose, this reference doesn't repeat that depth — it's the scannable
index; `LANGUAGE.md` is the fuller read.

All commands are case-insensitive. `expr` means any numeric/word/list
expression, evaluated the same way everywhere (see "Expressions"
below); `word`/`list` mean a value of that specific kind is expected.

**Keep this in sync**: whenever a new command is added to
`parser.c`'s `BUILTIN_SIGNATURES` (or an existing one's grammar
changes), add/update its row and example here in the same batch — and
check whether the change also touches `docs/BYTECODE_REFERENCE.md`
(a new opcode, or an existing one's compiled shape changing).

## Contents

- [Turtle motion & state](#turtle-motion--state)
- [Multiple turtles](#multiple-turtles)
- [Pen, color & canvas](#pen-color--canvas)
- [Turtle speed](#turtle-speed)
- [Expressions & math](#expressions--math)
- [Variables](#variables)
- [Output](#output)
- [Procedures](#procedures)
- [Conditionals](#conditionals)
- [Loops](#loops)
- [Words & lists](#words--lists)
- [Type & membership predicates](#type--membership-predicates)
- [Higher-order iteration](#higher-order-iteration)
- [Arrays](#arrays)
- [Vector operators](#vector-operators)
- [Property lists](#property-lists)
- [Prototype-style objects](#prototype-style-objects)
- [Error handling](#error-handling)
- [Deferred execution](#deferred-execution)
- [Files](#files)
- [Turtle sprites](#turtle-sprites)
- [Resizable canvas](#resizable-canvas)
- [Suspend/resume](#suspendresume)
- [Concurrent agents](#concurrent-agents)
- [Clock](#clock)
- [Event triggers](#event-triggers)
- [Appendix: documented elsewhere, not available in `bin/logo`](#appendix-documented-elsewhere-not-available-in-binlogo)

---

## Turtle motion & state

Turtle `0` starts at the canvas center (`250 250` on the default
500x500 canvas), facing angle `0` (north/up), pen down. Angle
increases clockwise. Every command here acts on the **current**
turtle (see "Multiple turtles").

| Command | Aliases | Args | Description |
|---|---|---|---|
| `FORWARD` | `FD` | `expr` | Move forward, drawing if pen is down |
| `BACK` | `BK` | `expr` | Move backward |
| `RIGHT` | `RT` | `expr` | Turn clockwise `expr` degrees |
| `LEFT` | `LT` | `expr` | Turn counter-clockwise `expr` degrees |
| `SETXY` | — | `x y` | Jump to an absolute position; heading unchanged |
| `SETX` | — | `expr` | Jump to a new x, y unchanged |
| `SETY` | — | `expr` | Jump to a new y, x unchanged |
| `SETHEADING` | `SETH` | `expr` | Set absolute heading (no movement) |
| `HOME` | — | — | Jump back to the start position, heading `0` |
| `GETX` | — | — (operator) | Current x as a number |
| `GETY` | — | — (operator) | Current y as a number |
| `POS` | — | — (operator) | Current position as `[x y]` |
| `HEADING` | — | — (operator) | Current heading (raw, not normalized to 0-360) |
| `DISTANCE` | — | `p1 p2` (operator) | Straight-line distance between two `[x y]` points |
| `TOWARDS` | — | `point` (operator) | Heading from here to face `point` |
| `ARC` | — | `angle radius` | Draw part/all of a circle centered here; turtle doesn't move |

```
REPEAT 4 [FD 100 RT 90]        ; a square

SETXY 0 0
SETHEADING TOWARDS [120 90]
FORWARD DISTANCE POS [120 90]  ; walk exactly to [120 90]
PRINT POS                       -> 120 90

ARC 360 80                      ; a full circle, radius 80
RT 45 ARC 90 40                 ; a quarter arc, offset by heading
```

`SETXY`/`HOME` draw a connecting line if the pen is down, exactly like
`FORWARD`. `SETXY`/`SETHEADING`/`RT`/`LT` *set* position/heading;
`POS`/`HEADING`/`GETX`/`GETY` *read* them back — both directions exist
for every axis. See `docs/LANGUAGE.md`'s "Turtle commands"/"Turtle
state queries" sections for edge behavior and the exact 0-360 vs raw
heading convention.

## Multiple turtles

| Command | Args | Description |
|---|---|---|
| `TELL` | `expr` (0-9) | Switch which turtle is current, creating it on first use |
| `WHO` | — (operator) | Current turtle's index |
| `TURTLES` | — (operator) | Count of turtles that exist so far (`TELL`'d or `LAUNCH`'d) |

```
TELL 1
FD 100
TELL 0
RT 90 FD 50
PRINT WHO   -> 0
PRINT TURTLES -> 2
```

A script that never calls `TELL` behaves as if there were only ever
one turtle. Turtle indices run `0`-`9` (`MAX_TURTLES` = 10, shared with
`LAUNCH`'s own agent turtles — see "Concurrent agents").

## Pen, color & canvas

| Command | Aliases | Args | Description |
|---|---|---|---|
| `PENUP` | `PU` | — | Stop drawing while moving |
| `PENDOWN` | `PD` | — | Resume drawing while moving |
| `SETPENCOLOR` | `SETPC` | `r g b` (0-255 each) | Color new lines are drawn in |
| `SETPENWIDTH` | `SETPW` | `expr` (clamped 0.5-20) | Width new lines are drawn with |
| `SETBACKGROUND` | `SETBG` | `r g b` (0-255 each) | Canvas background color (single, canvas-wide) |
| `CLEAR` | `CS` | — | Erase canvas, reset every turtle's position/angle |
| `CLEAN` | — | — | Erase canvas only — turtle position/angle untouched |
| `CLEARTEXT`\* | `CT`\* | — | Clear the history pane |
| `HIDETURTLE` | `HT` | — | Stop drawing the turtle marker (trail still draws) |
| `SHOWTURTLE` | `ST` | — | Draw the turtle marker again |
| `WRAP` | — | — | Canvas-edge crossing wraps to the opposite side |
| `FENCE` | — | — | Canvas-edge crossing stops the turtle there, with an error |
| `WINDOW` | — | — | No canvas boundary (the default) |
| `LABEL` | — | `word`/`list` | Draw text at the turtle's position, in its pen color |
| `FILL` | — | — | Flood-fill the region containing the turtle with pen color |
| `ERASERECT` | — | `width height` | Erase a rectangle centered on the turtle to background color |

\* `CLEARTEXT`/`CT` are documented here to match `LANGUAGE.md`'s own
listing, but see the Appendix — **not currently reachable from
`bin/logo`** (old-engine-only).

```
SETPENCOLOR 255 0 0
FD 80 RT 90
SETPC 0 100 255
FD 80

WRAP
SETXY 600 250      ; wraps to 100 250 on a 500-wide canvas

SETPENCOLOR 200 0 0
LABEL "hello
REPEAT 4 [FD 100 RT 90]
PENUP SETXY 300 200 PENDOWN
SETPENCOLOR 255 200 0
FILL
```

Each drawn line/label/fill/erase remembers the pen state (color,
width) it was made with — `SETPENCOLOR` doesn't recolor what's already
drawn. `FILL`/`ERASERECT`/`LABEL` are all frozen the instant they're
called: a line drawn afterward doesn't retroactively interact with
them. See `docs/LANGUAGE.md`'s "Turtle commands" section for the exact
`WRAP`/`FENCE` boundary behavior.

## Turtle speed

| Command | Args | Description |
|---|---|---|
| `SETSPEED` | `expr` (seconds) | Pause this long after every motion command; `0` (default) is instant |
| `SPEED` | — (operator) | Current setting, in seconds |

```
SETSPEED 0.1
REPEAT 4 [FD 100 RT 90]   ; each side/turn now takes 0.1s to appear
SETSPEED 0                ; back to instant
```

Applies to every turtle-*motion* command (`FD`/`BK`/`RT`/`LT`/`SETXY`/
`SETX`/`SETY`/`SETHEADING`/`HOME`/`ARC`) — not `PENUP`/`SETPENCOLOR`/
`TELL`/etc. Also settable from the command line: `bin/logo --speed 0.2
script.logo`. See `docs/LANGUAGE.md`'s "Turtle commands" section for
its interaction with templates/`RUN`/`LOAD`/concurrent agents.

## Expressions & math

Anywhere a command expects a number, a full expression is accepted:
literals (`100`, `3.5`, `-2`), operators (`+ - * /`, standard
precedence, unary `+`/`-`), parentheses (`(1 + 2) * 3`), variables
(`:name`), and the prefix operators below (tighter-binding than
`* / + -`, so `MOD 7 3 + 1` is `(MOD 7 3) + 1`).

| Operator | Args | Description |
|---|---|---|
| `MOD` | `a b` | Remainder, sign of `b` (`MOD -7 3` is `2`, not `-1`) |
| `POWER` | `a b` | `a` raised to the power `b` |
| `SQRT` | `a` | Square root |
| `ABS` | `a` | Absolute value |
| `ROUND` | `a` | Round to nearest integer |
| `INT` | `a` | Truncate toward zero (`INT -2.9` is `-2`, unlike `ROUND -2.9` = `-3`) |
| `SIN` `COS` `TAN` `ASIN` `ACOS` `ARCTAN` | `a` | Trig, degrees not radians |
| `SEC` `CSC` `COT` | `a` | Secant/cosecant/cotangent, degrees (`1/COS`, `1/SIN`, `1/TAN`) |
| `ASEC` `ACSC` `ACOT` | `a` | Their inverses, returning degrees |
| `ARCTAN2` | `y x` | Two-argument arctangent (full-circle heading), degrees |
| `LOG` | `a` | Base-10 logarithm |
| `LN` | `a` | Natural logarithm |
| `EXP` | `a` | `e` raised to a power (`LN`'s inverse) |
| `PI` | — (operator) | The constant π |
| `RANDOM` | `n` | Random integer in `[0, n)` |
| `RERANDOM` | — | Reseed `RANDOM` to a fixed, reproducible sequence (instead of the clock) |
| `BITAND` `BITOR` `BITXOR` | `a b` | Bitwise AND/OR/XOR |
| `BITNOT` | `a` | Bitwise complement |
| `LSHIFT` `RSHIFT` | `a n` | Bit-shift `a` left/right by `n` |

```
PRINT MOD 7 3            -> 1
PRINT MOD -7 3            -> 2
PRINT POWER 2 10          -> 1024
PRINT SQRT 16              -> 4
PRINT ABS -5                -> 5
PRINT ROUND 2.6             -> 3
PRINT INT -2.9               -> -2
PRINT SIN 90                  -> 1
PRINT SEC 60                   -> 2
PRINT ARCTAN2 1 1                -> 45
PRINT PI                          -> 3.14159
REPEAT 5 [PRINT RANDOM 10]
PRINT BITAND 12 10                 -> 8
PRINT LSHIFT 1 4                    -> 16
PRINT RSHIFT 16 4                    -> 1
```

Division by zero evaluates to `0`. `SQRT` of a negative, `ASIN`/`ACOS`
outside `[-1, 1]`, and `LOG`/`LN` of a non-positive number all return
`nan` rather than erroring. `BITAND`/`BITOR`/`BITXOR`/`BITNOT`/
`LSHIFT`/`RSHIFT` truncate through a 64-bit integer first, same as
`INT`'s own truncate-toward-zero, just wide enough for real bit
patterns. `LSHIFT`/`RSHIFT` each also tolerate a negative `n` by
flipping direction (so `LSHIFT a (-n)` behaves like `RSHIFT a n`), a
robustness fallback rather than the intended way to ask for the other
direction — use the other operator instead. A negative literal written
as a *later* argument to a prefix operator needs parentheses
(`RSHIFT 16 (-4)`, not `RSHIFT 16 -4`) — unparenthesized, the trailing
`-4` greedily continues as binary subtraction on the argument before it
instead of starting a fresh one.

## Variables

| Command | Args | Description |
|---|---|---|
| `MAKE` | `"name expr` | Set (or create) a variable |
| `THING` | `expr` (operator) | Read a variable back, name computed at runtime |
| `LOCAL` | `"name` | Declare a variable scoped to the current call |
| `NAMES` | — (operator) | List of every currently-defined global variable's name |
| `PROCEDURES` | — (operator) | List of every currently-defined procedure's name |

```
MAKE "size 100
FD :size
MAKE "size :size + 10

TO uselocal
  LOCAL "x
  MAKE "x 1
  PRINT :x
END
```

`:name` reads a variable inline (only ever a literal name written in
source); `THING` takes any expression evaluating to a word, so the
name to read can be computed (`THING WORD "item :n`). Procedure
parameters and `LOCAL`s are dynamically-scoped, real recursion-safe
variables, not text substitution — see `docs/LANGUAGE.md`'s
"Variables & scoping" section for the full shadowing rules. Real
recursion depth is ~2000 nested calls before "Recursion too deep, call
ignored" (raised from the original 200 — see
`docs/BYTECODE_VM_DESIGN.md`'s "VM-owned scope storage" entry).

## Output

| Command | Args | Description |
|---|---|---|
| `PRINT` | `expr` | Write to the history pane, with a trailing newline |

```
PRINT "hello
PRINT 2 + 2
PRINT [hello there, this prints as one line]
```

A word prints as itself; a list prints as its elements space-joined
(its own outer brackets never shown, but a nested sublist's brackets
are, since that's the only way to tell elements apart) — see
`docs/LANGUAGE.md`'s "Output" section for the exact nesting rule.

## Procedures

| Command | Args | Description |
|---|---|---|
| `TO ... END` | (special grammar) | Define a procedure, up to 8 parameters |
| `OUTPUT` | `expr` | End the call, handing `expr` back as a value |
| `STOP` | — | End the call with no value |
| `ERASE` | `"name` | Delete a procedure definition |
| `TEXT` | `"name` (operator) | Procedure's raw body, tokenized into a list of words |
| `SHOW` | `"name` | Print a procedure's own definition back out |
| `DEFINED?` | `word` (operator) | Whether a procedure by this name currently exists |

```
TO rect :w :h
  REPEAT 2 [FD :w RT 90 FD :h RT 90]
END
rect 100 40

TO fact :n
  IF :n = 0 [OUTPUT 1]
  OUTPUT :n * fact :n - 1
END
PRINT fact 5     -> 120

PRINT DEFINED? "rect   -> TRUE
PRINT DEFINED? "bogus  -> FALSE
```

Defining a `TO` with an existing name overwrites it in place. Calling
a procedure that never reaches `OUTPUT` as if it were a value is a
reported error, not a silent `0`. See `docs/LANGUAGE.md`'s
"Procedures"/"OUTPUT, STOP" sections for the full detail.

## Conditionals

| Form | Description |
|---|---|
| `IF <condition> [block]` | Run `block` only if true |
| `IF <condition> [block] ELSE [block]` | Same as `IFELSE` below |
| `IFELSE <condition> [block] [block]` | Run the first block if true, the second if false |

```
IF :size > 10 [PRINT "big]
IFELSE :size > 10 [PRINT "big] [PRINT "small]
IF :x > 0 AND :x < 10 [PRINT "in-range]
IF NOT :x = 0 [PRINT "nonzero]
```

A condition is an expression optionally followed by a relational
operator (`< > = <= >= <>`) and a second expression — no operator
means truthiness (nonzero number, non-empty word, or the literal word
`TRUE`; `FALSE` is the one special-cased false word). `AND`/`OR`/`NOT`
combine clauses, standard precedence, **no parentheses for grouping**
booleans (parens are arithmetic-only).

## Loops

| Command | Args | Description |
|---|---|---|
| `REPEAT` | `expr [block]` | Run `block` `expr` times |
| `REPCOUNT` | — (operator) | The innermost running `REPEAT`'s 1-indexed pass number, or `-1` outside any `REPEAT` |
| `WHILE` | `<condition> [block]` | Re-check `condition` before each pass |
| `FOR` | `[var start limit step] [block]` | Counted loop exposing `:var` |
| `FOREVER` | `[block]` | Loop forever — only `STOP`/`OUTPUT`/`THROW` end it |

```
REPEAT 4 [FD 100 RT 90]
REPEAT 5 [PRINT REPCOUNT]  -> 1  2  3  4  5

MAKE "i 0
WHILE :i < 4 [FD 80 RT 90 MAKE "i :i + 1]

FOR [i 1 5] [PRINT :i]
FOR [i 0 10 5] [PRINT :i]

TO countdown
  MAKE "i 5
  FOREVER [
    PRINT :i
    MAKE "i :i - 1
    IF :i < 1 [STOP]
  ]
END
```

`FOR`'s `step` defaults to `1`, or `-1` when counting down. `WHILE`/
`FOR`/`FOREVER` all stop themselves after 1,000,000 iterations as a
runaway-loop safety net.

`REPCOUNT` is dynamic, not lexical: a procedure called from inside a
`REPEAT` sees the *caller's* `REPCOUNT`, not `-1` — it reflects
whichever `REPEAT` is innermost at the moment `REPCOUNT` actually runs,
regardless of how many procedure calls sit in between. Nested `REPEAT`s
each get their own count; `REPCOUNT` always reports the innermost one,
and the outer one's own count is unaffected once the inner loop ends.
`WHILE`/`FOR`/`FOREVER` don't have a `REPCOUNT` of their own — `FOR`
already exposes its own counter as `:var`.

## Words & lists

| Command | Args | Description |
|---|---|---|
| `FIRST` | `thing` (operator) | First top-level element (list) / character (word) |
| `BUTFIRST` | `thing` (operator) | Everything after the first element/character |
| `LAST` | `thing` (operator) | Last top-level element/character |
| `BUTLAST` | `thing` (operator) | Everything except the last element/character |
| `COUNT` | `thing` (operator) | Number of top-level elements/characters |
| `ITEM` | `index thing` (operator) | Element/character at position `index` (1-indexed) |
| `PICK` | `thing` (operator) | A random element/character/array slot |
| `WORD` | `a b` (operator) | Concatenate two words, no space |
| `SENTENCE` | `a b` (operator, alias `SE`) | Splice two values into one flat list |
| `LIST` | `a b` (operator) | Wrap two values as a 2-element list |
| `FPUT` | `thing list` (operator) | Prepend `thing` as a new first element |
| `LPUT` | `thing list` (operator) | Append `thing` as a new last element |
| `FLATTEN` | `thing` (operator) | Collapse every level of nesting into one flat list |
| `PARSE` | `thing` (operator) | Tokenize printed text by whitespace into a list of words |
| `SUBST` | `old new thing` (operator) | Replace every element equal to `old` with `new` |
| `ASCII` | `word` (operator) | A single character's code point (0-255, plain C `char`, not Unicode) |
| `CHAR` | `n` (operator) | The inverse of `ASCII` — code point to single-character word |
| `UPPERCASE` `LOWERCASE` | `word` (operator) | Case-convert every character |
| `RANGE` | `from to` (operator) | List of integers from `from` to `to` inclusive, counting by ±1 |
| `SPACEDRANGE` | `from to count` (operator) | `count` equally-spaced numbers from `from` to `to` inclusive |

```
MAKE "colors [red green blue]
PRINT FIRST :colors       -> red
PRINT BUTFIRST :colors    -> green blue
PRINT ITEM 2 :colors      -> green

PRINT WORD "hello "world  -> helloworld
PRINT SENTENCE "a "b      -> a b
PRINT SE [1 2] [3 4]      -> 1 2 3 4
PRINT LIST [1 2] [3 4]    -> [1 2] [3 4]
PRINT FPUT "red [green blue] -> red green blue

PRINT FLATTEN [1 [2 3] [4 [5 6]] 7]  -> 1 2 3 4 5 6 7
PRINT PARSE "hello                    -> hello
PRINT SUBST "b "x [a b c b]           -> a x c x

PRINT ASCII "A          -> 65
PRINT CHAR 65            -> A
PRINT UPPERCASE "shout    -> SHOUT
PRINT RANGE 1 5                       -> 1 2 3 4 5
PRINT SPACEDRANGE 0 10 3              -> 0 5 10
```

A word is a sequence of characters, so `FIRST`/`BUTFIRST`/`LAST`/
`BUTLAST`/`COUNT`/`ITEM`/`PICK` all work one level deeper on a word
than on a list (substrings, no separate syntax needed) — see
`docs/LANGUAGE.md`'s "List operators (and substrings)" section for the
full detail. `'raw text with spaces'` (single-quoted) is the one way
to write a literal-whitespace word directly in source.

## Type & membership predicates

| Command | Args | Description |
|---|---|---|
| `WORD?` | `expr` (operator) | Is this a word? |
| `LIST?` | `expr` (operator) | Is this a list? |
| `NUMBER?` | `expr` (operator) | Is this a number? |
| `ARRAY?` | `expr` (operator) | Is this an array? |
| `EMPTY?` | `expr` (operator) | Is this list/word empty? |
| `MEMBER?` | `thing container` (operator) | Is `thing` an element (list) / substring (word) of `container`? |

```
PRINT WORD? "hi            -> TRUE
PRINT EMPTY? []             -> TRUE
PRINT MEMBER? "b [a b c]     -> TRUE
PRINT MEMBER? "ell "hello    -> TRUE
IF WORD? "hi [PRINT "yes]
```

All six return `TRUE`/`FALSE` (real, printable words — see
`docs/LANGUAGE.md`'s "TRUE, FALSE" section), usable directly in
`IF`/`WHILE`.

## Higher-order iteration

| Command | Args | Description |
|---|---|---|
| `MAP` | `[template] list` (operator) | New list from evaluating `template` once per element |
| `FILTER` | `[template] list` (operator) | Elements where `template` (as a condition) is true |
| `REDUCE` | `[template] list` (operator) | Fold left to right; template uses `?1`/`?2` |
| `FOREACH` | `[template] list` | Run `template` once per element for side effects |

```
PRINT MAP [? * 2] [1 2 3]         -> 2 4 6
PRINT FILTER [? > 2] [1 2 3 4]    -> 3 4
PRINT REDUCE [?1 + ?2] [1 2 3 4]  -> 10
FOREACH [PRINT ?] [1 2 3]
```

`?` stands for the current element inside the bracketed template
(`?1`/`?2` for `REDUCE`'s accumulator/element pair). Only the
bracketed-template form exists — no procedure-name template.

## Arrays

| Command | Args | Description |
|---|---|---|
| `ARRAY` | `size` (operator) | New array of `size` slots, each defaulting to `[]` |
| `ITEM` | `index array` (operator) | Read a slot (1-indexed) — shared with lists/words above |
| `SETITEM` | `index array value` | Overwrite one slot in place |
| `FILLARRAY` | `array value` | Overwrite every slot with the same value |

```
MAKE "a ARRAY 3
SETITEM 1 :a "red
SETITEM 2 :a "green
SETITEM 3 :a "blue
PRINT :a               -> {red green blue}
PRINT ITEM 2 :a        -> green
```

**Arrays are the one mutable, reference-like value in this
language** — `MAKE "b :a` shares the same underlying slots, so
`SETITEM` through either is visible from both. Always prints inside
`{ }`. See `docs/LANGUAGE.md`'s "Arrays" section for what operators
don't support them.

## Vector operators

| Command | Args | Description |
|---|---|---|
| `DOT` | `a b` (operator) | Dot product of two same-length numeric lists |
| `CROSS` | `a b` (operator) | 3D cross product of two 3-element numeric lists |

```
PRINT DOT [1 2 3] [4 5 6]     -> 32
PRINT CROSS [1 0 0] [0 1 0]   -> 0 0 1
```

## Property lists

| Command | Args | Description |
|---|---|---|
| `SETPROP` | `plistname propname value` | Store a value under a key |
| `GETPROP` | `plistname propname` (operator) | Retrieve it, or `[]` if unset |
| `REMOVEPROP` | `plistname propname` | Delete an entry |
| `PROPLIST` | `plistname` (operator) | Whole record as a flat `[name value name value ...]` list |

```
SETPROP "turtle1 "color "red
SETPROP "turtle1 "speed 5
PRINT GETPROP "turtle1 "color   -> red
PRINT PROPLIST "turtle1          -> color red speed 5
REMOVEPROP "turtle1 "color
```

A separate namespace from `MAKE`/`:name` variables, shared globally —
no `LOCAL`-style scoping for properties.

## Prototype-style objects

| Command | Args | Description |
|---|---|---|
| `NEW` | `objname prototypename` | Set `objname`'s `"prototype` property |
| `SEND` | `obj "message arglist` | Call a method, walking the prototype chain |

```
TO animal_speak :self
  PRINT SENTENCE :self GETPROP :self "sound
END
NEW "animal "nothing
SETPROP "animal "speak "animal_speak
SETPROP "animal "sound "generic
NEW "dog "animal
SETPROP "dog "sound "Woof
SEND "dog "speak []    -> dog Woof
```

An "object" is just a property list with a `"prototype` link — no
classes, no new value type. `SEND`'s own arglist is required (`[]` for
zero args) — see `docs/LANGUAGE.md`'s "Prototype-style objects"
section for why `SEND`'s own syntax deliberately differs from real
Logo's positional form, and the full method-resolution/error rules.

## Error handling

| Command | Args | Description |
|---|---|---|
| `CATCH` | `"tag [block]` | Run `block`; catch a matching `THROW` from anywhere inside it |
| `THROW` | `expr` | Unwind to the nearest enclosing `CATCH` with a matching tag |

```
CATCH "err [
  PRINT "before
  THROW "err
  PRINT "unreachable
]
PRINT "after            ; prints: before / after (THROW skips "unreachable)
```

A `THROW` with no matching `CATCH` prints an error and execution
resumes with the next top-level command — not a crash.

## Deferred execution

| Command | Args | Description |
|---|---|---|
| `RUN` | `thing` | Execute a stored word/list as Logo source |
| `APPLY` | `"name arglist` | Call procedure `name` with arguments taken from `arglist` |

```
RUN [FD 100 RT 90]
MAKE "prog [FD 50 RT 90]
RUN :prog

TO add2 :a :b
  PRINT :a + :b
END
APPLY "add2 [3 4]        -> 7
```

Both are commands, not operators — no `PRINT RUN [...]`. A
self-referential `RUN` is capped and reported rather than crashing.

## Files

| Command | Args | Description |
|---|---|---|
| `LOAD` | `"path` | Read a file and run its contents as Logo source |
| `SAVE` | `"path` | Write every currently-defined procedure out as `TO ... END` source |
| `OPENREAD` | `"path` (operator) | Open a file for reading, output a channel number |
| `OPENWRITE` | `"path` (operator) | Open a fresh write channel |
| `OPENAPPEND` | `"path` (operator) | `OPENWRITE`, preloaded with the file's existing content |
| `READLINE` | `channel` (operator) | Next line from an open read channel |
| `READWORD` | `channel` (operator) | Next whitespace-delimited word from an open read channel |
| `READCHAR` | `channel` (operator) | Next single raw character (whitespace included) from an open read channel |
| `EOF?` | `channel` (operator) | Has a read channel run out of lines? |
| `CLOSE` | `channel` | Close a channel — for a write channel, this is when it's saved |
| `FILEPRINT` | `channel thing` | Append `thing` (rendered like `PRINT`) plus a newline |
| `DELETEFILE` | `"path` | Remove a file from disk |
| `DIRECTORY` | — (operator) | Every file/subdirectory name in the current directory |
| `SAVEBYTECODE` | `"path` | Write the running program's own compiled bytecode to disk (VM only) |
| `LOADBYTECODE` | `"path` | Read a file written by `SAVEBYTECODE` and run it standalone (VM only) |

```
MAKE "ch OPENWRITE "scratch.txt
FILEPRINT :ch SENTENCE "first "line
CLOSE :ch

MAKE "rd OPENREAD "scratch.txt
WHILE NOT EOF? :rd [PRINT READLINE :rd]
CLOSE :rd

PRINT MEMBER? "scratch.txt DIRECTORY   -> TRUE
DELETEFILE "scratch.txt
```

`READWORD`/`READCHAR` are finer-grained than `READLINE`, sharing its
own channel argument and EOF/bad-channel sentinel (an empty word,
checkable via `EOF?` separately). `READWORD` skips any leading
whitespace (space/tab/CR/LF), then reads up to the next whitespace or
EOF, leaving that trailing whitespace for the *next* call to skip.
`READCHAR` is the more primitive of the two: the single next raw byte,
whitespace included, no skipping at all.

```
MAKE "ch OPENREAD "scratch.txt
PRINT READWORD :ch    -> first (assuming scratch.txt starts "first line")
PRINT READWORD :ch    -> line
TYPE READCHAR :ch     -> the next single character, whatever it is
CLOSE :ch
```

`SAVEBYTECODE`/`LOADBYTECODE` (`docs/CHANGELOG.md`'s "Bytecode save/
load/assembler" Stage D) save/reload a whole COMPILED program, not
Logo source — `SAVEBYTECODE "path` writes the currently-running
program's own bytecode (see `docs/BYTECODE_REFERENCE.md`'s text
format), and `LOADBYTECODE "path` reads it back and runs it, needing
nothing else — not even the original `.logo` file — to work correctly:

```
TO SQUARE :SIDE
REPEAT 4 [FD :SIDE RT 90]
END
PRINT "drawn
SAVEBYTECODE "square.lgb
```
then, in a later, completely separate run:
```
LOADBYTECODE "square.lgb   -> prints "drawn" and draws the square again
```

Unlike `LOAD`, a procedure defined inside a `LOADBYTECODE`d file is
**not** callable from the rest of the script that called `LOADBYTECODE`
— `LOAD`'s own procedures are visible to the outer script only because
the parser eagerly re-parses and hoists the loaded file's `TO...END`
blocks at compile time; a `.lgb` file isn't Logo source at all, so
there's nothing to hoist. `LOADBYTECODE` just runs the whole saved
program, top to bottom, as its own self-contained unit — the useful
pattern is a program that does its own real work at the top level
(as `SQUARE` above is called from, not left for the caller to invoke).
VM-only: there's no bytecode chunk for the tree-walking engines
(`eval_logo`/`ast_eval`, unreachable from `bin/logo` and used only for
this project's own internal shadow-diff testing) to save at all.

See `examples/bytecode_save.logo` and `examples/bytecode_load.logo`
for a full working demo (run the first, then the second, as two
separate `bin/logo` invocations — `SAVEBYTECODE` must be a program's
own last statement, for the same reason described above) plus
`examples/hand_assembled.lgb`, a bytecode file written by hand instead
of produced by `SAVEBYTECODE`, loaded by `bytecode_load.logo`'s own
last line.

**File menu equivalents** (native macOS menu bar, `⌘⇧S`/`⌘⇧O`): *File >
Save Bytecode…* compiles whatever's currently in the entry box and
saves the result, without running it — no side effects (no drawing, no
`PRINT` output), unlike the `SAVEBYTECODE` builtin itself, which only
ever runs mid-script and saves whatever chunk is already executing.
*File > Load Bytecode…* reads a chosen `.lgb` file and runs it exactly
like `LOADBYTECODE`, participating fully in this app's own suspend/
resume machinery (a loaded program can `WAIT`/`PAUSE`/etc. same as any
other).

`OPENWRITE`/`OPENAPPEND` don't touch disk until `CLOSE` flushes the
buffer — losing power or crashing first loses the data. **Known,
unfixed bug**: most word literals compile through `OP_PUSH_WORD`'s
unbounded `word_literals[]` table and aren't affected, but `MAKE`/
`LOCAL`/`ERASE`/`LOAD`'s own dedicated opcodes (`OP_SET_VAR`/
`OP_LOCAL`/`OP_ERASE`/`OP_LOAD`) still write their name/path argument
straight into `Instr.text[64]`, which silently truncates anything over
63 bytes at compile time — see the `INSTR_MAX_TEXT` entry in project
memory / `docs/CHANGELOG.md`.

## Turtle sprites

| Command | Args | Description |
|---|---|---|
| `LOADSPRITE` | `"name "path` | Load an image, register it under `name` (40x40) |
| `LOADSPRITESHEET` | `"name "path cols rows` | Load a grid of frames under `name` |
| `SETSPRITE` | `"name` | Assign a loaded shape to the current turtle (`"NONE` resets it) |
| `SETSPRITEFRAME` | `n` | Pick which grid cell is shown |
| `STAMPSPRITE` | — | Bake a permanent copy of the current shape onto the canvas |
| `ANIMATESPRITE` | `delay frames` | Play sprite-sheet frames in place, over real time |

```
LOADSPRITE "ant "ant.png
SETSPRITE "ant
REPEAT 12 [STAMPSPRITE FD 40 RT 30]
SETSPRITE "NONE

LOADSPRITESHEET "walker "walker.png 4 2
SETSPRITE "walker
ANIMATESPRITE 0.15 24
SETSPRITE "NONE
```

## Resizable canvas

| Command | Args | Description |
|---|---|---|
| `SETCANVASSIZE` | `width height` | Resize the canvas (50-4000 each), resets like `CLEAR` |
| `CANVASSIZE` | — (operator) | Current size as `[width height]` |

```
SETCANVASSIZE 800 400
PRINT CANVASSIZE
```

## Suspend/resume

These pause the running script — the window stays fully responsive
while waiting (a real GLib timer/event, not a blocking sleep).

| Command | Args | Description |
|---|---|---|
| `WAIT` | `expr` (seconds) | Pause before the next command |
| `WAITKEY` | — (operator) | Wait for a keypress, output its name |
| `INPUT` | — (operator) | Wait for a submitted line, output it as a word |
| `PAUSE` | — | Halt and wait for `CONTINUE`, letting other commands run meanwhile |
| `CONTINUE` | — (alias `CO`) | Resume the innermost active `PAUSE` |

```
TO drive
  MAKE "key WAITKEY
  WHILE MEMBER? :key [Up Down Left Right] [
    IF :key = "Up [FD 20]
    MAKE "key WAITKEY
  ]
END

PRINT 'What is your name?'
MAKE "name INPUT
PRINT SENTENCE "Hello :name
```

Only actually wait in the real app — a silent no-op in the headless
test driver. Nested `PAUSE`s stack; each `CONTINUE` resumes only the
innermost one.

## Concurrent agents

MultiLogo-style: multiple independently-scheduled turtle "processes,"
genuinely interleaved (not just `TELL` switching which turtle a single
sequential script steers). See `docs/CONCURRENT_AGENTS_DESIGN.md` for
the full design and mechanism — not yet in `docs/LANGUAGE.md` at all.

| Command | Args | Description |
|---|---|---|
| `LAUNCH` | `"procname arglist` | Spawn a fresh-turtle agent running `procname`, with `arglist` bound as its parameters (`[]` for none) |
| `AWAIT` | — | Block until every currently-launched agent has finished |
| `YIELD` | — | Hand control to other running agents, explicit only (no automatic per-loop yielding) |

```
TO polygon :sides :length :start_x :start_y :r :g :b
  SETPENCOLOR :r :g :b
  PENUP SETXY :start_x :start_y PENDOWN
  REPEAT :sides [FD :length RT (360 / :sides) YIELD]
END

LAUNCH "polygon [3 90 120 350 220 40 40]
LAUNCH "polygon [4 70 250 380 40 140 220]
AWAIT
PRINT "all-agents-done
```

`WAIT`/`WAITKEY`/`INPUT`/`PAUSE`/`ANIMATESPRITE` used *inside* an
agent are deliberately deferred (an explicit, reported error, not a
silent hang). `SETSPEED` has no visible slow-motion effect on agents
yet. See `examples/concurrent_agents.logo` for a full working demo.

## Clock

| Command | Args | Description |
|---|---|---|
| `TIME` | — (operator) | Current wall-clock time, `HH:MM:SS` |
| `DATE` | — (operator) | Current date, `YYYY-MM-DD` |
| `MILLISECONDS` | — (operator) | Milliseconds since the Unix epoch (1970-01-01) |

```
PRINT TIME    -> 14:32:07
PRINT DATE    -> 2026-08-11
PRINT MILLISECONDS -> 1.78649e+12
```

## Event triggers

Unlike `WAITKEY` above (which blocks the running script until the next
key), `ONKEY`/`ONCLICK`/`ONMOUSEMOVE`/`ONKEYUP`/`ONRELEASE` register a
procedure to run in the background whenever a key/click/pointer-motion/
key-release/button-release event happens, without pausing anything. See
`docs/ROADMAP.md`'s "Mouse/keyboard event triggers" for the original
design writeup.

| Command | Args | Description |
|---|---|---|
| `ONKEY` | `"procname` | Run `procname` on every keypress (entry box focused), called with the key's name as its one input (`:KEY`) — same name convention as `WAITKEY`'s own output |
| `OFFKEY` | — | Clear the current `ONKEY` handler |
| `ONKEYUP` | `"procname` | Same as `ONKEY`, but on key *release* rather than press |
| `OFFKEYUP` | — | Clear the current `ONKEYUP` handler |
| `ONCLICK` | `"procname` | Run `procname` on every mouse button press on the canvas, called with `:X :Y :BUTTON` (canvas-relative pixels, same coordinate space `SETXY`/`POS` use; button 1/2/3, GDK's own left/middle/right convention) |
| `OFFCLICK` | — | Clear the current `ONCLICK` handler |
| `ONRELEASE` | `"procname` | Same as `ONCLICK`, but on button *release* rather than press |
| `OFFRELEASE` | — | Clear the current `ONRELEASE` handler |
| `ONMOUSEMOVE` | `"procname` | Run `procname` on every pointer motion over the canvas, called with `:X :Y` |
| `OFFMOUSEMOVE` | — | Clear the current `ONMOUSEMOVE` handler |

```
TO KEYHANDLER :KEY
  IF :KEY = "space [PRINT "jump]
END
ONKEY "keyhandler

TO CLICKHANDLER :X :Y :BUTTON
  SETXY :X :Y
END
ONCLICK "clickhandler

TO MOUSEHANDLER :X :Y
  SETXY :X :Y
END
ONMOUSEMOVE "mousehandler

TO STOPHANDLER :KEY
  PRINT "released
END
ONKEYUP "stophandler
```

`procname` must take *exactly* one input for `ONKEY`/`ONKEYUP` (three
for `ONCLICK`/`ONRELEASE`, two for `ONMOUSEMOVE`) — a mismatch is a
reported error at registration time, and leaves any previously-
registered handler untouched rather than clearing it. A typo'd or
undefined `procname` is likewise reported immediately, not silently
ignored the first time it would have fired.

Each handler slot holds at most one procedure — registering a new one
replaces the old, same as `SETSPEED`. An event that fires while the
interpreter isn't idle (the main script or a previous handler
invocation is still running/suspended) is simply missed, not queued —
same "at most one script thread" rule every ordinary REPL submission
already follows; this is also what keeps `ONMOUSEMOVE`'s own much
higher firing rate (dozens of events/sec while the mouse moves) from
piling up invocations faster than they finish, with no separate
debounce logic needed. `ONKEY`/`ONKEYUP` only see keys while the entry
box has focus (the app's only keyboard-capture point); a handler
registered inside a script that also uses `LAUNCH` is not retained once
that script finishes (a known, narrow gap — register these from a
script that doesn't also `LAUNCH`, if both are needed).

---

## Appendix: documented elsewhere, not available in `bin/logo`

`docs/LANGUAGE.md` documents these commands in prose, but a systematic
audit against `src/parser.c`'s own grammar table (confirmed directly —
each one produces `unknown word: X` when parsed, not guessed from
reading) found none of them are actually reachable from the live app.
All are implemented only in the old, pre-Stage-2 text-based
`eval_logo` engine (`src/interpreter.c`), which only
`tests/test_interpreter.c` still exercises — `bin/logo` has run
entirely on the bytecode VM since Stage 2 finished, and none of these
were ever ported to the VM's own grammar (`src/parser.c`'s
`BUILTIN_SIGNATURES`).

| Command | Would-be category |
|---|---|
| `TYPE` | Output (like `PRINT`, no trailing newline) |
| `PR` | `PRINT` alias |
| `CLEARTEXT` / `CT` | Clear the history pane |
| `BACKTRACE` / `BT` | Debugger: print the current call stack |
| `EXECTIME` | Debugger: time how long a `RUN`-like call takes |
| `LOADPIC` | Background image |
| `SAVEPIC` | Export canvas as PNG |
| `MOUSEPOS` / `MOUSEX` / `MOUSEY` / `BUTTON?` | Mouse state |
| `JOYSTICK?` / `JOYSTICKAXIS` / `JOYSTICKBUTTON?` | Joystick/game-controller input |
| `TONE` / `PLAYSOUND` / `STOPSOUND` | Sound |

This is a real, previously-undocumented gap between what
`docs/LANGUAGE.md` claims ("the Logo dialect actually implemented in
`src/main.c`") and what the live app can actually run — worth its own
scoping pass if any of these are wanted in `bin/logo`, likely following
the same `eval_X_value`-core-extraction pattern the 35-name VM
instruction-coverage audit already used for every other builtin.
