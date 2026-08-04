# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. Update this alongside `LANGUAGE.md` as items land — move a
finished item's description into the language reference and delete it here
rather than marking it "done" in place.

## Language

- [ ] Boolean `AND`, `OR`, `NOT` in conditions.
- [ ] String/word variables and a real string type, distinct from numbers.
- [ ] Lists (`[1 2 3]` as a first-class value, not just a code block) plus
  basic list ops (`FIRST`, `BUTFIRST`, `LAST`, `COUNT`, ...).
- [ ] `PRINT` of a full quoted string (with spaces) and of lists, not just a
  single bare word.
- [ ] Surface parse/runtime errors to the user (e.g. "unknown command",
  unmatched brackets) instead of silently doing nothing.

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
