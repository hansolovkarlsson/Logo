# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. When an item ships, move its write-up into `docs/CHANGELOG.md`
(dated, with the full rationale/detail — see that file's own intro) instead
of marking it done in place here. Keeps this file trimmed to what's
genuinely still ahead; see `docs/CHANGELOG.md` for the full history of
everything that's already landed.

## Robustness

- [ ] Grow `tests/test_vm.c`'s (and neighbors') error-path coverage as
  new VM-only builtins land — `bin/logo` runs entirely on the VM, so
  that's the suite that actually protects it, not
  `tests/test_interpreter.c` (the old, frozen tree-walker `bin/logo`
  no longer runs; still useful for its own regression tests and
  shadow-diffing, just not a completeness target going forward). A
  2026-08-12 audit of every `append_output` message in `vm.c` found 10
  genuinely untested error/edge-case branches (`LAUNCH`/`AWAIT`/`YIELD`
  used inside a deferred context, `RUN`/`LOAD`'s own OUTPUT/STOP-
  escaping gap, `ONCLICK`/`ONKEYUP`/`ONMOUSEMOVE`/`ONRELEASE` registering
  an undefined procedure, `SETSPRITEFRAME` out of range) — all now
  covered; `LOADSPRITE`/`LOADSPRITESHEET`'s own "could not load" path is
  the one still open, since it needs a fake `load_sprite_image` stub the
  current test harness doesn't have (that callback is `NULL` in headless
  tests, so its failure branch is structurally unreachable today).

## Future / unplanned

Real ideas, not currently prioritized — pick up only on explicit request.

- [ ] Joystick event triggers (`ONJOYBUTTON`/etc) — the rest of the
  mouse/keyboard event-trigger work (`ONKEY`/`OFFKEY`/`ONCLICK`/
  `OFFCLICK`/`ONMOUSEMOVE`/`OFFMOUSEMOVE`/`ONKEYUP`/`OFFKEYUP`/
  `ONRELEASE`/`OFFRELEASE`) shipped 2026-08-11 (see `docs/CHANGELOG.md`'s
  own entries and `docs/COMMAND_REFERENCE.md`'s "Event triggers"
  section). Joystick is the odd one out — a real new dependency, not
  just another handler in the same shape as the rest: GTK4 has no
  built-in gamepad support on macOS, so this needs an extra dependency
  (e.g. GNOME's libmanette) or direct IOKit/HID access. Deprioritized
  2026-08-12 at the user's request — not important right now.
