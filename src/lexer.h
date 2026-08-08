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

#endif
