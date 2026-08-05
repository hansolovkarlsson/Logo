# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. Update this alongside `LANGUAGE.md` as items land — move a
finished item's description into the language reference and delete it here
rather than marking it "done" in place.

## Language

- [ ] A real multi-word string type (concatenation, substrings) distinct
  from the current single-token word — words/variables landed, but
  `MAKE "name` and `PRINT "word` still can't hold text with spaces in it.
- [ ] Lists as a first-class, storable, manipulable value (`MAKE "mylist
  [1 2 3]`, `FIRST`, `BUTFIRST`, `LAST`, `COUNT`, ...) — a bigger change,
  since it means the expression evaluator (currently pure `double`)
  needs a real tagged value type threaded through arithmetic itself, not
  just variables/`PRINT`/`=` the way words are handled now.

## Turtle & graphics

- [ ] `ARC`/circle drawing primitive.
- [ ] Multiple turtles / turtle identity (stretch — big change to the data
  model).

## Interface & workflow

- [ ] Syntax highlighting or at least bracket-matching feedback in the
  entry box.

## Robustness

- [ ] Replace the fixed-size C buffers (`token[64]`, procedure `body[2048]`,
  block bodies `[1024]`) with something that can't silently truncate long
  input.
- [ ] Basic automated tests for the interpreter core (`eval_logo`,
  expression parser) independent of the GTK UI, so language changes can be
  verified without manually clicking through the app.
