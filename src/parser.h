#ifndef LOGO_PARSER_H
#define LOGO_PARSER_H

// parser.h
//
// Stage 1's parser (see docs/BYTECODE_VM_DESIGN.md): recursive-descent
// over lexer.c's token stream, building ast.h's AST. No dependency on
// GTK/GLib/interpreter.h -- same reason lexer.h has none, and what
// makes this independently testable (tests/test_parser.c) before any
// tree-walking evaluator exists.
//
// This is a real but deliberately bounded slice of the language, not
// full coverage of interpreter.c's ~150 operators yet: the four
// resolved design decisions (hoisted TO arities, fully-parsed list
// literals, parenthesized boolean grouping, real line/col errors), the
// full expression/condition precedence chain, the block-based control
// structures (IF/IFELSE/WHILE/REPEAT), TO/END, and a representative
// set of ordinary commands/operators -- proving the architecture
// end-to-end. Growing BUILTIN_SIGNATURES (parser.c) to cover the rest
// is incremental follow-up work, not part of getting the mechanism
// itself right.

#include "ast.h"
#include "lexer.h"

#define MAX_PARSE_ERRORS 32

// The most a single parse will eagerly follow LOAD "literal-path calls
// for (see logo_parse's own comment on eager_loaded_sources below) --
// a fixed cap, same "loud stop, not silent unbounded growth" policy as
// every other fixed-size table in this codebase (MAX_HOISTED_PROCS,
// MAX_AST_NODES, ...), not a performance tuning knob.
#define MAX_EAGER_LOADS 16

typedef struct {
    char message[256];
    int line, col;
} ParseError;

typedef struct {
    AstPool pool;
    int program;    // index of the top-level AST_BLOCK -- the whole script's statements, in order
    ParseError errors[MAX_PARSE_ERRORS];
    int error_count;

    // Owned source-text buffers for every LOAD "literal-path this parse
    // eagerly followed at parse time, so a procedure a LOAD'd file
    // defines is actually callable from the loading script itself (see
    // docs/BYTECODE_VM_DESIGN.md's LOAD-cross-boundary-call fix).
    // AST_PROC_DEF.body_text/.text pointers built from one of these
    // point directly into it (same "pointer into source, never copied"
    // convention as LogoToken.text itself) -- so each buffer's address
    // must never change after allocation (no realloc/move: this is an
    // array of separately-owned buffers, not one growing buffer a
    // second LOAD could reallocate out from under the first one's
    // already-built AST nodes) and must outlive the pool referencing
    // it. Use parse_result_destroy (not a bare free()) to discard a
    // ParseResult that came from logo_parse, so these get released too.
    char *eager_loaded_sources[MAX_EAGER_LOADS];
    int eager_loaded_count;
} ParseResult;

// Parses `tokens` (as produced by logo_lex, including its trailing
// EOF/ERROR token) into `result`. Does a hoisting pre-pass first --
// collecting every top-level TO's name and parameter count -- before
// parsing any statement bodies, so a procedure can be called before
// its own TO...END appears later in the same token stream (see
// docs/BYTECODE_VM_DESIGN.md's forward-references decision). Parse
// errors (unknown identifiers, wrong argument counts, malformed
// blocks, ...) are collected into result->errors rather than
// aborting -- same "keep going, report what's wrong" convention
// interpreter.c's own error messages already follow -- capped at
// MAX_PARSE_ERRORS, with parsing continuing on a best-effort basis
// past each one so a single typo doesn't hide every other error in
// the same script.
void logo_parse(const LogoToken *tokens, int token_count, ParseResult *result);

// Parses `tokens` as a single standalone expression (arithmetic only,
// no comparisons/AND/OR/NOT) rather than a whole program -- used by
// eval.c's MAP/REDUCE to re-parse a template snippet at runtime after
// substituting an element's text in for `?`/`?1`/`?2`, mirroring
// interpreter.c's own parse_expr(app, &ptr) entry point serving the
// exact same purpose there. Returns the parsed expression's node
// index (into result->pool, which -- like logo_parse's -- is reset by
// this call), or -1 if result->error_count is nonzero afterward.
int logo_parse_expr(const LogoToken *tokens, int token_count, ParseResult *result);

// Same as logo_parse_expr, but for a condition (comparisons, NOT/AND/
// OR) -- FILTER's template is a predicate (e.g. `[? > 2]`), and a bare
// arithmetic expression parse doesn't understand `>` at all, mirroring
// interpreter.c's own parse_condition(app, &ptr) used for the same
// reason.
int logo_parse_condition(const LogoToken *tokens, int token_count, ParseResult *result);

// Frees `result` itself, plus any source-text buffers logo_parse
// allocated on its behalf (see ParseResult's own comment on
// eager_loaded_sources). The one correct way to discard any
// ParseResult logo_parse/logo_parse_expr/logo_parse_condition
// produced -- replaces a bare free(result) everywhere in this
// codebase; a no-op beyond the free() itself if no LOAD was ever
// eagerly followed. Safe to call with NULL.
void parse_result_destroy(ParseResult *result);

#endif
