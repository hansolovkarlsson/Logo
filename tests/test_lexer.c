// test_lexer.c
//
// Headless tests for lexer.c in isolation -- no interpreter.h, no
// LogoApp, no GTK, nothing but logo_lex itself. See
// docs/BYTECODE_VM_DESIGN.md's Stage 1: the whole point of splitting
// the lexer out this way is that it's testable on its own, with zero
// interaction with the existing engine, before any parser/AST code
// depends on it. Run via `make test-lexer`.

#include <stdio.h>
#include <string.h>

#include "../src/lexer.h"

static int failures = 0;
static const char *current_test = "";

#define TEST(name) static void name(void)
#define RUN(name) do { current_test = #name; name(); } while (0)

#define CHECK(cond) do { \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s: %s (line %d)\n", current_test, #cond, __LINE__); \
    } \
} while (0)

// Compares a token's type and exact text span in one go -- almost
// every test case below is a sequence of these.
static void check_token(LogoToken tok, LogoTokenType expected_type, const char *expected_text, int line) {
    if (tok.type != expected_type) {
        failures++;
        printf("FAIL %s: token type mismatch (line %d): got %d, expected %d\n",
               current_test, line, tok.type, expected_type);
        return;
    }
    size_t expected_len = strlen(expected_text);
    if ((size_t)tok.length != expected_len || strncmp(tok.text, expected_text, expected_len) != 0) {
        failures++;
        printf("FAIL %s: token text mismatch (line %d): got \"%.*s\", expected \"%s\"\n",
               current_test, line, tok.length, tok.text, expected_text);
    }
}
#define CHECK_TOKEN(tok, type, text) check_token((tok), (type), (text), __LINE__)

#define MAX_TEST_TOKENS 64

TEST(test_empty_source_is_just_eof) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("", toks, MAX_TEST_TOKENS);
    CHECK(n == 1);
    CHECK_TOKEN(toks[0], LOGO_TOK_EOF, "");
}

TEST(test_whitespace_only_is_just_eof) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("   \n\t  \n", toks, MAX_TEST_TOKENS);
    CHECK(n == 1);
    CHECK_TOKEN(toks[0], LOGO_TOK_EOF, "");
}

TEST(test_comment_to_end_of_line_is_skipped) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("FD 100 ; go forward\nRT 90", toks, MAX_TEST_TOKENS);
    CHECK(n == 5); // FD 100 RT 90 EOF
    CHECK_TOKEN(toks[0], LOGO_TOK_BAREWORD, "FD");
    CHECK_TOKEN(toks[1], LOGO_TOK_NUMBER, "100");
    CHECK_TOKEN(toks[2], LOGO_TOK_BAREWORD, "RT");
    CHECK_TOKEN(toks[3], LOGO_TOK_NUMBER, "90");
    CHECK_TOKEN(toks[4], LOGO_TOK_EOF, "");
}

TEST(test_a_comment_only_line_is_also_skipped) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("; just a comment\n; another one", toks, MAX_TEST_TOKENS);
    CHECK(n == 1);
    CHECK_TOKEN(toks[0], LOGO_TOK_EOF, "");
}

TEST(test_numbers_integer_and_decimal) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("123 3.14 .5", toks, MAX_TEST_TOKENS);
    CHECK(n == 4);
    CHECK_TOKEN(toks[0], LOGO_TOK_NUMBER, "123");
    CHECK_TOKEN(toks[1], LOGO_TOK_NUMBER, "3.14");
    CHECK_TOKEN(toks[2], LOGO_TOK_NUMBER, ".5");
}

TEST(test_number_immediately_before_an_operator_needs_no_space) {
    // Confirmed against today's interpreter: 5+3 lexes as NUMBER PLUS
    // NUMBER with no space required -- numbers are bounded by their
    // own digit/'.' character class, not by whitespace.
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("5+3", toks, MAX_TEST_TOKENS);
    CHECK(n == 4);
    CHECK_TOKEN(toks[0], LOGO_TOK_NUMBER, "5");
    CHECK_TOKEN(toks[1], LOGO_TOK_PLUS, "+");
    CHECK_TOKEN(toks[2], LOGO_TOK_NUMBER, "3");
}

TEST(test_a_digit_led_bareword_lexes_as_number_then_bareword) {
    // FD100 is one bareword (a whole different procedure name from
    // FD); a leading-digit run like 100D is NOT one bareword -- the
    // number scan claims "100" first, same as today's strtod fallback
    // would, leaving "D" as its own token.
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("100D", toks, MAX_TEST_TOKENS);
    CHECK(n == 3);
    CHECK_TOKEN(toks[0], LOGO_TOK_NUMBER, "100");
    CHECK_TOKEN(toks[1], LOGO_TOK_BAREWORD, "D");
}

