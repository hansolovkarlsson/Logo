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

## Language completeness (2026-08-11 Terrapin Logo comparison)

Cross-checked every command on
[Terrapin Logo's command reference](https://resources.terrapinlogo.com/logo/commands/)
against `src/parser.c`'s `BUILTIN_SIGNATURES` table and every command
documented in `docs/COMMAND_REFERENCE.md`. Terrapin documents ~470 names;
~341 don't exist here — but most of that gap isn't real. Terrapin is a full
IDE with its own text editor, robot hardware drivers, and native
menu-editing API, none of which apply to this project:

- Workspace/editor commands (`EDIT`/`EDALL`/`EDP`/..., `BURY*`/`UNBURY*`,
  `POT`/`POPS`/`PRIMITIVES`/`COPYDEF`/`DEFINE`) — no in-app text editor to
  bury/unbury/print from
- Menu & window chrome (`APPENDMENU*`, `ONCOMMAND`, `SELECT.FILE`,
  `SETWINSIZE`/`WPOS`, `FULLSCREEN`/`SPLITSCREEN`) — Terrapin's own native
  menu-editing API, doesn't map to a fixed GTK menu
- Hardware (`BLUEBOT.*`, `PROBOT.*`, `OPEN.PORT`) — physical robot drivers
- Bitmap-era graphics (`SHAPE`/`LOADSHAPE`/`STAMP`/`SNAP`/`FONT`/`PLAY`
  (sound)/`BYTEARRAY`/`GRID`) — this app already has a different
  (sprite-based) turtle-image system
- Byte-level serial I/O (`GETBYTE`/`PUTBYTE`/`PEEKBYTE`)

Some more are naming differences, not real gaps: `XCOR`/`YCOR` = our
`GETX`/`GETY`; `AGET`/`ASET` = our `ITEM`/`SETITEM`; `ARRAYP` = our
`ARRAY?`; `MODULO` = our `MOD`.

What's left is a real, if modest, list of general-purpose language
primitives genuinely missing — ranked roughly easiest first:

- [ ] `PI` — Pi constant
- [ ] `RERANDOM` — seed the RNG
- [ ] `ASCII`/`CHAR` — char <-> code point
- [ ] `UPPERCASE`/`LOWERCASE` — case conversion
- [ ] `LOGAND`/`LOGOR`/`LOGXOR`/`LOGNOT`/`LSH` — bitwise ops (direct C
  operators, same shape as the existing `MOD`/`POWER`)
- [ ] `ARCTAN2`, `SEC`/`CSC`/`COT`/`ASEC`/`ACSC`/`ACOT` — rest of the trig
  family (math.h wrappers)
- [ ] `TIME`/`DATE`/`MILLISECONDS` — clock reads
- [ ] `DEFINED?` — does this procedure name exist
- [ ] `ISEQ`/`RSEQ` — generate a list of sequential/spaced numbers
- [ ] `REPCOUNT` — current `REPEAT` loop's counter (common idiom:
  `REPEAT 10 [FD REPCOUNT]`) — needs the VM to expose the loop counter;
  probably the single highest-value item here, since real Logo programs
  lean on it constantly
- [ ] `TURTLES` — count of active turtles (worth it given this project's
  own concurrent-agents model differs from Terrapin's turtle-fleet model)
- [ ] `READWORD`/`READCHAR` — stdin reads finer-grained than the existing
  `READLINE`
- [ ] `EVAL` — run a list, collect outputs as a list

## Mouse/keyboard event triggers

Today, input is pull-only: `WAITKEY` blocks until a key is pressed and
outputs its name; there's no way to react to input as a background
concern while a script keeps running. `MOUSEPOS`/`MOUSEX`/`MOUSEY`/
`BUTTON?` were designed once for the old tree-walking engine but never
ported to the VM (see `docs/COMMAND_REFERENCE.md`'s "documented
elsewhere, not available in `bin/logo`" appendix) — no mouse story
exists in `bin/logo` at all yet, pull-based or otherwise.

Proposed push-based API, reusing `LAUNCH`'s existing "run a procedure as
a background agent" machinery rather than inventing a new execution
model:

| Command | Args | Description |
|---|---|---|
| `ONKEY` | `"procname` | Run `procname` as a background agent on every keypress, called with the key's name as its one parameter (`:KEY`) — same name convention as `WAITKEY`'s own output |
| `ONKEYUP` | `"procname` | Same, for key release |
| `ONCLICK` | `"procname` | Run `procname` on mouse button press, called with `:X :Y :BUTTON` |
| `ONRELEASE` | `"procname` | Mirror of `ONCLICK` for button release |
| `ONMOUSEMOVE` | `"procname` | Run `procname` on pointer motion, called with `:X :Y` |
| `OFFKEY` / `OFFKEYUP` / `OFFCLICK` / `OFFRELEASE` / `OFFMOUSEMOVE` | — | Clear the respective handler |

```
TO KEYHANDLER :KEY
  IF :KEY = "space [PRINT "jump]
END
ONKEY "keyhandler

TO CLICKHANDLER :X :Y :BUTTON
  SETXY :X :Y
END
ONCLICK "clickhandler
```

Design decisions (settled 2026-08-11, before any implementation):

- **One handler per event type**, not a list — registering a new one
  replaces the old, same as `SETSPEED`'s single global setting. Simpler,
  and nothing here needs multiple independent listeners on one event.
- **`ONMOUSEMOVE`'s firing rate**: motion events can fire 60+/sec, and
  spawning a fresh `LAUNCH`-style agent per event would let invocations
  pile up faster than they finish. Fix: track a per-handler "busy" flag;
  drop a new fire while the previous invocation of *that same handler*
  is still running, rather than queuing. `ONKEY`/`ONCLICK` are naturally
  low-frequency enough this shouldn't matter for them, but the same
  guard is cheap to apply uniformly.
- **Coordinate space for `:X`/`:Y`**: logical turtle coordinates
  (matching `SETXY`), not raw pixels — consistent with the never-ported
  `MOUSEPOS` design rather than introducing a second coordinate system.
- **Joystick support is a separate, later item** (see the earlier
  discussion in this doc's history) — GTK4 has no built-in gamepad
  support on macOS, so it needs an extra dependency (e.g. GNOME's
  libmanette) or direct IOKit/HID access, a bigger lift than mouse/
  keyboard and not needed to ship those.

- [ ] `ONKEY`/`ONKEYUP`/`OFFKEY`/`OFFKEYUP` — keyboard event triggers
- [ ] `ONCLICK`/`ONRELEASE`/`ONMOUSEMOVE`/`OFFCLICK`/`OFFRELEASE`/
  `OFFMOUSEMOVE` — mouse event triggers
- [ ] Joystick event triggers (`ONJOYBUTTON`/etc) — deferred until a
  gamepad library dependency is decided on
