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

typedef struct {
    char message[256];
    int line, col;
} ParseError;

typedef struct {
    AstPool pool;
    int program;    // index of the top-level AST_BLOCK -- the whole script's statements, in order
    ParseError errors[MAX_PARSE_ERRORS];
    int error_count;
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

#endif
