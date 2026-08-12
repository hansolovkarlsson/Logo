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

- [ ] `--headless` CLI flag for `bin/logomotive` — run a script with no
  window shown at all, for scripting/automation use rather than
  interactive use. Most of the hard part already exists:
  `tools/vmrun_cli.c` (`bin/vmrun`, built 2026-08-12 for doc
  verification) already runs a script with no GTK window/event loop,
  auto-resolving suspensions and printing output — the natural path is
  promoting that same approach into `bin/logomotive` itself as a real
  flag (alongside the existing `--speed` one in `main.c`), not building
  headless mode from scratch. Open design question: should headless
  mode honor real timing (`SETSPEED`/`WAIT` actually pausing) or run
  everything instantly like `vmrun`/the test harness do? Leaning
  instant-by-default, since headless implies automation/batch use, not
  watching a drawing unfold.

- [ ] View-menu toggle to show/hide the input window (entry/history
  pane) while the app is running, leaving the canvas alone — e.g. for
  a presentation/demo mode. Fits into the existing View menu (today
  only text-size controls); a checkbox item calling
  `gtk_widget_set_visible()` on the entry/history pane should be
  enough. Smaller and more contained than the `--headless` flag above.
