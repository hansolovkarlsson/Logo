// test_parser.c
//
// Headless tests for parser.c (and, transitively, lexer.c/ast.c) in
// isolation -- no interpreter.h, no LogoApp, no GTK. See
// docs/BYTECODE_VM_DESIGN.md's Stage 1. Run via `make test-parser`.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lexer.h"
#include "../src/parser.h"

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

#define CHECK_STREQ(actual, expected) do { \
    if (strcmp((actual), (expected)) != 0) { \
        failures++; \
        printf("FAIL %s: %s != %s (line %d)\n  actual:   \"%s\"\n  expected: \"%s\"\n", \
               current_test, #actual, #expected, __LINE__, (actual), (expected)); \
    } \
} while (0)

#define MAX_TEST_TOKENS 256

// Lexes and parses `source` in one step -- almost every test below
// starts this way. Heap-allocated, not returned by value or left as a
// stack local in each TEST function: ParseResult embeds AstPool's
// fixed MAX_AST_NODES-sized array, well over 6MB (same reasoning as
// LogoApp elsewhere in this project -- a struct this size is fine on
// the heap but not safe to put on the stack even once, let alone
// across the several tests that might reasonably run in the same
// call chain under a smaller thread stack, e.g. under AddressSanitizer's
// own inflated stack usage, confirmed to overflow here directly).
static ParseResult *parse_source(const char *source) {
    ParseResult *result = calloc(1, sizeof(ParseResult));
    LogoToken tokens[MAX_TEST_TOKENS];
    int n = logo_lex(source, tokens, MAX_TEST_TOKENS);
    if (n < 0) {
        failures++;
        printf("FAIL %s: source needed more than %d tokens\n", current_test, MAX_TEST_TOKENS);
        result->pool.node_count = 0;
        result->error_count = 0;
        result->program = -1;
        return result;
    }
    logo_parse(tokens, n, result);
    return result;
}

static const AstNode *node_at(ParseResult *r, int idx) {
    return &r->pool.nodes[idx];
}

static int child_count(ParseResult *r, int node_idx) {
    int count = 0;
    for (int c = node_at(r, node_idx)->first_child; c >= 0; c = node_at(r, c)->next_sibling) count++;
    return count;
}

static int nth_child(ParseResult *r, int node_idx, int n) {
    int c = node_at(r, node_idx)->first_child;
    for (int i = 0; i < n && c >= 0; i++) c = node_at(r, c)->next_sibling;
    return c;
}

TEST(test_a_number_literal) {
    ParseResult *r = parse_source("PRINT 42");
    CHECK(r->error_count == 0);
    int print_call = nth_child(r, r->program, 0);
    CHECK(node_at(r, print_call)->type == AST_CALL);
    CHECK_STREQ(node_at(r, print_call)->text, "PRINT");
    int arg = nth_child(r, print_call, 0);
    CHECK(node_at(r, arg)->type == AST_NUMBER);
    CHECK(node_at(r, arg)->number == 42);
}

TEST(test_arithmetic_precedence_multiplication_binds_tighter_than_addition) {
    // 1 + 2 * 3 should parse as 1 + (2 * 3), not (1 + 2) * 3.
    ParseResult *r = parse_source("PRINT 1 + 2 * 3");
    CHECK(r->error_count == 0);
    int print_call = nth_child(r, r->program, 0);
    int top = nth_child(r, print_call, 0);
    CHECK(node_at(r, top)->type == AST_BINOP);
    CHECK(node_at(r, top)->binop == AST_OP_ADD);
    int left = nth_child(r, top, 0);
    int right = nth_child(r, top, 1);
    CHECK(node_at(r, left)->type == AST_NUMBER);
    CHECK(node_at(r, left)->number == 1);
    CHECK(node_at(r, right)->type == AST_BINOP);
    CHECK(node_at(r, right)->binop == AST_OP_MUL);
}

