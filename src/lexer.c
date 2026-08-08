// lexer.c
//
// See lexer.h for the token types and overall design rationale
// (docs/BYTECODE_VM_DESIGN.md's Stage 1). This file's only job is
// recognizing character spans in Logo source text; it never decides
// what a bareword *means* (built-in keyword vs. user procedure name)
// -- that dictionary lookup happens later, in whatever consumes these
// tokens (the Stage 1 parser, not yet written).
//
// Several of the rules below look inconsistent with each other, and
// they are -- interpreter.c's own scanning grew feature-by-feature
// over many sessions, and this lexer preserves its exact quirks
// faithfully (confirmed directly against the running interpreter, not
// guessed) rather than silently normalizing them, per
// docs/BYTECODE_VM_DESIGN.md's migration strategy: the new front end
// has to reproduce the old one's actual behavior, not a tidier version
// of it, so the two can be diffed against each other output-for-output.
//   - A bareword or "quoted word stops only at whitespace or `[`/`]`
//     (never at `)`/an operator character). `PRINT ("foo)` really does
//     read "foo)" -- closing paren included -- as one word today
//     (interpreter.c's `"` scanner and consume_keyword's sscanf "%s"
//     both stop only on isspace, no bracket-awareness at all there).
//     But list literals are the one place interpreter.c's own word
//     scanning *does* stop at `[`/`]` too (see parse_list_literal's
//     extra `*str != '[' && *str != ']'` check, absent from every
//     other word scan in that file) -- and since list literals are
//     the common case where an element sits directly against the
//     closing bracket with no space (`[FD 10 RT 90]`, not `[FD 10 RT
//     90 ]`), this lexer adopts that stricter rule universally rather
//     than reproducing the old inconsistency between "inside a list"
//     and "everywhere else" -- a deliberate, documented simplification
//     (docs/BYTECODE_VM_DESIGN.md's list-literal decision already
//     commits to list literals being properly structured rather than
//     raw-text quirks, so smoothing this one over is in the same
//     spirit), not a silent divergence. The closing-paren swallowing
//     quirk above is unaffected -- `)` was never part of this rule
//     either way.
//   - A :varref's name stops at the first character that isn't a
//     letter/digit/underscore -- so it DOES stop at `)`/`]`/whitespace/
//     an operator character, unlike a bareword or quoted word.
//   - A number is bounded by its own digits-and-one-'.' character
//     class (like strtod), not by whitespace -- so `5+3` correctly
//     lexes as NUMBER(5) PLUS NUMBER(3) with no space needed, while
//     `FD100` lexes as one bareword "FD100", not FD followed by 100.
//   - A leading +/- is never part of a NUMBER token here, even though
//     interpreter.c's own number fallback (strtod) would technically
//     accept one -- today's parser always intercepts +/- as a unary
//     operator first (see parse_factor), so that strtod capability is
//     unreachable in practice. Keeping +/- as their own operator
//     tokens matches what actually happens today; unary-vs-binary stays
//     a parser-level decision (see docs/BYTECODE_VM_DESIGN.md's grammar
//     section), not a lexical one.

#include "lexer.h"
#include <ctype.h>
#include <string.h>

typedef struct {
    const char *p;
    int line;
    int col;
} LexState;

static void advance(LexState *s) {
    if (*s->p == '\n') {
        s->line++;
        s->col = 1;
    } else {
        s->col++;
    }
    s->p++;
}

// Whitespace and `;`-to-end-of-line comments, both insignificant --
// exactly interpreter.c's own skip_whitespace, just tracking line/col
// as it goes instead of only advancing a pointer.
static void skip_insignificant(LexState *s) {
    for (;;) {
        while (*s->p && isspace((unsigned char)*s->p)) advance(s);
        if (*s->p == ';') {
            while (*s->p && *s->p != '\n') advance(s);
            continue; // more whitespace, or another comment line, may follow
        }
        break;
    }
}

static int is_bareword_char(char c) {
    return c != '\0' && !isspace((unsigned char)c) && c != '[' && c != ']';
}