TEST(test_a_leading_sign_is_never_part_of_a_number_token) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("-5", toks, MAX_TEST_TOKENS);
    CHECK(n == 3);
    CHECK_TOKEN(toks[0], LOGO_TOK_MINUS, "-");
    CHECK_TOKEN(toks[1], LOGO_TOK_NUMBER, "5");
}

TEST(test_quoted_word_stops_at_whitespace) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("\"hello \"world", toks, MAX_TEST_TOKENS);
    CHECK(n == 3);
    CHECK_TOKEN(toks[0], LOGO_TOK_QUOTED_WORD, "hello");
    CHECK_TOKEN(toks[1], LOGO_TOK_QUOTED_WORD, "world");
}

TEST(test_quoted_word_swallows_a_trailing_paren_with_no_space) {
    // Confirmed directly against today's interpreter: PRINT ("foo)
    // really does read "foo)" (closing paren included) as one word --
    // the quoted-word scan stops only at whitespace/[/] (see the next
    // test), never at an operator/paren character. Preserved here
    // deliberately, not fixed, per the migration strategy in
    // docs/BYTECODE_VM_DESIGN.md (reproduce today's actual behavior).
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("(\"foo)", toks, MAX_TEST_TOKENS);
    CHECK(n == 3);
    CHECK_TOKEN(toks[0], LOGO_TOK_LPAREN, "(");
    CHECK_TOKEN(toks[1], LOGO_TOK_QUOTED_WORD, "foo)");
}

TEST(test_quoted_word_and_bareword_stop_at_a_closing_bracket) {
    // Unlike the paren case above, [ and ] DO terminate a bareword/
    // quoted-word scan -- matching interpreter.c's own list-literal
    // element scanner (the one place its word-scanning already stops
    // at [/] too, unlike everywhere else), adopted universally here
    // rather than reproducing that inconsistency (see the file
    // comment above). This is the common case, not an edge case --
    // [FD 10 RT 90] has no space before its closing bracket.
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("[FD 10 RT 90]", toks, MAX_TEST_TOKENS);
    CHECK(n == 7); // [ FD 10 RT 90 ] EOF
    CHECK_TOKEN(toks[3], LOGO_TOK_BAREWORD, "RT");
    CHECK_TOKEN(toks[4], LOGO_TOK_NUMBER, "90");
    CHECK_TOKEN(toks[5], LOGO_TOK_RBRACKET, "]");
}

TEST(test_raw_text_literal_can_span_spaces_and_newlines) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("'hello\nworld' next", toks, MAX_TEST_TOKENS);
    CHECK(n == 3);
    CHECK_TOKEN(toks[0], LOGO_TOK_RAW_TEXT, "hello\nworld");
    CHECK_TOKEN(toks[1], LOGO_TOK_BAREWORD, "next");
}

TEST(test_unterminated_raw_text_is_a_lex_error) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("'hello", toks, MAX_TEST_TOKENS);
    CHECK(n == 1);
    CHECK(toks[0].type == LOGO_TOK_ERROR);
}

TEST(test_varref_reads_a_plain_name) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex(":name", toks, MAX_TEST_TOKENS);
    CHECK(n == 2);
    CHECK_TOKEN(toks[0], LOGO_TOK_VARREF, "name");
}

TEST(test_varref_stops_at_punctuation_unlike_a_bareword_or_quoted_word) {
    // Confirmed against today's interpreter's own :name scanner
    // (isalnum/'_' only) -- a real, if inconsistent, difference from
    // how barewords/quoted words are bounded (whitespace only).
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("(:name)", toks, MAX_TEST_TOKENS);
    CHECK(n == 4);
    CHECK_TOKEN(toks[0], LOGO_TOK_LPAREN, "(");
    CHECK_TOKEN(toks[1], LOGO_TOK_VARREF, "name");
    CHECK_TOKEN(toks[2], LOGO_TOK_RPAREN, ")");
}

TEST(test_varref_name_can_include_digits_and_underscore) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex(":item_2", toks, MAX_TEST_TOKENS);
    CHECK(n == 2);
    CHECK_TOKEN(toks[0], LOGO_TOK_VARREF, "item_2");
}

TEST(test_bareword_can_end_in_a_question_mark) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("EMPTY? BUTTON?", toks, MAX_TEST_TOKENS);
    CHECK(n == 3);
    CHECK_TOKEN(toks[0], LOGO_TOK_BAREWORD, "EMPTY?");
    CHECK_TOKEN(toks[1], LOGO_TOK_BAREWORD, "BUTTON?");
}

