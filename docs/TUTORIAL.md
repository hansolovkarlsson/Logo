# Learning LogoMotive

A from-scratch tutorial for LogoMotive, this project's own Logo
dialect — for someone who has never programmed before. If you already
know how to program and just want the syntax, read
`docs/COMMAND_REFERENCE.md` instead; this document teaches ideas in
order, that one is a lookup table.

Every example on this page has been run against the real interpreter
before being written down — if you type one in and it doesn't do what's
described, that's a bug worth reporting, not a typo in your
understanding.

## How to run these examples

Save a chapter's example into a file, say `try.logo`, and run:

```sh
bin/logomotive try.logo
```

A window opens with the turtle's canvas on one side and a history pane
on the other. You can also just type commands straight into the entry
box at the bottom of that window instead of saving a file first —
everything below works either way.

## Contents

1. [Turtle basics](#1-turtle-basics)
2. [Repetition](#2-repetition)
3. [Procedures](#3-procedures)
4. [Variables & data](#4-variables--data)
5. [Conditionals](#5-conditionals)
6. [Recursion](#6-recursion)
7. [Lists in depth](#7-lists-in-depth)
8. [Color, pen, and multiple turtles](#8-color-pen-and-multiple-turtles)
9. [Input & interactivity](#9-input--interactivity)
10. [Capstone: a flower garden](#10-capstone-a-flower-garden)

---

## 1. Turtle basics

Picture a turtle standing at the center of the canvas, facing up. It
holds a pen. Tell it to move, and it drags the pen behind it — that
trail is your drawing.

```
FORWARD 100
RIGHT 90
FORWARD 100
```

`FORWARD` (or `FD` for short) moves the turtle that many steps in the
direction it's currently facing. `RIGHT` (`RT`) turns it clockwise by
that many degrees, without moving it. `LEFT`/`LT` turns the other way,
and `BACK`/`BK` moves backward.

Run those three lines and you'll get two sides of a square. Add two
more commands and you'd have all four — but typing the same pair of
commands four times is exactly the kind of repetition a computer
should be doing for you, which is the whole subject of the next
chapter.

Two more basics worth knowing now:

```
PENUP
FORWARD 50    ; moves, but draws nothing
PENDOWN
FORWARD 50    ; draws again
```

`PENUP` lifts the pen so the turtle can reposition without leaving a
line; `PENDOWN` puts it back down. And `CLEAR` (`CS`) wipes the canvas
and puts the turtle back at the center, facing up, if you want a fresh
start.

**Try it:**
- Draw a triangle (three `FORWARD`/`RIGHT 120` pairs).
- Draw a shape with a gap in one side, using `PENUP`/`PENDOWN`.

---

## 2. Repetition

`REPEAT` runs a block of commands a fixed number of times. The block
goes in square brackets:

```
REPEAT 4 [FORWARD 100 RIGHT 90]
```

That one line draws the whole square from Chapter 1. Squares are
4-sided with a 90-degree turn; the general shape is "however many
sides, turn `360 / sides` degrees each time":

```
REPEAT 6 [FORWARD 60 RIGHT 360 / 6]     ; a hexagon
REPEAT 100 [FORWARD 2 RIGHT 3.6]        ; enough tiny sides to look like a circle
```

Inside a `REPEAT`, the operator `REPCOUNT` tells you which pass you're
currently on, counting from 1:

```
REPEAT 5 [PRINT REPCOUNT]
```

```
1
2
3
4
5
```

That's useful for shapes that change slightly each time around — a
spiral, say, where each side is a little longer than the last:

```
REPEAT 20 [FORWARD REPCOUNT * 5 RIGHT 90]
```

There's also `WHILE`, for when you don't know the count in advance —
it re-checks its condition before every pass and stops as soon as the
condition goes false:

```
MAKE "steps 0
WHILE :steps < 4 [
  FORWARD 100
  RIGHT 90
  MAKE "steps :steps + 1
]
```

(`MAKE` and `:steps` — variables — are Chapter 4's subject; you can
read this as "keep a counter, stop at 4" for now.) And `FOR` bundles
the counter into the loop itself:

```
FOR [i 1 5] [PRINT :i]
```

```
1
2
3
4
5
```

**Try it:**
- Draw a five-pointed star with one `REPEAT` (hint: the turn angle for
  a star isn't `360 / sides`).
- Use `REPCOUNT` to draw a spiral of squares, each one bigger than the
  last.

---

## 3. Procedures

Once a shape is worth drawing more than once, give it a name. `TO`
starts a definition, `END` closes it:

```
TO square
  REPEAT 4 [FORWARD 100 RIGHT 90]
END

square
square
```

Now `square` is a command, exactly like `FORWARD` or `REPEAT` — you
can use it anywhere, as many times as you like. But a square that's
always the same size isn't very useful. Give the procedure an input:

```
TO square :size
  REPEAT 4 [FORWARD :size RIGHT 90]
END

square 100
square 30
```

`:size` inside the procedure stands for whatever value was passed when
it was called. A procedure can take up to eight inputs:

```
TO rectangle :w :h
  REPEAT 2 [FORWARD :w RIGHT 90 FORWARD :h RIGHT 90]
END

rectangle 150 60
```

Procedures can also call each other — that's how you build anything
non-trivial: small named pieces, combined into bigger ones.

```
TO triangle :size
  REPEAT 3 [FORWARD :size RIGHT 120]
END

TO house :size
  square :size
  PENUP FORWARD :size PENDOWN
  triangle :size
END

house 80
```

**Try it:**
- Write a `polygon :sides :size` procedure that draws any regular
  polygon (square is the special case where `:sides` is 4).
- Write a procedure that draws a simple row of houses using a `REPEAT`
  around a call to `house`.

---

## 4. Variables & data

`MAKE` creates or updates a variable; `:name` reads it back:

```
MAKE "size 50
FORWARD :size
MAKE "size :size + 10
FORWARD :size
```

The quote before `size` in `MAKE "size ...` means "the word `size`
itself," not "go look up what `size` currently holds" — `MAKE` needs
to know *which* variable to set, so it takes the name as a plain word.
`:size` (colon, no quote) means the opposite: "the value currently
stored in `size`." Mixing these up is the single most common beginner
mistake in Logo — if a script errors complaining about an undefined
variable that you're sure you set, check whether you meant `:name` and
wrote `"name`, or vice versa.

Numbers work as you'd expect (`+ - * /`, with `*`/`/` binding tighter
than `+`/`-`, and parentheses for grouping):

```
PRINT (2 + 3) * 4      -> 20
PRINT SQRT 16           -> 4
PRINT ROUND 3.7          -> 4
```

Words and lists are the other two kinds of data. A word is text,
written with a leading quote (`"hello`); a list is a bracketed sequence
of things (`[1 2 3]`, or `[red green blue]`, or even a list of lists):

```
MAKE "greeting "hello
MAKE "colors [red green blue]
PRINT :greeting
PRINT :colors
```

```
hello
red green blue
```

Chapter 7 goes much further into lists — pulling them apart, building
new ones, mapping over them. For now, just knowing the three kinds of
data (numbers, words, lists) and the `MAKE`/`:name` pair is enough to
write real programs.

**Try it:**
- Store your name in a variable and `PRINT SENTENCE "Hello :yourname`.
- Write a procedure that takes a `:size` input, stores `:size * 2` into
  a local variable, and draws a square with that doubled size.

---

## 5. Conditionals

`IF` runs a block only when a condition is true:

```
MAKE "age 10
IF :age >= 13 [PRINT "teenager]
```

Nothing prints — `10 >= 13` is false. Add an `ELSE` for the other
branch, or use `IFELSE` for the same thing spelled differently:

```
IF :age >= 13 [PRINT "teenager] ELSE [PRINT "kid]
IFELSE :age >= 13 [PRINT "teenager] [PRINT "kid]
```

```
kid
kid
```

Comparisons are `< > = <= >= <>` (`<>` means "not equal"). Combine
conditions with `AND`/`OR`/`NOT` — no parentheses needed around them,
parentheses in this language are for arithmetic only:

```
IF :age > 0 AND :age < 13 [PRINT "kid]
IF NOT :age = 0 [PRINT "nonzero]
```

A condition doesn't need a comparison at all — any nonzero number or
non-empty word counts as true, and the special word `FALSE` counts as
false:

```
IF 1 [PRINT "always-runs]
IF WORD? "hi [PRINT "yes-a-word]
```

`WORD?` above is one of a family of predicates (`NUMBER?`, `LIST?`,
`EMPTY?`, `MEMBER?`, ...) that answer yes/no questions about a value —
handy directly inside an `IF`.

**Try it:**
- Write a procedure `classify :n` that prints `"negative`, `"zero`, or
  `"positive` depending on its input.
- Combine `IF` with `REPEAT`/`REPCOUNT` to draw a shape where every
  *other* side is a different length.

---

## 6. Recursion

A procedure can call itself. That feels circular at first, but it's
Logo's natural replacement for a loop when each step depends on
shrinking a problem down to a smaller version of itself:

```
TO countdown :n
  IF :n = 0 [PRINT "liftoff STOP]
  PRINT :n
  countdown :n - 1
END

countdown 5
```

```
5
4
3
2
1
liftoff
```

Every recursive procedure needs two things: a **base case** that stops
the recursion (here, `:n = 0`), and a step that moves *toward* that
base case (`:n - 1`). Miss either one and the procedure calls itself
forever, until the interpreter reports "Recursion too deep."

`STOP` ends a procedure with no value, for a procedure used as a
command (like `countdown` above). `OUTPUT` ends a procedure *with* a
value, for a procedure used as an operator — something you can `PRINT`
or use inside an expression:

```
TO factorial :n
  IF :n = 0 [OUTPUT 1]
  OUTPUT :n * factorial :n - 1
END

PRINT factorial 5    -> 120
```

`factorial 5` computes `5 * factorial 4`, which needs `factorial 4`,
which needs `factorial 3`, and so on down to `factorial 0`, which
`OUTPUT`s `1` directly — the base case — and then every pending
multiplication resolves on the way back up.

Recursion is also how you draw things whose complexity grows with a
parameter — a tree that branches, a spiral that shrinks each time:

```
TO spiral :size
  IF :size > 200 [STOP]
  FORWARD :size
  RIGHT 20
  spiral :size + 5
END

spiral 5
```

**Try it:**
- Write `sum_to :n`, an operator that outputs `1 + 2 + ... + n`
  recursively (base case: `sum_to 0` outputs `0`).
- Modify `spiral` to also change pen color each time it recurses (see
  Chapter 8 for `SETPENCOLOR`).

---

## 7. Lists in depth

A list's elements are pulled apart with `FIRST`/`BUTFIRST`/`LAST`/
`BUTLAST`, counted with `COUNT`, and indexed with `ITEM` (1-indexed,
not 0):

```
MAKE "fruits [apple banana cherry]
PRINT FIRST :fruits      -> apple
PRINT BUTFIRST :fruits   -> banana cherry
PRINT COUNT :fruits      -> 3
PRINT ITEM 2 :fruits     -> banana
```

New lists get built with `FPUT`/`LPUT` (add one element to the front
or back), `SENTENCE` (splice two lists — or words — into one flat
list), or `LIST` (wrap two things together without flattening):

```
PRINT FPUT "red [green blue]     -> red green blue
PRINT SENTENCE [1 2] [3 4]       -> 1 2 3 4
PRINT LIST [1 2] [3 4]           -> [1 2] [3 4]
```

Rather than writing your own loop to process every element, three
higher-order operators do the common patterns directly. Each takes a
bracketed **template** where `?` stands for "the current element":

```
PRINT MAP [WORD ? "!] [apple banana cherry]        -> apple! banana! cherry!
PRINT FILTER [NOT ? = "banana] [apple banana cherry] -> apple cherry
PRINT REDUCE [SENTENCE ?1 ?2] [1 2 3 4]              -> 1 2 3 4
```

`MAP` transforms every element into a new list; `FILTER` keeps only
the elements where the template is true; `REDUCE` folds the whole list
down to one value, with `?1` as the running result so far and `?2` as
the next element. `FOREACH` is the side-effect version — it doesn't
build a new list, it just runs the template once per element:

```
FOREACH [PRINT ?] [apple banana cherry]
```

```
apple
banana
cherry
```

A word is really just a list of characters underneath, so all of the
above (`FIRST`, `ITEM`, `COUNT`, ...) work one level deeper on a word
too — `FIRST "hello` is `h`, `COUNT "hello` is `5`.

**Try it:**
- Given `MAKE "nums [4 8 15 16 23 42]`, use `FILTER` to get only the
  even numbers (hint: `MOD ? 2 = 0`).
- Use `REDUCE` to find the largest number in a list (hint: the
  template needs an `IFELSE` comparing `?1` and `?2`).

---

## 8. Color, pen, and multiple turtles

`SETPENCOLOR` (or `SETPC`) takes red, green, blue, each `0`-`255`:

```
SETPENCOLOR 255 0 0
FORWARD 80
SETPENCOLOR 0 100 255
RIGHT 90 FORWARD 80
```

`SETPENWIDTH` changes how thick new lines are drawn; `SETBACKGROUND`
changes the canvas color. Each line remembers the color/width it was
drawn with — changing the pen color doesn't repaint anything already
on the canvas.

By default there's exactly one turtle. `TELL` switches which turtle is
"current" — commands like `FORWARD`/`RIGHT`/`SETPENCOLOR` always act on
whichever turtle is current — creating a fresh one the first time you
`TELL` a new number:

```
TELL 1
SETPENCOLOR 0 200 0
FORWARD 100

TELL 0
RIGHT 90
FORWARD 50

PRINT WHO       -> 0 (the current turtle's own number)
PRINT TURTLES   -> 2 (how many turtles exist so far)
```

Each turtle has its own independent position, heading, and pen state —
switching back to turtle `0` with `TELL 0` picks up exactly where it
left off.

**Try it:**
- Draw a small scene with two turtles in different colors, each
  drawing a different shape.
- Write a procedure that draws a rainbow — a `REPEAT` around a shape,
  changing `SETPENCOLOR` a little each pass using `REPCOUNT`.

---

## 9. Input & interactivity

`INPUT` pauses the script and waits for the user to type a line into
the entry box, handing it back as a word:

```
PRINT [What is your name?]
MAKE "name INPUT
PRINT SENTENCE "Hello :name
```

`WAITKEY` is similar but waits for a single keypress instead of a
whole line, and outputs the key's name (`"space`, `"Up`, `"a`, ...):

```
TO drive
  MAKE "key WAITKEY
  IF :key = "Up [FORWARD 20]
  IF :key = "Left [LEFT 15]
  IF :key = "Right [RIGHT 15]
  drive
END

drive
```

That `drive` procedure recurses forever (Chapter 6), each time
blocking on the next keypress — a tiny turtle-driving game in six
lines.

`WAITKEY`/`INPUT` both *block* — nothing else can happen while they
wait. For a background handler that fires without pausing anything
else, register a procedure with `ONKEY`/`ONCLICK`/`ONMOUSEMOVE`
instead:

```
TO keyhandler :key
  IF :key = "space [PRINT "jump]
END
ONKEY "keyhandler
```

Once registered, `keyhandler` runs automatically on every keypress,
for as long as the window stays open — the rest of your script keeps
running (or finishes) independently. `ONCLICK "procname` (three
inputs: `:x :y :button`) and `ONMOUSEMOVE "procname` (two inputs: `:x
:y`) work the same way for the mouse. This needs the real windowed app
to see in action — try it with `bin/logomotive`, not by reading alone.

**Try it:**
- Extend the `drive` procedure above to also print `"bye` and stop when
  the spacebar is pressed.
- Register an `ONCLICK` handler that moves the turtle to wherever you
  click (hint: `SETXY :x :y`).

---

## 10. Capstone: a flower garden

Everything above, combined: procedures, recursion, loops, color, and
randomness, to plant a little garden of flowers at random positions.

```
TO petal :size
  REPEAT 2 [ARC 60 :size RIGHT 60]
END

TO flower :size :petals
  REPEAT :petals [
    SETPENCOLOR (RANDOM 200) + 55 (RANDOM 200) + 55 (RANDOM 200) + 55
    petal :size
    RIGHT 360 / :petals
  ]
END

TO garden :count
  IF :count = 0 [STOP]
  PENUP
  SETXY (RANDOM 300) - 150 (RANDOM 300) - 150
  PENDOWN
  flower 40 8
  garden :count - 1
END

garden 6
PRINT "done-planting
```

Read it bottom-up if it helps: `garden` is the entry point — it's
recursive (Chapter 6), counting down from `:count` and planting one
flower per call. Each `flower` is a `REPEAT` (Chapter 2) around a
`petal`, choosing a new random color (Chapter 4's `MAKE`/expressions,
via `RANDOM`) each time round. Each `petal` is two arcs, forming one
curved leaf shape. `PENUP`/`SETXY`/`PENDOWN` (Chapters 1 and 8) jump to
a random spot between flowers without drawing a connecting line.

**Try it, in roughly increasing difficulty:**
- Change `garden` to plant more flowers, or bigger ones.
- Give `flower` a `:hue_shift` input and use it to bias the random
  colors (e.g. always keep red high, for a garden of red-ish flowers).
- Use `FOR` instead of the recursive `garden` to plant the flowers
  (Chapter 2's `FOR` gives you the counter directly, no `IF`/`STOP`
  base case needed).
- Register an `ONCLICK` handler (Chapter 9) that plants one new flower
  wherever you click, instead of planting them all up front.

---

From here, `docs/COMMAND_REFERENCE.md` has the complete list of every
command this interpreter supports — sprites, sound, file I/O, arrays,
property lists, and more — organized as a lookup table rather than a
story. `examples/` in this repo has many more full programs to read.
