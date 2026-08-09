// test_eval.c
//
// Headless tests for eval.c, the Stage 1 tree-walking evaluator (see
// docs/BYTECODE_VM_DESIGN.md) -- lexes, parses, and runs each script
// against a real LogoApp, the same way tests/test_interpreter.c
// exercises eval_logo. Run via `make test-eval`.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/eval.h"
#include "../src/interpreter.h"
#include "../src/lexer.h"
#include "../src/parser.h"

static int failures = 0;
static const char *current_test = "";
static char captured_output[4096];

static void capture_sink(LogoApp *app, const char *text) {
    (void)app;
    strncat(captured_output, text, sizeof(captured_output) - strlen(captured_output) - 1);
}

// Heap-allocated, matching LogoApp's own established convention
// elsewhere in this project (several MB, dominated by the list-node
// pool -- never safe as a stack local, let alone alongside a second
// multi-MB struct like ParseResult in the same test).
static LogoApp *new_app(void) {
    LogoApp *app = calloc(1, sizeof(LogoApp));
    app->canvas_width = DEFAULT_CANVAS_WIDTH;
    app->canvas_height = DEFAULT_CANVAS_HEIGHT;
    init_turtle(app, &app->turtles[0]);
    app->turtle_count = 1;
    app->current_turtle = 0;
    app->bg_r = app->bg_g = app->bg_b = 1.0;
    app->output_sink = capture_sink;
    return app;
}

#define TEST(name) static void name(void)
#define RUN(name) do { current_test = #name; captured_output[0] = '\0'; name(); } while (0)

#define CHECK(cond) do { \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s: %s (line %d)\n", current_test, #cond, __LINE__); \
    } \
} while (0)

#define CHECK_NEAR(a, b) CHECK(fabs((a) - (b)) < 0.001)

#define CHECK_STREQ(actual, expected) do { \
    if (strcmp((actual), (expected)) != 0) { \
        failures++; \
        printf("FAIL %s: %s != %s (line %d)\n  actual:   \"%s\"\n  expected: \"%s\"\n", \
               current_test, #actual, #expected, __LINE__, (actual), (expected)); \
    } \
} while (0)

#define CHECK_CONTAINS(haystack, needle) CHECK(strstr((haystack), (needle)) != NULL)

#define MAX_TEST_TOKENS 512

// Lexes, parses, and runs `source` against `app` in one step -- almost
// every test below starts this way. ParseResult is heap-allocated for
// the same reason LogoApp is (see new_app above) -- AstPool's fixed
// node array makes it over 6.7MB, confirmed to overflow the stack
// under AddressSanitizer when tests/test_parser.c first declared one
// as a plain local.
static void run_source(LogoApp *app, const char *source) {
    LogoToken tokens[MAX_TEST_TOKENS];
    int n = logo_lex(source, tokens, MAX_TEST_TOKENS);
    if (n < 0) {
        failures++;
        printf("FAIL %s: source needed more than %d tokens\n", current_test, MAX_TEST_TOKENS);
        return;
    }
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    if (result->error_count > 0) {
        failures++;
        printf("FAIL %s: %d parse error(s), first: %s\n", current_test, result->error_count, result->errors[0].message);
        free(result);
        return;
    }
    ast_eval(app, &result->pool, result->program);
    free(result);
}

TEST(test_print_a_number_and_a_word) {
    LogoApp *app = new_app();
    run_source(app, "PRINT 42\nPRINT \"hello");
    CHECK_STREQ(captured_output, "42\nhello\n");
}

TEST(test_arithmetic_with_precedence) {
    LogoApp *app = new_app();
    run_source(app, "PRINT 1 + 2 * 3");
    CHECK_STREQ(captured_output, "7\n");
}

TEST(test_parenthesized_grouping) {
    LogoApp *app = new_app();
    run_source(app, "PRINT (1 + 2) * 3");
    CHECK_STREQ(captured_output, "9\n");
}

TEST(test_make_and_varref) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"x 5\nMAKE \"y :x + 1\nPRINT :y");
    CHECK_STREQ(captured_output, "6\n");
}

TEST(test_make_negative_number_is_not_swallowed_as_subtraction) {
    // The MAKE fidelity gap parser.c's ARG_QUOTED_WORD kind fixed --
    // confirmed directly against the real interpreter that MAKE "x -5
    // sets x to -5, not "x" - 5.
    LogoApp *app = new_app();
    run_source(app, "MAKE \"x -5\nPRINT :x");
    CHECK_STREQ(captured_output, "-5\n");
}

TEST(test_if_true_and_false_branches) {
    LogoApp *app = new_app();
    run_source(app, "IF 1 > 0 [PRINT \"positive]\nIF 1 < 0 [PRINT \"negative]");
    CHECK_STREQ(captured_output, "positive\n");
}

