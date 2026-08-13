# Session Handoff

**Date:** 2026-08-12
**Repo:** `~/Projects/Logo` (GitHub: `hansolovkarlsson/LogoMotive`)
**Branch:** `main` (all work committed and pushed directly, no feature branch)

## Core Goal

No single feature was being implemented this session. The work was two
independent pieces of **roadmap grooming** for LogoMotive (a C11 + GTK4 +
Cairo Logo interpreter with a bytecode VM):

1. Scope a proposed language feature — inline VM assembly blocks (`{...}`
   in Logo source) — and record the findings.
2. Scope the *environment* question for a future Linux port (which distro
   to develop/validate on) and record the findings.

Neither feature was implemented. Both are filed on `docs/ROADMAP.md`'s
"Future / unplanned" list for later, pick-up-on-request work.

## Current Status & Progress

**Fully completed:**

- **Inline VM assembly blocks (`{...}`) — scoped, documented, committed
  (`5506c90`), pushed, verified live.** Investigated whether Logo source
  could contain raw bytecode-assembler text delimited by `{`/`}` (parallel
  to how `[...]` delimits list literals). Verdict: no architectural
  blocker, and it's actually *cleaner* than the parked `.MACRO` idea
  because it splices into the chunk already being compiled instead of
  running a separate scratch chunk through a nested `vm_run` call.
- **Linux port — scoped, documented, committed (`f69a6f1`), pushed,
  verified live.** Confirmed `Makefile`/`build.sh` are already plain
  `pkg-config`/`gcc` with zero hardcoded Homebrew/macOS paths — the only
  macOS coupling is three helper scripts
  (`scripts/install_brew.sh`, `scripts/check_brew.sh`,
  `scripts/install_gtk.sh`). Recommended dev/validation environments (see
  below).
- Both commits verified live on `origin/main` via `git log` on the remote
  ref **and** a GitHub API blob-SHA comparison against the local file
  (exact match both times).

**Pending / not started:**

- Neither feature has any implementation work done. Both are sitting as
  roadmap bullets only, explicitly "pick up only on explicit request."
- No new `scripts/install_gtk_linux.sh`-equivalent script exists yet.
- No lexer/parser/compiler changes for `{...}` blocks exist yet.

## Key Decisions Made

- **Inline `{}` assembly beats `.MACRO` architecturally.** `.MACRO` is
  blocked because `RUN`/`LOAD`/`EXECTIME` run a separate scratch
  `BytecodeChunk` via a nested `vm_run` call sharing the caller's frame
  stack, but `VmFrame` has no record of *which chunk* a `return_pc`
  belongs to — so `STOP`/`OUTPUT` crossing that boundary can resume
  against the wrong chunk. Inline `{}` blocks avoid this entirely because
  they'd be assembled directly into the chunk already being compiled, at
  the exact point encountered — no separate chunk, no frame/chunk
  mismatch possible. This is *why* `{}` was scoped as safe to pursue while
  `.MACRO` remains parked.
- **Recommended `{}` v1 scope: statement-only, not expression-position.**
  Expression position would need `OP_CHECK_OUTPUT`, which reads flags
  (`last_call_produced_output`/`last_call_resolved`) set by *specific call
  opcodes*, not "is there a value on the stack" — raw assembly wouldn't
  set those flags correctly without inventing its own convention. Starting
  statement-only sidesteps this.
- **`{}` blocks have no memory-safety/ASan risk, only logical risk.**
  `push`/`pop` in `vm.c` are bounds-checked and silently no-op on
  over/underflow; `chunk->code` is an array of fixed-size tagged `Instr`
  structs, so any in-chunk jump target is a well-defined dispatch, never a
  misaligned decode. The real risk is that nothing statically verifies a
  block's net stack effect, so an unbalanced block can silently desync the
  value stack for the rest of that procedure call. Treated as an accepted
  "inline asm in C"-style escape hatch — document plainly, don't try to
  eliminate the risk.
- **Fedora Workstation chosen as the Linux *development* target**, not
  Ubuntu or Mint, because it's GNOME's own reference platform and carries
  the newest GTK4/libadwaita — it surfaces API/deprecation issues earliest.
- **Ubuntu 24.04 LTS and Linux Mint (Cinnamon) chosen as *pre-release
  validation* targets, not dev targets** — both lag Fedora's GTK4 version
  (Mint tracks Ubuntu LTS directly), so building there first would mean
  catching version-skew bugs later rather than earlier. But they're more
  representative of what real users actually run: Ubuntu LTS as the common
  baseline, Mint as a proxy for less technical users on older/modest
  hardware.
- **Linux port is expected to be mostly build-script work, not a source
  port** — confirmed via direct grep of `Makefile`/`build.sh` that neither
  contains macOS-specific paths; only the three Homebrew helper scripts
  are platform-coupled. This is an empirical confirmation of a claim
  already noted in persistent memory (`future_crossplatform_and_releases`),
  now backed by an actual grep in this session rather than being carried
  forward untested.
