# A real front end + bytecode VM for Logo

Status: **Stage 1 shipped in full** (real lexer/parser/AST/tree-walking
evaluator, every `docs/ROADMAP.md` coverage batch landed, `LOAD`
cross-boundary-call gap fixed for real — see the Progress log below).
**Stage 2 (this document's own bytecode VM) is underway**: the
vertical slice (`docs/ROADMAP.md`'s "Phase 5, Stage 2" checklist, item
1) shipped 2026-08-09 — `bytecode.h`/`bytecode.c`,
`compiler.h`/`compiler.c`, `vm.h`/`vm.c`, shadow-diffed against
`ast_eval` in `tests/test_vm.c`. Every named instruction-coverage batch
(lists/arrays, property lists, turtle/drawing, `SEND`, `THROW`/`CATCH`,
`REPEAT`/`FOREVER`/`FOR`, `MAP`/`FILTER`/`REDUCE`/`FOREACH`) has since
landed too — see the Progress log below. Suspend/resume's own GTK
integration (the real point of this whole stage) landed 2026-08-10,
scoped to `WAIT`/`WAITKEY`, then `INPUT`, then `PAUSE`/`CONTINUE`/`CO`
— every named suspend/resume item on Stage 2's checklist is done, and
`ANIMATESPRITE` (plus the sprite subsystem it needed) landed right
after as its own separate project. `bin/logo` now runs scripts through
the compiler+VM instead of `eval_logo`, the first real cutover of the
live app. `TEXT`/`SHOW`/`SAVE`/file I/O surfaced a much larger version
of the same gap: a scripted audit found 35 `parser.c`-declared
builtins total never reaching `vm.c` at all. **All 35 are now wired
in** (math operators, type predicates, list/word operators, turtle
short aliases, `APPLY`, and — needing genuinely new "compile a fresh
chunk + recursive `vm_run`" machinery, the one real new architecture
piece of this whole project — `RUN`/`LOAD`); a scripted re-run of the
audit confirms zero remain.

## Why

Four separate reasons converged on this, worth keeping distinct since they
pull toward different priorities:

1. **Real cooperative suspend/resume.** `WAITKEY`/`INPUT`/`PAUSE`/`WAIT`/
   `ANIMATESPRITE` all fake "pausing" today by busy-waiting — spinning a
   loop calling `g_main_context_iteration` + `g_usleep(1000)` — because
   `eval_logo` has no way to actually suspend its own execution state and
   hand control back to GTK's main loop. A VM with an explicit instruction
   pointer and an explicit stack could genuinely pause (save state, return,
   resume later) instead of polling.
2. **The recursion cap's fragility.** `MAX_SCOPE_DEPTH` is 200, but that
   number is an artifact of `eval_logo` being one giant function where the
   real C-stack safety edge is much lower (`-O1` is load-bearing — see the
   Makefile — to keep it from overflowing around depth ~102 at `-O0`). A
   VM's call frames would live in ordinary memory, not the C stack, so
   recursion depth stops being coupled to how large `eval_logo` itself has
   grown.
3. **Execution speed.** Every loop iteration (`WHILE`/`REPEAT`/`FOR` bodies)
   re-tokenizes and re-parses its block's text from scratch, with zero
   caching of structure between iterations. Real overhead, but the weakest
   of the four motivations on its own — nothing about this project's actual
   usage has hit this as a bottleneck yet.
4. **Growing Logo past "toy language" scope.** This is really the driving
   one: closures, generators, concurrent turtle agents (MultiLogo-style —
   see `FEATURE_ATLAS.md`), and just about anything else on a "make this a
   more serious language" wishlist wants a real, structured execution model
   underneath it, not string-rescanning.

## Two separable stages

"Bytecode VM" bundles two upgrades that don't have to land together, and
splitting them is itself the main design decision here:

- **Stage 1 — a real lexer + parser producing an AST**, still *tree-walked*
  (interpreted directly, no bytecode yet) rather than re-scanning raw text.
  This alone kills the re-parse-every-iteration overhead, gives real error
  messages (line/column instead of "somewhere in this string"), and is the
  structural substrate Stage 2 compiles from. It does **not** by itself fix
  motivations 1 or 2 above (a tree-walker still recurses the C stack, still
  can't suspend mid-statement) — but it's a complete, valuable, separately
  shippable project, and the harder one to get right architecturally.
- **Stage 2 — compile the AST to bytecode and write the VM that executes
  it**: explicit instruction pointer, explicit call frames in a heap/array
  instead of C recursion. This is what actually buys suspend/resume and
  recursion-depth independence.

This document focuses on Stage 1 in real detail, since it's the actual
next step, and sketches Stage 2 at the depth needed to confirm Stage 1's
design won't paint us into a corner.

## What has to survive

`interpreter.c` is ~3700 lines implementing every feature from Phases 1–5
(turtle graphics, procedures/scoping, words/lists/arrays, files, sprites,
mouse/joystick/sound, prototype objects — see `LANGUAGE.md` in full), with
`tests/test_interpreter.c` covering it in detail. This is a rewrite of the
*engine*, not the language: every existing `.logo` example and every
existing test should keep producing byte-identical output once Stage 1
replaces the text-rescanning front end. That's the actual hard part of
this project — the lexer/parser themselves are the "well-understood
textbook" piece; faithfully reproducing ~150 existing operators' exact
parsing quirks (see "Open decisions" below for the ones that are
genuinely ambiguous, not just tedious) is where the real risk lives.

Proposed migration strategy, to keep that risk manageable:

1. Build the new lexer/parser/AST-walker as **new code that coexists with
   the current one**, not an in-place rewrite of `eval_logo`. Something
   like a `logo_ast_eval(app, code)` entry point next to the existing
   `eval_logo`, behind a compile-time or runtime flag.
2. Run both engines against the same input for every test in
   `tests/test_interpreter.c` and diff their `captured_output` — a
   mechanical way to find every place the new parser disagrees with the
   old one's actual behavior, rather than trusting a manual re-read of
   150 operators to catch every quirk.
3. Only once that diff is clean does the new engine become the real one
   and the old text-scanning `eval_logo` gets deleted — not kept around
   as permanent dead weight.

## Stage 1: Lexer

Logo's lexical rules are worth calling out explicitly since they're
genuinely unusual compared to a C-like language — this is one of the more
interesting "aha" corners of implementing Logo specifically:

- **Keywords aren't lexically distinct from identifiers.** `FD`, `IF`,
  `REPEAT`, and a user's own procedure name `MYPROC` are all the same
  token *shape* — a bareword. Whether one means "the built-in forward
  command" or "call this user procedure" is resolved by dictionary lookup
  during parsing, not by anything the lexer can decide. The lexer's job is
  narrower than in most languages: it just recognizes a bareword *as* a
  bareword, full stop.
- **Sigils bind with no whitespace required.** `"hello` (a quoted word) and
  `:name` (a variable reference) are each a single token starting with a
  sigil character, immediately followed by a bareword with zero
  whitespace tolerance — `" hello` (space after the quote) is not the same
  token. `'raw text with spaces'` is the one exception that spans
  whitespace, terminated by the matching `'`.
- **Numbers and barewords look different but both start expressions** —
  the lexer needs a `strtod`-style greedy number scan, falling back to a
  bareword token, rather than a fixed keyword table deciding what starts a
  token.

Proposed token set:

```c
typedef enum {
    TOK_NUMBER,       // 123, 3.14, -5
    TOK_WORD_LITERAL,  // "hello or 'raw text'
    TOK_VARREF,        // :name
    TOK_BAREWORD,      // FD, REPEAT, MYPROC, SETPROP, ... (dictionary-resolved later)
    TOK_LBRACKET, TOK_RBRACKET,  // [ ]
    TOK_LPAREN, TOK_RPAREN,      // ( ) -- arithmetic grouping only, see below
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH,
    TOK_LT, TOK_GT, TOK_EQ, TOK_LE, TOK_GE, TOK_NE,
    TOK_EOF,
} TokenType;

typedef struct {
    TokenType type;
    // Points into the original source buffer rather than copying —
    // same "the source text outlives the parse" assumption a script
    // loaded whole from disk (LOAD) already makes today.
    const char *start;
    int length;
    int line, col;   // for real error messages -- the actual point of Stage 1
} Token;
```

Matches this project's existing "fixed pool, loud error if exceeded"
discipline (`list_pool`, `plist_entries`, the `Procedure`/`Variable`
tables) rather than introducing a new malloc-heavy style: a whole script
gets lexed into a fixed-size (or generously bump-allocated) token array up
front, not token-by-token with heap churn.

## Stage 1: Grammar

The existing recursive-descent precedence chain in `parse_factor` →
`parse_term` → `parse_expr` → `parse_comparison` → `parse_bool_not` →
`parse_bool_and` → `parse_condition` (tightest to loosest: unary/literals,
`* /`, `+ -`, relational, `NOT`, `AND`, `OR`) is already a completely
standard, correct precedence-climbing design — Stage 1 keeps this
structure exactly, just building AST nodes instead of immediately
computing `Value`s. This is the single biggest reason Stage 1 is lower-risk
than it might sound: **the grammar doesn't need to be redesigned, only
retargeted** from "evaluate immediately" to "build a tree, evaluate the
tree afterward."

One existing asymmetry worth deciding whether to keep or fix: arithmetic
has parenthesized grouping (`(1 + 2) * 3`, handled in `parse_factor`) but
boolean conditions don't (`NOT`/`AND`/`OR` combine strictly left-to-right
by precedence level with no way to group them — see `parse_condition`'s
own comment). Preserving it is zero-risk; fixing it is a small, genuine
language change to flag separately, not bundle silently into an engine
rewrite.

**Arity resolution is the one place prefix-notation Logo makes parsing
harder than usual.** `PRINT SUM 1 2` only parses correctly if the parser
already knows `SUM` takes exactly 2 arguments *while it's parsing* — there's
no bracket or comma to mark where a call's arguments end. Built-in arities
are a static table (already implicit in today's `interpreter.c`, just
scattered across 150 `if (consume_keyword(...))` blocks instead of
collected in one place). User-procedure arities come from each `TO ... END`
header's parameter count — which today's interpreter only knows *after*
executing that definition, which is exactly why forward references don't
work yet (confirmed directly: calling a procedure before its `TO` block has
run reports `I don't know how to X`, even though the same script works
fine with the definition moved earlier). Stage 1 has to make an explicit
choice here — see "Open decisions" below.

Proposed AST node shapes (a tagged union per this project's existing
`Value` style, not a class hierarchy):

```c
typedef enum {
    NODE_NUMBER, NODE_WORD, NODE_VARREF,
    NODE_LIST_LITERAL,      // [ ... ] -- see the homoiconicity note below
    NODE_BINOP,             // + - * / < > = <= >= <>
    NODE_BOOL_NOT, NODE_BOOL_AND, NODE_BOOL_OR,
    NODE_CALL,               // a built-in or user procedure, statement or expression position
    NODE_PROC_DEF,           // TO ... END
    NODE_BLOCK,               // a sequence of statements (a procedure body, a REPEAT/IF/WHILE block)
} NodeType;

typedef struct AstNode {
    NodeType type;
    Token token;              // where this node came from, for error messages
    double number;             // NODE_NUMBER
    char word[512];             // NODE_WORD, NODE_VARREF, NODE_CALL's name
    int children[MAX_NODE_CHILDREN];  // indices into a flat node pool, not pointers --
    int child_count;                   // matches list_pool's own index-not-pointer convention
} AstNode;
```

**Open decision: list literals and homoiconicity.** `RUN`/`APPLY` treat a
list like `[FD 10 RT 90]` as executable code, and today that works because
a list is *never* more than lightly parsed — it's stored close to its raw
text and only really parsed when `RUN` feeds it back through `eval_logo`.
A "real AST" pushes toward parsing a list literal's contents fully at parse
time (into real `AstNode`s, just like top-level code) — which is more
architecturally consistent, but means `RUN` needs to walk an already-built
AST fragment rather than re-lexing text, and list-as-data operations
(`FIRST`, `FPUT`, printing a list back out as source) need to reconstruct
source text *from* that AST when needed, rather than already having it.
Both directions are workable; this needs a decision before Stage 1's AST
node shapes are finalized, since it changes what `NODE_LIST_LITERAL`
actually holds.

## Stage 2 sketch

Now the actual next target, sequenced into `docs/ROADMAP.md`'s own
"Phase 5, Stage 2" checklist (resolved 2026-08-09) — this section is
the design detail behind that checklist's priority order, not yet
implementation. Compiling Stage 1's already-real AST to bytecode is the
more conventional part: a flat instruction array (`PUSH_NUMBER`,
`PUSH_VAR`, `CALL name argc`, `JUMP`/`JUMP_IF_FALSE` for `IF`/`WHILE`,
`RETURN`, ...) executed by a loop over an explicit value stack and an
explicit array of call frames (each frame holding a return instruction
pointer and its own variable bindings) — no C recursion involved in
Logo-level procedure calls at all. That last point is what actually
delivers motivations 1 and 2 from the top of this document: pausing
becomes "stop the loop, keep the frame array around"; resuming becomes
"restart the loop from the saved instruction pointer"; and recursion
depth becomes bounded by how big we choose to make the frame array, not
by `eval_logo`'s own C-stack footprint.

A few instruction-set/design points worth flagging ahead of the actual
build, expanding on `docs/ROADMAP.md`'s own checklist:
- **Beyond the example opcodes above**, Logo's own value model needs
  real list/array construction, a `SET_VAR` op for `MAKE`, property-list
  ops, and — the two genuinely hard pieces, called out as their own
  checklist item — `OUTPUT`/`STOP` (early return through nested blocks)
  and `THROW`/`CATCH` (non-local exit to an arbitrary ancestor frame,
  needing a real unwind-target stack, not just a jump target).
- **Value representation should stay boring**: the existing tagged-
  struct `EvalValue` shape (see eval.c), pushed/popped by value on an
  explicit value stack, rather than reaching for NaN-boxing or tagged
  machine words — motivation 3 (speed) is explicitly the weakest of the
  four above, not worth spending complexity on.
- **Migration strategy reuses the exact playbook that already worked
  for Stage 1**: shadow-diff the new compiler+VM against `ast_eval`
  itself, not `eval_logo` directly — Stage 1's own frontend is already
  proven byte-identical to `eval_logo`, so diffing against `ast_eval`
  isolates "does the compiler+VM match the tree-walker's semantics" as
  its own, separately checkable question.

## Decisions (resolved 2026-08-08)

1. **Forward references: lifted.** Stage 1 does a pre-pass registering
   every top-level `TO`'s name and arity before running anything, matching
   how C/Java/JS resolve top-level declarations rather than the current
   single-pass, define-before-use restriction (confirmed today: calling a
   procedure before its `TO...END` has executed reports `I don't know how
   to X`). Strictly additive — anything that worked before still works;
   this only makes previously-illegal orderings legal. Consequence for
   the migration strategy above: the old-vs-new output diff needs at least
   one test script that calls a procedure before its definition, to
   confirm the new engine's *intentional* divergence there is exactly
   that one case and nothing else.
2. **List literals: parsed fully into AST nodes at parse time.** Matches
   Logo's Lisp heritage (code-as-data — `RUN`/`APPLY` are Logo's `eval`)
   and fixes deferred execution's own re-lex-every-call overhead, not just
   loop bodies. Needs `value_to_source_text`'s logic extended to serialize
   an AST fragment back to source text (for printing a list, `FIRST`,
   `FPUT`, etc.), rather than inventing that from scratch.
3. **Boolean grouping: fixed.** `NOT`/`AND`/`OR` gain the same `(...)`
   grouping arithmetic already has via `parse_factor` — a small, strictly
   additive language change (nothing existing changes meaning), landing
   alongside the engine rewrite rather than deferred.
4. **Staging: Stage 1 ships as its own complete milestone.** Build,
   validate via the shadow-diff-against-old-engine strategy above, and cut
   over to the new AST-walking engine as a finished, real replacement for
   `eval_logo` — then reassess before committing to Stage 2's larger scope,
   rather than treating Stage 1 as scaffolding that's never independently
   "done." Matches how every other feature has shipped this session:
   implement, verify, ship, reassess.
   - **Reassessed 2026-08-09, per this decision's own instruction**:
     Stage 1 shipped in full (every `docs/ROADMAP.md` coverage batch,
     plus the `LOAD` cross-boundary-call gap fixed for real). Stage 2
     is now prioritized into a sequenced checklist in
     `docs/ROADMAP.md`'s own "Phase 5, Stage 2" section — see the
     "Stage 2 sketch" section above for the design detail behind that
     priority order.

## Progress

- **Lexer: done** (`src/lexer.h`/`src/lexer.c`, tested by
  `tests/test_lexer.c`, run via `make test-lexer` or as part of `make
  test`). Zero dependency on GTK/GLib/`interpreter.h` — a
  `logo_lex(source, tokens, max_tokens)` call, tested and confirmed
  clean under AddressSanitizer. Several lexical quirks were confirmed
  directly against the running interpreter rather than assumed, and are
  preserved deliberately rather than tidied up (see the file comment in
  `lexer.c`): a bareword/quoted-word stops only at whitespace (never at
  `)`/`]`), while a `:varref`'s name stops at the first non-alnum/`_`
  character — a real, if inconsistent, difference between the two that
  a faithful lexer has to reproduce, not paper over.
