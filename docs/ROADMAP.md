# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. Update this alongside `LANGUAGE.md` as items land — move a
finished item's description into the language reference and delete it here
rather than marking it "done" in place.

## Language

- [ ] True nested lists (a list containing another list as an element,
  not just flat text). The expression evaluator now threads a real
  tagged Value (number-or-word) through arithmetic as well as
  `PRINT`/`MAKE`/comparisons — see "Words & lists" in `LANGUAGE.md` — so
  words and list operators already work everywhere an argument is
  expected (`FD FIRST :colors`, `REPEAT COUNT :items [...]`, etc.).
  What's still missing is a genuinely recursive value representation: a
  list is still always flat, single-space-joined text, which is why
  there's no nesting and `SENTENCE`/`LIST` are the same operation. That
  representation change doesn't fit the current fixed-buffer, no-malloc
  style — it needs either a tree of fixed-size nodes from a pool or
  dynamic allocation somewhere, plus new parsing/printing for nested
  `[...]` and updates to every list operator to recurse.

## Robustness

- [ ] Grow `tests/test_interpreter.c`'s coverage as new language features
  land (it currently covers turtle motion, procedures/scoping, `IF`/
  `WHILE`/booleans, words, and the error-message paths — see `make test`).