int logo_lex(const char *source, LogoToken *out, int max_tokens) {
    LexState state = { source, 1, 1 };
    LexState *s = &state;
    int count = 0;

    for (;;) {
        skip_insignificant(s);
        if (count >= max_tokens) return -1;

        LogoToken tok;
        tok.line = s->line;
        tok.col = s->col;
        tok.text = s->p;
        char c = *s->p;

        if (c == '\0') {
            tok.type = LOGO_TOK_EOF;
            tok.length = 0;
            out[count++] = tok;
            return count;
        }

        if (c == '[') { advance(s); tok.type = LOGO_TOK_LBRACKET; tok.length = 1; out[count++] = tok; continue; }
        if (c == ']') { advance(s); tok.type = LOGO_TOK_RBRACKET; tok.length = 1; out[count++] = tok; continue; }
        if (c == '(') { advance(s); tok.type = LOGO_TOK_LPAREN; tok.length = 1; out[count++] = tok; continue; }
        if (c == ')') { advance(s); tok.type = LOGO_TOK_RPAREN; tok.length = 1; out[count++] = tok; continue; }
        if (c == '+') { advance(s); tok.type = LOGO_TOK_PLUS; tok.length = 1; out[count++] = tok; continue; }
        if (c == '*') { advance(s); tok.type = LOGO_TOK_STAR; tok.length = 1; out[count++] = tok; continue; }
        if (c == '/') { advance(s); tok.type = LOGO_TOK_SLASH; tok.length = 1; out[count++] = tok; continue; }
        if (c == '-') { advance(s); tok.type = LOGO_TOK_MINUS; tok.length = 1; out[count++] = tok; continue; }
        if (c == '=') { advance(s); tok.type = LOGO_TOK_EQ; tok.length = 1; out[count++] = tok; continue; }

        // <= >= <>, mirroring parse_comparison's own op1/op2 combining
        // exactly -- a lone < or > falls back to the one-char token.
        if (c == '<') {
            advance(s);
            if (*s->p == '=') { advance(s); tok.type = LOGO_TOK_LE; tok.length = 2; }
            else if (*s->p == '>') { advance(s); tok.type = LOGO_TOK_NE; tok.length = 2; }
            else { tok.type = LOGO_TOK_LT; tok.length = 1; }
            out[count++] = tok;
            continue;
        }
        if (c == '>') {
            advance(s);
            if (*s->p == '=') { advance(s); tok.type = LOGO_TOK_GE; tok.length = 2; }
            else { tok.type = LOGO_TOK_GT; tok.length = 1; }
            out[count++] = tok;
            continue;
        }

        if (c == '"') {
            advance(s);
            const char *start = s->p;
            while (is_bareword_char(*s->p)) advance(s);
            tok.type = LOGO_TOK_QUOTED_WORD;
            tok.text = start;
            tok.length = (int)(s->p - start);
            out[count++] = tok;
            continue;
        }

        if (c == '\'') {
            advance(s);
            const char *start = s->p;
            while (*s->p != '\0' && *s->p != '\'') advance(s); // can span whitespace/newlines -- the one literal that does
            if (*s->p != '\'') {
                tok.type = LOGO_TOK_ERROR;
                tok.text = "'...': missing closing '";
                tok.length = (int)strlen(tok.text);
                out[count++] = tok;
                return count;
            }
            tok.type = LOGO_TOK_RAW_TEXT;
            tok.text = start;
            tok.length = (int)(s->p - start);
            advance(s); // the closing '
            out[count++] = tok;
            continue;
        }

        if (c == ':') {
            advance(s);
            const char *start = s->p;
            while (isalnum((unsigned char)*s->p) || *s->p == '_') advance(s);
            tok.type = LOGO_TOK_VARREF;
            tok.text = start;
            tok.length = (int)(s->p - start);
            out[count++] = tok;
            continue;
        }

        if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)s->p[1]))) {
            const char *start = s->p;
            int seen_dot = 0;
            while (isdigit((unsigned char)*s->p) || (*s->p == '.' && !seen_dot)) {
                if (*s->p == '.') seen_dot = 1;
                advance(s);
            }
            tok.type = LOGO_TOK_NUMBER;
            tok.text = start;
            tok.length = (int)(s->p - start);
            out[count++] = tok;
            continue;
        }

        // Everything else: a bareword, bounded only by whitespace (or
        // end of input) -- never by `)`/`]`/an operator character,
        // same rule as a quoted word (see the file comment above).
        {
            const char *start = s->p;
            while (is_bareword_char(*s->p)) advance(s);
            tok.type = LOGO_TOK_BAREWORD;
            tok.text = start;
            tok.length = (int)(s->p - start);
            out[count++] = tok;
            continue;
        }
    }
}