TEST(test_ifelse_takes_the_false_branch) {
    LogoApp *app = new_app();
    run_source(app, "IFELSE 1 < 0 [PRINT \"yes] ELSE [PRINT \"no]");
    CHECK_STREQ(captured_output, "no\n");
}

TEST(test_boolean_not_and_or) {
    LogoApp *app = new_app();
    run_source(app,
        "IF NOT 1 = 2 [PRINT \"a]\n"
        "IF 1 = 1 AND 2 = 2 [PRINT \"b]\n"
        "IF 1 = 2 OR 3 = 3 [PRINT \"c]");
    CHECK_STREQ(captured_output, "a\nb\nc\n");
}

TEST(test_word_equality_is_case_insensitive_text_comparison) {
    LogoApp *app = new_app();
    run_source(app, "IF \"Hello = \"HELLO [PRINT \"matched]");
    CHECK_STREQ(captured_output, "matched\n");
}

TEST(test_while_loop) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"i 0\nWHILE :i < 5 [PRINT :i\nMAKE \"i :i + 1]");
    CHECK_STREQ(captured_output, "0\n1\n2\n3\n4\n");
}

TEST(test_repeat_loop) {
    LogoApp *app = new_app();
    run_source(app, "REPEAT 3 [PRINT \"hi]");
    CHECK_STREQ(captured_output, "hi\nhi\nhi\n");
}

TEST(test_turtle_motion_fd_and_rt) {
    LogoApp *app = new_app();
    double start_x = app->turtles[0].x;
    double start_y = app->turtles[0].y;
    run_source(app, "FD 100");
    CHECK_NEAR(app->turtles[0].x, start_x);
    CHECK_NEAR(app->turtles[0].y, start_y - 100); // heading 0 faces up (-y), matching interpreter.c's convention
    run_source(app, "RT 90\nFD 50");
    CHECK_NEAR(app->turtles[0].angle, 90);
}

TEST(test_setxy_and_setheading) {
    LogoApp *app = new_app();
    run_source(app, "SETXY 30 40\nSETHEADING 180");
    CHECK_NEAR(app->turtles[0].x, 30);
    CHECK_NEAR(app->turtles[0].y, 40);
    CHECK_NEAR(app->turtles[0].angle, 180);
}

TEST(test_penup_pendown) {
    LogoApp *app = new_app();
    CHECK(app->turtles[0].pen_down);
    run_source(app, "PENUP");
    CHECK(!app->turtles[0].pen_down);
    run_source(app, "PENDOWN");
    CHECK(app->turtles[0].pen_down);
}

TEST(test_home_and_clear) {
    LogoApp *app = new_app();
    run_source(app, "SETXY 10 10\nSETHEADING 45\nHOME");
    CHECK_NEAR(app->turtles[0].x, app->canvas_width / 2.0);
    CHECK_NEAR(app->turtles[0].y, app->canvas_height / 2.0);
    CHECK_NEAR(app->turtles[0].angle, 0);
}

TEST(test_procedure_with_output) {
    LogoApp *app = new_app();
    run_source(app,
        "TO double :n\n"
        "  OUTPUT :n * 2\n"
        "END\n"
        "PRINT double 21");
    CHECK_STREQ(captured_output, "42\n");
}

TEST(test_procedure_forward_reference) {
    // Calling a procedure before its own TO...END, which the real
    // interpreter reports as "I don't know how to X" (confirmed
    // directly) -- now works, per the resolved forward-references
    // decision.
    LogoApp *app = new_app();
    run_source(app,
        "square 5\n"
        "TO square :n\n"
        "  OUTPUT :n * :n\n"
        "END\n"
        "PRINT square 5");
    CHECK_STREQ(captured_output, "25\n");
}

TEST(test_recursive_procedure) {
    LogoApp *app = new_app();
    run_source(app,
        "TO countdown :n\n"
        "  IF :n = 0 [STOP]\n"
        "  PRINT :n\n"
        "  countdown :n - 1\n"
        "END\n"
        "countdown 3");
    CHECK_STREQ(captured_output, "3\n2\n1\n");
}

TEST(test_recursion_depth_cap_reports_error_not_a_crash) {
    LogoApp *app = new_app();
    run_source(app,
        "TO recur\n"
        "  recur\n"
        "END\n"
        "recur");
    CHECK_CONTAINS(captured_output, "Recursion too deep, call ignored");
}

TEST(test_math_operators) {
    LogoApp *app = new_app();
    run_source(app, "PRINT ABS -5\nPRINT SQRT 16\nPRINT POWER 2 10\nPRINT ROUND 4.6\nPRINT INT 4.9");
    CHECK_STREQ(captured_output, "5\n4\n1024\n5\n4\n");
}