- **Docs-only changes were committed/pushed without a separate
  "should I implement this" detour** — consistent with standing project
  convention that low-risk docs-only fixes can proceed without extra
  back-and-forth once the user says "add it to the roadmap," while actual
  code changes still require explicit confirmation before committing.

## Files & Paths Touched

- **`docs/ROADMAP.md`** — the only file modified this session, in two
  separate edits/commits:
  - Added **"Inline VM assembly blocks (`{...}`)"** bullet to the
    "Future / unplanned" section (commit `5506c90`).
  - Added **"Linux port"** bullet to the same section (commit `f69a6f1`),
    including dev-target recommendation (Fedora), validation targets
    (Ubuntu 24.04 LTS, Mint), and the empirical build-script findings.
- **Read-only investigation** (no changes) touched, during scoping:
  - `src/lexer.c` — `is_bareword_char()` (confirmed `{`/`}` currently
    lex as ordinary bareword characters, not free/special tokens).
  - `src/parser.c` — `AST_LIST_LITERAL`/`parse_list_literal` (existing
    template a `{...}` grammar rule would parallel).
  - `src/compiler.c` — `finish_call` and the `want_value`
    expression/statement distinction.
  - `src/vm.h` — `last_call_produced_output`/`last_call_resolved`/
    `last_send_message` field comments.
  - `src/vm.c` — `push`/`pop` bounds-checking; `OP_CHECK_OUTPUT`,
    `OP_CALL_BUILTIN`, `OP_CALL_PROC` dispatch sites.
  - `src/bytecode.c` / `src/bytecode.h` — `bytecode_assemble`,
    `asm_parse_operands`, `asm_resolve_target`, `asm_find_label` (existing
    save/load-bytecode assembler machinery, reusable for a future
    fragment-splicing variant).
  - `Makefile`, `build.sh` — grepped for Homebrew/macOS-specific paths
    (found none).
  - `scripts/install_brew.sh`, `scripts/check_brew.sh`,
    `scripts/install_gtk.sh` — confirmed as the only actual
    macOS/Homebrew-coupled files in the repo.

## Failed/Rejected Approaches

Nothing was actually attempted and failed this session — both pieces of
work were scoping-only, and no implementation was started. Worth carrying
forward so the next session doesn't retread the same ground:

- **Don't assume `{`/`}` are free/unused tokens.** The user's original
  premise was that `{}` "aren't used" in Logo source — actually,
  `is_bareword_char()` currently swallows both characters into ordinary
  barewords, so `{foo}` today lexes as one token, not three. Not a
  blocker, just needs the same lexer treatment `[`/`]` already got
  (two new token types, excluded from the bareword charset) — but don't
  skip this step assuming the characters are already inert.
- **Don't assume `OP_CALL_PROC` can call an existing Logo procedure by
  name from inside an assembled fragment as-is.** `asm_resolve_target`
  only resolves `"@N"` literals or labels from the local `labels[]` array
  populated during that same assemble pass — it does *not* fall back to
  `chunk->procs[]` by name. Any future implementation needs the same
  lookup `compile_call` already does via `find_proc_def`.
- **Don't let in-block jump targets accept bare `@N` absolute addresses.**
  The real pc is only known once a fragment is spliced into the
  in-progress chunk; targets inside a `{}` block should be restricted to
  labels defined within that same block.
- **Don't treat Ubuntu or Mint as viable Linux *development* targets** —
  they were considered and explicitly rejected for that role (both lag
  Fedora's GTK4 version), demoted to pre-release validation instead. If a
  future session is tempted to "just start on Ubuntu since that's what
  most users have," that tradeoff was already made deliberately in the
  other direction.

## Immediate Next Step

There is no implementation queued — both items are intentionally parked
on `docs/ROADMAP.md`'s "Future / unplanned" list, pick-up-only-on-request.
The next session should **wait for the user to explicitly pick one of the
8 items in that section** (or something else entirely) before starting
any implementation work. If/when the user says "implement the `{}`
inline-assembly v1 slice" or "start the Linux port," the exact next
concrete action would be:

- **For `{}` inline assembly:** start in `src/lexer.c` — add two new
  token types and exclude `{`/`}` from `is_bareword_char()`'s charset,
  following the existing `[`/`]` pattern exactly.
- **For the Linux port:** get access to a Fedora Workstation machine/VM
  (this session's dev environment is macOS and cannot itself validate
  Linux builds), then attempt `scripts/install_gtk.sh`'s Fedora
  equivalent (`dnf install gtk4-devel pkgconf-pkg-config` or similar) and
  a first `make` run to see what, if anything, actually breaks.
