# Roadmap

Planned work for the Logo interpreter, roughly in priority order within each
section. When an item ships, move its write-up into `docs/CHANGELOG.md`
(dated, with the full rationale/detail — see that file's own intro) instead
of marking it done in place here. Keeps this file trimmed to what's
genuinely still ahead; see `docs/CHANGELOG.md` for the full history of
everything that's already landed.

Nothing currently planned — every item previously listed here has
shipped (see `docs/CHANGELOG.md` for the full history). Joystick event
triggers (`ONJOYBUTTON`/etc, the one remaining item as of 2026-08-12)
were dropped rather than deferred: `JOYSTICK?`/`JOYSTICKAXIS`/
`JOYSTICKBUTTON?` (passive polling, already shipped in Phase 4) cover
the common case, and event-style triggers would need either a real new
dependency (e.g. GNOME's libmanette) or direct IOKit/HID access — not
worth it for a feature the user doesn't consider a priority.