TEST(test_trig_and_log_operators) {
    // SIN/COS/TAN take degrees, ASIN/ACOS/ARCTAN return degrees --
    // matches interpreter.c's own convention (SETHEADING/turtle angles
    // are degrees), confirmed directly against the running interpreter
    // rather than assumed.
    LogoApp *app = new_app();
    run_source(app,
        "PRINT SIN 90\nPRINT COS 0\nPRINT TAN 45\n"
        "PRINT ARCTAN 1\nPRINT ASIN 1\nPRINT ACOS 1\n"
        "PRINT LN 1\nPRINT LOG 100\nPRINT EXP 0\nPRINT EXP 1");
    CHECK_STREQ(captured_output, "1\n1\n1\n45\n90\n0\n0\n2\n1\n2.71828\n");
}

TEST(test_mod_operator) {
    // A negative second argument needs parens (MOD 7 (-3), not
    // MOD 7 -3) -- the same pre-existing "greedy subtraction" grammar
    // ambiguity already documented for SETXY/SETHEADING (a call
    // argument's own expression parse consumes a following "- expr" as
    // subtraction rather than stopping at the next argument boundary),
    // confirmed directly (MOD 7 -3 fails to parse; MOD 7 (-3) doesn't)
    // rather than assumed. MOD's result takes the sign of the divisor,
    // not the dividend (Python-style, not C fmod's truncation), also
    // confirmed against the real interpreter's own mod_result.
    LogoApp *app = new_app();
    run_source(app, "PRINT MOD 7 3\nPRINT MOD 7 (-3)\nPRINT MOD (-7) 3\nPRINT MOD (-7) (-3)\nPRINT MOD 5 0");
    CHECK_STREQ(captured_output, "1\n-2\n2\n-1\n0\n");
}

TEST(test_local_scope_shadows_global) {
    // Not named "setx": that's a genuine built-in in the real
    // interpreter (sets the turtle's X coordinate) this evaluator
    // doesn't implement yet -- harmless here (this file only runs the
    // new engine), but see tests/test_shadow_diff.c for why it matters
    // against the old one.
    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"x 100\n"
        "TO showvalue :x\n"
        "  PRINT :x\n"
        "END\n"
        "showvalue 1\n"
        "PRINT :x");
    CHECK_STREQ(captured_output, "1\n100\n");
}

TEST(test_list_literal_prints_space_separated_without_brackets) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"x [1 2 3]\nPRINT :x");
    CHECK_STREQ(captured_output, "1 2 3\n");
}

TEST(test_nested_list_literal_prints_with_inner_brackets) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"x [a [b c] d]\nPRINT :x");
    CHECK_STREQ(captured_output, "a [b c] d\n");
}

TEST(test_first_butfirst_last_butlast_on_a_list) {
    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"x [10 20 30]\n"
        "PRINT FIRST :x\n"
        "PRINT BUTFIRST :x\n"
        "PRINT LAST :x\n"
        "PRINT BUTLAST :x");
    CHECK_STREQ(captured_output, "10\n20 30\n30\n10 20\n");
}

TEST(test_first_butfirst_on_a_word) {
    // A word is a sequence of characters, not one atomic element --
    // same convention as the real interpreter.
    LogoApp *app = new_app();
    run_source(app, "PRINT FIRST \"hello\nPRINT BUTFIRST \"hello\nPRINT LAST \"hello\nPRINT BUTLAST \"hello");
    CHECK_STREQ(captured_output, "h\nello\no\nhell\n");
}

TEST(test_count_and_empty) {
    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"x [1 2 3 4]\n"
        "PRINT COUNT :x\n"
        "PRINT COUNT \"hello\n"
        "PRINT EMPTY? []\n"
        "PRINT EMPTY? :x");
    CHECK_STREQ(captured_output, "4\n5\nTRUE\nFALSE\n");
}

TEST(test_fput_and_lput) {
    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"x [2 3]\n"
        "PRINT FPUT 1 :x\n"
        "PRINT LPUT 4 :x");
    CHECK_STREQ(captured_output, "1 2 3\n2 3 4\n");
}

TEST(test_word_sentence_list_constructors) {
    LogoApp *app = new_app();
    run_source(app,
        "PRINT WORD \"hello \"world\n"
        "PRINT SENTENCE [1 2] [3 4]\n"
        "PRINT LIST [1 2] [3 4]");
    CHECK_STREQ(captured_output, "helloworld\n1 2 3 4\n[1 2] [3 4]\n");
}

TEST(test_list_passed_as_procedure_argument_and_output) {
    LogoApp *app = new_app();
    run_source(app,
        "TO firstOf :lst\n"
        "  OUTPUT FIRST :lst\n"
        "END\n"
        "PRINT firstOf [7 8 9]");
    CHECK_STREQ(captured_output, "7\n");
}