TEST(test_parenthesized_grouping_overrides_precedence) {
    // (1 + 2) * 3 should parse with the ADD as the left child of MUL.
    ParseResult *r = parse_source("PRINT (1 + 2) * 3");
    CHECK(r->error_count == 0);
    int print_call = nth_child(r, r->program, 0);
    int top = nth_child(r, print_call, 0);
    CHECK(node_at(r, top)->type == AST_BINOP);
    CHECK(node_at(r, top)->binop == AST_OP_MUL);
    int left = nth_child(r, top, 0);
    CHECK(node_at(r, left)->type == AST_BINOP);
    CHECK(node_at(r, left)->binop == AST_OP_ADD);
}

TEST(test_unary_minus) {
    ParseResult *r = parse_source("MAKE \"x -5");
    CHECK(r->error_count == 0);
    int make_call = nth_child(r, r->program, 0);
    int arg1 = nth_child(r, make_call, 1);
    CHECK(node_at(r, arg1)->type == AST_NEG);
    int operand = nth_child(r, arg1, 0);
    CHECK(node_at(r, operand)->type == AST_NUMBER);
    CHECK(node_at(r, operand)->number == 5);
}

TEST(test_word_and_varref) {
    ParseResult *r = parse_source("MAKE \"name :other");
    CHECK(r->error_count == 0);
    int make_call = nth_child(r, r->program, 0);
    CHECK_STREQ(node_at(r, make_call)->text, "MAKE");
    int name_arg = nth_child(r, make_call, 0);
    CHECK(node_at(r, name_arg)->type == AST_WORD);
    CHECK_STREQ(node_at(r, name_arg)->text, "name");
    int value_arg = nth_child(r, make_call, 1);
    CHECK(node_at(r, value_arg)->type == AST_VARREF);
    CHECK_STREQ(node_at(r, value_arg)->text, "other");
}

TEST(test_list_literal_is_untyped_atoms_not_an_expression_tree) {
    // [1 + 2] is a 3-element list (the atoms "1", "+", "2"), NOT the
    // number 3 -- list literals were never operator-precedence-parsed,
    // matching interpreter.c's own parse_list_literal exactly.
    ParseResult *r = parse_source("MAKE \"x [1 + 2]");
    CHECK(r->error_count == 0);
    int make_call = nth_child(r, r->program, 0);
    int list_node = nth_child(r, make_call, 1);
    CHECK(node_at(r, list_node)->type == AST_LIST_LITERAL);
    CHECK(child_count(r, list_node) == 3);
    CHECK_STREQ(node_at(r, nth_child(r, list_node, 0))->text, "1");
    CHECK_STREQ(node_at(r, nth_child(r, list_node, 1))->text, "+");
    CHECK_STREQ(node_at(r, nth_child(r, list_node, 2))->text, "2");
}

TEST(test_nested_list_literal) {
    ParseResult *r = parse_source("MAKE \"x [a [b c] d]");
    CHECK(r->error_count == 0);
    int make_call = nth_child(r, r->program, 0);
    int list_node = nth_child(r, make_call, 1);
    CHECK(child_count(r, list_node) == 3);
    int nested = nth_child(r, list_node, 1);
    CHECK(node_at(r, nested)->type == AST_LIST_LITERAL);
    CHECK(child_count(r, nested) == 2);
}

TEST(test_repeat_takes_an_expr_and_a_block) {
    ParseResult *r = parse_source("REPEAT 4 [FD 10 RT 90]");
    CHECK(r->error_count == 0);
    int repeat_call = nth_child(r, r->program, 0);
    CHECK(node_at(r, repeat_call)->type == AST_CALL);
    CHECK_STREQ(node_at(r, repeat_call)->text, "REPEAT");
    int count_arg = nth_child(r, repeat_call, 0);
    CHECK(node_at(r, count_arg)->type == AST_NUMBER);
    int block = nth_child(r, repeat_call, 1);
    CHECK(node_at(r, block)->type == AST_BLOCK);
    CHECK(child_count(r, block) == 2); // FD 10, RT 90
    int fd_call = nth_child(r, block, 0);
    CHECK_STREQ(node_at(r, fd_call)->text, "FD");
}

