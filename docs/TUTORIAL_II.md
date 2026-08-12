# Learning LogoMotive II

The sequel to `docs/TUTORIAL.md` — for someone who's worked through
that one (or already knows the basics: procedures, variables, `IF`,
recursion, lists) and wants the rest of the language. Where Tutorial I
stayed close to classic Logo, everything here is either a deeper cut
of something it only introduced, or one of LogoMotive's own additions:
prototype-style objects, concurrent multi-turtle agents, real file I/O,
sprites, and background event handlers.

Same rule as last time: every example on this page has been run
against the real interpreter before being written down.

## Contents

1. [Deeper recursion](#1-deeper-recursion)
2. [Property lists](#2-property-lists)
3. [Prototype-style objects](#3-prototype-style-objects)
4. [Arrays](#4-arrays)
5. [Sprites & animation](#5-sprites--animation)
6. [File I/O](#6-file-io)
7. [Concurrent agents](#7-concurrent-agents)
8. [Event-driven programs](#8-event-driven-programs)
9. [Error handling & deferred execution](#9-error-handling--deferred-execution)
10. [Capstone: a little zoo](#10-capstone-a-little-zoo)

---

## 1. Deeper recursion

Tutorial I's recursion was all "count down to a base case." Three
patterns worth adding to that.

**Accumulators.** `factorial` from Tutorial I does its multiplying on
the way *back up* the call stack, after the recursive call returns.
An accumulator does the work on the way *down* instead, carrying the
running total as an extra parameter:

```
TO sum_acc :n :acc
  IF :n = 0 [OUTPUT :acc]
  OUTPUT sum_acc :n - 1 :acc + :n
END

PRINT sum_acc 100 0
```

```
5050
```

`sum_acc 100 0` doesn't wait for anything — each call either finishes
immediately (`:n = 0`) or hands the *whole* answer straight to the
next call, no multiplication left pending. This shape matters for very
long chains: `factorial`'s own style leaves one pending multiplication
per call waiting to happen on the way back up; an accumulator leaves
nothing waiting, so the chain never gets "deeper" in any way that
costs more per link.

**Mutual recursion.** Two procedures can call *each other* — neither
one is "the" recursive one:

```
TO is_even :n
  IF :n = 0 [OUTPUT "TRUE]
  OUTPUT is_odd :n - 1
END

TO is_odd :n
  IF :n = 0 [OUTPUT "FALSE]
  OUTPUT is_even :n - 1
END

PRINT is_even 10
PRINT is_odd 10
```

```
TRUE
FALSE
```

Every call to `is_even` hands off to `is_odd` and vice versa, each one
peeling off 1 and asking the *other* procedure to finish the job — the
recursion is spread across two names instead of living in just one.

**Tree recursion.** So far every recursive call has been the *last*
thing a procedure does. Nothing stops a procedure from recursing
*twice* in the same call, branching instead of chaining — which is
exactly how you draw a tree:

```
TO tree :size :depth
  IF :depth = 0 [STOP]
  FORWARD :size
  RIGHT 25
  tree (:size * 0.7) (:depth - 1)
  LEFT 50
  tree (:size * 0.7) (:depth - 1)
  RIGHT 25
  BACK :size
END

tree 60 4
```

Read it as: draw one branch's trunk, then recurse *twice* from its
tip — once angled right, once angled left — each recursive call a
smaller tree of its own (`:size * 0.7`, one less `:depth`). The
`BACK :size` at the end walks back down the trunk afterward, so the
turtle ends up exactly where it started, ready for whatever comes
next. `:depth` is what keeps this finite — each branch's two children
get `:depth - 1`, until `:depth` hits `0` and that branch just stops.

**Try it:**
- Change `tree`'s branch angle (the `25`s) and count how differently
  the same `:depth` looks.
- Write `fib_acc`, a fast accumulator-style Fibonacci (the plain
  recursive `fib :n = fib (:n-1) + fib (:n-2)` version redoes huge
  amounts of work — `fib_acc` should carry the last two values forward
  instead of recomputing them).

---

## 2. Property lists

A property list is a named record: a bag of `name`/`value` pairs, set
and read by that name.

```
SETPROP "turtle1 "color "red
SETPROP "turtle1 "speed 5
PRINT GETPROP "turtle1 "color
PRINT PROPLIST "turtle1
```

```
red
color red speed 5
```

`SETPROP` takes the plist's own name, a property name, and a value.
`GETPROP` reads one property back (an empty list if it was never set —
not an error). `PROPLIST` dumps the whole record as one flat list,
alternating names and values. `REMOVEPROP` deletes a single entry.

Property lists live in their own namespace, entirely separate from
`MAKE`/`:name` variables — `SETPROP "turtle1 ...` has nothing to do
with a variable named `turtle1`, and there's no `LOCAL`-style scoping
for them: every property list is visible everywhere, for as long as
the program runs.

That "just a bag of named values, visible everywhere" shape is
unglamorous on its own, but it's the entire foundation the next
chapter is built on.

**Try it:**
- Give three or four made-up "creatures" (just words — `"dragon`,
  `"unicorn`) each a `"legs` and `"color` property, then `FOREACH` over
  a list of their names printing each one's `PROPLIST`.

---

## 3. Prototype-style objects

An "object" in LogoMotive is nothing new — it's a property list with
one special property: `"prototype`, a link to another property list to
fall back on. `SEND` calls a "method" (really just a procedure name
stored as a property) by walking that link until it finds one:

```
TO animal_speak :self
  PRINT SENTENCE :self GETPROP :self "sound
END

NEW "animal "nothing
SETPROP "animal "speak "animal_speak
SETPROP "animal "sound "generic

NEW "dog "animal
SETPROP "dog "sound "Woof

SEND "dog "speak []
```

```
dog Woof
```

Walk through what just happened: `NEW "dog "animal` sets `dog`'s own
`"prototype` property to `animal` — that's *all* `NEW` does. `dog`
itself never got a `"speak` property, so when `SEND "dog "speak []`
looks for one, it doesn't find it on `dog` and walks the prototype
link to `animal`, finds `"speak` there (bound to the procedure name
`animal_speak`), and calls it with `dog` bound to `:self`. Inside that
call, `GETPROP :self "sound` reads `dog`'s *own* `"sound` — `"Woof`,
not `animal`'s generic one — because property lookups always start at
the object you actually sent the message to, not wherever the method
was found.

That's inheritance, in full: data can be overridden per-object (each
animal gets its own `"sound`) while behavior is shared (one
`"speak` implementation, found through the prototype chain). Add a
second animal and the same `"speak` still works, unmodified:

```
NEW "cat "animal
SETPROP "cat "sound "Meow
SEND "cat "speak []
```

```
cat Meow
```

`SEND`'s own arglist is always required — `[]` for a method that takes
no extra inputs beyond `:self`, which is bound automatically and isn't
part of the arglist you write.

**Try it:**
- Add a `"legs` property to `animal` (default `4`) and a `walk`
  method that prints `SENTENCE :self SENTENCE "walks-on GETPROP :self
  "legs` (no parens needed — chained prefix calls read fine without
  them). Give one animal (a bird, say) its own overriding `"legs 2`.
- Build a three-level chain — `NEW "puppy "dog` — and confirm `SEND
  "puppy "speak []` still finds `animal_speak` two links up.

---

## 4. Arrays

Lists in this language are immutable — `MAKE "b :a` and any operation
on `:b` afterward never changes `:a`. Arrays are the one exception:

```
MAKE "a ARRAY 3
SETITEM 1 :a "red
SETITEM 2 :a "green
SETITEM 3 :a "blue
PRINT :a
PRINT ITEM 2 :a
```

```
{red green blue}
green
```

`ARRAY 3` creates a 3-slot array (each slot starts as `[]`); `SETITEM`
overwrites one slot in place, 1-indexed like `ITEM` (which arrays share
with lists and words, from Tutorial I). Arrays always print inside
`{ }` rather than `[ ]`, so you can tell at a glance which kind of
thing you're looking at.

Here's the part that makes them genuinely different from a list:

```
MAKE "b :a
SETITEM 1 :b "PURPLE
PRINT :a
```

```
{PURPLE green blue}
```

`MAKE "b :a` didn't copy the array — `:a` and `:b` are two names for
the *same* underlying slots, so writing through `:b` is visible
through `:a` too. Reach for an array instead of a list specifically
when you want that: a shared, mutable structure that more than one
part of a program can update in place, like a scoreboard or a grid
several procedures all poke at.

`FILLARRAY` overwrites every slot at once, useful for resetting one
back to a known state:

```
FILLARRAY :a 0
PRINT :a
```

```
{0 0 0}
```

**Try it:**
- Build a 5-slot array, fill it with `RANDOM 100` in each slot via
  `REPEAT`/`REPCOUNT`, then use `REDUCE` (Tutorial I, Chapter 7) to
  find the largest value.
- Write `swap :arr :i :j`, swapping two slots of an array in place.

---

## 5. Sprites & animation

Everything so far draws with the plain triangular turtle. `LOADSPRITE`
swaps that for a real image:

```
LOADSPRITE "ant "ant.png

SETSPRITE "ant

REPEAT 12 [
  STAMPSPRITE
  FD 40
  RT 30
]

SETSPRITE "NONE
```

`LOADSPRITE "name "path` loads an image file (any format
`gdk-pixbuf` understands — PNG, JPEG, ...) and registers it under
`name`, scaled to a fixed size. `SETSPRITE "name` makes the *current*
turtle look like that image from then on, both live on the canvas and
in `STAMPSPRITE`, which bakes a permanent copy at the turtle's current
position/heading (the turtle itself keeps moving afterward —
`STAMPSPRITE` is a snapshot, not a follower). `SETSPRITE "NONE` goes
back to the default triangle.

For a sprite *sheet* — several frames in one image, laid out in a
grid — `LOADSPRITESHEET` takes the grid dimensions, and
`ANIMATESPRITE` plays through frames over real time:

```
LOADSPRITESHEET "walker "walker.png 4 2

SETSPRITE "walker

ANIMATESPRITE 0.15 24

SETSPRITE "NONE
```

`LOADSPRITESHEET "walker "walker.png 4 2` loads an 8-frame grid (4
columns, 2 rows). `ANIMATESPRITE 0.15 24` advances one frame every
0.15 seconds, 24 times over — three full loops through all 8 frames,
in place (the turtle doesn't move), each frame paced and redrawn
rather than snapping straight to the last one. `SETSPRITEFRAME n`
jumps to a specific frame directly, if you want a single pose instead
of the automatic cycle.

Sprites need the real app to see — `bin/logomotive`'s window does the
actual image decoding, so these examples won't show anything useful
run any other way.

**Try it:**
- Load `turtle.png` and stamp a ring of them around the canvas center
  (same `REPEAT`/`STAMPSPRITE` shape as the ant above).
- Combine `TELL` (Tutorial I, Chapter 8) with sprites: give two
  different turtles two different loaded shapes at once.

---

## 6. File I/O

Reading and writing real files on disk — channels, opened and closed
explicitly, same shape as most languages' file handles.

```
MAKE "ch OPENWRITE "scratch.txt
FILEPRINT :ch "first-line
FILEPRINT :ch "second-line
CLOSE :ch

MAKE "rd OPENREAD "scratch.txt
WHILE NOT EOF? :rd [PRINT READLINE :rd]
CLOSE :rd

DELETEFILE "scratch.txt
```

```
first-line
second-line
```

`OPENWRITE`/`OPENREAD` open a channel (a plain number, output as an
operator) and don't touch the *content* of what you write until
`CLOSE` flushes it — losing power or crashing before `CLOSE` loses
whatever wasn't flushed yet. `FILEPRINT :channel :thing` writes
`:thing`, rendered the same way `PRINT` would, plus a trailing newline.
`EOF?` checks whether a read channel has run out of lines, meant to be
checked *before* each `READLINE` (as above), not after. `OPENAPPEND` is
`OPENWRITE`, just preloaded with the file's existing content instead
of starting empty. `DELETEFILE`/`DIRECTORY` round it out — remove a
file, or list what's in the current directory.

`READWORD`/`READCHAR` are finer-grained than `READLINE` — same
channel, same EOF convention, but word- or single-character-at-a-time
instead of a whole line:

```
MAKE "ch OPENWRITE "scratch2.txt
FILEPRINT :ch "hello there
CLOSE :ch

MAKE "rd OPENREAD "scratch2.txt
PRINT READWORD :rd
PRINT READWORD :rd
CLOSE :rd
DELETEFILE "scratch2.txt
```

```
hello
there
```

`READWORD` skips leading whitespace, then reads to the next whitespace
or EOF. `READCHAR` is more primitive still: the single next raw byte,
whitespace included, no skipping at all.

**Try it:**
- Write a procedure that saves a list of high scores (one per line) to
  a file, then a second procedure that reads them back and `PRINT`s
  the highest one.
- Use `DIRECTORY` and `MEMBER?` (Tutorial I, Chapter 5) together to
  check whether a file exists before trying to `OPENREAD` it.

---

## 7. Concurrent agents

Every turtle so far has belonged to one sequential script — even with
multiple turtles (`TELL`, Tutorial I Chapter 8), only one command runs
at a time. `LAUNCH` is different: it spawns a genuinely independent,
separately-scheduled turtle "agent," running alongside whatever else
is going:

```
TO polygon :sides :length :start_x :start_y :r :g :b
  SETPENCOLOR :r :g :b
  PENUP
  SETXY :start_x :start_y
  PENDOWN
  REPEAT :sides [
    FD :length
    RT (360 / :sides)
    YIELD
  ]
END

LAUNCH "polygon [3 90 120 350 220 40 40]
LAUNCH "polygon [4 70 250 380 40 140 220]
LAUNCH "polygon [6 50 380 350 40 180 90]
AWAIT

PRINT "all-agents-done
```

`LAUNCH "procname arglist` spawns a fresh turtle running `procname`,
with `arglist`'s elements bound to its parameters exactly like an
ordinary call. `AWAIT` blocks until every currently-launched agent has
finished. In between, `YIELD` is what actually makes this concurrent
rather than sequential: without it, each `LAUNCH`ed agent would just
run start-to-finish before the next one got a turn — indistinguishable
from calling `polygon` three times in a row. With a `YIELD` inside the
loop, each agent does one step, then hands control to the *other*
agents, and all three polygons grow in lockstep instead of one at a
time. (The scheduler itself is synchronous and headless — nothing here
depends on real OS threads — so what actually proves the interleaving
happened is that all three shapes come out whole and correctly closed
despite each only taking one step at a time.)

A few real constraints worth knowing before reaching for this:
`WAIT`/`WAITKEY`/`INPUT`/`PAUSE`/`ANIMATESPRITE` used *inside* an agent
are deliberately refused (a reported error, not a silent hang) —
they're not supported there yet. `SETSPEED` has no visible slow-motion
effect on agents either. Neither is a bug so much as a documented edge
of what `LAUNCH` covers today.

**Try it:**
- Launch five agents instead of three, each drawing a differently-sized
  polygon, and `AWAIT` them all.
- Remove the `YIELD` from `polygon` and watch what changes about the
  order the shapes appear in (they'll still all get drawn — just not
  interleaved anymore).

---

## 8. Event-driven programs

Tutorial I's Chapter 9 registered one `ONKEY` handler as a single line.
Here's a fuller program — arrow keys drive the turtle, a click
teleports it there instead, and both work *at the same time*, without
either one blocking the other:

```
TO keyhandler :key
  PRINT SENTENCE "you-pressed: :key
  IF :key = "Up [FD 20]
  IF :key = "Down [BK 20]
  IF :key = "Left [LT 15]
  IF :key = "Right [RT 15]
END

TO clickhandler :x :y :button
  PENUP
  SETXY :x :y
  PENDOWN
  PRINT SENTENCE (SENTENCE "clicked: :x) :y
END

ONKEY "keyhandler
ONCLICK "clickhandler

PRINT [Arrow keys drive the turtle; click the canvas to jump there.]
```

Notice the script *ends* right after registering both handlers — there
is no loop waiting for events, because there doesn't need to be.
`ONKEY`/`ONCLICK` register `keyhandler`/`clickhandler` and then get out
of the way; the app itself calls them whenever a real key or click
happens, for as long as the window stays open, entirely independent of
whatever else the entry box is doing at that moment. That's the real
difference from `WAITKEY` (Tutorial I): `WAITKEY` is *you* asking the
program to pause and wait for one key; `ONKEY` is the program telling
the app "call me back whenever," and moving on immediately.

`clickhandler`'s inputs are worth noting: `ONCLICK`'s procedure always
takes exactly three (`:x :y :button`, canvas-relative pixels plus
which mouse button), `ONMOUSEMOVE`'s always takes two (`:x :y`, no
button), and `ONKEY`'s always takes one (`:key`) — a mismatched
parameter count is a reported error the moment you register the
handler, not a mystery later when it silently never fires.

Like sprites, this one needs the real windowed app to actually see —
try it with `bin/logomotive`, not by reading alone.

**Try it:**
- Add a running total: a variable that counts clicks, printed inside
  `clickhandler`.
- Register `ONMOUSEMOVE` too, drawing a continuous line that follows
  the pointer wherever it moves (hint: just `SETXY :x :y` with the pen
  already down).

---

## 9. Error handling & deferred execution

`CATCH`/`THROW` unwind the call stack on purpose, for handling an
error without crashing the whole program:

```
TO checked_input :x
  IF NOT NUMBER? :x [THROW "badinput]
  PRINT SENTENCE "got: :x
END

CATCH "badinput [
  checked_input 42
  checked_input "oops
  PRINT "never-gets-here
]
PRINT "recovered
```

```
got: 42
recovered
```

`checked_input 42` succeeds normally and prints. `checked_input "oops`
throws — `"oops` is a word, not a number, so `NUMBER? "oops` is false
— and that `THROW "badinput` unwinds everything back to the nearest
enclosing `CATCH "badinput [...]`, skipping both the rest of that
failed call *and* the rest of the block (`"never-gets-here` never
prints) and landing on whatever comes right after the `]`. A `THROW`
with no matching `CATCH` anywhere prints an error and moves on to the
next top-level command, rather than crashing — this is the *deliberate*
version of that same recovery.

**A genuine gotcha, worth knowing before it surprises you**: a quoted
word that merely *looks* like a number, `"42`, is still a `WORD`, not
a `NUMBER` — `NUMBER? "42` is `FALSE`. Only a bare, unquoted `42` (or
one computed by arithmetic) is actually a number. `checked_input`
above only accepts the bare form for exactly this reason.

Three more ways to run code you didn't write directly into the script:

```
RUN [PRINT "hello-from-run]

TO add2 :a :b
  PRINT :a + :b
END
APPLY "add2 [3 4]

MAKE "jobs [[2 * 3] [4 * 5] [6 * 7]]
PRINT EVAL :jobs
```

```
hello-from-run
7
6 20 42
```

`RUN` executes a stored list as if it had been typed directly —
useful for code built up at runtime (`RUN` a template someone else's
procedure handed you). `APPLY "name arglist` calls `name` with
`arglist`'s elements as its inputs, the same relationship `LAUNCH` has
to its own arglist. Both `RUN` and `APPLY` are commands, not
operators — there's no `PRINT RUN [...]`, only `RUN [PRINT ...]`
itself producing output.

`EVAL` is the odd one out: it *is* an operator. Each element of its
list argument is either a nested `LIST` (run as one piece of code,
same "list = deferred code" convention `RUN` uses) or a plain value
(passed through unchanged, since there's no code there to run), and
the result always comes back the same length as the input — one slot
per element:

```
PRINT EVAL [[3 * 4] 99 [SQRT 16]]
```

```
12 99 4
```

The first and third elements are lists, so they run as code
(`3 * 4`, `SQRT 16`); the middle `99` isn't a list, so it just passes
straight through. That makes `EVAL` the right tool specifically for a
list of computations assembled at runtime, when you don't know in
advance how many there'll be or which ones are "code" versus plain
data:

```
MAKE "jobs [[2 * 3] [4 * 5] [6 * 7]]
PRINT EVAL :jobs
```

```
6 20 42
```

**Try it:**
- Wrap `checked_input` in a loop over a list containing a mix of
  numbers and words, printing either the successful result or
  `"skipped` for each one, using `CATCH` around a single call at a
  time (not the whole loop) so one bad entry doesn't stop the rest.
- Read a line of raw text (`READLINE`, this chapter) containing a
  short Logo command, `PARSE` it into a runnable list (tokenizes text
  by whitespace — `PARSE 'FD 100 RT 90'` becomes `[FD 100 RT 90]`),
  and `RUN` it.

---

## 10. Capstone: a little zoo

Objects for identity, property lists for per-animal data, concurrent
agents so every animal wanders independently, all at once.

```
TO animal_speak :self
  PRINT SENTENCE :self GETPROP :self "sound
END

NEW "animal "nothing
SETPROP "animal "speak "animal_speak

TO make_critter :name :sound :r :g :b
  NEW :name "animal
  SETPROP :name "sound :sound
  SETPROP :name "r :r
  SETPROP :name "g :g
  SETPROP :name "b :b
END

TO wander :name :steps
  SETPENCOLOR GETPROP :name "r GETPROP :name "g GETPROP :name "b
  REPEAT :steps [
    FD 15
    RT (RANDOM 60) - 30
    YIELD
  ]
  SEND :name "speak []
END

make_critter "dog "Woof 200 80 60
make_critter "cat "Meow 90 160 90
make_critter "bird "Tweet 220 190 60

LAUNCH "wander [dog 12]
LAUNCH "wander [cat 12]
LAUNCH "wander [bird 12]
AWAIT

PRINT "zoo-quiet-now
```

Read it in the order it actually runs: `make_critter` builds three
prototype-based objects (Chapter 3), each with its own `"sound` and
color stored as properties (Chapter 2) on top of the shared `"speak`
behavior every `animal` gets for free. `LAUNCH`ing `wander` once per
critter (Chapter 7) sets all three loose *at the same time* — each one
takes a step, turns a small random amount, `YIELD`s to let the others
have a turn, and only calls `SEND :name "speak []` once its own walk
is finished. `AWAIT` waits for all three to actually finish before the
zoo goes quiet.

**Try it, roughly increasing difficulty:**
- Add a fourth critter with its own sound and color.
- Give `wander` a `PENUP`/random-teleport start instead of all three
  animals beginning at the same spot.
- Combine this with Chapter 5: `LOADSPRITE` each animal a real image
  and `SETSPRITE` it before wandering, instead of the plain triangle.
- Combine this with Chapter 8: register an `ONCLICK` handler that
  `LAUNCH`es one *new* critter, at the clicked position, every time you
  click the canvas.

---

From here, `docs/COMMAND_REFERENCE.md` has every command in one place,
and `docs/CONCURRENT_AGENTS_DESIGN.md`/`docs/BYTECODE_VM_DESIGN.md` go
deep on how `LAUNCH` and the VM itself actually work, if that's the
kind of thing you enjoy reading.