TEST(test_list_equality_in_condition) {
    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"x [1 2 3]\n"
        "MAKE \"y [1 2 3]\n"
        "IF :x = :y [PRINT \"same]");
    CHECK_STREQ(captured_output, "same\n");
}

TEST(test_array_creates_cells_defaulting_to_empty_lists) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"a ARRAY 3\nPRINT ITEM 1 :a\nPRINT COUNT :a");
    CHECK_STREQ(captured_output, "\n3\n");
}

TEST(test_array_size_must_be_at_least_one) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"a ARRAY 0");
    CHECK_CONTAINS(captured_output, "ARRAY: size must be at least 1");
}

TEST(test_array_prints_with_braces) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"a ARRAY 2\nSETITEM 1 :a 10\nSETITEM 2 :a 20\nPRINT :a");
    CHECK_STREQ(captured_output, "{10 20}\n");
}

TEST(test_setitem_stores_number_word_and_list) {
    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"a ARRAY 3\n"
        "SETITEM 1 :a \"x\n"
        "SETITEM 2 :a 42\n"
        "SETITEM 3 :a [1 2 3]\n"
        "PRINT ITEM 1 :a\n"
        "PRINT ITEM 2 :a\n"
        "PRINT ITEM 3 :a");
    CHECK_STREQ(captured_output, "x\n42\n1 2 3\n");
}

TEST(test_setitem_index_out_of_range) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"a ARRAY 3\nSETITEM 0 :a \"x");
    CHECK_CONTAINS(captured_output, "SETITEM: index out of range");
    run_source(app, "SETITEM 4 :a \"x");
    CHECK_CONTAINS(captured_output, "SETITEM: index out of range");
}

TEST(test_setitem_on_non_array_reports_error) {
    LogoApp *app = new_app();
    run_source(app, "SETITEM 1 [1 2 3] \"x");
    CHECK_CONTAINS(captured_output, "SETITEM: expected an array");
}

TEST(test_setitem_array_inside_array_reports_error) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"a ARRAY 2\nMAKE \"b ARRAY 2\nSETITEM 1 :a :b");
    CHECK_CONTAINS(captured_output, "SETITEM: can't store an array inside an array");
}

TEST(test_fillarray_fills_every_cell) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"a ARRAY 2\nFILLARRAY :a [1 2]\nPRINT ITEM 1 :a\nPRINT ITEM 2 :a");
    CHECK_STREQ(captured_output, "1 2\n1 2\n");
}

TEST(test_fillarray_on_non_array_reports_error) {
    LogoApp *app = new_app();
    run_source(app, "FILLARRAY [1 2 3] \"x");
    CHECK_CONTAINS(captured_output, "FILLARRAY: expected an array");
}

TEST(test_item_out_of_range_on_array) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"a ARRAY 3\nPRINT ITEM 0 :a");
    CHECK_CONTAINS(captured_output, "ITEM: index out of range");
    run_source(app, "PRINT ITEM 4 :a");
    CHECK_CONTAINS(captured_output, "ITEM: index out of range");
}

TEST(test_count_of_array) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"a ARRAY 5\nPRINT COUNT :a");
    CHECK_STREQ(captured_output, "5\n");
}

TEST(test_make_aliases_array_not_copies) {
    // Arrays are this language's one mutable value -- see
    // set_var_array's own comment. MAKE "b :a must alias the same
    // list_pool cells, not copy them.
    LogoApp *app = new_app();
    run_source(app, "MAKE \"a ARRAY 2\nMAKE \"b :a\nSETITEM 1 :b 99\nPRINT ITEM 1 :a");
    CHECK_STREQ(captured_output, "99\n");
}

TEST(test_array_passed_as_procedure_argument_keeps_its_aliasing) {
    LogoApp *app = new_app();
    run_source(app,
        "TO mutate :a\n"
        "  SETITEM 1 :a 777\n"
        "END\n"
        "MAKE \"arr ARRAY 3\n"
        "mutate :arr\n"
        "PRINT ITEM 1 :arr");
    CHECK_STREQ(captured_output, "777\n");
}

TEST(test_type_predicates_on_a_word) {
    LogoApp *app = new_app();
    run_source(app,
        "PRINT WORD? \"hi\n"
        "PRINT LIST? \"hi\n"
        "PRINT NUMBER? \"hi\n"
        "PRINT ARRAY? \"hi");
    CHECK_STREQ(captured_output, "TRUE\nFALSE\nFALSE\nFALSE\n");
}

TEST(test_type_predicates_on_a_number) {
    LogoApp *app = new_app();
    run_source(app,
        "PRINT WORD? 5\n"
        "PRINT LIST? 5\n"
        "PRINT NUMBER? 5\n"
        "PRINT ARRAY? 5");
    CHECK_STREQ(captured_output, "FALSE\nFALSE\nTRUE\nFALSE\n");
}

