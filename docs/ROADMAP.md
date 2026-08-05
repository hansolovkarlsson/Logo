# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. Update this alongside `LANGUAGE.md` as items land — move a
finished item's description into the language reference and delete it here
rather than marking it "done" in place.

## Language

- [ ] String operations: concatenation, substrings, character access.
  `MAKE "name [some words]` now holds multi-word text (see "Words" in
  `LANGUAGE.md`), but there's still no way to build a string out of
  pieces at runtime (e.g. joining two variables) or take one apart.
- [ ] Lists as a first-class, storable, manipulable value (`MAKE "mylist
  [1 2 3]`, `FIRST`, `BUTFIRST`, `LAST`, `COUNT`, ...) — a bigger change,
  since it means the expression evaluator (currently pure `double`)
  needs a real tagged value type threaded through arithmetic itself, not
  just variables/`PRINT`/`=` the way words are handled now.

## Robustness

- [ ] Replace the fixed-size C buffers (`token[64]`, procedure `body[2048]`,
  block bodies `[1024]`) with something that can't silently truncate long
  input.
- [ ] Grow `tests/test_interpreter.c`'s coverage as new language features
  land (it currently covers turtle motion, procedures/scoping, `IF`/
  `WHILE`/booleans, words, and the error-message paths — see `make test`).
