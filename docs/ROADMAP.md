# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. When an item ships, move its write-up into `docs/CHANGELOG.md`
(dated, with the full rationale/detail — see that file's own intro) instead
of marking it done in place here. Keeps this file trimmed to what's
genuinely still ahead; see `docs/CHANGELOG.md` for the full history of
everything that's already landed.

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