TEST(test_while_takes_a_condition_and_a_block) {
    ParseResult *r = parse_source("WHILE :x < 10 [MAKE \"x :x + 1]");
    CHECK(r->error_count == 0);
    int while_call = nth_child(r, r->program, 0);
    int cond = nth_child(r, while_call, 0);
    CHECK(node_at(r, cond)->type == AST_COMPARE);
    CHECK(node_at(r, cond)->cmpop == AST_CMP_LT);
    int block = nth_child(r, while_call, 1);
    CHECK(node_at(r, block)->type == AST_BLOCK);
}

TEST(test_condition_grammar_not_and_or_precedence) {
    // NOT :a = 1 AND :b = 2 OR :c = 3 should parse as
    // ((NOT (a=1)) AND (b=2)) OR (c=3) -- NOT tightest, then AND, then OR.
    ParseResult *r = parse_source("IF NOT :a = 1 AND :b = 2 OR :c = 3 [PRINT 1]");
    CHECK(r->error_count == 0);
    int if_node = nth_child(r, r->program, 0);
    int cond = nth_child(r, if_node, 0);
    CHECK(node_at(r, cond)->type == AST_OR);
    int or_left = nth_child(r, cond, 0);
    CHECK(node_at(r, or_left)->type == AST_AND);
    int and_left = nth_child(r, or_left, 0);
    CHECK(node_at(r, and_left)->type == AST_NOT);
}

TEST(test_condition_grouping_with_parens_is_now_legal) {
    // NOT (:a = 1 OR :b = 2) -- the resolved "fix boolean grouping"
    // decision: today's interpreter has no way to write this at all.
    ParseResult *r = parse_source("IF NOT (:a = 1 OR :b = 2) [PRINT 1]");
    CHECK(r->error_count == 0);
    int if_node = nth_child(r, r->program, 0);
    int cond = nth_child(r, if_node, 0);
    CHECK(node_at(r, cond)->type == AST_NOT);
    int inner = nth_child(r, cond, 0);
    CHECK(node_at(r, inner)->type == AST_OR);
}

TEST(test_if_with_no_else_has_only_two_children) {
    // A space before ] matters here: "positive] with no space would
    // lex as one quoted word "positive]" (] included) -- the same
    // confirmed quirk as PRINT ("foo), see test_lexer.c. Today's
    // interpreter dodges this specifically for a block's own closing
    // bracket via extract_block's separate raw-text pre-pass (finding
    // the matching ] by counting brackets *before* tokenizing
    // anything); this lexer tokenizes in one uniform pass with no such
    // context, so it doesn't get that same protection -- a real,
    // narrow behavior gap from today's interpreter, not yet resolved
    // (see docs/BYTECODE_VM_DESIGN.md's Progress notes).
    ParseResult *r = parse_source("IF :x > 0 [PRINT \"positive ]");
    CHECK(r->error_count == 0);
    int if_node = nth_child(r, r->program, 0);
    CHECK(node_at(r, if_node)->type == AST_IF);
    CHECK(child_count(r, if_node) == 2);
}

TEST(test_if_with_bracket_else_and_no_else_keyword) {
    // Confirmed against today's interpreter: IF accepts a second
    // block directly, with no ELSE keyword required.
    ParseResult *r = parse_source("IF :x > 0 [PRINT \"positive ] [PRINT \"nonpositive ]");
    CHECK(r->error_count == 0);
    int if_node = nth_child(r, r->program, 0);
    CHECK(child_count(r, if_node) == 3);
}

TEST(test_ifelse_with_else_keyword) {
    ParseResult *r = parse_source("IFELSE :x > 0 [PRINT \"positive ] ELSE [PRINT \"nonpositive ]");
    CHECK(r->error_count == 0);
    int if_node = nth_child(r, r->program, 0);
    CHECK(child_count(r, if_node) == 3);
}

TEST(test_ifelse_missing_else_block_is_an_error) {
    ParseResult *r = parse_source("IFELSE :x > 0 [PRINT \"positive ]");
    CHECK(r->error_count == 1);
}

