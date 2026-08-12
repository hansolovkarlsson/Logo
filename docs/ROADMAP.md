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
were dropped rather than deferred: they'd need a real new dependency
(GNOME's libmanette, or direct IOKit/HID access) not worth taking on
for a feature the user doesn't consider a priority. Note this doesn't
just mean triggers are missing — `JOYSTICK?`/`JOYSTICKAXIS`/
`JOYSTICKBUTTON?` (plain polling) were never ported to the VM in the
first place either, and stay listed in `docs/COMMAND_REFERENCE.md`'s
own appendix of commands unreachable from `bin/logomotive` for the same
reason: no joystick support of any kind is currently planned.
