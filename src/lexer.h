#ifndef LOGO_LEXER_H
#define LOGO_LEXER_H

// lexer.h
//
// Stage 1 of the "real front end" redesign (see
// docs/BYTECODE_VM_DESIGN.md) -- turns Logo source text into a flat
// array of tokens. Deliberately has zero dependency on interpreter.h/
// LogoApp/GTK: a lexer's only job is recognizing character spans, and
// keeping it this decoupled is what makes it independently testable
// (see tests/test_lexer.c) before any parser/AST code exists to
// depend on it.
//
// <stddef.h> is the one include, and it doesn't compromise that: it's
// a freestanding header (no libc runtime behind it), needed only for
// NULL in logo_normalize_newlines below.

#include <stddef.h>

typedef enum {
    LOGO_TOK_EOF,
    LOGO_TOK_ERROR,        // malformed input (unterminated 'raw text', ...) -- .text is a static message, not a span of the source
    LOGO_TOK_NUMBER,       // 123, 3.14, .5
    LOGO_TOK_QUOTED_WORD,  // "hello
    LOGO_TOK_RAW_TEXT,     // 'raw text, spaces and all'
    LOGO_TOK_VARREF,       // :name
    LOGO_TOK_BAREWORD,     // FD, REPEAT, MYPROC, SETPROP, TRUE, ... -- dictionary-resolved later, not here
    LOGO_TOK_LBRACKET, LOGO_TOK_RBRACKET,  // [ ]
    LOGO_TOK_LPAREN, LOGO_TOK_RPAREN,      // ( )
    LOGO_TOK_PLUS, LOGO_TOK_MINUS, LOGO_TOK_STAR, LOGO_TOK_SLASH,
    LOGO_TOK_LT, LOGO_TOK_GT, LOGO_TOK_EQ,
    LOGO_TOK_LE, LOGO_TOK_GE, LOGO_TOK_NE,  // <= >= <>
} LogoTokenType;

typedef struct {
    LogoTokenType type;
    // For every type except LOGO_TOK_ERROR, points into the source
    // buffer passed to logo_lex (never copied, never NUL-terminated on
    // its own -- always read exactly `length` bytes) -- same "the
    // source text outlives the parse" assumption LOAD already makes
    // today when it hands a whole file to eval_logo. For
    // LOGO_TOK_ERROR, `text` instead points at a static diagnostic
    // message and `length` is that message's own strlen.
    const char *text;
    int length;
    // 1-based, matching how editors/error messages normally count --
    // the actual point of building a real lexer instead of reusing
    // interpreter.c's position-free sscanf/strtod scanning.
    int line;
    int col;
} LogoToken;

// Tokenizes `source` into `out` (caller-owned, `max_tokens` capacity).
// The array is always terminated by exactly one trailing LOGO_TOK_EOF
// (clean end of input) or LOGO_TOK_ERROR (malformed input, e.g. an
// unterminated 'raw text' literal) token -- either way, callers can
// stop at the first token of either kind rather than needing the
// returned count for that. Returns the number of tokens written
// (including that trailing EOF/ERROR), or -1 if `source` has more
// tokens than `max_tokens` can hold -- same "loud error, not silent
// truncation" policy this project's other fixed-size buffers already
// follow (see extract_block's own comment in interpreter.c).
int logo_lex(const char *source, LogoToken *out, int max_tokens);

// Rewrites `source` in place so every line ends with a single '\n': a
// CRLF pair loses its '\r', and a lone CR (classic Mac) becomes '\n'.
// Returns `source`. Every point where Logo *source text* enters the
// program calls this -- see the call sites in headless.c, parser.c,
// ui.c, vm.c, eval.c and interpreter.c.
//
// This is deliberately NOT about tokenizing, and the lexer itself
// needed no change for CRLF: isspace() already covers '\r', so both
// skip_insignificant and is_bareword_char stop on one exactly as they
// stop on a space, and `FD 10\r\n` tokenizes identically to `FD 10\n`.
//
// The problem is downstream, in the things that capture RAW SPANS of
// the source buffer rather than re-rendering parsed tokens. The
// clearest is AST_PROC_DEF's body_text/body_len (see ast.h), which
// TEXT/SHOW/SAVE print back verbatim: parse_proc_def sets that span to
// run from the body's first token up to the END token, so it includes
// the line ending before END, and on CRLF input every line of a
// printed procedure body ends up carrying a stray carriage return into
// the output. 'raw text' literals have the same exposure, since they
// span newlines by design. Normalizing once at ingest fixes all of
// them at the source rather than teaching each consumer to strip.
//
// Kept here (rather than in lexer.c) and static inline on purpose:
// interpreter.c needs it too, and the Makefile's TEST_TARGET builds
// interpreter.c WITHOUT lexer.c, so a linked symbol would break that
// target. lexer.h has no dependencies of its own, so including it
// costs those translation units nothing.
//
// Only ever shrinks the buffer, so it is safe in place on any mutable
// NUL-terminated string, and it is a no-op on the LF input macOS and
// Linux normally produce.
static inline char *logo_normalize_newlines(char *source) {
    if (source == NULL) return NULL;
    char *w = source;
    for (const char *r = source; *r != '\0'; r++) {
        if (*r == '\r') {
            if (r[1] == '\n') continue; // CRLF: drop the CR, keep the LF
            *w++ = '\n';                // lone CR: classic Mac line ending
            continue;
        }
        *w++ = *r;
    }
    *w = '\0';
    return source;
}

#endif