TEST(test_type_predicates_on_a_list) {
    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"x [1 2 3]\n"
        "PRINT WORD? :x\n"
        "PRINT LIST? :x\n"
        "PRINT NUMBER? :x\n"
        "PRINT ARRAY? :x");
    CHECK_STREQ(captured_output, "FALSE\nTRUE\nFALSE\nFALSE\n");
}

TEST(test_type_predicates_on_an_array) {
    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"a ARRAY 2\n"
        "PRINT WORD? :a\n"
        "PRINT LIST? :a\n"
        "PRINT NUMBER? :a\n"
        "PRINT ARRAY? :a");
    CHECK_STREQ(captured_output, "FALSE\nFALSE\nFALSE\nTRUE\n");
}

TEST(test_memberp_on_list_and_number) {
    LogoApp *app = new_app();
    run_source(app,
        "PRINT MEMBER? \"b [a b c]\n"
        "PRINT MEMBER? \"z [a b c]\n"
        "PRINT MEMBER? 2 [1 2 3]\n"
        "PRINT MEMBER? 5 5\n"
        "PRINT MEMBER? 5 6");
    CHECK_STREQ(captured_output, "TRUE\nFALSE\nTRUE\nTRUE\nFALSE\n");
}

TEST(test_memberp_on_word_is_substring) {
    LogoApp *app = new_app();
    run_source(app, "PRINT MEMBER? \"ell \"hello\nPRINT MEMBER? \"xyz \"hello");
    CHECK_STREQ(captured_output, "TRUE\nFALSE\n");
}

TEST(test_map_transforms_each_element) {
    LogoApp *app = new_app();
    run_source(app, "PRINT MAP [? * 2] [1 2 3]");
    CHECK_STREQ(captured_output, "2 4 6\n");
}

TEST(test_map_on_a_bare_number) {
    LogoApp *app = new_app();
    run_source(app, "PRINT MAP [? + 1] 5");
    CHECK_STREQ(captured_output, "6\n");
}

TEST(test_map_preserves_nested_list_elements) {
    // A list-typed element must round-trip through the template with
    // its brackets intact, or its tokens would spill into the
    // surrounding expression instead of staying one value.
    LogoApp *app = new_app();
    run_source(app, "PRINT MAP [COUNT ?] [[1 2] [3 4 5]]");
    CHECK_STREQ(captured_output, "2 3\n");
}

TEST(test_map_with_word_elements) {
    // Every bracket-list element is internally a VALUE_WORD regardless
    // of whether it looks numeric -- this exercises a genuinely
    // non-numeric word element, which must come back out of the
    // template quoted ('raw text'), not as bare text that looks like
    // an attempted command.
    LogoApp *app = new_app();
    run_source(app, "PRINT MAP [FIRST ?] [foo bar]");
    CHECK_STREQ(captured_output, "f b\n");
}

TEST(test_filter_keeps_matching_elements) {
    LogoApp *app = new_app();
    run_source(app, "PRINT FILTER [? > 2] [1 2 3 4]");
    CHECK_STREQ(captured_output, "3 4\n");
}

TEST(test_filter_with_word_elements) {
    // Exercises parse_list_literal's quoted-word handling directly:
    // "b inside the template list literal must keep its literal "
    // (matching interpreter.c's own raw list-literal scan, which never
    // treats " specially there) so that substituting an element's own
    // text back in and re-parsing sees a real quoted-word comparison
    // again, not a bareword "unknown word" parse failure.
    LogoApp *app = new_app();
    run_source(app, "PRINT FILTER [? = \"b] [a b c b]");
    CHECK_STREQ(captured_output, "b b\n");
}

TEST(test_reduce_folds_left_to_right) {
    LogoApp *app = new_app();
    run_source(app, "PRINT REDUCE [?1 + ?2] [1 2 3 4]");
    CHECK_STREQ(captured_output, "10\n");
}

TEST(test_reduce_of_single_element_list_is_that_element) {
    LogoApp *app = new_app();
    run_source(app, "PRINT REDUCE [?1 + ?2] [5]");
    CHECK_STREQ(captured_output, "5\n");
}

TEST(test_reduce_concatenates_word_elements) {
    // Exercises eval_value_to_source_text on the accumulator itself (a
    // WORD, not just the current element) across repeated iterations.
    LogoApp *app = new_app();
    run_source(app, "PRINT REDUCE [WORD ?1 ?2] [a b c]");
    CHECK_STREQ(captured_output, "abc\n");
}

TEST(test_foreach_runs_template_for_each_element) {
    LogoApp *app = new_app();
    run_source(app, "FOREACH [PRINT WORD ? \"!] [a b c]");
    CHECK_STREQ(captured_output, "a!\nb!\nc!\n");
}

TEST(test_foreach_on_a_bare_number) {
    LogoApp *app = new_app();
    run_source(app, "FOREACH [PRINT ?] 5");
    CHECK_STREQ(captured_output, "5\n");
}

