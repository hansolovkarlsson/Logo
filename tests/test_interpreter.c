// test_interpreter.c
//
// Headless tests for the interpreter core (eval_logo, the expression
// parser, procedures/scoping, conditionals, words, error messages) with
// no GTK widgets involved: LogoApp.output_sink is swapped for a plain
// buffer instead of ui.c's real history-pane sink, so these run in
// milliseconds from the command line. Run via `make test`.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/interpreter.h"

static int failures = 0;
static const char *current_test = "";
static char captured_output[4096];

// Swapped in for LogoApp.output_sink: appends everything PRINT/error
// messages send to a plain buffer instead of a GTK text view.
static void capture_sink(LogoApp *app, const char *text) {
    (void)app;
    strncat(captured_output, text, sizeof(captured_output) - strlen(captured_output) - 1);
}

// A fresh interpreter, matching the defaults ui.c's logo_activate sets
// (turtle at the canvas center, pen down, default color/width, white
// background) but with no GTK widgets — those fields stay NULL/zero and
// are simply never touched by the interpreter core.
static LogoApp new_app(void) {
    LogoApp app;
    memset(&app, 0, sizeof(app));
    app.turtle.x = 250;
    app.turtle.y = 250;
    app.turtle.angle = 0;
    app.turtle.pen_down = 1;
    app.turtle.pen_r = 0.1;
    app.turtle.pen_g = 0.1;
    app.turtle.pen_b = 0.1;
    app.turtle.pen_width = 2.0;
    app.bg_r = app.bg_g = app.bg_b = 1.0;
    app.output_sink = capture_sink;
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

// --- Turtle motion ---

TEST(test_forward_moves_and_draws) {
    LogoApp app = new_app();
    eval_logo(&app, "FD 100");
    CHECK_NEAR(app.turtle.x, 250.0);
    CHECK_NEAR(app.turtle.y, 150.0); // angle 0 = up (north): y decreases
    CHECK(app.line_count == 1);
    CHECK_NEAR(app.lines[0].x1, 250.0);
    CHECK_NEAR(app.lines[0].y1, 250.0);
    CHECK_NEAR(app.lines[0].x2, 250.0);
    CHECK_NEAR(app.lines[0].y2, 150.0);
}

TEST(test_penup_moves_without_drawing) {
    LogoApp app = new_app();
    eval_logo(&app, "PENUP FD 100");
    CHECK(app.line_count == 0);
    CHECK_NEAR(app.turtle.y, 150.0);
}

TEST(test_repeat_square_returns_to_start) {
    LogoApp app = new_app();
    eval_logo(&app, "REPEAT 4 [FD 100 RT 90]");
    CHECK(app.line_count == 4);
    CHECK_NEAR(app.turtle.x, 250.0);
    CHECK_NEAR(app.turtle.y, 250.0);
}

TEST(test_setxy_draws_direct_line) {
    LogoApp app = new_app();
    eval_logo(&app, "SETXY 400 300");
    CHECK(app.line_count == 1);
    CHECK_NEAR(app.turtle.x, 400.0);
    CHECK_NEAR(app.turtle.y, 300.0);
}

TEST(test_home_returns_and_resets_heading) {
    LogoApp app = new_app();
    eval_logo(&app, "SETXY 400 300 SETHEADING 45 HOME");
    CHECK_NEAR(app.turtle.x, 250.0);
    CHECK_NEAR(app.turtle.y, 250.0);
    CHECK_NEAR(app.turtle.angle, 0.0);
    CHECK(app.line_count == 2); // the SETXY jump, then HOME's jump back
}

TEST(test_setpencolor_and_width_stamp_the_segment) {
    LogoApp app = new_app();
    eval_logo(&app, "SETPENCOLOR 255 0 0 SETPENWIDTH 5 FD 50");
    CHECK(app.line_count == 1);
    CHECK_NEAR(app.lines[0].r, 1.0);
    CHECK_NEAR(app.lines[0].g, 0.0);
    CHECK_NEAR(app.lines[0].b, 0.0);
    CHECK_NEAR(app.lines[0].width, 5.0);
}

TEST(test_setbackground_sets_canvas_color) {
    LogoApp app = new_app();
    eval_logo(&app, "SETBACKGROUND 10 20 30");
    CHECK_NEAR(app.bg_r, 10.0 / 255.0);
    CHECK_NEAR(app.bg_g, 20.0 / 255.0);
    CHECK_NEAR(app.bg_b, 30.0 / 255.0);
}

// --- Procedures & scoping ---

TEST(test_procedure_definition_and_call) {
    LogoApp app = new_app();
    eval_logo(&app, "TO square :size\nREPEAT 4 [FD :size RT 90]\nEND\nsquare 60");
    CHECK(app.line_count == 4);
    CHECK_NEAR(app.turtle.x, 250.0);
    CHECK_NEAR(app.turtle.y, 250.0);
}

TEST(test_recursion_with_parameter) {
    LogoApp app = new_app();
    eval_logo(&app, "TO countdown :n\nIF :n > 0 [PRINT :n countdown :n - 1]\nEND\ncountdown 3");
    CHECK_STREQ(captured_output, "3\n2\n1\n");
}

TEST(test_parameter_shadows_outer_global) {
    LogoApp app = new_app();
    eval_logo(&app,
        "MAKE \"size 999\n"
        "TO square :size\nREPEAT 4 [FD :size RT 90]\nEND\n"
        "square 50\n"
        "PRINT :size");
    CHECK_STREQ(captured_output, "999\n"); // global untouched by the local parameter
}

TEST(test_make_without_local_mutates_outer_global) {
    LogoApp app = new_app();
    eval_logo(&app,
        "MAKE \"counter 0\n"
        "TO increment\nMAKE \"counter :counter + 1\nEND\n"
        "increment\nincrement\nincrement\n"
        "PRINT :counter");
    CHECK_STREQ(captured_output, "3\n"); // dynamic scoping: no local "counter" to shadow it
}

TEST(test_to_redefinition_overwrites) {
    LogoApp app = new_app();
    eval_logo(&app, "TO t\nPRINT \"one\nEND\nTO t\nPRINT \"two\nEND\nt");
    CHECK_STREQ(captured_output, "two\n");
}

TEST(test_erase_removes_procedure) {
    LogoApp app = new_app();
    eval_logo(&app, "TO t\nPRINT \"hi\nEND\nERASE \"t\nt");
    CHECK_CONTAINS(captured_output, "I don't know how to");
}

// --- Conditionals & booleans ---

TEST(test_if_ifelse_comparisons) {
    LogoApp app = new_app();
    eval_logo(&app, "IF 5 > 3 [PRINT \"yes]");
    CHECK_STREQ(captured_output, "yes\n");
}

TEST(test_boolean_and_or_not) {
    LogoApp app = new_app();
    eval_logo(&app,
        "MAKE \"x 5\n"
        "IF :x > 0 AND :x < 10 [PRINT \"in-range]\n"
        "IF :x < 0 OR :x > 100 [PRINT \"out]\n"
        "IF NOT :x = 0 [PRINT \"nonzero]");
    CHECK_STREQ(captured_output, "in-range\nnonzero\n");
}

TEST(test_while_loop) {
    LogoApp app = new_app();
    eval_logo(&app, "MAKE \"i 0\nWHILE :i < 3 [PRINT :i MAKE \"i :i + 1]");
    CHECK_STREQ(captured_output, "0\n1\n2\n");
}

// --- Words ---

TEST(test_word_variable_and_comparison) {
    LogoApp app = new_app();
    eval_logo(&app, "MAKE \"name \"World\nPRINT :name\nIF :name = \"World [PRINT \"matched]");
    CHECK_STREQ(captured_output, "World\nmatched\n");
}

TEST(test_print_list_joins_words_with_single_spaces) {
    LogoApp app = new_app();
    eval_logo(&app, "PRINT [hello   there    friend]");
    CHECK_STREQ(captured_output, "hello there friend\n");
}

// --- Errors ---

TEST(test_unknown_command_reports_error) {
    LogoApp app = new_app();
    eval_logo(&app, "FOOBAR");
    CHECK_STREQ(captured_output, "I don't know how to FOOBAR\n");
}

TEST(test_malformed_repeat_does_not_crash) {
    LogoApp app = new_app();
    eval_logo(&app, "REPEAT 4 FD 10\nPRINT \"still-here");
    CHECK_STREQ(captured_output, "REPEAT: expected [ block ]\nstill-here\n");
}

TEST(test_make_without_quote_reports_error) {
    LogoApp app = new_app();
    eval_logo(&app, "MAKE x 5");
    CHECK_CONTAINS(captured_output, "MAKE: expected a");
}

int main(void) {
    RUN(test_forward_moves_and_draws);
    RUN(test_penup_moves_without_drawing);
    RUN(test_repeat_square_returns_to_start);
    RUN(test_setxy_draws_direct_line);
    RUN(test_home_returns_and_resets_heading);
    RUN(test_setpencolor_and_width_stamp_the_segment);
    RUN(test_setbackground_sets_canvas_color);

    RUN(test_procedure_definition_and_call);
    RUN(test_recursion_with_parameter);
    RUN(test_parameter_shadows_outer_global);
    RUN(test_make_without_local_mutates_outer_global);
    RUN(test_to_redefinition_overwrites);
    RUN(test_erase_removes_procedure);

    RUN(test_if_ifelse_comparisons);
    RUN(test_boolean_and_or_not);
    RUN(test_while_loop);

    RUN(test_word_variable_and_comparison);
    RUN(test_print_list_joins_words_with_single_spaces);

    RUN(test_unknown_command_reports_error);
    RUN(test_malformed_repeat_does_not_crash);
    RUN(test_make_without_quote_reports_error);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d failure(s).\n", failures);
    return 1;
}