TEST(test_to_end_defines_a_procedure) {
    ParseResult *r = parse_source("TO square :size\n  REPEAT 4 [FD :size RT 90]\nEND");
    CHECK(r->error_count == 0);
    int def = nth_child(r, r->program, 0);
    CHECK(node_at(r, def)->type == AST_PROC_DEF);
    CHECK_STREQ(node_at(r, def)->text, "square");
    CHECK(node_at(r, def)->param_count == 1);
    CHECK_STREQ(node_at(r, def)->param_names[0], "size");
    int body = nth_child(r, def, 0);
    CHECK(node_at(r, body)->type == AST_BLOCK);
    CHECK(child_count(r, body) == 1); // one REPEAT statement
}

TEST(test_forward_reference_now_works) {
    // The resolved "lift forward references" decision: calling a
    // procedure before its own TO...END, which reports "I don't know
    // how to X" in today's interpreter (confirmed directly), now
    // parses cleanly because hoist_procedures runs first.
    ParseResult *r = parse_source("square 50\nTO square :size\n  REPEAT 4 [FD :size RT 90]\nEND");
    CHECK(r->error_count == 0);
    int call = nth_child(r, r->program, 0);
    CHECK(node_at(r, call)->type == AST_CALL);
    CHECK_STREQ(node_at(r, call)->text, "square");
    CHECK(child_count(r, call) == 1);
}

TEST(test_procedure_call_with_wrong_looking_name_is_unknown) {
    ParseResult *r = parse_source("PRINT bogus 1");
    // Two errors, not one: "bogus" fails to resolve as PRINT's one
    // argument (reported, placeholder substituted), which leaves "1"
    // dangling as its own unrecognized top-level statement -- reporting
    // every problem found rather than stopping at the first one, per
    // parser.h's own "keep going" convention.
    CHECK(r->error_count == 2);
    CHECK(strstr(r->errors[0].message, "bogus") != NULL);
}

TEST(test_true_false_literals) {
    ParseResult *r = parse_source("MAKE \"x TRUE");
    CHECK(r->error_count == 0);
    int make_call = nth_child(r, r->program, 0);
    int val = nth_child(r, make_call, 1);
    CHECK(node_at(r, val)->type == AST_WORD);
    CHECK_STREQ(node_at(r, val)->text, "TRUE");
}

TEST(test_error_reports_line_and_col) {
    ParseResult *r = parse_source("PRINT 1\nPRINT bogus");
    CHECK(r->error_count == 1);
    CHECK(r->errors[0].line == 2);
}

TEST(test_multiple_statements_at_top_level) {
    ParseResult *r = parse_source("FD 10\nRT 90\nFD 10");
    CHECK(r->error_count == 0);
    CHECK(child_count(r, r->program) == 3);
}

int main(void) {
    RUN(test_a_number_literal);
    RUN(test_arithmetic_precedence_multiplication_binds_tighter_than_addition);
    RUN(test_parenthesized_grouping_overrides_precedence);
    RUN(test_unary_minus);
    RUN(test_word_and_varref);
    RUN(test_list_literal_is_untyped_atoms_not_an_expression_tree);
    RUN(test_nested_list_literal);
    RUN(test_repeat_takes_an_expr_and_a_block);
    RUN(test_while_takes_a_condition_and_a_block);
    RUN(test_condition_grammar_not_and_or_precedence);
    RUN(test_condition_grouping_with_parens_is_now_legal);
    RUN(test_if_with_no_else_has_only_two_children);
    RUN(test_if_with_bracket_else_and_no_else_keyword);
    RUN(test_ifelse_with_else_keyword);
    RUN(test_ifelse_missing_else_block_is_an_error);
    RUN(test_to_end_defines_a_procedure);
    RUN(test_forward_reference_now_works);
    RUN(test_procedure_call_with_wrong_looking_name_is_unknown);
    RUN(test_true_false_literals);
    RUN(test_error_reports_line_and_col);
    RUN(test_multiple_statements_at_top_level);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