TEST(test_foreach_accumulates_via_make) {
    // Exercises parse_list_literal's :varref handling directly: :sum
    // inside the template list literal must keep its leading : (same
    // bug class, same fix, as the earlier "b quoted-word case) so that
    // re-parsing the substituted statement each iteration reads the
    // running total rather than an unrelated bareword "sum" that
    // always evaluates to 0.
    LogoApp *app = new_app();
    run_source(app, "MAKE \"sum 0\nFOREACH [MAKE \"sum :sum + ?] [1 2 3 4]\nPRINT :sum");
    CHECK_STREQ(captured_output, "10\n");
}

TEST(test_foreach_with_quoted_word_template) {
    LogoApp *app = new_app();
    run_source(app, "FOREACH [IF ? = \"b [PRINT \"match]] [a b c]");
    CHECK_STREQ(captured_output, "match\n");
}

TEST(test_foreach_stop_ends_loop_early) {
    LogoApp *app = new_app();
    run_source(app, "FOREACH [IF ? = 3 [STOP] PRINT ?] [1 2 3 4 5]");
    CHECK_STREQ(captured_output, "1\n2\n");
}

TEST(test_foreach_preserves_nested_list_elements) {
    LogoApp *app = new_app();
    run_source(app, "FOREACH [PRINT FIRST ?] [[10 20] [30 40]]");
    CHECK_STREQ(captured_output, "10\n30\n");
}

TEST(test_setprop_getprop_round_trips_a_number_word_and_list) {
    LogoApp *app = new_app();
    run_source(app,
        "SETPROP \"turtle1 \"speed 5\n"
        "SETPROP \"turtle1 \"color \"red\n"
        "SETPROP \"turtle1 \"pos [10 20]\n"
        "PRINT GETPROP \"turtle1 \"speed\n"
        "PRINT GETPROP \"turtle1 \"color\n"
        "PRINT GETPROP \"turtle1 \"pos");
    CHECK_STREQ(captured_output, "5\nred\n10 20\n");
}

TEST(test_getprop_of_missing_property_is_the_empty_list) {
    LogoApp *app = new_app();
    run_source(app, "PRINT GETPROP \"turtle1 \"nosuchkey\nPRINT EMPTY? GETPROP \"turtle1 \"nosuchkey");
    CHECK_STREQ(captured_output, "\nTRUE\n");
}

TEST(test_setprop_on_same_key_overwrites_not_duplicates) {
    LogoApp *app = new_app();
    run_source(app,
        "SETPROP \"turtle1 \"color \"red\n"
        "SETPROP \"turtle1 \"color \"blue\n"
        "PRINT GETPROP \"turtle1 \"color\n"
        "PRINT COUNT PROPLIST \"turtle1"); // one key/value pair, not two
    CHECK_STREQ(captured_output, "blue\n2\n");
}

TEST(test_removeprop_removes_a_property) {
    LogoApp *app = new_app();
    run_source(app,
        "SETPROP \"turtle1 \"color \"red\n"
        "REMOVEPROP \"turtle1 \"color\n"
        "PRINT GETPROP \"turtle1 \"color");
    CHECK_STREQ(captured_output, "\n");
}

TEST(test_proplist_lists_alternating_keys_and_values) {
    LogoApp *app = new_app();
    run_source(app,
        "SETPROP \"turtle1 \"speed 5\n"
        "SETPROP \"turtle1 \"color \"red\n"
        "PRINT PROPLIST \"turtle1");
    CHECK_STREQ(captured_output, "speed 5 color red\n");
}

TEST(test_different_proplist_names_dont_share_properties) {
    LogoApp *app = new_app();
    run_source(app,
        "SETPROP \"turtle1 \"color \"red\n"
        "SETPROP \"turtle2 \"color \"blue\n"
        "PRINT GETPROP \"turtle1 \"color\n"
        "PRINT GETPROP \"turtle2 \"color");
    CHECK_STREQ(captured_output, "red\nblue\n");
}

// --- Prototype-style objects (NEW/SEND) -----------------------------
// SEND obj "message arglist here, not the old engine's positional
// SEND obj "message arg1 arg2... -- arglist is always required ([]
// for a zero-argument message), a deliberate syntax difference this
// engine's real static parsing needs (see BUILTIN_SIGNATURES's own
// comment on SEND in parser.c for why).

TEST(test_send_calls_a_method_registered_directly_on_the_object) {
    LogoApp *app = new_app();
    run_source(app,
        "TO dog_bark :self\n"
        "  PRINT SENTENCE :self \"barks\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"bark \"dog_bark\n"
        "SEND \"dog \"bark []");
    CHECK_STREQ(captured_output, "dog barks\n");
}

