# A real front end + bytecode VM for Logo

Status: **design discussion, no code yet.** This is Phase 5's "compile to a
bytecode VM" item (see `ROADMAP.md`), given its own document because it's
the largest architectural bet this project has taken on — a redesign of
the execution engine itself, not a language feature. Nothing here is
final; it's a proposal to iterate on before any of it gets built.

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

## Stage 2 sketch (for context, not the immediate target)

Once Stage 1's AST exists, compiling it to bytecode is the more
conventional part: a flat instruction array (`PUSH_NUMBER`, `PUSH_VAR`,
`CALL name argc`, `JUMP`/`JUMP_IF_FALSE` for `IF`/`WHILE`, `RETURN`, ...)
executed by a loop over an explicit value stack and an explicit array of
call frames (each frame holding a return instruction pointer and its own
variable bindings) — no C recursion involved in Logo-level procedure
calls at all. That last point is what actually delivers motivations 1 and
2 from the top of this document: pausing becomes "stop the loop, keep the
frame array around"; resuming becomes "restart the loop from the saved
instruction pointer"; and recursion depth becomes bounded by how big we
choose to make the frame array, not by `eval_logo`'s own C-stack
footprint.

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
- **Next**: growing `BUILTIN_SIGNATURES`/`eval.c` together toward
  fuller language coverage (more turtle commands, `FOREACH`, file I/O,
  `TELL`/multi-turtle), and growing the shadow-diff corpus alongside it
  — the mechanism itself is proven; the value from here on is
  coverage.