TEST(test_brackets_and_parens) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("[ ( ) ]", toks, MAX_TEST_TOKENS);
    CHECK(n == 5);
    CHECK_TOKEN(toks[0], LOGO_TOK_LBRACKET, "[");
    CHECK_TOKEN(toks[1], LOGO_TOK_LPAREN, "(");
    CHECK_TOKEN(toks[2], LOGO_TOK_RPAREN, ")");
    CHECK_TOKEN(toks[3], LOGO_TOK_RBRACKET, "]");
}

TEST(test_relational_operators_including_two_char_forms) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("< > = <= >= <>", toks, MAX_TEST_TOKENS);
    CHECK(n == 7);
    CHECK_TOKEN(toks[0], LOGO_TOK_LT, "<");
    CHECK_TOKEN(toks[1], LOGO_TOK_GT, ">");
    CHECK_TOKEN(toks[2], LOGO_TOK_EQ, "=");
    CHECK_TOKEN(toks[3], LOGO_TOK_LE, "<=");
    CHECK_TOKEN(toks[4], LOGO_TOK_GE, ">=");
    CHECK_TOKEN(toks[5], LOGO_TOK_NE, "<>");
}

TEST(test_a_realistic_line_of_source) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("TO square :size\n  REPEAT 4 [FD :size RT 90]\nEND", toks, MAX_TEST_TOKENS);
    CHECK(n == 13); // TO square :size REPEAT 4 [ FD :size RT 90 ] END EOF
    CHECK_TOKEN(toks[0], LOGO_TOK_BAREWORD, "TO");
    CHECK_TOKEN(toks[1], LOGO_TOK_BAREWORD, "square");
    CHECK_TOKEN(toks[2], LOGO_TOK_VARREF, "size");
    CHECK_TOKEN(toks[3], LOGO_TOK_BAREWORD, "REPEAT");
    CHECK_TOKEN(toks[4], LOGO_TOK_NUMBER, "4");
    CHECK_TOKEN(toks[5], LOGO_TOK_LBRACKET, "[");
    CHECK_TOKEN(toks[6], LOGO_TOK_BAREWORD, "FD");
    CHECK_TOKEN(toks[7], LOGO_TOK_VARREF, "size");
    CHECK_TOKEN(toks[8], LOGO_TOK_BAREWORD, "RT");
    CHECK_TOKEN(toks[9], LOGO_TOK_NUMBER, "90");
    CHECK_TOKEN(toks[10], LOGO_TOK_RBRACKET, "]");
    CHECK_TOKEN(toks[11], LOGO_TOK_BAREWORD, "END");
    CHECK_TOKEN(toks[12], LOGO_TOK_EOF, "");
}

TEST(test_line_and_col_tracking) {
    LogoToken toks[MAX_TEST_TOKENS];
    int n = logo_lex("FD 1\nRT 2", toks, MAX_TEST_TOKENS);
    CHECK(n == 5);
    CHECK(toks[0].line == 1 && toks[0].col == 1);  // FD
    CHECK(toks[1].line == 1 && toks[1].col == 4);  // 1
    CHECK(toks[2].line == 2 && toks[2].col == 1);  // RT
    CHECK(toks[3].line == 2 && toks[3].col == 4);  // 2
}

TEST(test_too_many_tokens_reports_overflow_not_silent_truncation) {
    LogoToken toks[3];
    int n = logo_lex("FD 1 RT 2", toks, 3); // 5 tokens (incl. EOF) won't fit in 3
    CHECK(n == -1);
}

int main(void) {
    RUN(test_empty_source_is_just_eof);
    RUN(test_whitespace_only_is_just_eof);
    RUN(test_comment_to_end_of_line_is_skipped);
    RUN(test_a_comment_only_line_is_also_skipped);
    RUN(test_numbers_integer_and_decimal);
    RUN(test_number_immediately_before_an_operator_needs_no_space);
    RUN(test_a_digit_led_bareword_lexes_as_number_then_bareword);
    RUN(test_a_leading_sign_is_never_part_of_a_number_token);
    RUN(test_quoted_word_stops_at_whitespace);
    RUN(test_quoted_word_swallows_a_trailing_paren_with_no_space);
    RUN(test_quoted_word_and_bareword_stop_at_a_closing_bracket);
    RUN(test_raw_text_literal_can_span_spaces_and_newlines);
    RUN(test_unterminated_raw_text_is_a_lex_error);
    RUN(test_varref_reads_a_plain_name);
    RUN(test_varref_stops_at_punctuation_unlike_a_bareword_or_quoted_word);
    RUN(test_varref_name_can_include_digits_and_underscore);
    RUN(test_bareword_can_end_in_a_question_mark);
    RUN(test_brackets_and_parens);
    RUN(test_relational_operators_including_two_char_forms);
    RUN(test_a_realistic_line_of_source);
    RUN(test_line_and_col_tracking);
    RUN(test_too_many_tokens_reports_overflow_not_silent_truncation);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