TEST(test_send_finds_a_method_through_the_prototype_chain) {
    LogoApp *app = new_app();
    run_source(app,
        "TO animal_speak :self\n"
        "  PRINT SENTENCE :self GETPROP :self \"sound\n"
        "END\n"
        "NEW \"animal \"nothing\n"
        "SETPROP \"animal \"speak \"animal_speak\n"
        "SETPROP \"animal \"sound \"generic\n"
        "NEW \"dog \"animal\n"
        "SETPROP \"dog \"sound \"Woof\n"
        "SEND \"dog \"speak []\n" // own sound property wins
        "SEND \"animal \"speak []"); // falls back to its own sound
    CHECK_STREQ(captured_output, "dog Woof\nanimal generic\n");
}

TEST(test_send_inherits_methods_but_not_data_fields) {
    LogoApp *app = new_app();
    run_source(app,
        "TO animal_speak :self\n"
        "  PRINT SENTENCE :self GETPROP :self \"sound\n"
        "END\n"
        "NEW \"animal \"nothing\n"
        "SETPROP \"animal \"speak \"animal_speak\n"
        "NEW \"dog \"animal\n"
        "SETPROP \"dog \"sound \"Woof\n"
        "NEW \"puppy \"dog\n" // no own "sound" -- GETPROP never chain-walks
        "SEND \"puppy \"speak []");
    CHECK_STREQ(captured_output, "puppy\n"); // sound came back empty
}

TEST(test_send_passes_extra_message_arguments_after_self) {
    // A bareword inside the arglist, not "alice -- a quoted word inside
    // a list literal keeps its literal " as part of the stored text
    // (confirmed directly against interpreter.c's own list-literal
    // scanning, which never treats " specially there), so "alice would
    // arrive as the 6-character word `"alice`, not a clean `alice`.
    LogoApp *app = new_app();
    run_source(app,
        "TO animal_greet :self :name\n"
        "  PRINT SENTENCE :self SENTENCE \"hi :name\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"greet \"animal_greet\n"
        "SEND \"dog \"greet [alice]");
    CHECK_STREQ(captured_output, "dog hi alice\n");
}

TEST(test_send_operator_form_captures_output) {
    LogoApp *app = new_app();
    run_source(app,
        "TO dog_getname :self\n"
        "  OUTPUT :self\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"getname \"dog_getname\n"
        "PRINT SEND \"dog \"getname []\n"
        "MAKE \"n SEND \"dog \"getname []\n"
        "PRINT :n");
    CHECK_STREQ(captured_output, "dog\ndog\n");
}

TEST(test_send_operator_form_errors_if_method_never_outputs) {
    LogoApp *app = new_app();
    run_source(app,
        "TO dog_bark :self\n"
        "  PRINT \"Woof\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"bark \"dog_bark\n"
        "PRINT SEND \"dog \"bark []");
    CHECK_CONTAINS(captured_output, "Woof\n");
    CHECK_CONTAINS(captured_output, "bark: didn't output a value\n");
}

TEST(test_send_to_unknown_message_reports_does_not_understand) {
    LogoApp *app = new_app();
    run_source(app, "NEW \"dog \"nothing\nSEND \"dog \"bark []");
    CHECK_CONTAINS(captured_output, "SEND: dog does not understand bark\n");
}

TEST(test_send_on_a_data_property_reports_not_a_method) {
    LogoApp *app = new_app();
    run_source(app, "NEW \"dog \"nothing\nSETPROP \"dog \"sound \"Woof\nSEND \"dog \"sound []");
    CHECK_CONTAINS(captured_output, "sound is not a method on dog");
}

TEST(test_send_method_without_self_param_reports_error) {
    LogoApp *app = new_app();
    run_source(app,
        "TO badmethod\n"
        "  PRINT \"oops\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"bark \"badmethod\n"
        "SEND \"dog \"bark []");
    CHECK_CONTAINS(captured_output, "badmethod must take :self as its first input");
}

TEST(test_send_cyclic_prototype_chain_is_bounded_not_infinite) {
    LogoApp *app = new_app();
    run_source(app, "NEW \"a \"b\nNEW \"b \"a\nSEND \"a \"speak []");
    CHECK_CONTAINS(captured_output, "SEND: a does not understand speak\n");
}

TEST(test_send_wrong_argument_count_reports_error) {
    // No old-engine equivalent: the old engine's positional-consuming
    // parser structurally can't miscount this way (it just consumes
    // exactly proc->param_count - 1 more expression tokens, whatever
    // they are). This is a real check made possible -- and needed --
    // specifically by SEND's new explicit-arglist convention here,
    // mirroring APPLY's own existing "wrong number of inputs" check.
    LogoApp *app = new_app();
    run_source(app,
        "TO animal_greet :self :name\n"
        "  PRINT SENTENCE :self SENTENCE \"hi :name\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"greet \"animal_greet\n"
        "SEND \"dog \"greet []"); // missing the required :name argument
    CHECK_CONTAINS(captured_output, "SEND: wrong number of inputs for message \"greet");
}

