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
    RUN(test_local_scope_shadows_global);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
