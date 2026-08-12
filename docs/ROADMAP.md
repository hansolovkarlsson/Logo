# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. When an item ships, move its write-up into `docs/CHANGELOG.md`
(dated, with the full rationale/detail — see that file's own intro) instead
of marking it done in place here. Keeps this file trimmed to what's
genuinely still ahead; see `docs/CHANGELOG.md` for the full history of
everything that's already landed.

## Robustness

- [ ] Grow `tests/test_interpreter.c`'s coverage as new language features
  land (it currently covers turtle motion, procedures/scoping, `IF`/
  `WHILE`/booleans, words, and the error-message paths — see `make test`).

## Mouse/keyboard event triggers

`ONKEY`/`OFFKEY`/`ONCLICK`/`OFFCLICK` shipped 2026-08-11,
`ONMOUSEMOVE`/`OFFMOUSEMOVE` shortly after, and `ONKEYUP`/`OFFKEYUP`/
`ONRELEASE`/`OFFRELEASE` (the key/button *release* mirrors of `ONKEY`/
`ONCLICK`) after that (see `docs/CHANGELOG.md`'s own entries for the
full design and implementation writeup, and
`docs/COMMAND_REFERENCE.md`'s "Event triggers" section for day-to-day
usage). Only one item left, and it's the odd one out — a real new
dependency, not just another handler in the same shape as the rest:

- [ ] Joystick event triggers (`ONJOYBUTTON`/etc) — GTK4 has no built-in
  gamepad support on macOS, so this needs an extra dependency (e.g.
  GNOME's libmanette) or direct IOKit/HID access, a bigger lift than
  mouse/keyboard and not needed to ship those.
