# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. Update this alongside `LANGUAGE.md` as items land — move a
finished item's description into the language reference and delete it here
rather than marking it "done" in place.

## Language

- [ ] Building strings/lists at runtime: concatenation, joining two
  variables, `FPUT`/`LPUT`/`LIST`-style construction. `MAKE "name [some
  words]`/`"word` for literals and `FIRST`/`BUTFIRST`/`LAST`/`COUNT` for
  reading a list apart both landed (see "Words & lists" in
  `LANGUAGE.md`) — what's still missing is building one up from pieces
  rather than writing it whole as a literal.
- [ ] True nested lists (a list containing another list as an element,
  not just flat text) and list operators working inside plain arithmetic
  (`FD FIRST :colors`) — the current implementation represents a list as
  the same flat, single-space-joined word/text value as everything else,
  which is why it composes with `PRINT`/`MAKE`/`=` for free but doesn't
  nest or reach into numeric contexts. Doing either properly is the
  original "bigger change" this item used to describe: a real tagged
  value type threaded through the expression evaluator (currently pure
  `double`), not just the word-aware layer (`PRINT`/`MAKE`/comparisons).

## Robustness

- [ ] Grow `tests/test_interpreter.c`'s coverage as new language features
  land (it currently covers turtle motion, procedures/scoping, `IF`/
  `WHILE`/booleans, words, and the error-message paths — see `make test`).