int main(void) {
    RUN(test_print_a_number_and_a_word);
    RUN(test_arithmetic_with_precedence);
    RUN(test_parenthesized_grouping);
    RUN(test_make_and_varref);
    RUN(test_make_negative_number_is_not_swallowed_as_subtraction);
    RUN(test_if_true_and_false_branches);
    RUN(test_ifelse_takes_the_false_branch);
    RUN(test_boolean_not_and_or);
    RUN(test_word_equality_is_case_insensitive_text_comparison);
    RUN(test_while_loop);
    RUN(test_repeat_loop);
    RUN(test_turtle_motion_fd_and_rt);
    RUN(test_setxy_and_setheading);
    RUN(test_penup_pendown);
    RUN(test_home_and_clear);
    RUN(test_procedure_with_output);
    RUN(test_procedure_forward_reference);
    RUN(test_recursive_procedure);
    RUN(test_recursion_depth_cap_reports_error_not_a_crash);
    RUN(test_math_operators);
    RUN(test_trig_and_log_operators);
    RUN(test_mod_operator);
    RUN(test_local_scope_shadows_global);
    RUN(test_list_literal_prints_space_separated_without_brackets);
    RUN(test_nested_list_literal_prints_with_inner_brackets);
    RUN(test_first_butfirst_last_butlast_on_a_list);
    RUN(test_first_butfirst_on_a_word);
    RUN(test_count_and_empty);
    RUN(test_fput_and_lput);
    RUN(test_word_sentence_list_constructors);
    RUN(test_list_passed_as_procedure_argument_and_output);
    RUN(test_list_equality_in_condition);
    RUN(test_array_creates_cells_defaulting_to_empty_lists);
    RUN(test_array_size_must_be_at_least_one);
    RUN(test_array_prints_with_braces);
    RUN(test_setitem_stores_number_word_and_list);
    RUN(test_setitem_index_out_of_range);
    RUN(test_setitem_on_non_array_reports_error);
    RUN(test_setitem_array_inside_array_reports_error);
    RUN(test_fillarray_fills_every_cell);
    RUN(test_fillarray_on_non_array_reports_error);
    RUN(test_item_out_of_range_on_array);
    RUN(test_count_of_array);
    RUN(test_make_aliases_array_not_copies);
    RUN(test_array_passed_as_procedure_argument_keeps_its_aliasing);
    RUN(test_type_predicates_on_a_word);
    RUN(test_type_predicates_on_a_number);
    RUN(test_type_predicates_on_a_list);
    RUN(test_type_predicates_on_an_array);
    RUN(test_memberp_on_list_and_number);
    RUN(test_memberp_on_word_is_substring);
    RUN(test_map_transforms_each_element);
    RUN(test_map_on_a_bare_number);
    RUN(test_map_preserves_nested_list_elements);
    RUN(test_map_with_word_elements);
    RUN(test_filter_keeps_matching_elements);
    RUN(test_filter_with_word_elements);
    RUN(test_reduce_folds_left_to_right);
    RUN(test_reduce_of_single_element_list_is_that_element);
    RUN(test_reduce_concatenates_word_elements);
    RUN(test_foreach_runs_template_for_each_element);
    RUN(test_foreach_on_a_bare_number);
    RUN(test_foreach_accumulates_via_make);
    RUN(test_foreach_with_quoted_word_template);
    RUN(test_foreach_stop_ends_loop_early);
    RUN(test_foreach_preserves_nested_list_elements);
    RUN(test_setprop_getprop_round_trips_a_number_word_and_list);
    RUN(test_getprop_of_missing_property_is_the_empty_list);
    RUN(test_setprop_on_same_key_overwrites_not_duplicates);
    RUN(test_removeprop_removes_a_property);
    RUN(test_proplist_lists_alternating_keys_and_values);
    RUN(test_different_proplist_names_dont_share_properties);
    RUN(test_send_calls_a_method_registered_directly_on_the_object);
    RUN(test_send_finds_a_method_through_the_prototype_chain);
    RUN(test_send_inherits_methods_but_not_data_fields);
    RUN(test_send_passes_extra_message_arguments_after_self);
    RUN(test_send_operator_form_captures_output);
    RUN(test_send_operator_form_errors_if_method_never_outputs);
    RUN(test_send_to_unknown_message_reports_does_not_understand);
    RUN(test_send_on_a_data_property_reports_not_a_method);
    RUN(test_send_method_without_self_param_reports_error);
    RUN(test_send_cyclic_prototype_chain_is_bounded_not_infinite);
    RUN(test_send_wrong_argument_count_reports_error);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