- **Parser: a real, bounded slice done** (`src/ast.h`/`src/ast.c`,
  `src/parser.h`/`src/parser.c`, tested by `tests/test_parser.c`, run
  via `make test-parser` or as part of `make test`). Covers the full
  expression/condition precedence chain, list literals, `IF`/`IFELSE`/
  `WHILE`/`REPEAT`, `TO`/`END` with hoisted arities, and a
  representative set of ordinary commands/operators (see
  `BUILTIN_SIGNATURES` in `parser.c`) — not all ~150 of
  `interpreter.c`'s operators yet; growing that table is incremental
  follow-up work, not part of getting the mechanism right. All four
  resolved decisions are implemented: hoisted `TO` arities (forward
  references work), list literals fully parsed into `AST_LIST_LITERAL`/
  `AST_WORD` atoms, parenthesized boolean grouping, and real line/col
  error messages, collected rather than aborting on the first one.
  - **`ParseResult` must be heap-allocated, never a stack local** — the
    same `LogoApp` lesson recorded elsewhere in this project's memory,
    rediscovered here directly: `AstPool`'s fixed `MAX_AST_NODES`-sized
    array makes `ParseResult` over 6.7MB, confirmed to overflow the
    stack under AddressSanitizer when tests first declared it as a
    plain local (`tests/test_parser.c`'s `parse_source` now returns a
    `calloc`'d pointer instead).
  - **A real, unplanned fidelity gap surfaced by testing, not
    guessing**: `interpreter.c`'s `REPEAT`/`IF`/`WHILE`/`TO` extract a
    block's raw text via a *separate* bracket-counting pass before ever
    tokenizing it (`extract_block`), so a quoted word inside a block
    never sees that block's own closing `]`. This lexer tokenizes the
    whole source in one uniform pass with no such context, so
    `[PRINT "positive]` (no space before `]`) lexes the closing bracket
    into the quoted word — a real, narrow behavior gap from today's
    interpreter for blocks specifically, not yet resolved. Confirmed
    directly (not assumed) before writing this note.
  - **A related, higher-impact case that *was* fixed**: `interpreter.c`'s
    list-literal element scanner already stops at `[`/`]` in addition
    to whitespace (unlike its own general word-scanning elsewhere,
    confirmed via `parse_list_literal`'s own extra check) — and since
    almost every real list literal has its last element touching the
    closing bracket (`[FD 10 RT 90]`, not `[FD 10 RT 90 ]`), this
    lexer adopted that stricter `[`/`]`-stops-a-word rule universally
    rather than reproducing the old inconsistency. A deliberate,
    documented simplification in the spirit of the list-literal
    decision above, not a silent divergence (see `lexer.c`'s file
    comment and `is_bareword_char`).
  - **`MAKE`'s own fidelity gap, caught by a failing test**: confirmed
    directly that `MAKE`'s varname argument is read via a raw
    `sscanf(ptr, "%s", ...)` in `interpreter.c`, not `parse_expr` —
    unlike `SETPROP`'s name argument, which does accept a computed
    expression. `MAKE "x -5` only sets `x` to `-5` *because* of this
    restriction (a general-expression varname would swallow the minus
    as subtraction instead). Modeled as its own `ARG_QUOTED_WORD`
    argument kind rather than folded into `ARG_EXPR`.
- **Tree-walking evaluator: a real, bounded slice done**
  (`src/eval.h`/`src/eval.c`, tested by `tests/test_eval.c`, run via
  `make test-eval` or as part of `make test`). Runs the AST built by
  `parser.c` against a real `LogoApp` — the first component that
  actually *executes* Stage 1's output rather than just building it.
  Covers everything `parser.c`'s `BUILTIN_SIGNATURES` does: turtle
  motion (`FD`/`BK`/`RT`/`LT`/`SETXY`/`SETHEADING`/`PENUP`/`PENDOWN`/
  `HOME`/`CLEAR`/`CS`), `PRINT`/`MAKE`, `IF`/`IFELSE`/`WHILE`/`REPEAT`,
  `TO`/`END` with parameters/`OUTPUT`/`STOP`/recursion (including the
  same `MAX_SCOPE_DEPTH` recursion-too-deep guard, confirmed clean
  under AddressSanitizer), and the math operators. Confirmed forward
  references actually *work end to end* now, not just parse cleanly.
  - **Resolved the evaluator-design decision this way**: exposed a
    small, curated set of already-correct, pure state-manipulation
    functions from `interpreter.c` via `interpreter.h`
    (`current_turtle`/`move_turtle_to`/`move_turtle_forward`/`home_x`/
    `home_y`/`find_var`/`set_var`/`set_var_word`/`random_below`) rather
    than reimplementing turtle/variable logic fresh. `interpreter.c`'s
    actual *parsing* (tokenizing, `eval_logo`'s text-driven dispatch)
    remains completely untouched and private — only the state
    underneath it is now shared. This is the one place Stage 1 has
    touched `interpreter.c` at all so far.
  - **Deferred, documented, not silent**: list/array values aren't
    evaluated yet (a variable holding one, or a list literal used as a
    value, reports a clear message rather than silently misbehaving) —
    needs `list_pool` allocation, currently private to `interpreter.c`
    and not yet exposed. Ctrl+C interrupt checking is also absent
    (`g_interrupt_requested` is a file-static flag with no exposed
    getter).
  - **One deliberate simplification worth knowing**: unlike the old
    interpreter (where `FD`/`PRINT`/`MAKE`/etc. are reachable only as
    statements, never as expression-position operators — a separate
    keyword list from `parse_factor`'s own operator chain), this
    evaluator's `AST_CALL` node serves both roles uniformly, so e.g.
    `PRINT FD 10` parses and runs here where the old engine would
    behave quite differently. Narrow — nobody writes real scripts this
    way — and documented in `eval.c`'s own file comment.
- **Shadow-diff strategy: started** (`tests/test_shadow_diff.c`, run
  via `make test-shadow-diff` or as part of `make test`). Runs a
  curated corpus of 20 scripts through both engines — `eval_logo`
  against a fresh `LogoApp`, and `logo_lex` → `logo_parse` → `ast_eval`
  against a separate one — and asserts they agree on both captured
  output text and final turtle state (`x`/`y`/`angle`/`pen_down`),
  since `FD`/`RT`/etc. don't print anything a text-only diff would
  catch. Bounded to scripts within `BUILTIN_SIGNATURES`'s current
  coverage — this isn't "replay all of `tests/test_interpreter.c`"
  yet, since most of it uses operators the new engine doesn't
  implement (`SETPROP`, lists, arrays, `TELL`, ...), which would just
  report parse errors, not real behavioral disagreements. `RANDOM` is
  deliberately excluded (both engines share one continuing RNG stream
  via the exposed `random_below`, so diffing it would compare two
  different-but-both-correct draws, not catch a real divergence).
  - **Found a real, meaningful divergence on the first run** — see
    `test_local_scope_shadows_global`'s own comment: a test procedure
    happened to be named `setx`, which is a genuine built-in in the
    old engine (sets the turtle's X coordinate) that `BUILTIN_SIGNATURES`
    doesn't have yet. The old engine's `SETX` always wins over a
    same-named user procedure; the new engine, not knowing about
    `SETX`, just calls the user's procedure instead — silently
    different behavior, caught mechanically rather than by manual
    review. Not a bug in either engine's actual logic, but a real,
    general risk category worth naming: as `BUILTIN_SIGNATURES` grows,
    a script (real or test) that names a procedure after an
    not-yet-implemented built-in will diverge exactly this way until
    that built-in lands. Fixed by renaming the test's procedure, with
    the collision itself recorded as the reason, not silently avoided.
- **Shadow-diff corpus: grown from 20 to 33 scripts**, plus the
  comparison itself strengthened to check every drawn line segment's
  endpoints (`app->lines`/`line_count`), not just the turtle's final
  resting position — two engines could agree on where the turtle ends
  up while disagreeing about the path taken to get there, which
  final-position-only comparison would miss. New corpus additions
  cover nested loops, mutual/forward-and-backward procedure references,
  multi-parameter and zero-parameter procedures, procedure calls
  nested as arguments, `TRUE`/`FALSE` literals, `<=`/`>=`/`<>`, the new
  parenthesized-boolean-grouping capability (confirmed to actually run
  correctly, not just parse — nothing to diff against here since
  today's interpreter can't express it at all), early `STOP` without
  `OUTPUT`, boundary values (`REPEAT 0`, `WHILE FALSE`), and deeper
  recursion.
  - **Two more real findings, both fixed by adjusting the test, not
    the engines**: a script combining `SETXY -50 -75` with a following
    `SETHEADING` call exercised the *already-documented*
    AST_CALL-in-expression-position simplification concretely — the
    grammar's own `-50 - 75` greedy-subtraction parsing (shared by both
    engines) left `SETHEADING 450` to become the *next* argument's
    expression, which the old engine's parse_factor doesn't recognize
    as an operator but this one does. Fixed with parens
    (`SETXY -50 (-75)`), not a new special case.
  - **A genuinely new discovery, in the old engine itself**: a
    recursion pattern calling itself as an expression (`OUTPUT sumTo
    ...`, the "procedure as operator" fallback) recurses through a
    longer per-level C-stack chain than plain statement-position
    self-recursion, and overflows the real stack under
    AddressSanitizer *much* sooner — bisected directly: somewhere
    between depth 34 and 36, versus the ~102-186 level range
    previously documented for statement-position recursion (see the
    Makefile's own `-O1` comment). Not a new-engine bug at all; a
    pre-existing fragility in `eval_logo` that this corpus's use of
    real accumulator-style recursion happened to trip for the first
    time. The test now runs at depth 20, comfortably below it.
- **List-value support: done**. Real list values now flow through the
  whole evaluator — list literals build genuine `app->list_pool`
  chains (mirroring `interpreter.c`'s own `parse_list_literal` exactly:
  untyped raw-text leaves, never number-vs-word typed at construction
  time), and `FIRST`/`BUTFIRST`/`LAST`/`BUTLAST`/`COUNT`/`EMPTY?`/
  `FPUT`/`LPUT`/`WORD`/`SENTENCE`(`SE`)/`LIST` all work — as do list
  variables, list arguments/`OUTPUT`, and list equality in conditions.
  Exposed three more pure, `Value`-independent functions from
  `interpreter.c` (`list_alloc_node`, `list_node_copy`,
  `set_var_list`) — the same curated-sharing approach as the turtle/
  variable helpers, so a list built by one engine is indistinguishable
  from one built by the other (same pool). 9 new `tests/test_eval.c`
  cases plus 7 new shadow-diff scripts, all confirming exact agreement
  with the real interpreter, including a real-shaped
  recurse-over-a-list-accumulating-a-sum script.
  - **A second genuinely new stack-fragility discovery, this time in
    the NEW engine**: adding all these list operators as inline
    branches in `exec_call` (one large function, a branch per
    built-in, recursing through `call_ast_procedure` for every
    ordinary procedure call) pushed its per-call stack frame large
    enough to overflow the previously-clean 200-level recursion test
    under AddressSanitizer — confirmed directly, the exact same class
    of fragility already documented for `eval_logo` itself, now
    self-inflicted in brand-new code. Fixed by extracting `WORD` (the
    single biggest offender, two `char[512]` buffers) into its own
    function, `eval_word_concat`, matching the pattern already used
    for `FPUT`/`LPUT`/`SENTENCE`/`LIST` — confirmed clean under ASan
    again afterward. Worth remembering as `BUILTIN_SIGNATURES`/
    `exec_call` keep growing: large per-branch locals inlined directly
    into `exec_call` cost every recursion level, not just calls to
    that one operator, so anything with a sizeable local buffer should
    be its own function from the start.
- **Property lists: done** (`SETPROP`/`GETPROP`/`REMOVEPROP`/`PROPLIST`).
  Share `app->plist_entries`/`plist_entry_count` directly with
  `eval_logo`'s own property-list operators — a property set by one
  engine is visible to the other, same pattern as the shared
  `list_pool`. Exposed one more `Value`-independent function,
  `find_plist_entry`. 6 new `tests/test_eval.c` cases and 4 new
  shadow-diff scripts, including a real-shaped "property list as
  lightweight per-object state" script, matching the same use case
  that motivated `NEW`/`SEND` in an earlier phase.
  - **The same stack-fragility class recurred a third time — this
    time fixed for real, structurally, not just locally.** Even with
    `SETPROP`/`GETPROP`/`REMOVEPROP`/`PROPLIST` each already factored
    into their own functions (learning the lesson from `WORD`), adding
    them as four more `exec_call` branches was enough on its own to
    overflow the 200-level recursion test under ASan again — not
    because of any one large branch this time, but because `exec_call`
    had accumulated ~36 branches total, and even small per-branch
    locals add up at that count. This confirmed the fix needed to be
    structural, not another one-off extraction: **every single
    built-in, however small, was refactored into its own `do_*`
    function** (`do_fd`, `do_print`, `do_first`, `do_getprop`, ...),
    including the recursive user-procedure-call fallback
    (`do_user_procedure_call`). `exec_call` itself is now just a
    dispatch table — its own stack frame is exactly `{arg_idx[8],
    argc, name}` regardless of how large `BUILTIN_SIGNATURES` grows,
    confirmed clean under ASan again afterward. **This is now the
    required pattern going forward**: every new built-in added to
    `exec_call` must be its own function from the start, not a new
    inline branch — the file comment directly above the `do_*`
    functions in `eval.c` says so explicitly.
- **`NEW`/`SEND` (prototype-style objects): done.** Reuses the
  property-list infrastructure that just landed — `SEND`'s prototype-
  chain walk (`eval_resolve_message`/`eval_resolve_method` in
  `eval.c`) is just repeated `find_plist_entry` lookups (bounded by
  `MAX_PROTOTYPE_CHAIN_DEPTH`), followed by an ordinary call through
  `call_ast_procedure` — no new state, no new architecture, just
  composition of what already existed.
  - **A real, deliberate syntax difference from the old engine, found
    during design rather than by surprise**: the old engine's `SEND
    obj "message arg1 arg2...` only works because it parses and
    executes in the same pass — it evaluates `obj`/`message` first,
    then uses those runtime values to look up the method's own
    parameter count via a property-list chain walk, and only then
    decides how many more expression tokens to consume. A real static
    parser builds the whole AST before any value exists, so there is
    no way to know at parse time how many trailing tokens a `SEND`
    call owns — the same wall real languages with equivalent dynamic
    dispatch hit (Smalltalk's `perform:withArguments:`, Python's
    `.apply()`-style calls). Resolved by giving `SEND` an explicit
    argument list here — `SEND obj "message arglist`, `[]` for a
    zero-argument message — mirroring `APPLY`'s own existing
    convention rather than inventing a new one. Confirmed this also
    made a real, new check possible that the old engine's positional
    parsing structurally can't produce (`SEND: wrong number of inputs
    for message`, mirroring `APPLY`'s own count check) — a genuinely
    new-engine-only test case, not a regression.
  - This being a real syntax difference means the shadow-diff corpus
    can't literally diff the same script text for `SEND`. Extended the
    harness with `shadow_diff_pair(old_source, new_source)` — two
    different-but-equivalent scripts, one per engine's own real syntax
    — alongside `shadow_diff`'s usual same-text comparison, for this
    exact kind of deliberate divergence. A found-and-fixed bug along
    the way, from mismatching against the old engine's exact behavior
    rather than guessing: `SEND`'s "didn't output a value" message
    needs the *message* name (`bark`), not `SEND`'s own name, AND must
    stay silent entirely in statement position (nobody asked for a
    value there) — both confirmed directly against `interpreter.c`'s
    own `SEND` operator form before the fix, not assumed.
- **Arrays: done** (`ARRAY`/`ITEM`/`SETITEM`/`FILLARRAY`). The one
  mutable value type in the language, ported directly from
  `interpreter.c`'s own implementation: `ARRAY size` allocates `size`
  contiguous `app->list_pool` cells (direct index math —
  `list_pool[start + i]` — not chain-walked like a list, the whole
  point of reaching for an array over a list); `SETITEM`/`FILLARRAY`
  mutate cells in place; `MAKE "b :a` aliases the same cells rather
  than copying, and so does passing an array into a procedure
  parameter — both fixed by giving `call_ast_procedure`'s parameter-
  binding loop and `do_make` a `VALUE_ARRAY` branch that copies both
  `list_head` *and* `number` (an array's `EvalValue.number` field
  holds its length, not a value). Exposed one more `Value`-independent
  function from `interpreter.c`, `set_var_array`. Also fixed a latent
  gap this surfaced: `eval_expr`'s `AST_VARREF` case previously had no
  real `VALUE_ARRAY` branch at all (it printed "array variables aren't
  supported yet" and returned 0) — now returns a real `array_val`,
  which is what makes reading an array back out of a variable work at
  all. Same "one built-in, one `do_*` function" structural rule
  followed throughout (`do_array`/`do_item`/`do_setitem`/
  `do_fillarray`, plus a small shared `eval_array_store_cell` helper
  for the per-cell-assignment logic `SETITEM` and `FILLARRAY` share).
  14 new `tests/test_eval.c` cases (creation, braces-formatted PRINT,
  storing each value type, out-of-range/wrong-type/nested-array
  errors, `COUNT`, aliasing via both `MAKE` and a procedure parameter)
  and 3 new shadow-diff scripts (arrays use identical syntax in both
  engines — `interpreter.c`'s `ARRAY`/`ITEM`/`SETITEM`/`FILLARRAY`
  already take plain expression arguments, so no `shadow_diff_pair`
  syntax-divergence handling was needed here, unlike `SEND`).
  - **A stale comment, found and deliberately not propagated**:
    `interpreter.c`'s own `ARRAY` comment claims arrays "can't be
    passed into a user-defined procedure's parameter today" and get
    "coerced to a plain number at the call boundary" — but the actual
    `call_procedure` code there already handles `VALUE_ARRAY`
    parameters correctly (copies both `list_head` and `number`,
    exactly the aliasing behavior the rest of the comment describes
    elsewhere). Matched the real, verified code behavior when porting
    to the new engine, not the outdated comment; left a quiet note
    rather than making a production of the discrepancy.
  - Confirmed clean under AddressSanitizer on both `test_eval` and
    `test_shadow_diff` — no new stack-fragility regression, which is
    exactly what the structural `do_*`-per-built-in fix from the
    property-lists milestone was meant to guarantee going forward.
- **Type predicates: done** (`WORD?`/`LIST?`/`NUMBER?`/`ARRAY?`). The
  simplest milestone in this whole effort so far — each just checks
  the evaluated argument's own `ValueType` tag, which is shared
  verbatim between the two engines (`logo_types.h`'s `ValueType` enum
  is the same one both `Value` and `EvalValue` use), so there was
  nothing to port beyond the four one-line `do_*` functions
  (`do_wordp`/`do_listp`/`do_numberp`/`do_arrayp`) and their
  `BUILTIN_SIGNATURES` entries. No new state, no new sharing with
  `interpreter.c`, no new stack-fragility risk worth calling out. 4 new
  `tests/test_eval.c` cases (one per value kind — word, number, list,
  array — checking all four predicates against it) and 1 new
  shadow-diff script covering every predicate against every value kind
  in one pass. `EMPTY?` (already implemented, checks emptiness not
  type) and `MEMBER?`/`EOF?`/`BUTTON?`/`JOYSTICK?`/`JOYSTICKBUTTON?`
  (list-membership and hardware/file-input predicates, not type
  predicates, `EOF?`/`BUTTON?`-family out of scope for a headless
  evaluator regardless) were deliberately left out of this pass.
  Confirmed clean under AddressSanitizer on both `test_eval` and
  `test_shadow_diff`.
- **Higher-order list operators: done** (`MEMBER?`/`MAP`/`FILTER`/
  `REDUCE`). `MEMBER?` was a plain port (list membership via
  `eval_values_equal`, word substring via `strstr`, everything else
  falling back to whole-value equality — mirrors interpreter.c's own
  `MEMBER?` exactly, including that fallback). `MAP`/`FILTER`/`REDUCE`
  needed real new architecture: interpreter.c's own versions work by
  substituting an element's text into a template string (`[? * 2]`,
  `[?1 + ?2]`) and re-parsing the result, which is only possible there
  because parsing and execution are the same pass. This evaluator has
  no such single-pass parser to hook into mid-execution, so it goes
  through the same lex -> parse -> eval pipeline any script does, just
  on a short synthesized snippet each time — two new public entry
  points, `logo_parse_expr`/`logo_parse_condition` in `parser.c`, parse
  a standalone expression/condition instead of a whole program
  (reusing the same `Parser` setup `logo_parse` already has, just
  handing off to `parse_expr_value`/`parse_condition` directly instead
  of `parse_statement` in a loop). `eval.c`'s own
  `eval_apply_template_expr`/`eval_apply_template_condition` mirror
  interpreter.c's `apply_template_expr`/`apply_template_condition`
  (built on ported `eval_substitute_placeholder`/
  `eval_value_to_source_text` helpers), reusing one caller-owned,
  caller-reused `ParseResult` scratch buffer across a whole MAP/FILTER/
  REDUCE call rather than one per list element (its `AstPool` is
  ~6.7MB — the same heap-only rule as everywhere else `AstPool`/
  `LogoApp` appears in this project). 21 new `tests/test_eval.c` cases
  and 2 new shadow-diff scripts.
  - **A real fidelity bug found and fixed along the way, in
    `parse_list_literal` itself, not in the new MAP/FILTER/REDUCE
    code**: confirmed directly against the running interpreter that
    `PRINT [a "b c]` really does print `a "b c` — interpreter.c's own
    list-literal scanning never treats `"` specially, so a quoted
    word's leading `"` survives as literal data in the stored element
    text (`APPLY "greet ["alice]` genuinely passes the 6-character
    word `"alice`, confirmed directly too, not `alice`). This lexer's
    own `"` handling had always stripped that mark before producing
    the `QUOTED_WORD` token, and `parse_list_literal` copied the
    token's already-stripped text straight into its `AST_WORD` leaf —
    so a template like `FILTER [? = "b] [a b c b]` silently emptied out
    instead of returning `b b`, because the substituted text lost the
    one character (`"`) that told the runtime re-parse it was looking
    at a quoted word rather than an unresolvable bareword. Fixed by
    re-adding the `"` when a list literal's element token is a
    `QUOTED_WORD`, making the new engine's list-literal storage
    bit-for-bit match interpreter.c's for this case. This is a genuine
    correctness fix, not a new capability — confirmed no existing test
    exercised a quoted word as list-literal *data* before this (every
    prior use was inside a block, e.g. `IF ... [PRINT "x]`, a
    completely different grammar path unaffected by this change).
  - **One existing test corrected, not the new fix reverted**: the
    fix's necessary consequence is that `["alice]` used as a SEND
    arglist is *also* now the literal 6-character word `"alice`
    (confirmed the exact same quirk in interpreter.c's own `APPLY`,
    the convention `SEND`'s own arglist mirrors) —
    `test_send_passes_extra_message_arguments_after_self` had been
    unknowingly relying on the *old*, incorrect quote-stripping
    behavior. Corrected to use a bareword (`[alice]`), the actually
    correct, idiomatic way to pass a clean word through a list-literal
    argument, verified directly against `interpreter.c`'s own `APPLY`
    before changing.
  - **A known, deliberately avoided boundary case in the shadow-diff
    corpus**: `FILTER`'s template is a condition, and this evaluator's
    parenthesized-boolean-grouping capability (already documented as
    new-engine-only) means a grouped template like
    `[(? > 2) AND (? < 5)]` runs here but fails to parse at all in the
    old engine — not a MAP/FILTER/REDUCE bug, the same pre-existing gap
    already on record. The shadow-diff script uses the equivalent
    ungrouped form (`[? > 2 AND ? < 5]`, supported by both) instead.
  - Confirmed clean under AddressSanitizer on both `test_eval` and
    `test_shadow_diff` — the new dynamic lex/parse/eval snippet path
    (a form of recursion through the front end that didn't exist
    before this) introduced no new stack-fragility regression.
  - **An unrelated, pre-existing finding surfaced while ASan-testing
    this**: running AddressSanitizer against `tests/test_interpreter.c`
    directly (not part of this task, but done as an extra check) hit
    the *already-documented* expression-position recursion fragility
    in `eval_logo` itself (see [[eval_logo_recursion_margin]]) — some
    existing test recurses close enough to that ~34-36-level margin to
    overflow under instrumentation. Not caused by anything in this
    milestone (`interpreter.c` is completely untouched by it — `git
    diff` confirms zero changes), not investigated further here since
    it's out of scope for higher-order list operators specifically, but
    worth another pass at some point.
- **`FOREACH`: done.** Runs a statement template (`[PRINT ?]`, or
  anything else, including multiple commands or a nested `IF`) once per
  list element, mirroring interpreter.c's own `FOREACH` exactly (a
  plain `eval_logo` call per element there). Unlike `MAP`/`FILTER`/
  `REDUCE`, there's no expression/condition-only entry point to reuse
  here — a `FOREACH` template can be an arbitrary sequence of commands,
  so `do_foreach` goes through `logo_parse` itself (the same
  whole-program entry point every real script uses, not
  `logo_parse_expr`/`logo_parse_condition`) on the substituted text each
  iteration, then runs the result through `exec_block` — the same
  caller-reused scratch `ParseResult` pattern as the other three. A
  malformed substituted statement is skipped rather than executed
  (parse errors read as inert, same fallback used elsewhere for a bad
  template). `OUTPUT`/`STOP` inside the template ends the loop early via
  `app->stop_requested`, the same flag `REPEAT`/`WHILE` already check —
  no `THROW`/interrupt equivalent exists in this engine yet to also
  check, unlike interpreter.c's own three-way condition there.
  - **A second, more consequential fidelity bug found by this milestone
    specifically, in the exact same function as before
    (`parse_list_literal`), not in `do_foreach`**: confirmed directly
    against the running interpreter that `PRINT [MAKE "sum :sum + ?]`
    really does print `MAKE "sum :sum + ?` with the `:` intact —
    `interpreter.c`'s list-literal scan never treats `:` specially
    either, same as `"`. This lexer's own `:varref` handling strips the
    leading `:` before the token ever reaches `parse_list_literal` (see
    `lexer.c`'s `:` case), and — until this fix — nothing re-added it
    the way the earlier fix did for `LOGO_TOK_QUOTED_WORD`. A `FOREACH`
    accumulator script (`MAKE "sum 0  FOREACH [MAKE "sum :sum + ?] [1 2
    3 4]  PRINT :sum`) silently produced `0` instead of `10`: the
    substituted-and-reparsed statement read back as `MAKE "sum sum + 1`
    each time — an unrelated bareword `sum`, not the running total —
    because the one character telling the runtime re-parse "this is a
    variable read" had already been lost when the template was first
    built. This is the first case that actually exercised a `:varref`
    as list-literal *data* (`MAP`/`FILTER`/`REDUCE`'s own templates
    never read a variable inside the template itself) — caught by
    directly comparing a probe program's output against the real
    interpreter before writing any test, not discovered via a failing
    test. **Fixed** the same way as the quoted-word case: when a list
    literal's element token is `LOGO_TOK_VARREF`, re-prepend the `:` via
    `snprintf(..., ":%.*s", tok->length, tok->text)` instead of the
    plain `token_text_copy`.
  - 6 new `tests/test_eval.c` cases (basic per-element run, a bare
    non-list argument, the accumulator case that caught the `:varref`
    bug, a quoted-word template, early `STOP`, nested-list elements) and
    1 new shadow-diff script covering the same ground. Confirmed clean
    under AddressSanitizer on both `test_eval` and `test_shadow_diff` —
    the new `logo_parse`-on-a-snippet path (heavier than `MAP`/
    `FILTER`/`REDUCE`'s expression/condition-only snippets, since it
    hoists procedures and builds a whole `AST_BLOCK` per iteration)
    introduced no new stack-fragility regression either.
- **Math operators: done** (`MOD`/`SIN`/`COS`/`TAN`/`ASIN`/`ACOS`/
  `ARCTAN`/`LN`/`LOG`/`EXP`, the first batch off `docs/ROADMAP.md`'s new
  Phase 5 Stage 1 checklist). Direct ports of interpreter.c's own
  `parse_factor` cases: `SIN`/`COS`/`TAN` take degrees (converted to
  radians), `ASIN`/`ACOS`/`ARCTAN` return degrees (converted back) —
  matches the turtle-angle convention used throughout, not the C math
  library's native radians. `LOG` is base-10 (`log10`), `LN` is natural
  log. `MOD`'s own `mod_result` helper (result takes the sign of the
  divisor, Python-style, not C `fmod`'s sign-of-dividend truncation) is
  pure/stateless, so it's mirrored directly as `eval_mod_result` in
  `eval.c` rather than exposed through `interpreter.h` — no state to
  share, unlike the turtle/variable/list helpers.
  - **Not a new bug, but confirmed directly rather than assumed**: `MOD 7
    -3` (no parens) fails to parse — the same pre-existing "greedy
    subtraction" call-argument ambiguity already documented for
    `SETXY`/`SETHEADING` (a call argument's own expression parse
    consumes a following `- expr` as subtraction rather than stopping at
    the next argument boundary). `MOD 7 (-3)` parses and runs correctly
    in both engines (confirmed the old engine supports plain arithmetic
    parenthesization generally — a separate, older capability from the
    boolean-grouping-in-conditions feature that's new-engine-only). Test
    scripts use the parenthesized form for negative `MOD` arguments, same
    as the existing `SETXY` test does.
  - 2 new `tests/test_eval.c` cases and 1 new shadow-diff script.
    Confirmed clean under AddressSanitizer on both `test_eval` and
    `test_shadow_diff`.
- **Turtle state queries: done** (`GETX`/`GETY`/`HEADING`/`POS`/
  `CANVASSIZE`/`SETX`/`SETY`/`DISTANCE`/`TOWARDS`, the second batch off
  `docs/ROADMAP.md`'s Phase 5 Stage 1 checklist). Direct ports: `GETX`/
  `GETY`/`HEADING` read `current_turtle(app)` directly (already shared);
  `POS`/`CANVASSIZE` reuse the existing `eval_list_wrap_pair` helper
  `LIST` already built on; `SETX`/`SETY` reuse `move_turtle_to` (already
  shared) holding the other axis fixed; `DISTANCE`/`TOWARDS` needed one
  new pure helper, `eval_list_as_two_numbers` (mirrors interpreter.c's
  own `list_as_two_numbers`), plus `TOWARDS`'s own `atan2`-based compass
  bearing, ported verbatim including its `[0, 360)` normalization.
  - **A third, differently-shaped instance of the `parse_list_literal`
    fidelity gap, found while probing `TOWARDS`/`DISTANCE` against the
    real interpreter** (not by a failing test — the same
    probe-before-testing habit that caught the `:varref` case):
    `[0 -100]` came back as a 3-element list (`0`, `-`, `100`) from this
    engine instead of interpreter.c's genuine 2-element `0 -100`,
    confirmed directly (`COUNT [0 -100]` is `2` in the real interpreter,
    `3` here before the fix). Root cause is related to, but distinct
    from, the earlier two bugs: those were about a lexer token *losing*
    a marker character (`"`, `:`) before `parse_list_literal` ever saw
    it; this one is about the lexer always splitting a *leading sign*
    off as its own `LOGO_TOK_MINUS`/`LOGO_TOK_PLUS` token (needed
    elsewhere, for subtraction/negation), where interpreter.c's own
    list-literal scan has no token concept at all — just a run of
    non-whitespace/non-bracket characters, so a glued sign is always
    part of the following atom there. **Fixed** in `parse_list_literal`:
    when an element token is `LOGO_TOK_MINUS`/`LOGO_TOK_PLUS` and the
    very next token is textually adjacent (checked via the tokens' own
    source pointers — `next->text == tok->text + tok->length` — not just
    type, since there's no lexer-recorded "had whitespace before me"
    flag), merge the sign onto that next token's own rendered element
    text instead of emitting it as a separate element. Confirmed `[0 -
    100]` (real subtraction, whitespace on both sides of the `-`) is
    correctly left as three elements, and a glued `+` (`[0 +5]`) merges
    the same way as `-`. This is the more consequential of the two
    "worth a dedicated audit" bugs flagged after the `FOREACH` milestone
    — negative coordinates in a list literal (`[x -y]`) are far more
    common in real scripts than the quoted-word/varref cases that
    surfaced the first two.
  - 9 new `tests/test_eval.c` cases (including one exercising the
    leading-sign fix directly against `COUNT`/`PRINT`/`FIRST BUTFIRST`,
    plus the "real subtraction stays unmerged" and glued-`+` boundary
    cases) and 1 new shadow-diff script (turtle motion plus queries in
    one script, so the shadow-diff harness's own line-segment/final-
    state comparison exercises the fix too, not just `PRINT` text).
    Confirmed clean under AddressSanitizer on both `test_eval` and
    `test_shadow_diff`.
- **Remaining word/list operators: mostly done** (`PICK`/`FLATTEN`/
  `PARSE`/`SUBST`/`DOT`/`CROSS`, third batch off `docs/ROADMAP.md`'s
  Phase 5 Stage 1 checklist). Direct ports: `PICK` shares `random_below`
  (already shared) across list/word/array/bare-number, matching
  interpreter.c's own fallback exactly; `FLATTEN`/`PARSE`/`SUBST` needed
  three new pure helpers mirroring interpreter.c's own
  `list_flatten_into`/`list_tokenize_words`/`list_subst_into` (all
  `app->list_pool`-based, no new state to expose); `DOT`/`CROSS` are
  plain numeric-list ports, `CROSS` reusing a new
  `eval_list_as_three_numbers` (same shape as `DISTANCE`/`TOWARDS`'s own
  `eval_list_as_two_numbers`).
  - **A design mistake caught before it shipped, not after**: the first
    draft of `eval_list_tokenize_words`/`eval_list_subst_into` returned
    the new list's head node index directly (`-1` meaning "pool
    exhausted, bail out"), copying the shape of other single-purpose
    helpers in this file. But `-1` is also the **legitimate** head of a
    real empty list (e.g. `PARSE` of empty/all-whitespace text, or
    `SUBST` producing an empty result) — collapsing "ran out of pool
    space" and "correctly produced nothing" onto the same sentinel is
    exactly the bug interpreter.c's own versions avoid by returning a
    `gboolean` success flag *and* writing the head through an
    out-parameter. Caught while writing the first draft's own doc
    comment (an unexplainable `if (sub_head < -1) return -1;` check —
    impossible for any real `int`, a sign something upstream didn't add
    up) rather than by a failing test; rewritten to the same
    out-parameter pattern interpreter.c uses before it was ever run.
  - **`TEXT` deliberately deferred, not silently skipped**: interpreter.c's
    own `TEXT` reads a procedure's raw source-text body (via
    `find_procedure`/`Procedure.body`, its private text-based procedure
    storage) and tokenizes it the same way `PARSE` does. This engine's
    own procedures are `AST_PROC_DEF` nodes with no raw source text
    retained anywhere — only the already-parsed tree — so there is
    nothing for `TEXT` to read today. Needs either storing each
    procedure's original source text alongside its AST (new state, not
    yet part of `AstPool`/`AST_PROC_DEF`) or re-serializing the AST back
    to text (lossy, and a much bigger undertaking than this batch's
    other six operators combined) — left as a genuinely open design
    question for a future pass, not bundled into this one.
  - 7 new `tests/test_eval.c` cases and 2 new shadow-diff scripts
    (turtle-state-queries' own script grew by one; a new script covers
    this batch, including `PICK` restricted to single-element
    containers so its outcome is deterministic regardless of which draw
    either engine's shared RNG stream happens to make — same reasoning
    already on record for excluding multi-element `RANDOM`/`PICK`
    scripts from this corpus). Confirmed clean under AddressSanitizer on
    both `test_eval` and `test_shadow_diff`.
- **Variable/procedure introspection: done** (`THING`/`LOCAL`/`NAMES`/
  `PROCEDURES`, fourth batch off `docs/ROADMAP.md`'s Phase 5 Stage 1
  checklist). `THING` is a direct port (shares `find_var`, already
  exposed). `LOCAL` shares `app->scopes`/`scope_depth` directly — the
  same fields `call_ast_procedure` already pushes/pops for every call,
  so a `LOCAL` declared inside an `AST_PROC_DEF`'s body lands in exactly
  the scope frame the caller expects, no new state needed. `NAMES`
  shares `app->variables`/`var_count` (the same globals table
  `find_var`/`set_var` already read/write) directly, so it's also a
  one-to-one port.
  - **`PROCEDURES` needed a genuinely different implementation, not a
    port**: interpreter.c's own `PROCEDURES` walks `app->procedures[]`
    (its private, text-based `Procedure` table, populated as `eval_logo`
    parses each `TO`) — but this engine's own `TO` definitions are
    `AST_PROC_DEF` nodes living in `pool`, never registered into that
    table at all (see the `NEW`/`SEND` milestone's own note on the two
    engines' separate procedure representations). `do_procedures`
    instead walks `pool->nodes[]` directly collecting every
    `AST_PROC_DEF`'s name, the same "search every node in the flat
    array, regardless of nesting" reach `find_proc_def` already uses.
    Confirmed the enumeration order matches interpreter.c's own for
    ordinary top-to-bottom scripts (both engines register a `TO` in
    source order), so shadow-diffing `PROCEDURES`' own `PRINT` output
    works — this isn't guaranteed for every conceivable script shape
    (e.g. hoisting), but held for every case tested.
  - 6 new `tests/test_eval.c` cases and 1 new shadow-diff script
    (`LOCAL` exercised both inside and outside a procedure call, and
    twice with the same name in one call, mirroring interpreter.c's own
    "already local" no-op check). Confirmed clean under AddressSanitizer
    on both `test_eval` and `test_shadow_diff`.
- **Deferred execution and control flow: done** (`RUN`/`APPLY`/`FOR`/
  `FOREVER`/`CATCH`/`THROW`, fifth batch off `docs/ROADMAP.md`'s Phase 5
  Stage 1 checklist).
  - **`RUN`/`APPLY` reuse `FOREACH`'s own re-entrant lex/parse
    machinery**, not new machinery: `do_run` lexes/parses its argument's
    text into a fresh, caller-heap-allocated `ParseResult` and runs it
    through `exec_block`, exactly like a `FOREACH` template does per
    element, guarded by the existing `app->run_depth`/`MAX_RUN_DEPTH`
    fields (already shared, no exposure needed) against a
    self-referential `RUN` blowing the C stack. `APPLY` is a smaller
    departure from interpreter.c's own version than usual: interpreter.c
    resolves its procedure name through `find_procedure` (its private
    text-based table), but this engine resolves through
    `find_proc_def`/`call_ast_procedure` instead — the same pair
    `do_user_procedure_call`/`do_send` already use, since (as the
    `PROCEDURES` milestone already established) this engine's procedures
    were never registered into that table at all. Confirmed directly
    that neither `RUN` nor `APPLY` ever hands a value back to its caller
    (docs/LANGUAGE.md: "deliberately not something this interpreter's
    commands hand back") — both are `void` `do_*` functions, statement
    position only, matching their `BUILTIN_SIGNATURES` entries.
  - **`FOR` needed its own dedicated parse function, not a
    `BUILTIN_SIGNATURES` entry**: its header (`[var start limit step]`)
    is a bareword name followed by 2-3 *expressions* — confirmed
    directly against interpreter.c that the limit/step positions accept
    a full expression, not just a literal (`FOR [i 1 :n + 1] [...]`
    really does work today) — a shape no existing `ArgKind` covers, so
    it gets a new `AST_FOR` node type and `parse_for`, parsed the same
    way `parse_if`/`parse_proc_def` are (see `parser.c`). Unlike
    `FOREACH`'s template, there's no substitution step here, so
    start/limit/step are parsed directly off the live token stream via
    `parse_expr_value` rather than being collected as text and re-lexed
    later. `exec_for` (`eval.c`) evaluates start/limit/step once up
    front (matching interpreter.c, which doesn't re-evaluate them per
    iteration either), defaults a missing step to `+1`/`-1` based on
    direction, and sets the loop variable via a plain `set_var` each
    iteration — no `push_scope`, matching interpreter.c's own
    non-scoping `FOR`/`REPEAT`/`WHILE` blocks exactly.
  - **`FOREVER` is a direct, small port**: `do_forever` mirrors
    `do_while`'s shape with no condition at all, capped by the same
    `MAX_WHILE_ITERATIONS` ceiling.
  - **`CATCH`/`THROW` needed genuinely new unwind plumbing, the one
    piece of this batch with no prior-batch precedent**: shares
    `app->throw_requested`/`throw_tag` directly with interpreter.c (both
    already plain `LogoApp` fields, no exposure needed) but required
    teaching every existing loop (`exec_block`, `do_repeat`, `do_while`,
    plus this batch's own `do_forever`/`exec_for`) to also break on
    `throw_requested`, not just `stop_requested` — otherwise an
    uncaught `THROW` inside a loop body would keep iterating right past
    it. `do_catch` runs its block then clears `throw_requested` only if
    it's still set *and* its tag matches this `CATCH`'s own tag; a
    non-matching tag is deliberately left set so it keeps propagating
    toward whichever ancestor `CATCH` (if any) matches, exactly
    mirroring interpreter.c.
  - **`ast_eval` itself had to change shape**, not just gain a check:
    interpreter.c's `eval_logo` is reused at every nesting depth, with an
    `eval_depth` counter distinguishing the genuine outermost call (which
    recovers from an uncaught `THROW` — reports it, clears the flag, and
    lets the *rest* of the top-level script keep running) from every
    inner call (which just breaks, letting the throw keep propagating).
    This engine has no equivalent counter — `ast_eval` is the one true
    top level, and every nested execution goes through the plain
    `exec_block` used for loop bodies/procedure bodies/`RUN`'d chunks
    instead. So `ast_eval` was rewritten from a single `exec_block` call
    into its own per-statement loop that checks `throw_requested` after
    *each* top-level statement and recovers right there, while
    `exec_block` (used everywhere else) just breaks on it with no
    recovery — reproducing interpreter.c's `eval_depth == 1` vs. `> 1`
    split without needing a counter, since there's structurally only one
    place `ast_eval` itself runs.
  - Ground-truth-verified directly against a standalone probe binary
    linked against `interpreter.c` before writing any permanent test
    (this project's usual habit) — every case matched byte-for-byte,
    including `FOR`'s `step must not be 0` error, `APPLY`'s "no such
    procedure"/"wrong number of inputs" errors, `RUN`'s "too deeply
    nested" cap, and both matched- and unmatched-tag `CATCH`/`THROW`
    paths. One case was *not* carried into the permanent suite: `PRINT
    APPLY ...` (using `APPLY`/`RUN` in expression position) turned out to
    be an old-engine parsing accident, not a designed behavior —
    interpreter.c's `parse_factor` doesn't recognize `APPLY`/`RUN` as
    operators at all, so `strtod` silently returns `0` without consuming
    any input, and the *same* `APPLY ...` text then runs a second time as
    the next top-level statement. Reproducing that specific accident
    would mean copying a bug, not a behavior, so it's left untested on
    both sides rather than shadow-diffed.
  - 15 new `tests/test_eval.c` cases and 3 new shadow-diff scripts.
    Confirmed clean under AddressSanitizer on both `test_eval` and
    `test_shadow_diff`.
- **`TELL`/multi-turtle: done** (sixth batch off `docs/ROADMAP.md`'s
  Phase 5 Stage 1 checklist). Turned out to need far less new state
  exposure than that checklist entry originally guessed: `ROADMAP.md`
  had flagged `app->turtles[]`/`turtle_count`/`MAX_TURTLES`/
  `init_turtle` as needing fresh exposure from `interpreter.c`, but all
  four were already public — `turtles[]`/`turtle_count` are plain
  `LogoApp` struct fields (no `static` involved, nothing to remove),
  `MAX_TURTLES` is already a `#define` in `logo_types.h`, and
  `init_turtle` was already exposed in `interpreter.h` (used directly
  by every test file's own `new_app()`). `do_clear` was already reading
  `app->turtle_count` directly, confirming this in passing before
  `TELL` itself was even written. So this batch is just two new direct
  ports, `do_tell`/`do_who`, and — because every existing turtle
  command (`do_fd`, `do_setxy`, `do_home`, `do_clear`, ...) already goes
  through `current_turtle(app)`/`app->turtle_count` — those needed zero
  changes to already work correctly with more than one turtle the
  moment `TELL` could actually make a second one current.
  - **Found and fixed a real gap in the shadow-diff harness itself, not
    in the evaluator**: `TurtleSnapshot`/`snapshot_turtle` only ever
    captured `turtles[0]`, so a `TELL`-based script's divergence on
    `turtle_count` or any non-current turtle's own state would have
    passed silently — exactly the kind of blind spot `lines_match` was
    already built to close for drawn paths, just not yet done for
    multi-turtle state. Extended `TurtleSnapshot` to arrays sized
    `MAX_TURTLES` and the comparison to walk every turtle up to
    `turtle_count`, confirmed still passing on every pre-existing
    single-turtle script (`turtle_count` is always `1` there, so the
    loop degenerates to the old single-turtle check exactly).
  - 5 new `tests/test_eval.c` cases (including confirming a script that
    never calls `TELL` still has `turtle_count == 1`, matching
    `docs/LANGUAGE.md`'s own stated guarantee) and 1 new shadow-diff
    script exercising `TELL`/`WHO`, out-of-range `TELL`, and `HOME`
    only resetting whichever turtle is current. Confirmed clean under
    AddressSanitizer on both `test_eval` and `test_shadow_diff`.
- **File I/O: mostly done** (seventh batch off `docs/ROADMAP.md`'s
  Phase 5 Stage 1 checklist): `OPENREAD`/`OPENWRITE`/`OPENAPPEND`/
  `READLINE`/`EOF?`/`DIRECTORY`/`CLOSE`/`FILEPRINT`/`DELETEFILE`/`LOAD`.
  Direct ports sharing `app->file_channels[]` directly (a plain
  `LogoApp` field) plus the newly-exposed `find_free_file_channel` (the
  one bit of channel-slot logic worth sharing rather than re-deriving,
  same reasoning as `find_var`/`find_plist_entry`). `DELETEFILE`/`LOAD`'s
  path argument is `ARG_QUOTED_WORD`, not `ARG_EXPR` — matching
  interpreter.c's own raw `sscanf("%s")`-plus-leading-quote-check
  convention for these two, the same restriction `MAKE`'s varname
  already has. `SAVE` is NOT part of this batch (see below).
  - **A real, severe architectural limitation discovered while testing
    `LOAD`, not a narrow fidelity bug**: interpreter.c's `LOAD` works
    like Terrapin's own — the *main* documented use case
    (`docs/LANGUAGE.md`: "write/edit a script externally, LOAD it, test,
    repeat") is loading a file of `TO` definitions and then calling them
    from the script that did the loading. This works in interpreter.c
    because `eval_logo` parses and executes one statement at a time —
    by the time a `LOAD`'d file's own nested `eval_logo` call registers
    a procedure into `app->procedures[]`, a later call to it in the
    *same* top-level script (parsed and run afterward, in sequence)
    finds it immediately. This engine parses its entire top-level script
    **once, up front**, before executing anything (hoisting procedure
    arities via a pre-pass over the whole token stream, see parser.c) —
    so a call to a `LOAD`'d procedure fails to resolve at *parse time*,
    long before `do_load` would ever run, regardless of what `LOAD`
    itself does at runtime. The result isn't a runtime "I don't know how
    to X" — it's a parse error, and per this engine's own
    error-collection design (`ParseResult.error_count`), that means the
    **entire script fails to run at all**, not just the one call.
    Confirmed directly: the same script (define a procedure via `LOAD`,
    then call it) runs correctly end-to-end in interpreter.c, and fails
    to parse at all here — verified with a standalone probe before
    writing anything permanent, not assumed.
    `LOAD` still works correctly for everything that doesn't cross that
    boundary: a loaded file's own statements execute (`PRINT`, `MAKE`,
    turtle motion), including defining and calling its own procedure
    *from within itself* (confirmed byte-for-byte identical against
    interpreter.c) — `do_load` reuses `RUN`/`FOREACH`'s own re-entrant
    lex/parse-into-a-scratch-`ParseResult` machinery, exec_block'd as a
    nested call (not a fresh top-level recovery point, exactly mirroring
    interpreter.c's own `LOAD` being a plain nested `eval_logo` call). A
    dedicated `test_eval.c` case
    (`test_load_defined_procedure_is_not_callable_from_the_loading_script`)
    pins the current (parse-error) behavior down as a documented,
    intentional gap rather than leaving it silently unverified — this is
    a genuinely bigger, harder problem than `TEXT`/`SAVE`'s "no source
    text retained" gap: fixing it for real would mean teaching the
    parser to eagerly follow `LOAD` calls with a literal path argument
    during its own hoisting pre-pass, recursively reading and hoisting
    the target file's procedures before the main script's own calls are
    resolved — a real, `#include`-shaped feature, out of scope for this
    batch.
  - **`SAVE` deferred alongside `TEXT`, not attempted**: `serialize_procedures`
    walks interpreter.c's own `app->procedures[]` table (each entry's
    already-known body text) — the exact same table `PROCEDURES`/`TEXT`
    already couldn't use, since this engine's `TO` definitions are
    `AST_PROC_DEF` nodes that never touch it. Same open design question,
    not a new one.
  - 14 new `tests/test_eval.c` cases (mirroring `tests/test_interpreter.c`'s
    own file I/O test shapes closely, real file I/O against `build/`'s
    already-gitignored scratch space) and 2 new shadow-diff scripts — one
    exercising the general file I/O operators fully self-contained
    (write/read/append/delete all in one script, safe to run through
    both engines in sequence since each engine's own `OPENWRITE`
    truncates fresh), one exercising `LOAD` restricted to the confirmed-
    agreeing case only (a loaded file's own statements, not a cross-
    boundary procedure call). Confirmed clean under AddressSanitizer on
    both `test_eval` and `test_shadow_diff`.
- **Drawing/canvas primitives: done** (eighth batch off
  `docs/ROADMAP.md`'s Phase 5 Stage 1 checklist, and — alongside a
  fixed miscategorization, see below — the last "new coverage" batch on
  that checklist): `ARC`/`LABEL`/`FILL`/`ERASERECT`/`WRAP`/`FENCE`/
  `WINDOW`/`CLEAN`/`HIDETURTLE`/`SHOWTURTLE`/`SETPENCOLOR`/
  `SETPENWIDTH`/`SETBACKGROUND`/`SETCANVASSIZE`, plus `ERASE` (bundled
  into this batch per `docs/ROADMAP.md`'s own listing, though it's
  actually procedure deletion, not a drawing primitive — see below).
  - **Smaller than it looked at a glance**: `WRAP`/`FENCE`/`WINDOW` are
    one-line `app->edge_mode` setters — the real behavior lives in
    `move_turtle_to`, already shared with interpreter.c since the very
    first turtle-motion batch. `ARC` needed one new exposure
    (`record_line`, the helper `move_turtle_to` itself already uses
    internally, now public since `ARC` deliberately bypasses
    `move_turtle_to`'s own position-tracking to draw around the turtle
    without moving it). `LABEL`/`FILL`/`ERASERECT` are direct ports
    writing into `app->labels[]`/`raster_ops[]` — already plain, public
    `LogoApp` fields, same as `lines[]` before them. `SETPENCOLOR`/
    `SETPENWIDTH`/`SETBACKGROUND`/`SETCANVASSIZE` needed one new shared
    header constant pair (`MIN_PEN_WIDTH`/`MAX_PEN_WIDTH`, moved from a
    private `interpreter.c` `#define` into `logo_types.h` so both
    engines clamp to the identical bounds, not a hand-copied duplicate
    that could drift) plus two small pure local mirrors of
    interpreter.c's own `clamp01`/`clamp_range` (stateless, so kept
    local rather than exposed, same reasoning as `eval_mod_result`).
  - **`ERASE` is procedure deletion, not a drawing primitive — a real
    miscategorization in `docs/ROADMAP.md`'s own original batch listing,
    now fixed there rather than perpetuated**: interpreter.c's `ERASE`
    physically removes an entry from `app->procedures[]`, shifting
    later entries down. This engine has no such array — `TO`
    definitions are `AST_PROC_DEF` nodes in `pool`, found by name via
    `find_proc_def`'s own linear scan — so "deleting" one means making
    it permanently unmatchable instead: `do_erase` blanks the found
    node's own `.text` to the empty string, which can never equal a
    real call's name (an `AST_CALL` node's own `.text` is always
    non-empty). `find_proc_def` then naturally reports "I don't know
    how to X" for a later call, with no changes needed there; `PROCEDURES`
    needed one small update (skip a blank-text entry) so an erased
    procedure stops appearing in its own output too, matching
    interpreter.c exactly.
  - **A real gap found and closed in the shadow-diff harness itself**,
    the same pattern as the `TELL`/multi-turtle milestone: the harness
    had never compared pen color/width/visibility (nothing in the
    corpus varied them before this batch), nor `LABEL`/`FILL`/
    `ERASERECT`'s own recorded data, nor background color/canvas size/
    edge mode at all — every one of those could have silently diverged
    between engines with no test ever catching it. Extended
    `TurtleSnapshot` with pen color/width/visibility fields, added
    `labels_match`/`raster_ops_match` alongside the existing
    `lines_match`, and added direct `bg_r/g/b`/`canvas_width/height`/
    `edge_mode` comparisons to `shadow_diff_pair` itself.
  - Ground-truth verified via a standalone dual-engine probe comparing
    every relevant `LogoApp` field directly (not just `PRINT`-visible
    output) before writing any permanent test — byte-for-byte identical
    across every operator, including `SETCANVASSIZE`'s error path and
    `FENCE`'s edge-clamping. The one divergence found was already-known,
    not new: `SETPENCOLOR 300 -10 128` (no parens) hits the same
    pre-existing "greedy subtraction" call-argument ambiguity already
    documented for `SETXY`/`SETHEADING`/`MOD` — worked around with
    `SETPENCOLOR 300 (-10) 128`, matching those tests' own convention.
  - 26 new `tests/test_eval.c` cases and 2 new shadow-diff scripts (one
    for the drawing/canvas primitives, one for `ERASE`). Confirmed clean
    under AddressSanitizer on both `test_eval` and `test_shadow_diff`.
- **`TEXT`/`SAVE`: done** (ninth batch, and — with this — every batch on
  `docs/ROADMAP.md`'s Phase 5 Stage 1 checklist has now landed). The
  open design question was real, not a formality: both need a
  procedure's *original* source text, and re-serializing the AST back
  into text risked exactly the fidelity mismatch `TEXT` can't afford —
  `docs/LANGUAGE.md` is explicit that a body's literal punctuation
  survives untouched (`PRINT "hi` tokenizes to a word whose text is
  literally `"hi`, quote mark included), which a "pretty-printer"
  reconstruction wouldn't reliably reproduce.
  - **The actual fix: capture the raw source span at parse time, not a
    re-derived rendering.** `AstNode` gained two new fields,
    `body_text`/`body_len` (`AST_PROC_DEF` only) — a pointer *into the
    original source buffer* plus a byte length, the same "the source
    text outlives the parse" convention `LogoToken.text` itself already
    uses, deliberately NOT an embedded `char[]` the way `.text`/
    `.param_names` are (interpreter.c's own `Procedure.body` is
    `char[8192]`; inlining that into every `AstNode` — most of which
    are nowhere near `AST_PROC_DEF` — would have bloated `AstPool`'s
    already-multi-MB fixed array by roughly `MAX_AST_NODES * 8KB` for no
    benefit). `parse_proc_def` captures it for free: `body_text` is
    just the position of the first body token (already exactly where
    interpreter.c's own `ptr` sits after its params-parsing loop, since
    the lexer already skips whitespace between tokens the same way
    `skip_whitespace` does), and `body_len` is the byte distance to the
    closing `END` token's own position (exactly where interpreter.c's
    `strcasestr(ptr, "END")` would land). No copying, no fixed cap on
    procedure body length.
  - **`TEXT` reuses `eval_list_tokenize_words` directly against that
    span** — the same whitespace-tokenizing core `PARSE` already used,
    generalized from a NUL-terminated-string signature to an explicit
    `(text, text_len)` one so it works on a non-NUL-terminated span
    straight out of the source buffer, with `do_parse`'s own call site
    updated to pass `strlen(text)`.
  - **`SAVE` needed one more real fix, caught by direct comparison, not
    guessed**: a first-draft `do_save` matched `append_procedure_text`'s
    own rendering (`"TO name :params\n" + body + "\nEND\n"`) but missed
    that `serialize_procedures` appends *one more* `'\n'` per procedure
    on top of that, in its own separate loop step — invisible unless
    you diff actual SAVE output byte-for-byte, which a probe comparing
    a real multi-procedure `SAVE` against interpreter.c's own did before
    this shipped (missing without it: no blank line between procedures,
    and none after the last one). Fixed by adding the matching extra
    `'\n'` in `do_save`'s own loop.
  - **A real, pre-existing doc-comment error found and fixed along the
    way, unrelated to the bug above**: `ast.h`'s own comment on
    `param_names` claimed it matched interpreter.c's `Procedure.param_names`
    convention — but interpreter.c's own raw `sscanf`-based capture
    keeps the leading `:` (confirmed directly in its own TO-parsing and
    `append_procedure_text`), while this engine's `LOGO_TOK_VARREF`
    already strips it at the lexer level, same as an ordinary
    `:varref`. Harmless until `SAVE` needed to reconstruct
    `"TO name :params"` text — the one place the difference actually
    matters — so `do_save` adds the `:` back explicitly, and the stale
    comment is corrected rather than left to mislead the next reader.
  - **The `LOAD`-cross-boundary gap (see the File I/O milestone above)
    is orthogonal, confirmed rather than assumed**: `SAVE` produces
    genuinely valid, later-`LOAD`-able Logo source (a fresh `LOAD` of a
    `SAVE`'d file succeeds with no parse error, and a procedure defined
    within a `LOAD`'d file can still call itself), but a *round trip
    within one script* (`SAVE`, then `LOAD` the same file back and call
    what it defines) still hits the exact same parse-time limitation
    already documented for `LOAD` generally — checked directly before
    writing the test, not assumed, and captured as its own test case
    rather than silently skipped.
  - **`SHOW` added too, once `TEXT`/`SAVE` made it trivial**: a genuine
    gap in `docs/ROADMAP.md`'s own original coverage diff (never listed
    anywhere, not even as deferred — the same class of miss as the
    `ERASE`/`WINDOW` mistakes already caught and fixed in earlier
    batches). Confirmed real by actually running `examples/text.logo`
    through `bin/logi`, which uses `SHOW` alongside `TEXT` and failed
    immediately with "unknown word: SHOW". `SHOW "name` prints one
    procedure's own `TO...END` definition, the exact same rendering
    `SAVE` already builds for every procedure — so implementing it was
    a small, mechanical extraction: the per-procedure rendering logic
    moved out of `do_save`'s own loop into a shared
    `eval_append_procedure_text` helper, and `do_show` just calls it for
    the one procedure `find_proc_def` resolves (reporting `SHOW: no
    such procedure "name` otherwise, including for an `ERASE`'d one,
    for free — same blank-`.text`-is-unmatchable mechanism as
    everywhere else). Ground-truth verified byte-for-byte against
    interpreter.c before writing anything permanent, including the
    "expected a \"word" parse-time-vs-runtime-message difference every
    other `ARG_QUOTED_WORD` builtin already has. `examples/text.logo`
    now runs end-to-end through `bin/logi` with no divergence at all.
  - 19 new `tests/test_eval.c` cases total (mirroring
    `tests/test_interpreter.c`'s own `TEXT`/`SHOW`/`SAVE`/`LOAD` test
    shapes closely) and 3 new shadow-diff scripts — one for `TEXT`
    (fully `PRINT`-observable, so an ordinary `shadow_diff`), one for
    `SHOW` (likewise), one for `SAVE` (a dedicated direct file-content
    comparison, since `shadow_diff` itself only ever compared captured
    output/turtle state/drawn data, never arbitrary file content).
    Confirmed clean under AddressSanitizer on both `test_eval` and
    `test_shadow_diff`.
- **`LOAD` cross-boundary-call gap: fixed for real** (not a Stage 1
  coverage batch — a genuine architectural fix to the one real
  limitation the File I/O milestone above left open, at the user's
  explicit request after discussing the tradeoffs). The root cause was
  real, not superficial: this engine's `logo_parse` parses and hoists
  the entire top-level script once, up front, before executing
  anything — so a call to a procedure a `LOAD`'d file defines could
  never resolve, no matter what `do_load` does at runtime, because the
  caller's own parse (and its own "unknown word" error) would already
  be finished long before `do_load` ever ran. interpreter.c never had
  this problem because `eval_logo` parses and executes one statement at
  a time, registering a `LOAD`'d procedure into `app->procedures[]` the
  moment its own nested `eval_logo` call reaches it — any later
  top-level statement in the same script, parsed afterward in sequence,
  already finds it.
  - **The fix: teach the hoisting pre-pass to eagerly follow a `LOAD
    "literal-path` call** (a real, if narrow, `#include`-shaped
    feature — LOAD's own `ARG_QUOTED_WORD` grammar means a literal
    quoted-word path is the *only* argument shape `LOAD` can ever
    syntactically take in valid Logo, in either engine, so there's no
    "computed path" case left over to worry about). `ast.h`'s
    `AstNode` gained `body_text`/`body_len` in the previous milestone
    for `TEXT`/`SAVE`; this fix needed the analogous move for the
    *parser* itself: `ParseResult` gained `eager_loaded_sources[]`
    (`MAX_EAGER_LOADS`, a fixed cap, same "loud stop, not silent
    unbounded growth" policy as every other fixed table here) — owned
    source-text buffers, read via plain `fopen`/`fread` (not
    `g_file_get_contents`, keeping parser.c's own "no GTK/GLib
    dependency" promise intact) so a `LOAD`'d file's own `AstNode`
    pointers have something stable to point into for as long as the
    pool referencing them is alive. This meant every one of the 18
    call sites across this codebase that used to `free()` a
    `ParseResult` directly now has to release those buffers too — a new
    `parse_result_destroy` (parser.h/parser.c) replaces every bare
    `free(result)`/`free(scratch)`, eval.c's own scratch-`ParseResult`
    reuse (`do_map`/`do_filter`/`do_reduce`/`do_foreach`/`do_run`/
    `do_load`) included.
  - **A real forward-reference bug, caught before it ever shipped, not
    guessed**: the first draft built a `LOAD`'d file's own real
    `AST_PROC_DEF` nodes *during* the hoisting pre-pass itself, the
    moment its own `TO` was found — but at that point `p->hoisted[]`
    isn't complete yet, so a loaded file's procedure calling *another*
    one defined later in the same file (or in a different loaded file,
    or in the outer script) would spuriously fail to parse as "unknown
    word," even though the callee genuinely does get hoisted
    eventually — just not yet. The forward-reference freedom an
    ordinary top-level `TO` already has (confirmed
    `test_procedure_forward_reference`'s own precedent) has to extend
    to a `LOAD`'d one too, or this fix would be trading one broken case
    for another. Fixed by genuinely splitting the work into two passes:
    pass 1 (`hoist_from_tokens`/`eager_follow_load`) only ever
    discovers names/arities and reads+stores every reachable `LOAD`'d
    file's content, recursing through nested `LOAD`s; pass 2
    (`build_eager_procedures`) runs only once pass 1 has finished
    *everywhere*, re-lexing each stored buffer and building its own
    real `AST_PROC_DEF` nodes then, with `p->hoisted[]` finally whole.
    Caught by deliberately writing a forward-reference test
    (`a` calling `b`, `b` defined later in the same loaded file) before
    considering the feature done, not by accident.
  - **Recursion, bounded the same way `MAX_RUN_DEPTH` already is**: a
    `LOAD`'d file calling `LOAD` itself is exactly as valid as the
    top-level script doing so, capped by `MAX_EAGER_LOAD_DEPTH` (8)
    against a self- or mutually-referential chain — a real limit, not a
    soft guess, same reasoning as every other depth cap in this
    codebase. The token buffer eager-following needs is heap-allocated,
    never a stack local, specifically *because* this recurs — the same
    class of mistake already on record in the
    `eval_logo_recursion_margin` memory.
  - Ground-truth verified directly against a standalone probe linked to
    `interpreter.c` before writing anything permanent — byte-for-byte
    identical, including the forward-reference case and a two-level
    recursive `LOAD` chain. 5 new `tests/test_eval.c` cases (the old
    `test_load_defined_procedure_is_not_callable_from_the_loading_script`
    flipped to confirm the fix instead of the bug, plus recursion and
    both directions of forward-reference) and 3 new shadow-diff scripts
    (one updated comment removing the old "deliberately not exercised"
    caveat that's no longer true). Confirmed clean under
    AddressSanitizer on `test_eval`, `test_shadow_diff`, *and*
    `test_parser` (the binary that most directly exercises the new
    parse-time file I/O and recursion).
- **Next**: every batch on `docs/ROADMAP.md`'s Phase 5 Stage 1 checklist
  has now landed, and the `LOAD` cross-boundary-call gap that used to
  be the one open item is fixed for real. Ask the user what to tackle
  next — Stage 2 (the bytecode VM itself) is the next genuinely large
  bet on `docs/ROADMAP.md`, but nothing here assumes that's the choice.
- **Stage 2 vertical slice: done** (`docs/ROADMAP.md`'s "Phase 5, Stage
  2" checklist, item 1). Three new source pairs, no dependency changes
  to Stage 1's own files beyond a couple of exposed helpers:
  - `src/bytecode.h`/`src/bytecode.c` — the instruction format (a flat
    array of tagged `Instr` structs, not packed bytes — same "flat
    struct, some unused space per node, in exchange for simplicity"
    convention `ast.h`'s own `AstNode` already established; none of
    this project's own four VM motivations depend on compact byte
    encoding). Conditions are ordinary values on the same stack as
    everything else (`OP_CMP_*`/`OP_AND`/`OP_OR`/`OP_NOT` all push
    `num_val(0)`/`num_val(1)`, consumed by `OP_JUMP_IF_FALSE` via
    `eval_is_truthy`) — confirmed directly in `eval_condition`'s own
    code that `AND`/`OR` never short-circuit here, so no fused
    compare-and-jump instructions are needed. Every call (builtin or
    procedure) always leaves exactly one value on the stack, mirroring
    `exec_call`'s own `*result`-defaults-to-`num_val(0)` convention —
    a statement-position call just gets a trailing `OP_POP`.
  - `src/compiler.h`/`src/compiler.c` — `compile_program`, structured
    almost node-for-node like `eval.c`'s own tree-walking functions
    (`compile_expr`/`compile_condition`/`compile_statement`/
    `compile_block` parallel `eval_expr`/`eval_condition`/
    `exec_statement`/`exec_block`). Procedure calls need genuine
    backpatching, not just "compile procedures before top-level code":
    one procedure's body can call another not yet compiled (pass 1
    walks the AST pool in array order, not call order), so an
    `OP_CALL_PROC` to an unresolved name gets a placeholder target and
    a pending-patch entry, resolved once every procedure's own address
    is known — the same shape as the `LOAD` fix's own hoist-then-build
    split, one layer down (addresses instead of names/arities).
    `compile_call` only recognizes `PRINT`/`OUTPUT`/`STOP`/`WHILE`/
    `MAKE` by name; everything else is assumed a user procedure — safe
    for this slice (the parser itself already guarantees any other
    resolved call name is a hoisted procedure), but `compile_call`'s
    own builtin dispatch needs to become a real lookup, not an
    if-chain, once instruction coverage grows past this batch.
  - `src/vm.h`/`src/vm.c` — `vm_run`'s dispatch loop is a flat `switch`
    over an explicit `pc`, not recursive C calls: a Logo-level call
    becomes a `VmFrame` push (`{return_pc, value_stack_base}`) and a
    `pc` jump, not a new C stack frame, decoupling Logo-level recursion
    depth from C stack depth for the first time in this project.
    **Variable *bindings* still reuse `app->scopes[]`/`scope_depth`/
    `MAX_SCOPE_DEPTH` (200) completely unchanged**, via a new shared
    `eval_push_scope_for_call` (the scope-push half of
    `call_ast_procedure`'s own setup, factored out so both engines call
    the same code) — a deliberate, explicitly-flagged scope narrowing
    for this slice: real recursion-depth independence needs VM-owned
    scope storage, not done here. Every opcode handler is a checked
    replica of the matching `eval.c` logic, including
    `eval_condition`'s own `AST_COMPARE` non-numeric fallback
    (`eval_values_equal` for `EQ`/`NE`, 0 otherwise) and `eval_expr`'s
    own `AST_CALL`-in-expression-position "didn't output a value"
    diagnostic (`OP_CHECK_OUTPUT`, driven by a `last_call_produced_output`
    flag `OP_OUTPUT`/`OP_STOP` set on every return).
  - `do_print` in `eval.c` split into a value-taking core
    (`eval_print_value`, exposed via `eval.h`) so `vm.c`'s
    `OP_CALL_BUILTIN "PRINT"` handler shares it instead of a parallel
    reimplementation — the template every future builtin port will
    follow, since every existing `do_*` function is tree-walker-shaped
    (calls `eval_expr` on its own AST argument internally) and doesn't
    fit a stack machine's "argument already evaluated, sitting on the
    stack" calling convention.
  - **A real bug caught by the shadow-diff corpus, not by inspection**:
    `exec_call_proc`'s failure paths (recursion too deep, unknown
    procedure, VM frame-stack overflow) pushed a placeholder result but
    never advanced `pc` — so a failing `OP_CALL_PROC` re-executed
    itself forever. Invisible on every test that only calls procedures
    successfully; `test_recursion_depth_cap_reports_error_not_a_crash`
    (deliberately unbounded recursion, ported from `test_eval.c`'s own
    case of the same name) hung the test binary at 100% CPU until
    caught. Fixed by advancing `pc` past the failed call on every
    failure path, same as the success path's own jump.
  - `tests/test_vm.c` (`make test-vm`, part of `make test`): 15 cases,
    shadow-diffed against `ast_eval` (not `eval_logo` directly, per
    this document's own migration strategy) — literals/arithmetic/
    grouping, `MAKE`/varref, `IF`/`IFELSE`, `AND`/`OR`/`NOT`,
    case-insensitive word equality, `WHILE`, procedures with `OUTPUT`
    (including forward-referenced and mutually-recursive, exercising
    the backpatcher directly), ordinary recursion, the recursion-depth
    cap, and a procedure that never calls `OUTPUT` used in expression
    position. Confirmed clean under AddressSanitizer. All 6 test
    suites (`test_interpreter`/`test_lexer`/`test_parser`/`test_eval`/
    `test_shadow_diff`/`test_vm`) pass via `make test`.
  - **Next**: `docs/ROADMAP.md`'s Stage 2 checklist item 2 — grow
    instruction coverage the same incremental way Stage 1 grew
    `BUILTIN_SIGNATURES`, each batch shadow-diffed against `ast_eval`
    before moving on.
- **Lists/arrays/`MAKE`-adjacent ops: done** (first batch off Stage 2's
  own instruction-coverage checklist item, 2026-08-09). Covers list
  literals, `FIRST`/`BUTFIRST`/`LAST`/`BUTLAST`/`COUNT`/`EMPTY?`/
  `FPUT`/`LPUT`/`WORD`/`SENTENCE`/`SE`/`LIST`, `ARRAY`/`ITEM`/
  `SETITEM`/`FILLARRAY`, `THING`/`LOCAL`/`NAMES`.
  - **`find_proc_def` moved from `eval.c` to `ast.h`/`ast.c`.** It only
    ever touched `AstPool`, never `LogoApp` — despite `eval.c` being
    its main caller, it belongs in the AST-only module (same "an X is
    just data" reasoning as the rest of that file), and the move lets
    `compiler.c` (deliberately `interpreter.h`-free) call it directly
    rather than duplicating the scan or pulling in `eval.h`'s whole GTK
    dependency chain for one pool-only function.
  - **`compile_call`'s builtin-vs-procedure dispatch is now a real
    lookup, not a growing if-chain** — exactly the generalization
    `compiler.h`'s own file comment flagged as necessary once coverage
    grew past the vertical slice's PRINT-only special case.
    `find_proc_def(pool, name) >= 0` means a user procedure
    (`OP_CALL_PROC`, with backpatching as before); everything else is
    assumed a builtin (`OP_CALL_BUILTIN`, name + argc, no arity
    checking needed here — the parser already guarantees it). Growing
    instruction coverage further now only touches `vm.c`'s own
    `call_builtin` dispatch (one `strcasecmp` branch per builtin,
    forwarding to `eval.c`'s exposed value-taking core) and `eval.c`
    itself — `compiler.c` doesn't need to change again for an ordinary
    value-returning builtin.
  - **`OP_PUSH_LIST_LITERAL` is a deliberate exception to every other
    opcode's self-contained-payload shape**: its `.a` holds the
    `AST_LIST_LITERAL` node's own index in the `AstPool`, not a flat
    value. A list literal's contents are recursive, variable-arity raw
    AST data (untyped words, possibly nested literals), not something
    any `Instr` field could hold — so the compiler leaves it in the
    AST, and the VM (which already carries a live `AstPool` reference
    for `OP_CALL_PROC`'s own `find_proc_def` lookups) builds the actual
    list fresh each time the instruction runs, via a new
    `eval_build_list_literal` extracted from `eval_expr`'s own
    `AST_LIST_LITERAL` case (exposed through `eval.h`, called by both).
  - **A real, unplanned fidelity gap this generalization surfaced, not
    guessed**: a void builtin or special form (`MAKE`/`LOCAL`/`WHILE`/
    `SETITEM`/`FILLARRAY`) used in *expression* position needs to
    report `"<name>: didn't output a value"` too — `eval_expr`'s own
    generic `AST_CALL` wrapper does this for any `do_*` that leaves
    `produced` at its default 0, not just a user procedure that never
    calls `OUTPUT`. The vertical slice's `OP_CHECK_OUTPUT` only ever
    fired after `OP_CALL_PROC`, so `PRINT PRINT 5`-shaped expressions
    (any void builtin used for its "value") would have silently
    diverged from `ast_eval` the first time such a test existed. Fixed
    generally: `vm->last_call_produced_output` (previously set only by
    `OP_CALL_PROC`'s own return path) is now also set by
    `OP_CALL_BUILTIN` (via a `*produced` out-parameter on the new
    `call_builtin` dispatch — 1 for a real value, 0 for a void one like
    `SETITEM`/`FILLARRAY`) and by a new `OP_VOID_RESULT` opcode (pushes
    `num_val(0)` and clears the flag) that `MAKE`/`LOCAL`/`WHILE`'s own
    compiled shape now ends with — and `compile_call` emits
    `OP_CHECK_OUTPUT` uniformly after *every* call form in expression
    position (a shared `finish_call` helper), not just procedure calls.
    Locked in with a dedicated test
    (`test_setitem_used_in_expression_position_reports_error`), not
    left as a documented-but-unverified gap.
  - **A real, pre-existing bug caught along the way, not introduced by
    this batch**: `do_butlast`'s own WORD/bare-number cases (strip the
    last character; empty list) got dropped in an early pass of this
    refactor when `eval_list_butlast` (the LIST-only helper it already
    delegated to for lists) was mistaken for `do_butlast`'s *entire*
    body. Caught immediately by `make test` — `BUTLAST "hello` started
    returning nothing instead of `"hell"` — fixed by adding the actual
    missing piece, a new `eval_butlast_value` wrapper with the full
    three-way type dispatch (mirroring `eval_first_value`/
    `eval_last_value`'s own shape), with `eval_list_butlast` staying
    as its LIST-only inner helper. A reminder that "this do_* already
    delegates to a value-taking helper" has to be checked per branch,
    not assumed from one matching case.
  - 18 new `tests/test_vm.c` cases, ported directly from
    already-confirmed `test_eval.c` scripts (riding on already-verified
    ground truth rather than fresh guesses) — list literals (flat and
    nested), FIRST/BUTFIRST/LAST/BUTLAST on both lists and words,
    COUNT/EMPTY?, FPUT/LPUT, WORD/SENTENCE/LIST, a list passed as a
    procedure argument and returned via OUTPUT, list equality in a
    condition, ARRAY/ITEM/SETITEM/FILLARRAY (including array aliasing
    through MAKE), the SETITEM-in-expression-position diagnostic,
    THING/LOCAL/NAMES. Confirmed clean under AddressSanitizer; all 6
    test suites pass via `make test`.
  - **Next**: continue `docs/ROADMAP.md`'s Stage 2 instruction-coverage
    checklist — property lists, turtle/drawing commands, or whatever
    batch the user picks next, each shadow-diffed against `ast_eval`
    the same way.
- **Property lists / `NEW`: done** (second batch off Stage 2's
  instruction-coverage checklist item, 2026-08-09, user said "do
  next"). `SETPROP`/`GETPROP`/`REMOVEPROP`/`PROPLIST`/`NEW`, sharing
  `app->plist_entries` directly with `ast_eval`'s own property-list
  operators, same as always.
  - **The previous batch's dispatch generalization paid off
    immediately**: `eval_getprop`/`eval_setprop`/`eval_removeprop`/
    `eval_proplist` were *already* plain `(app, EvalValue...) ->
    EvalValue`/`void` functions (their own `do_*` wrappers already just
    called `eval_expr` then forwarded) — this batch needed zero new
    `compiler.c` changes and zero new opcodes, just `static` removed,
    declarations added to `eval.h`, and four new branches in `vm.c`'s
    `call_builtin`. `NEW` needed one small addition: a new
    `eval_new_declare(app, obj_val, proto_val)` wrapping `eval_setprop`
    with the `"prototype"` key, so that string literal lives in one
    place instead of being duplicated between `do_new` and `vm.c`.
  - **`SEND` deliberately excluded, not forgotten**: it dynamically
    resolves which procedure to call at runtime (through `obj`'s
    prototype chain via `eval_resolve_method`), then calls it through
    `call_ast_procedure` with its own `resolved`/`produced`
    error-suppression shape (SEND's own "wrong number of inputs"/
    "didn't output a value" messages need to fire instead of, not in
    addition to, the generic wrapper's) — structurally closer to
    needing its own opcode (dynamic dispatch, not a static
    `OP_CALL_PROC` target) than an ordinary builtin call. Left as its
    own dedicated follow-up rather than rushed into this batch.
  - 6 new `tests/test_vm.c` cases (round-tripping a number/word/list
    property, missing-property-is-empty-list, overwrite-not-duplicate,
    `REMOVEPROP`, `PROPLIST`'s alternating key/value output, and `NEW`
    setting the `"prototype"` property — all ported from confirmed
    `test_eval.c` scripts). Confirmed clean under AddressSanitizer; all
    6 test suites pass via `make test`.
  - **Next**: continue `docs/ROADMAP.md`'s Stage 2 instruction-coverage
    checklist — turtle/drawing commands, `SEND` as its own batch, or
    whatever the user picks next.
- **Turtle/drawing commands, plus `ERASE`/`PROCEDURES`: done** (third
  batch off Stage 2's instruction-coverage checklist item, 2026-08-09,
  user said "do next"). The whole turtle-motion/query/pen/canvas/
  drawing-primitive surface, ~30 operators, plus `ERASE` (pulled in for
  its own `OP_ERASE`, the argument shape `MAKE`/`LOCAL` already needed)
  and `PROCEDURES` (pulled in alongside `ERASE` since a real test of
  "does `ERASE` actually remove it" needs a way to observe the
  procedure list). Mechanically the same shape as the two batches
  before it — an `eval_X_value` core extracted per operator with a real
  `eval_expr` call to factor out, exposed via `eval.h`, wired into
  `vm.c`'s `call_builtin` — except the zero-argument ones
  (`GETX`/`WHO`/`PENUP`/`CLEAR`/... ~17 of them) needed no split at all
  (no `eval_expr` call existed to factor out), so they're exposed
  directly under their own `do_` names rather than renamed, a
  deliberate small inconsistency with the `eval_X_value` convention
  noted directly in `eval.h` rather than silently left unexplained.
  - **A real, unplanned correctness gap this batch surfaced and fixed
    in `vm.c` itself, not just eval.c plumbing**: `ERASE` mutates the
    AST it's given (blanking an `AST_PROC_DEF`'s own name) as its whole
    mechanism -- so a call the compiler already committed to
    `OP_CALL_PROC` (because the name resolved to a real procedure *at
    compile time*) can still fail *at runtime* if `ERASE` ran first.
    `exec_call_proc`'s own "unknown procedure" path didn't print
    anything before this batch (unlike `do_user_procedure_call`'s own
    `"I don't know how to X"`) and didn't suppress `OP_CHECK_OUTPUT`'s
    generic message the way `do_user_procedure_call`'s `*resolved=0`
    does -- both fixed via a new `Vm.last_call_resolved` flag,
    mirroring `eval_expr`'s own `resolved && !produced` check exactly,
    including reproducing a real *double* message `ast_eval` itself
    prints for recursion-too-deep in expression position (which does
    NOT clear `resolved`, confirmed directly against
    `do_user_procedure_call`'s own code before assuming otherwise).
  - **A second, harder bug this surfaced: the test harness, not the
    VM.** `tests/test_vm.c`'s own `shadow_diff_vm` used to parse the
    source once and hand the *same* `AstPool` to both `ast_eval` and
    `compile_program`/`vm_run` -- harmless for every prior batch
    (nothing mutated the AST), but a real hazard against `ERASE`:
    whichever engine ran first would blank the procedure's name before
    the second engine's own run ever saw it, producing a failure that
    read like a VM defect but was actually a stale test-harness
    assumption. Caught immediately by the very first `ERASE` test
    (`ast_eval`'s own run reported "no such procedure" for a procedure
    that plainly still existed in the source). Fixed by parsing `source`
    independently per engine -- two full lex+parse passes, two
    entirely separate `ParseResult`s -- the same "never let one run's
    side effects leak into another's" discipline `test_shadow_diff.c`
    already used for its own two engines (there, trivially true since
    the old engine never touches an AST at all; here, a real
    requirement once one of the *new* engines' own operators can mutate
    the tree it's given).
  - 27 new `tests/test_vm.c` cases: turtle motion + observable-getter
    queries (`GETX`/`GETY`/`HEADING`/`POS`), `SETXY`/`SETHEADING`/
    `SETX`/`SETY`, `DISTANCE`/`TOWARDS`, `HOME`/`CLEAR`, `WHO`/`TELL`
    (including out-of-range), `CANVASSIZE`/`SETCANVASSIZE` (including
    out-of-range), a pen/canvas-appearance smoke test and a
    drawing-primitives smoke test (text-diff-only coverage --
    `shadow_diff_vm` doesn't snapshot full turtle state the way
    `test_shadow_diff.c`'s own `TurtleSnapshot` does, a known, named
    scope limit of this file, not silently assumed sufficient), and the
    three `ERASE` cases (delete-then-call, unknown-procedure error,
    removed-from-`PROCEDURES`). Confirmed clean under AddressSanitizer;
    all 6 test suites pass via `make test`.
  - **Next**: continue `docs/ROADMAP.md`'s Stage 2 instruction-coverage
    checklist -- `SEND` as its own batch (its dynamic dispatch/
    resolved-suppression shape, deferred twice now), `THROW`/`CATCH`'s
    unwind design, `MAP`/`FILTER`/`REDUCE`/`FOREACH` templates, or
    whatever the user picks next.
- **`SEND`: done** (fourth batch off Stage 2's instruction-coverage
  checklist item, 2026-08-09, user said "do next -- SEND"). The piece
  deferred twice already, for a real reason confirmed both times: its
  callee isn't known until compile time at all -- `SEND obj "message
  arglist` resolves `message` through `obj`'s own prototype chain
  against *live* `app->plist_entries` state, which doesn't exist yet
  during compilation. Every other call construct in this VM (ordinary
  builtins, procedures, even `ERASE`'s own runtime `find_proc_def` re-
  lookup) either has a statically known target or resolves a *name* the
  compiler already knows; `SEND` resolves an entirely dynamic *value*.
  - **The actual fix: give `BytecodeChunk` a persistent name->address
    table.** `compiler.c`'s own backpatching was already computing
    exactly the `{name, start_pc}` data `SEND` needs (see the vertical
    slice's own Progress entry) -- it just discarded it once
    `compile_program` returned. Moved that table onto `BytecodeChunk`
    itself (`bytecode.h`'s new `ProcAddr`/`MAX_CHUNK_PROCS`, populated
    by `compile_program`'s own pass 1, looked up via
    `bytecode_find_proc`) so it survives into `vm.c`'s own runtime --
    `compiler.c`'s `Compiler` struct lost its own local copy entirely
    (no more duplication, `find_proc_addr` is gone, every call site now
    reads `chunk->procs[]` directly), and `OP_CALL_PROC`'s own
    backpatching is completely unaffected (still resolved at compile
    time, still just as fast -- this table is *additional* infrastructure,
    not a replacement for what already worked).
  - **Two new opcodes**, added to `compile_call`'s own special-form list
    (`SEND`'s three arguments are ordinary expressions, unlike
    `MAKE`/`LOCAL`/`ERASE`'s raw-name argument, but the *call mechanism*
    itself needed its own opcode, not the generic `OP_CALL_BUILTIN`/
    `OP_CALL_PROC` path):
    - `OP_SEND` -- pops obj/message/arglist, resolves via
      `eval_resolve_method` (now exposed, shared verbatim with
      `do_send` -- it never called another procedure, so it's safe to
      reuse directly), unpacks arguments via a new shared
      `eval_send_unpack_args` (extracted from `do_send`'s own
      `:self`-prepending logic, same reasoning), then either pushes a
      `VmFrame` + scope and jumps into the resolved procedure's
      compiled body exactly like `OP_CALL_PROC`'s own success path
      (looking up that address via `bytecode_find_proc`, since it's
      only known now, not at compile time), or -- on any of `SEND`'s
      own resolution/arity/recursion failures -- prints the exact
      message `do_send` itself would and pushes `word_val("")`
      directly, no frame involved. **A real fidelity detail confirmed
      by re-reading `do_send`'s own code, not assumed**: `do_send`
      clears its own `resolved` flag on *every* "didn't produce a
      value" outcome, including recursion-too-deep -- unlike an
      ordinary call (`do_user_procedure_call`), which only clears it
      for a genuinely unknown name and leaves it set (so a recursion-
      too-deep ordinary call still gets the generic "didn't output a
      value" message on top of "Recursion too deep, call ignored").
      `exec_send` reproduces this exact asymmetry, not a simplified
      version of it.
    - `OP_CHECK_SEND_OUTPUT` -- `OP_CHECK_OUTPUT`'s own job (report
      "<name>: didn't output a value" if a call in expression position
      didn't produce one), but reading the name from a new
      `Vm.last_send_message` runtime field (set by `OP_SEND` itself)
      instead of a compile-time `.text`, since `compile_call` has no
      way to know `SEND`'s own target message ahead of time the way it
      does for every other call's own name.
  - 11 new `tests/test_vm.c` cases, ported directly from
    `test_eval.c`'s own already-confirmed `SEND` corpus: direct method
    call, prototype-chain lookup, inherited methods vs. non-inherited
    data fields (`GETPROP` never chain-walks), extra message arguments
    after `:self`, operator-form output capture (both the success case
    and the "never outputs" diagnostic), every one of `do_send`'s own
    distinct error messages (unknown message, a data property used as
    a method, a method missing its own `:self` parameter, wrong
    argument count), and a cyclic prototype chain staying bounded
    rather than looping forever. All 11 passed on the very first run --
    no bug this time, a sign the earlier batches' groundwork (shared
    `eval_push_scope_for_call`, the `last_call_resolved`/
    `last_call_produced_output` machinery `ERASE`'s own batch had
    already generalized) was doing its job. Confirmed clean under
    AddressSanitizer; all 6 test suites pass via `make test`.
  - **Next**: continue `docs/ROADMAP.md`'s Stage 2 instruction-coverage
    checklist -- `THROW`/`CATCH`'s own unwind design (the other
    genuinely hard piece, alongside suspend/resume) or `MAP`/`FILTER`/
    `REDUCE`/`FOREACH` templates are the remaining named items, or
    whatever the user picks next.
- **`THROW`/`CATCH`: done** (fifth batch off Stage 2's own checklist,
  2026-08-09, user said "do next -- THROW/CATCH"). Flagged from the
  start as one of the two genuinely hard pieces, alongside suspend/
  resume -- turned out easier than the checklist's own original framing
  ("an explicit unwind-target stack the VM consults") anticipated, once
  `ast_eval`'s *actual* mechanism was re-read rather than assumed: it
  doesn't do real stack unwinding either. `THROW` just sets a shared
  `app->throw_requested`/`throw_tag` flag; every loop/block construct
  (`exec_block`, `do_while`, ...) cooperatively checks it after each
  statement/iteration and breaks its own per-statement loop early,
  letting the flag cascade up through however many nested C calls
  happen to be on the stack at that moment -- no `longjmp`, no explicit
  unwind targets, nothing exotic. `STOP`/`OUTPUT` already work
  identically in `ast_eval` (both just set `stop_requested`, checked
  the exact same way) -- and this VM's own `OP_STOP`/`OP_OUTPUT`
  already handle that case correctly, via a direct frame-pop + `pc`
  jump (confirmed to be a more *direct* implementation of the identical
  semantics, not a different one, since it can never "fall through" to
  a later instruction the way a cooperative check would need to) -- so
  only `THROW`'s own genuinely-multi-frame cooperative propagation
  needed new design work at all.
  - **The mechanism: compile-time-inserted forward jumps standing in
    for the tree-walker's own recursive breaks.** `compile_block` now
    emits a new `OP_CHECK_THROW` after *every* statement (not just
    loop iterations) -- `.a` patched, once the whole block is compiled,
    to jump to that block's own true end if `app->throw_requested`.
    This composes correctly across every level purely as a consequence
    of how blocks nest, with zero bespoke "how many frames do I unwind"
    logic anywhere: a nested block's own checks skip to *its* end;
    `WHILE`'s own compiled loop gained one extra `OP_CHECK_THROW`
    (before its loop-back `OP_JUMP`, since `OP_STOP` can never reach
    that point but `THROW` can) so a still-propagating throw skips the
    next iteration's condition re-check entirely, landing exactly where
    a false condition would; and a procedure body's own already-
    existing auto-appended `OP_STOP` (there since the vertical slice,
    for "fell off the end without OUTPUT") turned out to double
    *exactly* as the correct "throw propagated all the way to the end
    of this procedure, now actually return" landing pad -- no new code
    needed there at all, just the pre-existing instruction correctly
    being where an unresolved throw's own forward-jump chain naturally
    ends up.
  - **`CATCH` needed no jump logic of its own**: `OP_CATCH_CHECK`, run
    unconditionally right after `CATCH`'s own block (itself compiled
    with the same per-statement `OP_CHECK_THROW`s as any other block),
    just mirrors `do_catch` -- if `app->throw_requested` and the tag
    matches, clear it (absorbing the throw here); otherwise leave it
    completely alone, so the *enclosing* block's own next
    `OP_CHECK_THROW` (for the statement containing this `CATCH`) sees
    it still set and keeps propagating. `compile_program`'s own
    top-level statements get a different treatment via a new
    `compile_block` parameter (`is_top_level`): `OP_CHECK_UNCAUGHT_THROW`
    instead of `OP_CHECK_THROW` -- reports `"THROW: no CATCH found for
    ..."` and *keeps running the rest of the script*, mirroring
    `ast_eval_from`'s own top-level recovery loop exactly (never a
    jump, unlike every nested block's own skip-to-end).
  - **A real bug the test corpus caught, not guessed**: the very first
    design used a single `Vm`-level scratch field
    (`vm->pending_catch_tag`) to carry `CATCH`'s own evaluated tag
    between "evaluate it" (before the block) and "check it" (after) --
    broke immediately on a *nested* `CATCH`, where the inner `CATCH`'s
    own tag silently overwrote the outer one's before the outer ever
    got to check it (`test_nested_catch_with_non_matching_inner_tag_
    propagates_to_outer` failed with an extra, wrong "no CATCH found"
    message). Fixed by leaving the tag value on the VM's own value
    stack instead of a scratch field at all -- the stack already
    supports nesting correctly via ordinary push/pop, and
    `compile_block` is always stack-neutral overall (every statement's
    own `finish_call` nets to zero), so the tag value sits exactly
    where it was pushed, completely undisturbed by whatever the block
    itself does, until `OP_CATCH_CHECK` finally pops it.
  - `THROW` itself needed no new opcode at all -- an ordinary
    `OP_CALL_BUILTIN`, sharing a newly exposed `eval_throw_value` with
    `do_throw` (same pattern as the two previous batches).
    `eval_catch_check`/`eval_report_uncaught_throw` are similarly
    shared with `do_catch`/`ast_eval_from` rather than duplicated.
  - **Confirmed, not assumed: a test that turned out to be invalid
    syntax, caught by the parser itself, not a VM gap.** An initial
    test tried `PRINT CATCH "tag [...]` (`CATCH` used for its own
    output, mirroring the same "void construct in expression position"
    tests every other special form got) -- failed with a genuine parse
    error in *both* engines. Traced to `parser.c`'s own `try_parse_call`,
    which deliberately marks any `ARG_BLOCK`/`ARG_CONDITION` builtin
    (`CATCH`, `WHILE`, `REPEAT`, `FOREVER`) unusable in expression
    position at parse time -- not a VM-specific restriction at all.
    Removed the test (it exercised something that can never actually
    happen) rather than working around it; `compile_call`'s own
    `want_value` handling for `CATCH` (and `WHILE`'s, already shipped
    in an earlier batch) is therefore provably unreachable in practice,
    kept only for structural consistency with every other call form's
    uniform `finish_call` tail, and documented as such in the test file
    rather than left as an unexplained gap.
  - **Known, pre-existing, separate gap surfaced while working on this,
    not introduced by it**: `REPEAT`/`FOREVER`/`FOR` were never ported
    to this VM at all (no special form recognizes their own `ARG_BLOCK`
    header shape yet -- compiling one today silently mis-compiles its
    block argument as a bogus expression). They therefore don't
    participate in `THROW`'s own cooperative-unwind mechanism either --
    only `WHILE` (already ported, in the vertical slice) does. Left for
    its own future batch, not folded into this one.
  - 9 new `tests/test_vm.c` cases: 3 ported from `test_eval.c`'s own
    confirmed corpus (basic catch-recovers, no-matching-catch recovers
    at the top level, an uncaught throw lets later statements keep
    running), plus 6 new cases specifically targeting cross-frame/
    cross-construct propagation the existing tree-walker corpus didn't
    happen to exercise: a throw through two nested procedure calls
    (the core multi-frame case), breaking a `WHILE` loop early, a
    throw inside an `IF` branch, nested `CATCH` with a non-matching
    inner tag (the one that caught the scratch-field bug), and a
    procedure that throws in expression position (confirming both the
    "didn't output a value" diagnostic *and* the uncaught-throw report
    fire, in the right order -- a genuinely intricate cascade, traced
    by hand against `ast_eval`'s own code before trusting the diff).
    All 9 passed on the first run after the nested-`CATCH` fix;
    confirmed clean under AddressSanitizer; all 6 test suites pass via
    `make test`.
  - **Next**: `MAP`/`FILTER`/`REDUCE`/`FOREACH` templates (the last
    named item on Stage 2's own checklist), suspend/resume's actual GTK
    integration (the real point of this whole stage, deliberately
    last), or the known `REPEAT`/`FOREVER`/`FOR` gap noted above, or
    whatever the user picks next.
- **`REPEAT`/`FOREVER`/`FOR`: done** (sixth batch off Stage 2's own
  checklist, 2026-08-09, user said "do next — REPEAT/FOREVER/FOR" --
  closing the gap the `THROW`/`CATCH` batch had explicitly deferred).
  All three needed persistent loop-control state -- a remaining count,
  an iteration counter, `FOR`'s own limit/step/internal counter -- that
  survives a *recursive* call within the loop body. A hidden Logo
  variable was considered and rejected immediately: it would collide
  across nested/recursive invocations of the same compiled loop,
  exactly the class of bug `CATCH`'s own tag-clobbering already
  surfaced in the previous batch. The actual fix: a new opcode,
  `OP_PEEK` (`.a` = depth below the current top; pushes a *copy* of
  that value without removing the original), keeps this state on the
  VM's own value stack instead -- naturally protected by whatever
  `VmFrame` a recursive call pushes, the same way `exec_for`'s own
  plain C-local `limit`/`step` doubles are naturally protected by the
  C call stack itself.
  - **`REPEAT`**: count truncated once via a small newly-exposed
    `eval_int_value` (`INT`'s own value-taking core, split from
    `do_int` the same way every earlier batch's builtins were --
    added to `vm.c`'s `call_builtin` dispatch too, as a genuine side
    effect, not a wholesale "port every math operator" batch, just the
    one `REPEAT` itself needs), then kept as REPEAT's own single
    persistent stack slot, decremented in place each iteration via
    plain `push 1; OP_SUB` (works because nothing else sits on top of
    it at that point -- ordinary arithmetic already does an in-place
    "replace" when the operand being replaced is the direct target of
    the pop). No `MAX_WHILE_ITERATIONS` cap needed -- `do_repeat`'s own
    C `for`-loop was never capped either, naturally bounded by `count`
    itself.
  - **`FOREVER`**: the same single-counter shape as `REPEAT`, but
    counting *up* and capped against `MAX_WHILE_ITERATIONS` (matching
    `do_forever`'s own cap) -- a genuine necessity here, unlike
    `WHILE`'s still-open gap: an uncapped `FOREVER` can hang the VM
    outright, not just diverge from a safety net nobody happened to
    need yet.
  - **`FOR`: the harder case, and where a real, second bug surfaced.**
    `FOR`'s first working version re-read its own loop variable back
    from the *Logo-visible* variable (via `OP_PUSH_VAR`) for its own
    condition check and increment -- but `exec_for`'s own tree-walker
    equivalent never does this: its loop control is a genuinely
    separate, C-local `double i`, naturally protected by the C call
    stack; only the *Logo-visible* copy (written via a plain
    `set_var(node->text, i)` purely for the block's own benefit, per
    that function's own comment: "no push_scope ... unlike a real
    parameter") is deliberately left unscoped. Confusing "the value the
    loop uses to decide when to stop" with "the value the block sees"
    meant a recursive call whose own `FOR` loop happened to share the
    same variable name would silently corrupt the *outer* loop's own
    next comparison, once the inner call returned and left the global
    variable at whatever value its own loop finished on.
    **Caught directly by `tests/test_vm.c`'s own recursion-safety
    test** (`test_for_limit_and_step_survive_a_recursive_call_in_its_
    own_body`) -- `nested 2` produced only 3 of the expected 6 output
    lines, `ast_eval`'s own output confirmed correct first via direct
    inspection before concluding the VM was wrong, not the test.
    Traced with temporary `VM_DEBUG`-gated instrumentation (per-
    instruction `pc`/opcode/stack-contents dumps compared against a
    hand-derived expected trace) after several rounds of pure
    reasoning about stack offsets kept concluding "this should already
    be correct" -- confirms this project's own "confirmed directly,
    not guessed" bar sometimes needs an actual runtime trace, not just
    careful reading, and that's fine. Fixed by giving `FOR` a
    *third*, genuinely separate persistent stack slot for its own
    internal counter (never read back from Logo state at all) --
    needing one more new opcode, `OP_POKE` (the write-back half of
    `OP_PEEK`: pops the top value and overwrites a persistent slot
    below it), since this counter sits *underneath* `limit`/`step` on
    the stack rather than on top, so the "push 1; add" in-place trick
    that works for `REPEAT`/`FOREVER`'s own lone top-of-stack counter
    doesn't apply here. `FOR`'s own loop is compiled as two near-
    duplicate variants (ascending via `OP_CMP_LE`, descending via
    `OP_CMP_GE`), the direction picked once via a runtime branch on
    step's own sign rather than re-derived every iteration -- a
    deliberate code-size-for-simplicity trade favoring a compiler that
    isn't hand-written by a user. No `MAX_WHILE_ITERATIONS` cap for
    `FOR` itself in this batch either -- left alongside `WHILE`'s own
    still-open gap, since `FOR`'s own termination is normally
    guaranteed by its own start/limit/step arithmetic, a smaller risk
    than `FOREVER`'s unbounded-by-default shape.
  - 11 new `tests/test_vm.c` cases: 6 ported directly from
    `test_eval.c`'s own confirmed corpus (`REPEAT`, `FOREVER` until
    `STOP`, `FOR` counting up/down, an explicit step, a full-expression
    limit, `step` = 0's own error), plus a fractional-count truncation
    case, `THROW` breaking a `FOR` loop early, and -- the two cases
    that matter most for this whole design -- `REPEAT` and `FOR` each
    recursing into a call containing the exact same loop construct,
    confirming the outer invocation's own state survives the inner
    one's entire run intact. Confirmed clean under AddressSanitizer;
    all 6 test suites pass via `make test`.
  - **Next**: `MAP`/`FILTER`/`REDUCE`/`FOREACH` templates (the last
    named item on Stage 2's own instruction-coverage checklist),
    suspend/resume's actual GTK integration (the real point of this
    whole stage, deliberately last), the still-open `WHILE`/`FOR`
    iteration-cap gap, or whatever the user picks next.
- **`MAP`/`FILTER`/`REDUCE`/`FOREACH` templates: done** (seventh batch
  off Stage 2's own checklist, 2026-08-09, user said "do next —
  MAP/FILTER/REDUCE/FOREACH" — the last named item on the
  instruction-coverage checklist). Unlike every earlier batch, this
  isn't just a port: `eval_logo` and `ast_eval` both re-lex and
  re-parse the template once *per element*, substituting the current
  element's own printed text in for `?`/`?1`/`?2` first
  (`eval_substitute_placeholder`/`eval_apply_template_expr`/
  `eval_apply_template_condition` in `eval.c`). This VM compiles the
  template exactly *once*, when a literal `[...]` is visible at compile
  time, and binds real values at runtime instead of round-tripping
  through text.
  - **The mechanism**: `compile_template_call` (new, `compiler.c`)
    renders the list literal's own children (`AST_WORD`/nested
    `AST_LIST_LITERAL` — never operator-precedence-parsed to begin
    with, so this is a direct AST walk, not a runtime
    `list_pool`-round-trip) back to source text via a new
    `render_list_literal_source`, rewriting every `?`/`?1`/`?2` token
    it finds into a real, compiler-generated, per-call-site-unique
    variable reference (`:__tmplN__`, via a new
    `Compiler.template_counter` — considered and rejected a single
    *shared* name across all call sites first: a template that reads
    the placeholder both before *and* after a nested higher-order call
    within the same expression would have a real collision risk).
    That rendered text is lexed and parsed *once*, right then, into a
    real expression/condition/block AST (which one depends on which of
    the four builtins it is: `MAP`/`REDUCE` want an expression,
    `FILTER` a condition, `FOREACH` a full statement block). The parsed
    result lives in its own throwaway scratch `AstPool`, though, and
    can't be compiled straight out of that: a template that itself
    calls a user procedure needs `OP_CALL_PROC`'s own `find_proc_def`
    lookup to see *every* procedure in the real program, and
    `OP_PUSH_LIST_LITERAL`'s own stored node index (if the template
    itself contains a nested list literal) is only meaningful against
    whichever *one* `AstPool` `vm_run` was actually given for this
    entire run. A new `ast_graft` (deep-copies a parsed subtree, node
    for node, into another pool) solves both by grafting the freshly
    parsed template body into the *same* `AstPool` the rest of the
    program already uses — which also makes nesting free, since a
    nested template's own grafted body grafts into that identical pool
    again, not a fresh scratch one each level. Four new opcodes,
    `OP_MAP_COMPILED`/`OP_FILTER_COMPILED`/`OP_REDUCE_COMPILED`/
    `OP_FOREACH_COMPILED` (`.a` = the compiled template's own start pc,
    `.text` = its placeholder's base name), are reached only via a
    jump-around at compile time (the template's own compiled bytecode
    sits inline in the same chunk, ending in `OP_HALT`, never fallen
    into by ordinary linear execution) and a *recursive* `vm_run` call
    at runtime, once per list element — `vm_run`'s own `OP_HALT`
    handler is a plain C `return;`, correctly scoped to just that one
    recursive invocation since `pc` is a local variable each call, the
    same trick that already made `THROW`/`CATCH`'s cooperative unwind
    compose for free. Each iteration just binds the placeholder
    variable to the current element's real `EvalValue` (an ordinary
    `set_var`-equivalent) and re-executes the same already-compiled
    bytecode.
    A runtime-computed template (not a literal `[...]`), or a literal
    one that's syntactically broken (discovered right there at compile
    time, when the rendered text fails to lex/parse), isn't special-
    cased at all: `compile_call` just falls through to the *exact same*
    generic `OP_CALL_BUILTIN` dispatch every other builtin already uses,
    landing on newly exposed `eval_map_value`/`eval_filter_value`/
    `eval_reduce_value`/`eval_foreach_value` (refactored from
    `do_map`/`do_filter`/`do_reduce`/`do_foreach`, parameterized by
    already-evaluated `EvalValue`s instead of raw AST argument indices,
    same "expose the value-taking core" pattern every earlier batch
    used) — so a broken template gets `ast_eval`'s own exact defensive
    per-element behavior (`num_val(0)` elements for `MAP`, nothing kept
    for `FILTER`, the accumulator left unchanged for `REDUCE`, a silent
    no-op for `FOREACH`) for free, not a second implementation of it
    that could drift.
  - **Two real bugs, both caught directly by `tests/test_vm.c`, neither
    guessed:**
    1. **Numeric-looking list elements silently failed every ordering
       comparison.** `FILTER [? > 2] [1 2 3 4]` kept nothing at all.
       Root cause, confirmed by reading `node_to_value`/
       `eval_build_list_literal` directly: a list literal's own
       elements are *always* internally `VALUE_WORD`, even
       numeric-looking ones like `"1"` — never `LIST_ELEM_NUMBER`. The
       *old* engines' own text-substitution approach never noticed this,
       because substituting the element's own printed text into the
       template and re-lexing the whole thing from scratch makes a
       numeric-looking token lex as a genuine `LOGO_TOK_NUMBER` (hence a
       real `VALUE_NUMBER` once evaluated), regardless of what internal
       type tag the original list element happened to carry —
       `exec_compare`'s own non-numeric fallback (mirroring
       `eval_condition`'s own `AST_COMPARE` case exactly) is
       unconditionally false for `>`/`<`/`<=`/`>=` on anything that
       isn't *already* `VALUE_NUMBER`, unlike arithmetic's own
       `eval_to_number`, which coerces any numeric-looking word
       regardless of type tag. This VM's direct-value binding has no
       re-lex step to do that promotion implicitly, so a new
       `coerce_template_element` (`vm.c`) replicates it explicitly,
       applied everywhere an element (or `REDUCE`'s own running
       accumulator) gets bound to a placeholder.
    2. **The placeholder variable isn't safe against reentrancy of the
       *same* compiled call site** — the harder one, and a genuine
       correction to this design's own earlier assumption (see the
       summary this batch continued from) that a per-call-site-unique
       name was sufficient protection. It rules out two *different*
       `MAP`/`FILTER`/`REDUCE`/`FOREACH` call sites ever colliding, but
       not the *same* call site being reached again through recursion —
       a template that, for one element, calls a procedure which itself
       recurses back into the very same `MAP`/`FILTER`/`REDUCE`/
       `FOREACH` call — while the outer activation's own loop is still
       mid-iteration, paused inside its own template's evaluation, about
       to read `?` *after* that recursive call returns. The first
       working version unconditionally deleted the placeholder once the
       whole loop finished; the *inner* (recursive) invocation's own
       cleanup deleted it out from under the still-in-flight *outer*
       one, so the outer's own delayed `?` read came back unbound
       (`num_val(0)`) instead of its own current element. Confirmed by
       hand-tracing a concrete recursive script before fixing, exactly
       this project's own "confirmed directly, not guessed" bar. Fixed
       the same general way the `FOR` loop bug from the previous batch
       was — state that must survive a recursive reentry of the same
       compiled construct can't be a single shared slot that's merely
       written then read — but the fix itself needed no new opcode or
       compile-time stack-depth tracking at all here, unlike `FOR`'s own
       `OP_PEEK`/`OP_POKE`: a new `save_var`/`restore_var` pair captures
       whatever the placeholder was bound to (if anything) once, right
       before the loop starts, and restores exactly that afterward
       instead of unconditionally deleting. Since each
       `exec_*_compiled` invocation's own `saved` value is an ordinary
       C-local, save/restore naturally nest correctly across however
       many recursive reentries happen, for free, riding on the C call
       stack's own natural nesting rather than needing any VM-level
       mechanism for it — `FOR`'s own fix needed `OP_PEEK`/`OP_POKE`
       specifically because its persistent state had no equivalent
       natural C-stack home; this state does.
  - **A separate, narrower limitation, documented rather than fixed**:
    `OUTPUT`/`STOP` executed *directly* inside a template body (not
    through a further nested real procedure call) can pop a frame that
    belongs to whatever real procedure encloses the whole `MAP`/
    `FILTER`/`REDUCE`/`FOREACH` call — but C's own call/return means
    control still returns to the `exec_*_compiled` loop function that
    made the recursive `vm_run` call, not further up to wherever that
    frame actually belonged. A `frame_count` "floor," recorded before
    the loop starts and checked after every recursive `vm_run` call,
    stops the loop early the moment that's detected, as a fail-safe —
    not a full fix. A narrower sub-case doesn't even get that
    mitigation: `STOP` with *no* enclosing real procedure at all (e.g. a
    top-level `FOREACH`) doesn't stop the loop early either, since this
    VM's `OP_STOP` is a hard, immediate frame-pop (`exec_return`), not
    the tree-walker's own cooperative `app->stop_requested` flag that
    every loop construct (including `do_foreach`'s own) checks after
    each iteration — `exec_return`'s own top-level fallback (no
    enclosing frame to pop) never touches that flag, so nothing signals
    the C loop to stop early in that specific case. Both are consequences
    of the hard-frame-based `OP_STOP` design already established for
    `WHILE`/`REPEAT`/`FOREVER`/`FOR` (correct *there*, since those
    compile inline in the same `pc`-space as their enclosing procedure —
    the jump lands exactly on that procedure's own return, skipping
    every loop opcode in between by construction), not something new
    introduced by this batch — just newly exposed by templates being
    the first construct compiled into a genuinely *separate* recursive
    `vm_run` invocation. Deliberately left as a known, narrow,
    documented gap rather than tested as if it worked: fully solving it
    would mean threading a real stop signal through recursive `vm_run`
    calls, disproportionate to this one construct. (Confirmed the
    tree-walker has its own analogous quirk in the opposite direction —
    `do_map`'s own loop never checks `stop_requested` at all, so `STOP`
    inside a `MAP` template doesn't stop `MAP` early there either, just
    gets silently remembered until whatever encloses it next checks the
    flag.)
  - `eval_delete_var` (new, `eval.c` — swap-with-last removal from
    `app->variables[]`, mirroring `REMOVEPROP`'s own pattern) closes one
    other real, but easy, gap: without it, an internal per-call-site
    placeholder variable would leak into a later `NAMES` call, unlike
    `ast_eval`'s own pure-text-substitution mechanism, which never
    creates a real variable for `?` at all.
  - 20 new `tests/test_vm.c` cases: 14 ported from `test_eval.c`'s own
    confirmed corpus — deliberately *not* 15: `STOP` directly inside a
    `FOREACH` template was left out on purpose, since it exercises
    exactly the documented gap above and would be asserting the VM
    matches `ast_eval` on a case it's known not to — plus 6 new,
    specifically targeting this batch's own new mechanism: a template
    calling a real user procedure (exercising `ast_graft`'s entire
    reason for existing), a runtime-computed template and a
    compile-time-malformed literal one (both confirming the
    dynamic-fallback path actually gets taken and reproduces
    `ast_eval`'s own behavior), nested `MAP`/`REDUCE` templates sharing
    one chunk without their own placeholders colliding, and the
    recursion-reentrancy case that drove the `save_var`/`restore_var`
    fix above. Confirmed clean under AddressSanitizer, with one
    exception: `test_recursion_depth_cap_reports_error_not_a_crash`
    (an *existing* test, unrelated to this batch, that deliberately
    recurses without bound to confirm the depth cap reports an error
    rather than crashing) stack-overflows under AddressSanitizer
    specifically — confirmed to reproduce *identically* on the
    pre-batch codebase (stashed this batch's own changes, rebuilt,
    reran) before concluding it's pre-existing and not a regression;
    ASan's own larger per-frame stack usage apparently pushes that
    test's real (uncapped-by-design) recursion past the depth
    `MAX_SCOPE_DEPTH`'s own check catches under a normal build. Left
    exactly as found — not this batch's issue to fix. All 6 `make test`
    suites pass; `bin/logo`/`bin/logi` both still build.
- **Suspend/resume's actual GTK integration** (2026-08-10) — the real
  point of this whole stage, scoped narrow on purpose after a
  scoping pass surfaced two things `docs/ROADMAP.md`'s own checklist
  wording had undersold: `WAIT`/`WAITKEY`/`INPUT`/`PAUSE`/
  `ANIMATESPRITE` existed *only* in `eval_logo` before this batch (a
  deliberate earlier decision — porting a busy-wait shim into
  `ast_eval` first would have been throwaway work once the real
  mechanism landed), and `vm_run` had never been called from `ui.c` at
  all — `bin/logo` ran exclusively on `eval_logo` until now. So this
  was never just "wire an already-integrated VM into GTK"; it was
  building the suspend mechanism, porting `WAIT`/`WAITKEY` into the
  new pipeline, and cutting the live app over, together — `INPUT`/
  `PAUSE`/`ANIMATESPRITE` deliberately left for later batches, the same
  "grow coverage one group at a time" pattern as every other Stage 2
  batch (see `docs/ROADMAP.md`'s own entry for the full writeup this
  summarizes).
  The mechanism itself: `vm_run` now returns a `VmRunResult`
  (`VM_RUN_HALTED`/`VM_RUN_SUSPENDED_WAIT`/`VM_RUN_SUSPENDED_WAITKEY`)
  instead of `void`. Two new opcodes, `OP_WAIT`/`OP_WAITKEY`, can make
  it *return early*, mid-chunk — `Vm` gained a `pc` field (where to
  resume; every other opcode's `pc` is a plain C local that can't
  survive a `return`) and a `suspend_seconds` payload for `OP_WAIT`.
  Two new entry points, `vm_resume`/`vm_resume_with_key`, restart
  `vm_run` from `vm->pc`. Deliberately not a GTK-aware design: `vm.c`
  itself never touches a clock or an event loop — it just reports
  "suspended, here's why," leaving *how* to actually wait to `ui.c`,
  the same seam discipline `request_redraw` already established.
  Because a suspended `Vm`'s stack/frames are left fully intact,
  resuming correctly unwinds back through however many *ordinary*
  nested procedure calls were mid-flight — confirmed directly by a
  test suspending 3 real calls deep and resuming back out through all
  3 `OUTPUT`s correctly, the actual payoff of the frame-array design,
  not just an assumption.
  The one deliberately-not-fixed gap: `MAP`/`FILTER`/`REDUCE`/
  `FOREACH` templates already recurse into `vm_run` via plain C
  recursion (the previous batch above) — a suspend from inside that
  recursive call can't propagate out to whatever's driving the
  outermost `vm_run`. Rather than redesign templates off C recursion
  first, a new `vm_run_depth` counter lets `OP_WAIT`/`OP_WAITKEY`
  detect this and refuse outright with a clear runtime message, same
  "documented gap, not silent corruption" spirit as the template
  batch's own `frame_floor` mitigation for `OUTPUT`/`STOP`.
  `ui.c`'s two former `eval_logo(app, text)` call sites (REPL
  Enter-submit, `LOAD`'s file-open completion) are now one
  `run_logo_script`, lexing/parsing/compiling/running through the VM;
  a file-scope `SuspendedRun` keeps a paused run alive across however
  many GTK callbacks it takes to resume — a real one-shot
  `g_timeout_add` for `WAIT`, and a direct `vm_resume_with_key` call
  from `on_entry_key_pressed` (not a busy-wait flag) for `WAITKEY`.
  `eval_logo` itself is untouched, still exercised by
  `tests/test_interpreter.c`, just no longer this app's own execution
  path.
  Testing couldn't shadow-diff against `ast_eval` (no suspend concept
  there at all) — 6 new `tests/test_vm.c` cases call `vm_run`/
  `vm_resume`/`vm_resume_with_key` directly instead, entirely headless,
  confirming exactly the payoff of keeping `vm.c` GTK-free. Confirmed
  clean under AddressSanitizer (same one pre-existing, unrelated crash
  noted above); all 6 `make test` suites pass; `bin/logo` builds
  warning-free and was confirmed to launch and run without crashing.
  The interactive click-test itself (typing `WAIT`/`WAITKEY` into the
  running app) needs a human to confirm — this environment had no
  Accessibility permission for scripted GTK UI control. **Confirmed
  working by the user directly, both commands.**
- **`INPUT`** (2026-08-10) — the easiest of the three commands the
  batch above deferred: mechanically identical to `WAITKEY`, just
  resumed with a whole submitted line instead of a single key name. A
  new `VM_RUN_SUSPENDED_INPUT`/`OP_INPUT` mirror `OP_WAITKEY` exactly
  (including the same `vm_run_depth`-based template refusal); a new
  `vm_resume_with_input` mirrors `vm_resume_with_key`'s own
  push-then-continue shape, kept as its own function rather than a
  shared one since `ui.c` gates the two on different flags/events
  (`waiting_for_key` vs. `waiting_for_input`). `ui.c`'s existing
  `waiting_for_input` branch (already there for `eval_logo`'s own
  busy-wait, unused by the live app until now) got the same treatment
  as `WAITKEY`'s own branch: capture the line, call
  `vm_resume_with_input` directly instead of setting a flag for a
  busy-wait loop. 2 new headless `tests/test_vm.c` cases, ASan-clean,
  all 6 `make test` suites pass, `bin/logo` builds warning-free and
  runs without crashing.
- **A real concurrency bug in the suspend mechanism, found and fixed
  while scoping `ANIMATESPRITE`** (2026-08-10): `WAIT` suspends with
  neither `waiting_for_key` nor `waiting_for_input` set (unlike
  `WAITKEY`/`INPUT`), so `ui.c`'s ordinary Enter-submit/`LOAD` paths had
  no way to know a script was already suspended — a second submission
  would silently overwrite the single `g_suspended_run` slot the first
  script's own eventual timer callback depends on, resuming the wrong
  `Vm` (or a freed one) once it fired. Fixed by checking
  `g_suspended_run != NULL` at the top of `run_logo_script` itself (the
  one entry point both call sites share) and refusing a concurrent
  submission with a short message instead — this app only ever runs one
  script "thread" at a time, matching how `WAITKEY`/`INPUT` already
  behaved, just closing the one gap that had no dedicated flag to
  piggyback on.
- **`ANIMATESPRITE` scoped, found blocked, not implemented** (same
  day): it operates on `Turtle.sprite_index`, which only `SETSPRITE`
  ever sets away from `-1` — and `SETSPRITE`/`LOADSPRITE`/
  `STAMPSPRITE`/`SETSPRITEFRAME` don't exist anywhere in `parser.c`/
  `eval.c` at all (sprites were deliberately out of Stage 1's own scope
  from the start). So `ANIMATESPRITE` can't reach any real behavior
  through this pipeline yet regardless of its own suspend design — it
  would always hit the `"no sprite set"` path. Porting the whole sprite
  subsystem first is its own separate, larger scoping question; left
  blocked rather than taken on as part of this batch, at the user's own
  call.
- **`PAUSE`/`CONTINUE`/`CO`** (2026-08-10) — the last actual open
  suspend/resume item, and it composed cleanly with `WAIT`'s own
  design rather than needing anything fundamentally new: `PAUSE`
  suspends/resumes exactly like `WAIT` (no value produced, resumed via
  the plain `vm_resume`), reusing the already-shared `app->pause_depth`
  and printing `interpreter.c`'s own exact `"Paused (level N)..."`
  message. `CONTINUE`/`CO` needed no opcode at all — ordinary zero-arg
  builtins that just decrement `app->pause_depth`, matching
  `do_continue` exactly.
  The one genuinely new piece: reconciling this with the previous
  batch's own concurrency fix. `WAIT`/`WAITKEY`/`INPUT` correctly
  *block* a second concurrent submission, but `PAUSE`'s entire point is
  the opposite — the REPL must keep running ordinary commands while
  paused, so the user can inspect/modify the paused call's own live
  variables (falls out for free: variable storage is shared global
  state via `app->scopes[]`, not per-`Vm`, matching `eval_logo`'s own
  reentrant-call design exactly). Solved with a **separate** stack
  (`ui.c`'s `g_paused_runs`, deliberately not folded into the
  single-slot `g_suspended_run`) so a non-empty pause stack never trips
  the concurrency guard. Nesting is fully supported (the user chose
  this over a narrower single-level-only pass, since `app->pause_depth`
  already naturally supports it — a small stack instead of one slot
  wasn't much more code). A new `maybe_resume_paused_runs`, called at
  the tail of `handle_vm_result` after every script action, checks the
  innermost paused run's own captured level against `app->pause_depth`
  and resumes it once `CONTINUE` has dropped it low enough, recursing
  back through `handle_vm_result` so a chain of nested pauses unwinds
  one `CONTINUE` at a time — matching `interpreter.c`'s own busy-wait
  condition exactly, just checked on each transition instead of polled.
  Ctrl+C-based force-unpause deliberately left out, matching the
  already-documented gap that interrupt-checking isn't wired into this
  pipeline at all yet. 5 new headless `tests/test_vm.c` cases — the one
  that matters most: two independently compiled/run scripts sharing one
  `LogoApp`, confirming nested `PAUSE`s capture strictly increasing
  levels and a single `CONTINUE` resumes exactly the innermost one, in
  the right order. Confirmed clean under AddressSanitizer (same one
  pre-existing crash); all 6 `make test` suites pass; `bin/logo`/
  `bin/logi` build warning-free and run without crashing. The
  interactive reentrant-nesting check itself needs the user to confirm
  directly.
  - **This closes out every named suspend/resume item on Stage 2's own
    checklist.** `ANIMATESPRITE` itself was picked up as its own
    separate project right after — see below, it landed too.

- **`ANIMATESPRITE` + the sprite subsystem** (2026-08-10) — originally
  found blocked while scoping the batch above (`SETSPRITE`/`LOADSPRITE`/
  `STAMPSPRITE`/`SETSPRITEFRAME` didn't exist anywhere in `parser.c`/
  `eval.c`, sprites having been deliberately out of Stage 1's own scope
  from the start), scoped as its own real project instead of folded
  into suspend/resume, and picked up right after `PAUSE` closed out
  every other named item.
  The five ordinary sprite commands landed `vm.c`-only, at the user's
  own call (same scope decision as `WAIT`/`WAITKEY`/`INPUT`/`PAUSE` —
  `bin/logo` no longer runs on `ast_eval`, so porting there too would
  only buy extra shadow-diff test infrastructure for a subsystem that
  only ever executes live). All five needed zero special `compiler.c`
  treatment — confirmed directly that `ARG_QUOTED_WORD` arguments
  already compile through the generic `OP_CALL_BUILTIN` fallback path
  like any other expression, the same reason `DELETEFILE`/`TEXT`/
  `SHOW` never needed a special branch either.
  `ANIMATESPRITE` itself needed the one genuinely new mechanism: unlike
  every suspend point so far (each suspends exactly once),
  `ANIMATESPRITE` can suspend *multiple times* per call — once per
  remaining frame. A new `Vm.suspend_frames_remaining` (alongside the
  already-existing `suspend_seconds`, reused for the per-frame delay)
  tracks this; `OP_ANIMATESPRITE` advances the first frame and
  suspends if `delay > 0`, and a new `vm_resume_animatesprite` advances
  one more frame per call, either suspending again or finally falling
  through to a real `vm_run` continuation once done — `ui.c` just
  re-arms its timer each time via the existing `handle_vm_result`
  dispatch, needing no new orchestration logic beyond one more
  `VmRunResult` case. Uses the same `g_suspended_run` concurrency-block
  slot as `WAIT` (not `PAUSE`'s reentrant stack). A `delay <= 0`
  animation runs its whole frame loop synchronously with no suspend at
  all — a faithful port of `interpreter.c`'s own real behavior (its own
  busy-wait loop is the only thing that ever lets GTK repaint an
  intermediate frame, so a zero-delay animation was never really
  animated there either).
  11 new headless `tests/test_vm.c` cases — the ordinary commands'
  error/no-op paths mirror `tests/test_interpreter.c`'s own sprite
  corpus exactly; the two that matter most directly poke `app`'s own
  sprite fields to set up "a sprite exists" without the GUI-only load
  path, confirming a 3-frame animation suspends exactly 3 times with
  the right frame advancing each time, and a zero-delay animation
  advances all frames (wrapping correctly) synchronously in one shot.
  Confirmed clean under AddressSanitizer (same one pre-existing crash);
  all 6 `make test` suites pass; `bin/logo`/`bin/logi` build
  warning-free and run without crashing. The real image-decoding half
  still needs manual confirmation against the running app, same
  limitation `test_interpreter.c`'s own sprite corpus already accepts.
  **A separate, previously-unnoticed gap found while scoping this, not
  fixed here**: `TEXT`/`SHOW`/`DELETEFILE`/`LOAD`/`SAVE` and the whole
  file-I/O family are all in `parser.c`'s own `BUILTIN_SIGNATURES` but
  were never wired into `vm.c`'s `call_builtin` — confirmed directly,
  zero hits. They parse fine but silently no-op through the VM today.
  Flagged clearly; the user chose to keep it as its own separate,
  similarly-sized follow-up batch.
- **`TEXT`/`SHOW`/`SAVE`/`DELETEFILE` + general file I/O** (2026-08-10)
  — the gap flagged above, fixed for 12 of the 13 originally-named
  commands. Unlike sprites/`PAUSE`, these already existed in `eval.c`
  for `ast_eval` — the fix was the established `eval_X_value`-core
  split (same pattern as lists/property-lists/turtle-drawing): each
  `do_X` split into a thin wrapper (unchanged) plus a new core taking
  already-evaluated `EvalValue`s, exposed for `vm.c`'s `call_builtin`.
  Zero new opcodes, zero `compiler.c` changes; confirmed via a full
  `make test` run that the refactor changed nothing about `ast_eval`'s
  own behavior. `LOAD` deliberately excluded — `do_load` runs a loaded
  file's own top-level statements via `exec_block` (the tree-walker),
  which needs its own dedicated opcode and a runtime nested-compile-
  and-run mechanism, a real design question of its own.
  21 new headless `tests/test_vm.c` cases, direct single-engine VM
  tests rather than `shadow_diff_vm` — real file I/O against `build/`
  means running the same script through both engines back-to-back
  would double up non-idempotent side effects (a second `DELETEFILE`/
  `OPENAPPEND` behaves differently the second time). Confirmed clean
  under AddressSanitizer (same one pre-existing crash); all 6
  `make test` suites pass; `bin/logo`/`bin/logi` build and run cleanly.
  **A much bigger version of the same gap, found via this batch's own
  testing, not gone looking for**: writing a `DIRECTORY` test hit
  `LIST?`, also silently broken through the VM. A scripted audit
  (`parser.c`'s own `BUILTIN_SIGNATURES` names diffed against every
  name `vm.c`/`compiler.c` actually recognize) found **35** such names
  total: 15 math operators, 7 list/word operators, the 4 type
  predicates, `APPLY`/`RUN`, 6 turtle-command short aliases (full names
  already work, just not `HT`/`ST`/`SETH`/`SETPC`/`SETPW`/`SETBG`), and
  `LOAD` itself. Reported to the user in full rather than fixed ad hoc;
  scope/priority for the rest is their own call, not yet decided.
- **Math operators — the first slice of the 35-name audit** (2026-08-10):
  `ABS`/`SQRT`/`POWER`/`RANDOM`/`ROUND`/`MOD`/`SIN`/`COS`/`TAN`/`ASIN`/
  `ACOS`/`ARCTAN`/`LN`/`LOG`/`EXP` now work through the VM. Even
  simpler than file-I/O: every one is a pure function needing no
  `app`/`pool` at all, the same shape `eval_int_value` already
  established, so each new `eval_X_value` core is a one-liner around
  the existing `fabs`/`sqrt`/`pow`/… call. Zero new opcodes, zero
  `compiler.c` changes, confirmed via `make test` that `ast_eval`'s own
  behavior is unchanged. 5 new `tests/test_vm.c` cases via ordinary
  `shadow_diff_vm` — except `RANDOM`, which draws from the same
  process-global RNG stream in both engines, so shadow-diffing it
  directly would spuriously "diverge"; tested as a single-engine bounds
  check instead. Confirmed clean under AddressSanitizer (same one
  pre-existing crash); all 6 `make test` suites pass; `bin/logo`/
  `bin/logi` build and run cleanly.
- **The remaining 20 names — closing out the 35-name audit entirely**
  (2026-08-10): type predicates, list/word operators, and turtle short
  aliases were more mechanical `eval_X_value`-core work, zero new
  opcodes. `APPLY`/`RUN`/`LOAD` were the genuinely new pieces.
  `APPLY` resolves its callee dynamically by name (`find_proc_def`, not
  a prototype chain), mechanically close to `OP_SEND`'s own success
  path, but never hands back a value at all even when the applied
  procedure calls `OUTPUT` — a new `OP_VOID_DISCARD` (pop whatever's
  there, push void) enforces that on every path uniformly.
  `RUN`/`LOAD` needed the one genuinely new mechanism in this whole
  project: both execute a *whole statement sequence* via
  `interpreter.c`'s own `exec_block` (the tree-walker), no VM
  equivalent existed. New `OP_RUN`/`OP_LOAD` re-lex/parse/
  `compile_program` the source into a fresh, independent
  `BytecodeChunk` at runtime, then run it via a *recursive* `vm_run`
  call sharing this same `Vm`'s stack/frames — the same trick `MAP`/
  `FILTER`/`REDUCE`/`FOREACH` templates established, against a
  genuinely separate chunk instead of a shared one. `RUN` keeps
  `do_run`'s own `run_depth` cap; `LOAD` has none, matching `do_load`'s
  own documented asymmetry. `LOAD`'s path stays a compile-time literal
  — the parser's own eager-`LOAD`-following pre-pass already makes a
  loaded file's own procedures callable at compile time; this
  recursive call's only job is the loaded file's own top-level code.
  A real, narrow correctness wrinkle found while designing this (not
  by accident): because `RUN`/`LOAD`'s own recursive call uses a
  *different* chunk than the outer one, a bare `OUTPUT`/`STOP` at the
  snippet's own top level would pop a frame belonging to the enclosing
  procedure and set `pc` to an index only meaningful in the *outer*
  chunk — the same `frame_floor` mitigation the template batch already
  accepts as a documented gap applies here too. Also generalized the 5
  existing suspend-refusal messages to mention `RUN`/`LOAD` alongside
  templates, since `vm_run_depth > 1` is now reachable through either.
  15 new `tests/test_vm.c` cases, all ordinary `shadow_diff_vm` —
  unlike the file-I/O batch, every one of these is pure (`LOAD` only
  reads, never writes/deletes; `APPLY`/`RUN` never touch the
  filesystem), so shadow-diffing never double-mutates anything.
  Confirmed clean under AddressSanitizer (same one pre-existing crash);
  all 6 `make test` suites pass; `bin/logo`/`bin/logi` build and run
  cleanly.
  **This closes the 35-name audit entirely — a scripted re-run confirms
  zero `parser.c`-declared builtins remain unreachable from `vm.c`.**
- **VM-owned scope storage — real recursion-depth independence**
  (2026-08-10): the one named gap the audit batches above left open —
  since Stage 2 first shipped, the VM's own `pc`/`VmFrame` array had
  already decoupled a Logo-level call from the C stack (a call is a
  frame push + `pc` jump, never a new C function call), but variable
  *bindings* still went through `app->scopes[]`/`MAX_SCOPE_DEPTH` (200)
  shared with `eval_logo`/`ast_eval`, via `eval_push_scope_for_call` —
  so real recursion depth stayed capped at the same limit those two
  C-stack-recursive engines needed anyway (empirically ~100-186 before
  ASan flags a real overflow). This batch gives the VM its own,
  separate, much deeper scope stack.
  Full audit of every direct `app->scopes[]`/`app->scope_depth` touch
  first (not guessed): exactly five functions own all of it --
  `find_var`, `find_or_create_var` (static, `interpreter.c`),
  `set_var`/`set_var_word`/`set_var_list`/`set_var_array`,
  `eval_local_declare`, and `eval_push_scope_for_call` — every other
  caller (`THING`, `MAKE`, `LOCAL`, `AST_VARREF` reads, `FOR`'s own
  loop variable, `MAP`/`FILTER`/`REDUCE`/`FOREACH`'s template-variable
  binding) already funneled through those. A new `ScopeStack` struct
  (`logo_types.h`: `Scope *scopes; int *scope_depth; int capacity;`)
  gets threaded through all five instead of them reaching into
  `app->scopes` directly — `app_scope_stack(app)` (new,
  `interpreter.c`) is what `eval_logo`/`ast_eval` still pass, unchanged
  in every observable way; a new `vm_scope_stack(vm)` (`vm.c`-internal)
  is what every VM opcode/helper passes instead, backed by two new `Vm`
  fields (`Scope scopes[MAX_VM_SCOPE_DEPTH]; int scope_depth;`,
  `MAX_VM_SCOPE_DEPTH` = 2000, chosen after asking the user directly —
  weighed against a dynamically-`realloc`-grown alternative, which
  would have been this codebase's first departure from its consistent
  fixed-pool style; 2000 was picked as the fixed size, ~9MB per `Vm`
  thanks to `Scope`'s own `Variable`-array-of-`word[512]` shape, a
  ~10x improvement over the old real-world ceiling). One shared
  implementation, two independent storage arrays, same discipline
  `eval_push_scope_for_call`'s own original design already established
  for stack/frames — the VM still can't drift from `ast_eval` on
  scoping *semantics*, only *capacity* now differs.
  `call_builtin` (`vm.c`) gained a `Vm *vm` parameter (needed by
  `THING`'s own `vm_scope_stack` call) — its one call site was trivial
  to update. `bind_template_var`/`save_var`/`restore_var` similarly
  gained `Vm *vm`, threaded from their own four `exec_*_compiled`
  callers, which already had it.
  A genuine, caught-not-assumed regression along the way: `MAX_VM_FRAMES`
  (originally 256, deliberately larger than the old `MAX_SCOPE_DEPTH`
  200 specifically so `eval_push_scope_for_call`'s own vocal "Recursion
  too deep, call ignored" message always fired before `exec_call_proc`'s
  own silent frame-count safety net) was first raised to exactly 2000,
  matching the new scope cap — but since a `VmFrame` and a `Scope` are
  always pushed in lockstep, this made the two checks tie at the same
  depth, and the *silent* one runs first in `exec_call_proc`'s own code
  order. `test_recursion_depth_cap_reports_error_not_a_crash` caught it
  immediately (empty VM output instead of the expected message) — fixed
  by restoring the same "frames deliberately larger than scopes"
  relationship (`MAX_VM_FRAMES = MAX_VM_SCOPE_DEPTH + 16`), not by
  guessing a value that happened to work.
  Since scope storage now lives inside `Vm` itself, `Agent`'s own
  Phase 6 save/restore mechanism (`agent.c`) needed a real fix, not
  just cleanup: its separate `scopes[MAX_SCOPE_DEPTH]`/`scope_depth`
  fields and the memcpy dance copying them to/from `app->scopes` would
  have silently broken agent scope isolation once `vm.c` stopped
  writing to `app->scopes` at all — removed entirely, since each
  `Agent`'s own embedded `Vm` already isolates its scopes for free
  (structural, not copy-based). `ui.c`'s `run_logo_script` and
  `tests/test_agent.c`'s own mirroring helper both had the same
  now-dead copy removed.
  A new `tests/test_vm.c` case proves the actual point empirically, not
  just in principle: 1000 levels of real recursion (`countdown`
  counting down to 0 via `OUTPUT`) succeeds cleanly under the VM with
  no "Recursion too deep" anywhere in its output — this can't be a
  `shadow_diff_vm` case, since `ast_eval` would never survive 1000
  levels itself. Confirmed clean under AddressSanitizer for both
  `test_vm` and `test_agent` this run (the previously-noted
  `eval_logo`-only recursion crash reproduced identically against a
  `git stash`d pre-change baseline, confirming it's pre-existing and
  unrelated, not something this batch introduced); all 7 `make test`
  suites pass; `bin/logo` builds warning-free and was confirmed live —
  both a normal launch and `examples/concurrent_agents.logo` (exercising
  the just-changed `Agent`/scheduler code) stayed alive with no crash.
- **`INSTR_MAX_TEXT` word-literal truncation — fixed** (scoped
  2026-08-10, built 2026-08-11, found as a byproduct of debugging the
  LAUNCH-after-resume fix — see `docs/CONCURRENT_AGENTS_DESIGN.md`'s own
  section of that name; own memory entry
  `instr_max_text_truncation_bug.md`). Root cause: `Instr.text`
  (`bytecode.h`) was shared by six opcodes for two fundamentally
  different kinds of content — *identifiers*
  (`OP_PUSH_VAR`/`OP_SET_VAR`/`OP_CALL_BUILTIN`/`OP_CALL_PROC`/
  `OP_CHECK_OUTPUT`, plus `BytecodeChunk.proc_table[].name`) and
  *literal word data* (`OP_PUSH_WORD`). `INSTR_MAX_TEXT` (64) was sized
  for the former — and is already generous there, confirmed by checking
  the rest of the codebase: identifiers are consistently capped at just
  32 bytes elsewhere (`Variable.name`, `Procedure.name`,
  `param_names`) — but silently applied the same 64-byte ceiling to the
  latter too, where it didn't belong: a literal word (especially a
  `'raw text with spaces'` literal, which can be a full sentence) has
  no such natural bound, and the AST layer itself already budgets
  `AST_MAX_TEXT` = 512 for exactly this, matching `EvalValue.word[512]`/
  `Variable.word[512]`'s own established convention throughout the
  runtime. `INSTR_MAX_TEXT` = 64 was the one place in the whole pipeline
  that didn't honor that 512-byte word budget.
  **Resolved with the user**: left `Instr.text[64]` untouched for
  identifiers (no evidence it's ever a real problem there), and gave
  `OP_PUSH_WORD` its own separate storage sized to the language's
  actual word budget (512) via a new `BytecodeChunk`-level side table
  (`word_literals[MAX_CHUNK_WORD_LITERALS][AST_MAX_TEXT]` +
  `word_literal_count`, `bytecode.h`/`bytecode.c`, `MAX_CHUNK_WORD_LITERALS`
  = 2048) — the same pattern `BytecodeChunk.proc_table` already
  established for `SEND`'s own runtime-resolved calls. `OP_PUSH_WORD`'s
  own `Instr.a` (an existing generic operand field, already reused
  per-opcode for jump targets/argc/target pc elsewhere) is now an index
  into this table instead of inlining the text. `Instr` itself never
  grows, so every *other* opcode's own memory footprint is untouched —
  chosen over simply raising `INSTR_MAX_TEXT` to 512 directly (a
  one-line change, but one that would grow *every* instruction in
  `MAX_INSTRUCTIONS` = 8192 by 448 bytes regardless of whether it ever
  carries text, ~3.5MB of waste per chunk vs. this approach's ~1MB,
  paying for capacity almost no instruction actually uses).
  `bytecode_add_word_literal(chunk, text)` (mirroring
  `bytecode_find_proc`'s own shape) registers a literal and returns its
  index, or `-1` if `MAX_CHUNK_WORD_LITERALS` is exhausted — same "loud
  return value, not a silent crash" convention `bytecode_emit` already
  uses for chunk-full. All three `OP_PUSH_WORD` emission sites in
  `compiler.c` (the `AST_WORD` case in `compile_expr`, plus `FOREVER`'s
  and `FOR`'s own compiled-in error-message literals) now call it
  instead of `snprintf`-ing directly into `instr.text`; `vm.c`'s
  `OP_PUSH_WORD` handler reads `chunk->word_literals[instr->a]` instead
  of `instr->text` (falling back to an empty word if `.a` is `-1`,
  matching the overflow case above rather than reading out of bounds).
  `bytecode.h` now includes `ast.h` for `AST_MAX_TEXT` — the one
  dependency it needed to size the side table correctly; `ast.h` is
  exactly as GTK/GLib-free as `bytecode.h` itself, so this doesn't
  compromise the "no dependency on GTK/GLib/interpreter.h" property
  either file claims.
  New regression test:
  `test_vm.c`'s
  `test_a_word_literal_longer_than_the_old_63_byte_instr_text_limit_is_not_truncated`
  (a 100-byte literal, with a marker byte right past the old 63-byte
  ceiling, round-trips through `PRINT` byte-for-byte). Verified: warning-free
  build, all 7 `make test` suites pass, and a standalone ASan build of
  just this scenario (lex → parse → compile → `vm_run`, isolated from
  `test_vm.c`'s own unrelated pre-existing `eval_logo`/ASan
  deep-recursion stack overflow — confirmed identical against a
  `git stash`d pre-change baseline) ran clean with the correct output.
  - **Next**: `PAUSE`'s own Ctrl+C-based force-unpause, the still-open
    `WHILE`/`FOR` iteration-cap gap, and the
    `OUTPUT`/`STOP`-inside-a-template limitation remain smaller,
    optional follow-ups. `ANIMATESPRITE`'s own sprite subsystem still
    needs manual confirmation of its real image-decoding half. No
    other named gap is currently open in Stage 2.
- **Self-contained `BytecodeChunk` — Stage A of bytecode save/load/
  assemble** (2026-08-11; user: "something that would be cool now
  though is the ability to save the bytecode to file, load bytecode,
  and even have an assembler for it" — see `docs/ROADMAP.md`'s own new
  "Bytecode save/load/assembler" section for the full A/B/C/D staging).
  **The finding that drove this**: `BytecodeChunk` wasn't actually
  self-contained despite `bytecode.h`'s own "no dependency on GTK/GLib/
  interpreter.h" framing — `OP_CALL_PROC`/`OP_SEND`/`OP_APPLY`/
  `OP_LAUNCH` all called `find_proc_def(pool, name)` at *runtime*, on
  every single call, not to find the target address (already
  backpatched into the instruction at compile time) but to read the
  callee's own `param_count`/`param_names` off the AST node for
  argument binding; `OP_PUSH_LIST_LITERAL` stored an AST node index and
  rebuilt the list from that raw subtree fresh each visit; `TEXT`/
  `SAVE`/`SHOW` read a procedure's `body_text` — a pointer into the
  *original source buffer*, not even the AST — to reproduce exact
  source. A saved/loaded/hand-assembled chunk without its original
  `AstPool` and source text would have crashed or misbehaved the
  moment a program called a procedure, used a list literal, or called
  `TEXT`/`SAVE`/`SHOW`. **Resolved with the user** (`AskUserQuestion`,
  two rounds): make the chunk genuinely self-contained rather than
  bundling a trimmed `AstPool` fragment alongside it — an assembler's
  own hand-written input has no `AstPool` to lean on anyway, so
  building the assembler (Stage C) basically forces this shape
  regardless; better to do it once, properly, now.
  **What moved onto the chunk**: `ProcAddr` (`bytecode.h`) gained
  `param_count`/`param_names[AST_MAX_PARAMS][32]`/
  `source_text[MAX_PROC_SOURCE_TEXT]` (8192, matching
  `interpreter.c`'s own established `Procedure.body` budget) —
  populated once in `compile_program`'s own pass 1, copied straight off
  each `AST_PROC_DEF` node exactly as `find_proc_def`-based lookups used
  to read it. A new `bytecode_find_proc_entry(chunk, name)` (returns the
  whole `ProcAddr`, `NULL` if not found) is what every call opcode now
  uses instead of a `find_proc_def(pool, ...)` + separate
  `bytecode_find_proc(chunk, ...)` pair (`OP_APPLY`/`OP_SEND` used to
  need both; now one lookup does it all).
  **List literals**: rather than inventing a second, parallel node-pool
  serialization for nested list structure, `compiler.c`'s own
  `AST_LIST_LITERAL` case now calls `render_list_literal_source` — the
  exact renderer `compile_template_call` already trusted for
  `MAP`/`FILTER`/`REDUCE`/`FOREACH`'s own compiled-template fast path,
  turning an `AST_LIST_LITERAL` subtree back into ordinary,
  re-parseable Logo source text — wraps it in `[`/`]`, and stores it in
  a new `chunk->list_literals[MAX_CHUNK_LIST_LITERALS][MAX_LIST_LITERAL_TEXT]`
  table (2048 entries × 2048 bytes). `OP_PUSH_LIST_LITERAL`'s `.a` is
  now an index into that table. `vm.c`'s handler calls a new
  `eval_build_list_literal_from_text(app, text)` (`eval.c`) which
  lexes/parses the text fresh into a throwaway scratch `ParseResult`
  and defers to the original `eval_build_list_literal` for construction
  — the same "re-parse text at runtime" shape `OP_RUN`/`OP_LOAD` already
  use for arbitrary code, just for one list-literal expression. **Known,
  accepted tradeoff**: unlike `MAP`/`FILTER`/`REDUCE`'s own per-call
  *reused* scratch `ParseResult`, this allocates and frees a fresh
  ~6.7MB `ParseResult` on *every visit* — a list literal inside a hot
  loop now pays a real, measurable-but-modest re-parse cost it didn't
  before. Deliberately not optimized away in this stage (would need a
  `Vm`-owned persistent scratch pool, threaded through every `free(vm)`
  call site across `agent.c`/`ui.c`/every test file) — flagged here as
  a real, well-scoped follow-up rather than silently accepted or
  silently over-engineered around.
  **`eval_push_scope_for_call`** (`eval.h`/`eval.c`) — the shared
  scope-binding function every call construct in both engines already
  funneled through — had its signature changed from taking `AstNode
  *def` to taking `(const char *proc_name, int param_count, const char
  param_names[][32], ...)` directly, decoupling it from `AstPool`
  entirely; `call_ast_procedure` (`ast_eval`'s own path, untouched
  otherwise) just passes `def->text`/`def->param_count`/
  `def->param_names` at its own call site.
  **`SEND` specifically**: `eval_resolve_method` (pool-dependent) was
  left untouched, but `exec_send` no longer calls it — `eval_resolve_message`
  (the pure `app->plist_entries` prototype-chain walk half, previously
  `static`, now exposed via `eval.h`) is called directly, and the
  resulting procedure name is resolved against `chunk->procs[]` via
  `bytecode_find_proc_entry` instead of `find_proc_def(pool, ...)` —
  `exec_send` now reproduces `eval_resolve_method`'s own three error
  messages ("does not understand"/"is not a method on"/"must take
  :self as its first input") locally, the same "reimplement the exact
  message independently in `vm.c`" pattern every other call opcode
  here already follows.
  **A real regression, caught by the existing shadow-diff test suite,
  not written in from the start**: `ERASE` (`eval_erase_declare`)
  blanks an `AST_PROC_DEF` node's own `.text` so no future lookup can
  match it — but `chunk->procs[]` is now an independent copy of every
  procedure's identity, so blanking only the AST side left
  `OP_CALL_PROC`/`OP_SEND`/`OP_APPLY`/`OP_LAUNCH` (all chunk-only
  lookups now) still able to find and call an "erased" procedure —
  caught immediately by `test_erase_deletes_a_procedure_so_it_can_no_
  longer_be_called` failing (`tree` correctly reported "I don't know
  how to foo"; `vm` printed `1`, having called it anyway). Fixed with a
  new `bytecode_erase_proc(chunk, name)` (mirrors `eval_erase_declare`'s
  own "blank the name" mechanism, just against `chunk->procs[]`),
  called from `OP_ERASE`'s handler alongside the existing
  `eval_erase_declare` call, not instead of it.
  **Deliberately out of scope for Stage A**: `TEXT`/`SAVE`/`SHOW` are
  ordinary builtins dispatched through `call_builtin` straight into
  `eval_text_value`/`eval_save_value`/`eval_show_value`
  (`eval.c`), which still read `pool` directly (`find_proc_def` +
  `body_text`/`body_len`, or `eval_append_procedure_text`) — fully
  functional today (a live `AstPool` is still always available for an
  ordinarily compiled-and-run script), but **not yet chunk-only**. A
  future bytecode-loaded-only program (no original `AstPool` at all)
  would need these three ported to read `chunk->procs[]`'s own new
  `source_text` field instead, or would need to accept them reporting
  "no such procedure" on a loaded program specifically — a real,
  separate decision for whichever of Stage B/C/D actually needs it,
  not silently bundled into "self-contained" here.
  **Verified**: warning-free build; all 7 `make test` suites pass
  (after the `ERASE` fix above — the *only* observed output diff
  across the whole batch, everything else byte-for-byte identical to
  before, matching the "no new syntax, no behavior change" scope);
  a standalone headless driver covering plain + nested list literals
  (quote/varref/leading-sign preservation, matching the exact
  `PRINT [a "b c]`-class fidelity `parser.c`'s own `parse_list_literal`
  already guarantees), an ordinary procedure call, `SEND` success + all
  three failure paths, 500-level recursion (well past the old 200
  shared limit, proving `chunk`-based param binding doesn't regress
  depth), `ERASE` then a failed call, `APPLY`, `LAUNCH` (correctly
  returns `VM_RUN_SUSPENDED_LAUNCH`), and `TEXT`/`SAVE`/`SHOW` (still
  working) — run twice, plain and under ASan, both clean; a live
  `bin/logo` launch confirmed via process liveness (no GUI screenshot,
  per this project's own constraint).
- **Disassembler — Stage B of bytecode save/load/assemble** (2026-08-11;
  see `docs/ROADMAP.md`'s own "Bytecode save/load/assembler" section).
  `bytecode_disassemble(chunk, FILE *out)` (`bytecode.h`/`bytecode.c`)
  renders a `BytecodeChunk` as text — no `Vm`/`LogoApp` involved at
  all, since Stage A's own self-containment work already made a chunk
  carry everything a reader (or, eventually, Stage C's assembler) would
  need. Two sections: `PROCS` lists every still-defined entry in
  `chunk->procs[]` (name, `start_pc`, `param_count`/`param_names`, the
  full `source_text`) — the one piece of chunk state an instruction's
  own operands don't always surface, since a procedure reachable only
  via `SEND`/`APPLY`/`LAUNCH` may have no static `OP_CALL_PROC`
  anywhere referencing it; `CODE` lists every instruction in order,
  each proc's own `start_pc` doubling as an inline `name:` label right
  before the instruction at that `pc`. `word_literals[]`/
  `list_literals[]` indices are resolved into their own literal text
  inline at each use site (e.g. `OP_PUSH_WORD "hello"`,
  `OP_PUSH_LIST_LITERAL [1 2 3]`) rather than left as raw indices next
  to a separate table a reader would have to cross-reference. Jump
  targets (`OP_JUMP`/`OP_JUMP_IF_FALSE`/`OP_CHECK_THROW`) print as
  `@N`. A new `bytecode_opcode_name(OpCode)` backs every instruction's
  own mnemonic — a plain one-case-per-enum-member switch with no
  `default:`, so `-Wswitch` itself (already part of this project's own
  `-Wall -Wextra`) fails the build the moment a future opcode is added
  without a matching name, the same safety net the exhaustive `switch`
  in `vm.c`'s own `vm_run` already relies on for execution; exposed via
  `bytecode.h` for Stage C's eventual reverse lookup (name → `OpCode`).
  **Verified**: a new, GTK-free `tests/test_bytecode.c` (`make
  test-bytecode`, wired into `make test`) lexes/parses/compiles real
  Logo snippets via `compiler.c` directly — no `interpreter.h`/`LogoApp`
  needed, since `compiler.c` itself has no such dependency either — and
  asserts on the disassembly text via `open_memstream` (confirmed
  available on this project's own macOS toolchain), covering arithmetic,
  a word literal, a nested list literal, an `IF`'s own jump target, a
  user procedure's `PROCS` entry + inline label + call site, `ERASE`
  correctly dropping an entry from `PROCS`, and the header's own
  instruction/proc/literal counts. All 8 `make test` suites pass,
  warning-free rebuild, and a manual dump of a recursive `FACT`
  procedure plus a `REPEAT` loop visually confirmed the format reads
  correctly end to end. Not yet built: Stage C (assembler, text →
  chunk) and Stage D (`SAVEBYTECODE`/`LOADBYTECODE` builtins) — this
  disassembler's own text format is deliberately not claimed as Stage
  C's eventual input syntax; that's a decision for when Stage C is
  actually scoped.
- **Assembler — Stage C of bytecode save/load/assemble** (2026-08-11;
  see `docs/ROADMAP.md`'s own "Bytecode save/load/assembler" section).
  `bytecode_assemble(text, chunk, error, error_size)` (`bytecode.h`/
  `bytecode.c`) parses text into a `BytecodeChunk`, appending via the
  exact same `bytecode_emit`/`bytecode_add_word_literal`/
  `bytecode_add_list_literal` functions `compiler.c` itself calls, so a
  hand-assembled chunk is indistinguishable at runtime from a compiled
  one. A hand-rolled, line-oriented parser -- same "reimplement
  precisely, don't reach for a heavier abstraction" style `lexer.c`/
  `parser.c` already use for Logo source itself, just for this format.
  **Grammar decision**: rather than inventing a brand-new assembly
  syntax, `bytecode_assemble` accepts exactly what `bytecode_disassemble`
  already emits (Stage B's own `@N` absolute-pc jump targets, `PROCS:`
  header, `---- source ----` blocks) unchanged -- Stage B needed no
  retroactive changes at all -- and *additionally* accepts symbolic
  labels: any `name:` line anywhere in `CODE:` (already used for a
  proc's own entry point; a hand-writer can add ordinary ones too)
  becomes a resolvable jump target, alongside `@N`, for
  `OP_JUMP`/`OP_JUMP_IF_FALSE`/`OP_CHECK_THROW`/`OP_CALL_PROC`/the
  `OP_MAP_COMPILED` family's own operands. This is a genuine two-pass
  assembly: pass 1 walks the whole text once, populating `chunk->procs[]`
  from `PROCS:` and recording every label's own resolved pc (an
  ordinary instruction line just increments a running counter, not yet
  parsed); pass 2 re-walks `CODE:` alone, this time actually parsing
  each instruction's operands (labels already resolved, so a forward
  reference works) and calling `bytecode_emit`.
  **Two deliberate "don't trust stale data" decisions**, both aimed at
  making hand-editing safe: a proc's own `start=` field in its `PROCS:`
  entry is read (so the line's own grammar is symmetric with
  `bytecode_disassemble`'s output) but never actually used -- its real
  `start_pc` always comes from resolving its own `<name>:` label in
  `CODE:` after pass 1, found via the same `asm_find_label` case-
  insensitive lookup (`strcasecmp`, matching `bytecode_find_proc`'s own
  convention) every ordinary jump target uses; and `CODE:`'s own
  leading `<pc>:` address column, when present, is likewise read past
  (`asm_strip_pc_prefix`) but never trusted, since `bytecode_assemble`
  tracks its own running address. Together these mean inserting or
  removing an instruction in a hand-edited file never requires manually
  renumbering anything that follows.
  **Reverse opcode lookup**: `asm_lookup_opcode` is a linear scan
  comparing each mnemonic against `bytecode_opcode_name(i)` for `i` in
  `[0, OPCODE_COUNT)` -- a new `#define OPCODE_COUNT (OP_MOTION_DELAY + 1)`
  right after the enum, the one place Stage C's own reverse table
  relies on `OpCode` staying a plain contiguous-int enum (already an
  assumption `bytecode_opcode_name`'s own forward switch makes).
  **Errors are loud, not silent**: an unknown opcode mnemonic, an
  undefined or duplicate label, a `PROCS:` entry with no matching
  `CODE:` label, a params-list-count/`argc=` mismatch, or a fixed table
  filling up (`MAX_CHUNK_PROCS`/`MAX_INSTRUCTIONS`/`MAX_ASM_LABELS`/
  word or list literal tables) each fail immediately with a one-line,
  line-numbered message via a small `asm_error` `vsnprintf` helper --
  the same "loud error, not silent corruption" policy every other
  fixed-pool function in `bytecode.c` already follows -- rather than
  emitting a partially-built, silently-wrong chunk.
  **Verified**: warning-free build. Structural round-trip tests
  (`tests/test_bytecode.c`, GTK-free, extending Stage B's own suite) --
  disassemble a compiled chunk, reassemble it, and assert the
  instruction stream and every proc's own metadata match exactly --
  cover arithmetic, a nested list literal (quote/varref/leading-sign
  fidelity), `IF`'s own jump target, recursion (`FACT`), multiple
  procedures (forward reference + a real call chain), and a compiled
  `MAP` template; plus dedicated tests for hand-written labels with a
  forward jump, an omitted `<pc>:` column, a deliberately-wrong `start=`
  field being correctly ignored, and every error path listed above.
  Separately, a new `test_vm.c` test -- the one check that genuinely
  needs the full `Vm`/`LogoApp` stack, so it couldn't live in the
  GTK-free suite -- compiles a recursive `FACT` program normally,
  disassembles the resulting chunk, reassembles it via
  `bytecode_assemble`, and runs the REASSEMBLED chunk against a
  completely empty `AstPool` (`node_count=0` -- no original AST
  whatsoever) via an ordinary `vm_run` call, confirming its output
  (`720` for `FACT 6`) matches the originally-compiled run's own output
  exactly: genuine proof, not just structural equality, that a chunk
  built from nothing but text can execute completely standalone --
  directly answering the user's own original ask ("save the bytecode
  to file, load bytecode"). All 8 `make test` suites pass; both
  `test_bytecode` and `test_vm` also run clean under ASan; a live
  `bin/logo` launch confirmed via process liveness (no GUI screenshot,
  per this project's own constraint). Not yet built: Stage D
  (`SAVEBYTECODE`/`LOADBYTECODE` builtins wiring `bytecode_disassemble`/
  `bytecode_assemble` to actual files, plus round-trip tests through
  real disk I/O).
- **`SAVEBYTECODE`/`LOADBYTECODE` — Stage D of bytecode save/load/
  assemble** (2026-08-11; see `docs/ROADMAP.md`'s own "Bytecode
  save/load/assembler" section — this closes the initiative). Two new
  `ARG_QUOTED_WORD` builtins, `parser.c`'s `BUILTIN_SIGNATURES` table,
  same shape as `SAVE`/`LOAD`/`DELETEFILE` right next to them.
  **Deliberately NOT dedicated opcodes** (unlike `LOAD`/`ERASE`): with
  no special compile-time AST coordination needed, their path argument
  just flows through `compile_call`'s ordinary generic dispatch --
  `compile_expr` on an `ARG_QUOTED_WORD` argument's own `AST_WORD` node
  emits an ordinary `OP_PUSH_WORD` via `bytecode_add_word_literal`, the
  same unbounded-effectively `word_literals[512]` table every other
  word literal already uses -- so unlike `LOAD`/`ERASE`'s own dedicated-
  opcode `Instr.text[64]` fields, there's no `INSTR_MAX_TEXT`-style
  truncation risk on a long path at all. This is a real, concrete
  benefit of NOT reaching for a new opcode reflexively.
  **`SAVEBYTECODE "path`**: `call_builtin` (`vm.c`) gained a new
  `BytecodeChunk *chunk` parameter (threaded from its own single call
  site, `vm_run`'s `OP_CALL_BUILTIN` case, which already had `chunk` in
  scope) purely so `SAVEBYTECODE` can reach the chunk it's actually
  running inside of -- every other builtin ignores it. A new
  `exec_savebytecode` disassembles that chunk (`bytecode_disassemble`
  into an `open_memstream` buffer) and writes it via
  `g_file_set_contents`, matching `SAVE`/`eval_save_value`'s own exact
  messaging shape ("Saved `<path>`\n" / "SAVEBYTECODE: could not write
  file\n").
  **`LOADBYTECODE "path`**: a new `exec_loadbytecode` reads the file
  (`g_file_get_contents`), calls `bytecode_assemble` into a fresh
  scratch chunk, then runs it via a RECURSIVE `vm_run` call sharing
  this same `Vm`'s own stack/frames -- the identical mechanism
  `exec_run`/`exec_load` already use for `RUN`/`LOAD` (same
  `frame_floor` OUTPUT/STOP-escape guard too), except the scratch
  `AstPool` it passes is genuinely never read: the assembled chunk is
  already fully self-contained (Stage A), so an empty, `calloc`'d one
  only exists to satisfy `vm_run`'s own signature.
  **A real gap this surfaced, fixed as part of this batch**:
  `compile_program`'s own top-level entry point (`program_start`, its
  return value) is NOT `0` in general -- procedure bodies compile
  first, top-level statements after -- and Stage B/C's own text format
  had no way to record or recover it at all; `bytecode_assemble` was
  quietly relying on its *caller* already knowing the right value from
  having compiled the program itself moments earlier, which is exactly
  what `LOADBYTECODE` can never do (there's no compile step at all).
  Fixed by adding `int start_pc` directly to `BytecodeChunk` (set by
  `compile_program` itself, alongside its existing return value, not
  instead of it -- every existing caller is unaffected), plus a new,
  REQUIRED `"START: @N"` (or `"START: <label>"`, resolved the same
  two-pass way as any other jump target) line in `bytecode_disassemble`/
  `bytecode_assemble`'s own text format, right after the header
  comment. `tests/test_bytecode.c`'s own round-trip tests were extended
  to check `start_pc` too, and several of its hand-written-assembly
  tests needed a `START:` line added once it became required --
  existing coverage, not a regression.
  **A real, deliberately-documented-not-silently-accepted limitation**,
  found by actually testing the intended doc example before writing it
  down (a `LOADBYTECODE "square.lgb` followed immediately by `SQUARE
  50` in the same script) and watching it fail with "unknown word:
  SQUARE": unlike `LOAD`, whose own procedures the PARSER eagerly
  re-parses and hoists into the *caller's* `AstPool` at compile time
  (see `OP_LOAD`'s own `bytecode.h` comment), a procedure inside a
  `LOADBYTECODE`d file is never compile-time-visible to the script that
  loaded it -- there's no Logo source in a `.lgb` file to hoist from at
  all, just an already-compiled instruction stream discovered only at
  runtime. A `LOADBYTECODE`d program has to be a genuinely self-
  contained unit that does its own real work at its own top level (the
  corrected doc example: `SQUARE` is CALLED from within the saved
  program itself, not left for the caller to invoke afterward).
  **VM-only, confirmed harmless for the tree-walkers**: `SAVEBYTECODE`/
  `LOADBYTECODE` are recognized by `parser.c`'s shared grammar table but
  have no branch in `eval.c`'s own dispatch at all -- `ast_eval`/
  `eval_logo` have no `BytecodeChunk` concept to save in the first
  place, and already-existing `do_user_procedure_call` machinery
  gracefully reports "I don't know how to SAVEBYTECODE" if either
  engine (unreachable from `bin/logo`, used only for this project's own
  shadow-diff testing) is ever asked -- no new special-casing needed.
  **Verified**: a live end-to-end scratch run -- compile a real
  recursive multi-procedure program (`FACT`), run it, `SAVEBYTECODE` it
  to a real file, then in a COMPLETELY SEPARATE process-like session
  with no compile step at all, `LOADBYTECODE` that file and confirm
  identical output (`720`) -- plus every error path (a missing file, a
  write to an unwritable path, a corrupted/non-bytecode file, each
  reporting a specific, clear message rather than crashing). Dedicated
  `tests/test_vm.c` tests cover the same real-file round trip and every
  error path permanently. All 8 `make test` suites pass, warning-free
  full rebuild, both `test_bytecode` and `test_vm` clean under ASan, a
  live `bin/logo` launch confirmed via process liveness. This is the
  last stage of the bytecode save/load/assembler initiative -- the
  user's own original request ("save the bytecode to file, load
  bytecode, and even have an assembler for it") is now fully delivered.
  **Worked examples added afterward** (`examples/bytecode_save.logo` +
  `examples/bytecode_load.logo`, run as two separate `bin/logo`
  invocations, plus `examples/hand_assembled.lgb` -- a bytecode file
  written entirely by hand, never compiled from Logo source at all,
  demonstrating the assembler side directly): building the first
  version of these caught a genuine bug before it ever reached the
  repo -- a single script that both `SAVEBYTECODE`d itself and then
  `LOADBYTECODE`d that same file recurses forever, since `SAVEBYTECODE`
  captures the WHOLE compiled top-level program (every statement, not
  just the ones already run), so a later `LOADBYTECODE` of the same
  file ends up baked into the saved copy too, reloading a program that
  reloads itself. Fixed by splitting the demo into two genuinely
  separate compilations/processes -- the same shape `SAVEBYTECODE`
  requires in general: it must be a program's own last statement.
  **GUI menu equivalents added afterward** (`ui.c`): *File > Save
  Bytecode…*/*File > Load Bytecode…*, same native `GtkFileDialog`
  pattern the existing Open/Save/Export-as-PNG items already use, with
  `.lgb`-filtered dialogs (`bytecode_file_filters`, alongside the
  existing `logo_file_filters`) and `<Meta><Shift>o`/`<Meta><Shift>s`
  accelerators (Shift-modified variants of Open/Save's own single-
  letter ones). **A real design question worth recording**: what does
  "Save Bytecode" even mean from a menu click, given the Logo-level
  builtin's own defined behavior is "save whatever chunk is CURRENTLY
  EXECUTING," and nothing is executing at the moment of a menu click?
  Resolved by making the menu action compile (not run) the entry box's
  own current text and disassemble that -- deliberately NOT running it
  first (no turtle motion, no `PRINT` output, no side effects at all),
  since a menu click causing surprise canvas/output side effects would
  be bad GUI behavior, unlike the builtin's own mid-script invocation
  which is always an explicit, deliberate part of the running program's
  own logic. The disassembled text is captured synchronously BEFORE the
  async save dialog even opens (bundled into a small `SaveBytecodeContext`
  passed as the dialog callback's own `user_data`), since the entry
  box's text could otherwise change while the user is still choosing a
  destination. *Load Bytecode* is simpler: a new `run_bytecode_script`
  mirrors `run_logo_script`'s own shape (same `g_suspended_run` "one
  script at a time" guard, same `SuspendedRun`-wrap-and-dispatch through
  `handle_vm_result`) but calls `bytecode_assemble` instead of `logo_lex`
  +`logo_parse`+`compile_program` -- a loaded program participates fully
  in this app's own suspend/resume machinery (`WAIT`/`PAUSE`/etc. all
  work inside it), unlike `vm.c`'s own `exec_loadbytecode`, which runs
  a LOADBYTECODE'd program via a synchronous nested `vm_run` call
  because that one is always reached mid-instruction-stream inside an
  already-running script, never as the top-level thing being run the
  way a menu click's own target always is. Verified: warning-free
  rebuild, all 8 `make test` suites still pass (`ui.c` isn't linked into
  any test binary, so this only confirms nothing else broke), a live
  `bin/logo` launch confirmed via process liveness (no GUI screenshot
  or interaction -- no Accessibility permission is granted in this
  environment, see this project's own established constraint on GUI
  automation).

## ONKEY/ONCLICK -- background event triggers

Shipped 2026-08-11 (see `docs/CHANGELOG.md`'s own "Mouse/keyboard event
triggers" entry for the full writeup; recorded here for the suspend/
resume-design angle specifically). No new opcodes: both go through the
existing generic `OP_CALL_BUILTIN` dispatch with an `ARG_QUOTED_WORD`
procedure-name argument, same shape as Stage D's `SAVEBYTECODE`/
`LOADBYTECODE` -- registration (`exec_onkey`/`exec_onclick` in `vm.c`)
is a plain, non-suspending builtin call that validates the named
procedure exists in the CURRENTLY EXECUTING chunk with the right arity,
then stores its name in `LogoApp.onkey_handler`/`onclick_handler`.

The interesting design problem was firing a handler later, from a live
GTK key/click event, well after the script that registered it may have
already finished and had its own chunk freed. Two mechanisms this
needed that didn't exist before:

- **`SuspendedRun.owns_chunk`**: `TRUE` for every ordinary
  `run_logo_script`/`run_bytecode_script` run (owns its own
  `result`/`chunk` outright, freed normally when it finishes), `FALSE`
  only for a run spawned by `ui.c`'s new `fire_onkey`/`fire_onclick`,
  whose `result`/`chunk` are *borrowed* from whichever chunk registered
  the handler. `free_suspended_run` and the `VM_RUN_SUSPENDED_LAUNCH`
  case (which used to free `run->chunk`/`run->result` unconditionally)
  both now check this flag first -- otherwise a handler that itself
  calls `LAUNCH` would free a chunk still needed elsewhere.
- **`g_onkey_owner`/`g_onclick_owner`** (`RetainedChunk` -- a bare
  `{result, chunk}` pair): `handle_vm_result`'s `VM_RUN_HALTED` case now
  checks, before freeing a finishing run's own chunk, whether either
  currently-registered handler name resolves against it
  (`bytecode_find_proc_entry`) -- if so, that chunk is adopted into the
  matching retained-owner slot instead of being freed. Two independent
  slots, not one shared one: `ONKEY` and `ONCLICK` can be registered by
  two different scripts at different times, each needing its own chunk
  kept alive; `release_retained_chunk` does a pointer-equality check
  against the OTHER slot before actually freeing, so the common case
  (one script registers both, sharing one chunk) doesn't double-free.
  Released the moment the corresponding handler is cleared (`OFFKEY`/
  `OFFCLICK`), checked unconditionally at the top of every
  `handle_vm_result` call rather than only at clear-time, so it happens
  promptly regardless of what triggered the check.

Firing itself (`fire_handler` in `ui.c`) builds a scope via the same
`eval_push_scope_for_call` every other call site (`OP_CALL_PROC`,
`LAUNCH`, `APPLY`) uses, against a fresh, freshly-`calloc`'d `Vm`
(`scopes[0]`/`scope_depth = 1`, matching `agent.c`'s own pattern for a
freshly spawned agent), then runs it through the ordinary
`vm_run`+`handle_vm_result` path -- a fired handler is a genuine
top-level run in every respect except which chunk backs it, so it gets
`WAIT`/`PAUSE`/`LAUNCH`/etc for free, no special-casing needed. It
silently declines to fire at all if `g_suspended_run != NULL` (the
interpreter isn't idle) -- the existing "one script thread at a time"
rule already enforced for ordinary submissions turns out to double as
exactly the debounce a fast-firing handler needs, with no extra
bookkeeping.

Known, accepted gap: a handler registered by a script that also calls
`LAUNCH` isn't retained, since `VM_RUN_SUSPENDED_LAUNCH`'s own
`scheduler_run` call happens before `VM_RUN_HALTED`'s retention check
could ever run for that chunk. Not fixed here -- combining concurrent
turtle agents with background event handlers in one script is an
advanced combination beyond this feature's own scope, documented as a
limitation rather than chased down.

**`ONMOUSEMOVE`/`OFFMOUSEMOVE` added afterward**: a third handler,
identical shape (`exec_onmousemove` validates a 2-param `:X :Y`
procedure; a third `RetainedChunk` slot, `g_onmousemove_owner`, wired
into the existing `on_canvas_motion` controller alongside `MOUSEPOS`'s
own live `mouse_x`/`mouse_y` update). `release_retained_chunk` needed a
real change, not just a third call site: with two slots it took a
single "other" slot to check for the shared-chunk case; with three, a
chunk shared across all three needs checking against both siblings, so
it now scans a fixed 3-element array of every owner instead of taking
an explicit "other" parameter. The debounce question this feature's own
open item worried about (motion events firing 60+/sec) resolved itself
for free: `fire_handler`'s existing idle check already drops a fire
while the interpreter isn't idle, exactly the behavior needed, no new
mechanism required -- confirmed with a live `examples/onmousemove.logo`
run, not just reasoned about.
