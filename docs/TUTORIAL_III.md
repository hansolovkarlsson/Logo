# Learning LogoMotive III

The third pass — for someone who's been through Tutorials I and II and
wants what's left. This one is shaped differently than the first two:
instead of one coherent system per chapter (objects, agents), it's a
toolbox of independent techniques that round out real corners of the
language — precise coordinate navigation, canvas/pen polish, trig-driven
pattern drawing, a couple of math families, playback pacing, and
saving a compiled program to run standalone. Shorter than Tutorials I
and II for an honest reason: this really is what's left.

Same rule as always: every example on this page has been run against
the real interpreter before being written down.

## Contents

1. [Precise navigation](#1-precise-navigation)
2. [Canvas edges & pen polish](#2-canvas-edges--pen-polish)
3. [Trigonometry for pattern-drawing](#3-trigonometry-for-pattern-drawing)
4. [Logarithms & bitwise operators](#4-logarithms--bitwise-operators)
5. [Pacing & pausing](#5-pacing--pausing)
6. [Saving & loading compiled programs](#6-saving--loading-compiled-programs)
7. [Capstone: a small orrery](#7-capstone-a-small-orrery)

---

## 1. Precise navigation

Every turtle movement so far has been relative — turn, then move
forward, always from wherever the turtle already was facing. This
chapter is the other way of thinking about it: absolute coordinates.

```
SETXY 100 50
PRINT POS
PRINT GETX
PRINT GETY
SETHEADING 90
PRINT HEADING
```

```
100 50
100
50
90
```

`SETXY x y` jumps straight to a position, heading unchanged (drawing a
connecting line along the way if the pen is down, exactly like
`FORWARD`). `POS` reads the current position back as `[x y]`; `GETX`/
`GETY` read just one axis. `SETHEADING`/`HEADING` do the same thing for
facing direction — `SETHEADING` sets it directly (no turning motion,
just a new absolute angle), `HEADING` reads it back.

Two operators exist specifically to compute *how* to get somewhere:
`DISTANCE`, the straight-line distance between two points, and
`TOWARDS`, the heading that would face a given point. Combined, they're
a genuine "walk exactly there" recipe:

```
SETXY 0 0
SETHEADING TOWARDS [120 90]
FORWARD DISTANCE POS [120 90]
PRINT POS
```

```
120 90
```

`SETHEADING TOWARDS [120 90]` turns to face the target directly (no
guesswork, no trial-and-error turning); `FORWARD DISTANCE POS [120
90]` then walks exactly far enough — `DISTANCE` between the current
`POS` and the target — to land precisely on it, confirmed by the final
`PRINT POS`.

`ARC` rounds this chapter out — draw part or all of a circle centered
on the turtle's current position, without the turtle itself moving:

```
SETXY 0 0
ARC 360 80
```

`ARC 360 80` draws a full circle, radius 80. A smaller first argument
draws just that many degrees of arc instead of the whole circle — the
turtle's own heading determines where around the circle that arc
starts.

**Try it:**
- Write `goto :x :y`, a two-line procedure wrapping the
  `SETHEADING TOWARDS`/`FORWARD DISTANCE` pattern above into a single
  reusable call.
- Draw a five-pointed star by computing each point's absolute
  position with `SETXY` instead of the relative `REPEAT`/turn version
  from Tutorial I.

---

## 2. Canvas edges & pen polish

What happens when the turtle walks off the edge of the canvas is
configurable:

```
WRAP
SETXY 600 250
PRINT POS
```

```
100 250
```

`WRAP` (the default) makes crossing an edge reappear on the opposite
side — on the default 500-wide canvas, `x = 600` wraps around to
`x = 100`, confirmed by the `PRINT POS` above. `FENCE` is the other
extreme: crossing an edge stops the turtle right at the boundary, with
a reported error, rather than continuing anywhere. `WINDOW` removes
the boundary entirely — the turtle can wander arbitrarily far off the
visible canvas with no wrap and no error.

A few more drawing tools, beyond plain lines:

```
SETPENCOLOR 200 0 0
LABEL "hello
REPEAT 4 [FD 100 RT 90]
PENUP SETXY 30 30 PENDOWN
SETPENCOLOR 255 200 0
FILL
```

`LABEL "word` draws text at the turtle's current position, in the
current pen color — a caption, not a shape. `FILL` flood-fills the
region containing the turtle with the current pen color, stopping at
whatever lines already bound it (draw the square first, *then* move
inside it and `FILL`, as above — order matters, since `FILL` only sees
lines that already exist at the moment it's called). `ERASERECT width
height` erases a rectangle centered on the turtle back to the
background color, the undo tool for both of the above. `HIDETURTLE`/
`SHOWTURTLE` toggle whether the turtle marker itself is drawn — the
trail it leaves is unaffected either way.

Last one: the canvas size itself is just another value you can read
and change.

```
PRINT CANVASSIZE
SETCANVASSIZE 800 600
PRINT CANVASSIZE
```

```
500 500
800 600
```

**Try it:**
- Draw a shape, `FENCE` it, then try walking the turtle off the edge
  on purpose and see the reported error.
- Draw two overlapping circles (`ARC 360 radius`, Chapter 1) and
  `FILL` each with a different color from a point inside just that one.

---

## 3. Trigonometry for pattern-drawing

`SIN`/`COS`/`TAN` work in degrees here, not radians — no conversion
needed to match the rest of the language's angles:

```
PRINT SIN 90
PRINT COS 0
PRINT TAN 45
PRINT ARCTAN2 1 1
```

```
1
1
1
45
```

`ARCTAN2 y x` is the two-argument form — given a *direction* (`y`/`x`
components) rather than a ratio, it returns the correct full-circle
heading, the same computation `TOWARDS` (Chapter 1) does internally.

The real payoff is patterns that would be awkward to build any other
way. A point on a circle, computed directly instead of walked around
one turn at a time:

```
TO circle_point :radius :angle
  OUTPUT LIST (:radius * COS :angle) (:radius * SIN :angle)
END

PRINT circle_point 50 0
PRINT circle_point 50 90
```

```
50 0
3.06162e-15 50
```

That `3.06162e-15` instead of a clean `0` is worth pausing on, not
skipping past — `COS 90` is mathematically zero, but floating-point
arithmetic doesn't always land on *exactly* zero, so a value close
enough to be visually identical still isn't identical if you compare
it directly. Treat trig results as "close to," not "exactly," when a
program's own logic depends on the answer.

A sine wave, drawn by walking `x` forward steadily while `y` follows
`SIN`:

```
TO sine_wave :amplitude :cycles
  MAKE "x 0
  WHILE :x < 360 * :cycles [
    PENUP
    SETX :x
    SETY (SIN :x) * :amplitude
    PENDOWN
    MAKE "x :x + 5
  ]
END

sine_wave 40 2
```

Every `SIN :x` traces one point of the wave; stepping `:x` forward by
`5` each pass and `SETX`/`SETY`-ing there directly (Chapter 1) is what
turns a formula into an actual drawn curve.

**Try it:**
- Draw a full circle using `circle_point` and `REPEAT`/`REPCOUNT`
  instead of `ARC` — one `SETXY` per degree (or every few degrees) around
  the loop.
- Change `sine_wave` into a spiral by also growing `:amplitude` a
  little on every pass.

---

## 4. Logarithms & bitwise operators

Two families that don't come up daily, but are worth knowing exist —
this chapter is more reference than story.

```
PRINT LOG 100
PRINT LN 2.718281828
PRINT EXP 1
PRINT POWER 2 10
```

```
2
1
2.71828
1024
```

`LOG` is base-10, `LN` is natural log, `EXP` is `LN`'s inverse (`e` to
a power) — the same three-way relationship most languages offer.
`POWER` (from Tutorial I) rounds it out for plain exponentiation.

Bitwise operators treat a number as raw bits rather than a quantity:

```
PRINT BITAND 12 10
PRINT BITOR 12 10
PRINT BITXOR 12 10
PRINT LSHIFT 1 4
PRINT RSHIFT 16 4
```

```
8
14
6
16
1
```

The practical use is compact flag storage — several independent yes/no
settings packed into one number, each living at its own bit:

```
TO has_flag :flags :flag
  IF BITAND :flags :flag = :flag [OUTPUT "TRUE]
  OUTPUT "FALSE
END

MAKE "FLAG_RED 1
MAKE "FLAG_BOLD 2
MAKE "FLAG_ITALIC 4
MAKE "style BITOR :FLAG_RED :FLAG_BOLD

PRINT has_flag :style :FLAG_RED
PRINT has_flag :style :FLAG_ITALIC
```

```
TRUE
FALSE
```

`:style` packs "red" and "bold" together into one number via `BITOR`;
`has_flag` checks whether one specific bit is set in it via `BITAND`.

**A real gotcha, worth knowing before it costs you a debugging
session**: comparisons (`=`/`<`/`>`/etc.) and `AND`/`OR`/`NOT` only
work directly inside an `IF`/`IFELSE`/`WHILE` condition — not as a
general expression anywhere else. `PRINT 3 = 3` and `OUTPUT :a = :b`
both fail to parse; only the shape `IF <expr> <relop> <expr> [...]`
is valid. `has_flag` above is written the way it has to be — `IF
BITAND :flags :flag = :flag [OUTPUT "TRUE]` followed by a plain
`OUTPUT "FALSE` on the next line — specifically *because* a boolean
can't be built up and returned as a value the more obvious-looking
way. Also worth knowing: `(BITAND :flags :flag)` (with parens) doesn't
parse either, same "parens are arithmetic-only" rule from Tutorial I
— just leave them off, as above.

Two small operators worth knowing exist, out of scope for their own
chapter: `TIME`/`DATE`/`MILLISECONDS` read the real-world clock
(`PRINT TIME` → `14:32:07`), and `DOT`/`CROSS` are vector operators for
same-length numeric lists (`DOT [1 2 3] [4 5 6]` → `32`, a dot
product; `CROSS` needs exactly 3-element lists, a 3D cross product).

**Try it:**
- Extend the flags example with a `set_flag`/`clear_flag` pair
  (`BITOR` to set a bit, `BITAND`/`BITNOT` to clear one).
- Write `is_power_of_two :n` using only bitwise operators (hint:
  `BITAND :n (:n - 1)` is `0` exactly when `:n` is a power of two —
  the parens are fine here since `:n - 1` is arithmetic, but the `= 0`
  check on the result still needs to live inside an `IF`, the same
  restructuring `has_flag` above needed).

---

## 5. Pacing & pausing

`SETSPEED` (from Tutorial I, briefly) throttles every motion command
after it by a fixed delay — useful for watching a drawing unfold
instead of appearing all at once:

```
SETSPEED 0.1
PRINT SPEED
REPEAT 4 [FD 50 RT 90]
SETSPEED 0
PRINT SPEED
```

```
0.1
0
```

`WAIT seconds` is a one-off pause instead of an ongoing throttle —
useful between unrelated steps of a demo, where slowing down *every*
motion command isn't what you want:

```
PRINT "before
WAIT 1
PRINT "after
```

```
before
after
```

`PAUSE` is different again: it halts the script entirely and waits for
`CONTINUE`, from anywhere — a real breakpoint, not a timed delay:

```
PRINT "before
PAUSE
PRINT "after
```

```
before
Paused (level 1). Type CONTINUE to resume.
after
```

Nested `PAUSE`s stack, and each `CONTINUE` (alias `CO`) resumes only
the innermost one — useful for a program that pauses itself at more
than one depth (a paused sub-procedure, itself called from a paused
outer one) without a single `CONTINUE` accidentally resuming both at
once.

**Try it:**
- Write a short "slideshow" — a few drawings in a row, each one
  finishing with `PAUSE` so the next only appears once you type
  `CONTINUE`.
- Compare `SETSPEED 0.1` against a plain `WAIT 0.1` sprinkled between
  every motion command by hand — same visible effect, very different
  amount of typing.

---

## 6. Saving & loading compiled programs

Every builtin so far has existed in classic Logo too, one way or
another. This one doesn't — `SAVEBYTECODE`/`LOADBYTECODE` are specific
to LogoMotive's own compiler+VM design, and there's nothing quite like
them in the original language.

`SAVEBYTECODE "path` writes the *currently-running program's own
compiled bytecode* to disk — not the source text, the actual compiled
form:

```
TO SQUARE :SIDE
  REPEAT 4 [FD :SIDE RT 90]
END

PRINT "drawing-a-square
SQUARE 50
SAVEBYTECODE "square.lgb
```

```
drawing-a-square
Saved square.lgb
```

Later — a genuinely separate run, no relation to the script above
beyond the file it left behind — `LOADBYTECODE` reads that file back
and runs it directly:

```
LOADBYTECODE "square.lgb
```

```
drawing-a-square
Saved square.lgb
```

Notice the second run's output is identical to the first, including
`Saved square.lgb` printing *again* — because the saved program is the
*whole* thing, top to bottom, and `SAVEBYTECODE` was that program's own
last statement. `LOADBYTECODE` doesn't selectively re-run "just the
useful part"; it runs everything that was compiled in, which happens
to include a second save of the same file to itself. This is exactly
why `SAVEBYTECODE` has to be a program's own *last* statement — a
later statement would get baked into the saved copy too, right down to
a `LOADBYTECODE` of itself recursing forever, if you were unlucky
enough to write one.

The practical value: a `.lgb` file made this way needs nothing else to
run — not the original `.logo` source, not even a copy of it sitting
somewhere. `LOADBYTECODE` doesn't parse or compile anything; it just
runs an already-compiled program directly. That makes it the right
tool for handing someone (or some future run of your own) a finished
program without also handing them the source, or for a program whose
own last act is to leave behind a ready-to-run copy of itself.

**Try it:**
- Save two different procedures' worth of drawing to two separate
  `.lgb` files, then write a third script that `LOADBYTECODE`s both in
  sequence.
- Predict, then confirm, what happens if you `LOADBYTECODE` a file
  whose own last statement was itself a `LOADBYTECODE` of a *different*
  file — does the chain stop, or keep going?

---

## 7. Capstone: a small orrery

Coordinates and trig for the orbits, multiple turtles for the planets,
speed for the pacing, and a save at the end so the finished piece can
run again without this source file.

```
TO orbit :radius :angle :steps
  IF :steps = 0 [STOP]
  PENUP
  SETX :radius * COS :angle
  SETY :radius * SIN :angle
  PENDOWN
  DOTMARK
  orbit :radius (:angle + 6) (:steps - 1)
END

TO DOTMARK
  FD 1 BK 1
END

TO planet :n :radius :r :g :b
  TELL :n
  SETPENCOLOR :r :g :b
  SETSPEED 0
  orbit :radius 0 60
END

TELL 0
SETPENCOLOR 220 190 60
PENUP HOME PENDOWN
LABEL "sun

planet 1 60 90 160 220
planet 2 110 220 90 90

TELL 0
PRINT "orrery-complete
SAVEBYTECODE "orrery.lgb
```

```
orrery-complete
Saved orrery.lgb
```

Read it in the order it runs: turtle `0` sits at the center and labels
itself `"sun` (Chapter 2). `planet` (`TELL`, from Tutorial I) switches
to a *different* turtle per call and walks it through 60 steps of
`orbit` — each step computing a new absolute position with
`:radius * COS :angle`/`:radius * SIN :angle` (Chapter 3) and jumping
there directly, tracing a circle one small `DOTMARK` at a time. Two
calls to `planet`, two different radii and colors, two independent
orbits. The final `SAVEBYTECODE` (Chapter 6) means this whole
finished piece — sun, both orbits, everything — can be handed off and
re-run with `LOADBYTECODE "orrery.lgb` alone, no source required.

One naming trap worth knowing about directly, since it's easy to hit
by accident: the helper procedure above is called `DOTMARK`, not
`DOT` — `DOT` is already a builtin (the vector dot product, Chapter
4), and redefining it as a procedure produces a parse error rather
than quietly shadowing it.

**Try it, roughly increasing difficulty:**
- Add a third planet at yet another radius and speed.
- Give each planet a real `SETSPEED` (not `0`) so the orbits actually
  animate in real time instead of snapping straight to their finished
  shape.
- Combine this with Tutorial II's concurrent agents: `LAUNCH` each
  planet's own `orbit` walk instead of calling them one after another,
  so all the orbits actually grow in lockstep.

---

From here, `docs/COMMAND_REFERENCE.md` still has a handful of smaller
things this series never turned into full lessons — array/word type
predicates, procedure introspection (`NAMES`/`PROCEDURES`/`TEXT`/
`ERASE`), the exact `WRAP`/`FENCE` boundary rules in full — worth a
skim if something in your own program doesn't quite work the way you
expected.
