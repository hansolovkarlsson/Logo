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

TEST(test_who_reports_the_current_turtle_starting_at_zero) {
    LogoApp *app = new_app();
    run_source(app, "PRINT WHO");
    CHECK_STREQ(captured_output, "0\n");
}

TEST(test_tell_switches_the_current_turtle_and_creates_it) {
    LogoApp *app = new_app();
    run_source(app, "TELL 1\nPRINT WHO\nSETXY 350 350\nPRINT POS");
    CHECK_STREQ(captured_output, "1\n350 350\n");
    CHECK(app->turtle_count == 2);
    CHECK_NEAR(app->turtles[1].x, 350);
    CHECK_NEAR(app->turtles[1].y, 350);
}

TEST(test_tell_leaves_the_other_turtles_state_untouched) {
    LogoApp *app = new_app();
    run_source(app, "SETXY 10 20\nTELL 1\nSETXY 350 350\nTELL 0\nPRINT POS");
    CHECK_STREQ(captured_output, "10 20\n");
}

TEST(test_tell_out_of_range_reports_error_and_leaves_current_turtle) {
    LogoApp *app = new_app();
    run_source(app, "TELL 10\nTELL -1\nPRINT WHO");
    CHECK_CONTAINS(captured_output, "TELL: turtle index must be 0-9");
    CHECK_CONTAINS(captured_output, "0\n");
}

TEST(test_a_script_with_no_tell_behaves_as_a_single_turtle) {
    LogoApp *app = new_app();
    run_source(app, "PRINT WHO");
    CHECK(app->turtle_count == 1);
}

// --- Drawing/canvas primitives (ARC/LABEL/FILL/ERASERECT/WRAP/FENCE/
// WINDOW/CLEAN/HIDETURTLE/SHOWTURTLE/SETPENCOLOR/SETPENWIDTH/
// SETBACKGROUND/SETCANVASSIZE) and ERASE (procedure deletion) ---

TEST(test_arc_draws_segments_without_moving_the_turtle) {
    LogoApp *app = new_app();
    run_source(app, "ARC 360 80");
    CHECK(app->line_count == 72); // 360/5 = 72 segments, matching interpreter.c's own segment count
    CHECK_NEAR(app->turtles[0].x, app->canvas_width / 2.0);
    CHECK_NEAR(app->turtles[0].y, app->canvas_height / 2.0);
}

TEST(test_label_records_position_color_and_text) {
    LogoApp *app = new_app();
    run_source(app, "SETPENCOLOR 10 20 30\nLABEL \"hi");
    CHECK(app->label_count == 1);
    CHECK_STREQ(app->labels[0].text, "hi");
    CHECK_NEAR(app->labels[0].r, 10.0 / 255.0);
    CHECK_NEAR(app->labels[0].g, 20.0 / 255.0);
    CHECK_NEAR(app->labels[0].b, 30.0 / 255.0);
}

TEST(test_fill_records_a_raster_op_with_the_current_pen_color) {
    LogoApp *app = new_app();
    run_source(app, "REPEAT 4 [FD 50 RT 90]\nSETPENCOLOR 200 50 10\nFILL");
    CHECK(app->raster_op_count == 1);
    CHECK(app->raster_ops[0].kind == RASTER_OP_FILL);
    CHECK(app->raster_ops[0].line_count_at_call == 4);
}

TEST(test_eraserect_records_a_raster_op_centered_on_the_turtle) {
    LogoApp *app = new_app();
    run_source(app, "ERASERECT 30 40");
    CHECK(app->raster_op_count == 1);
    CHECK(app->raster_ops[0].kind == RASTER_OP_ERASE_RECT);
    CHECK_NEAR(app->raster_ops[0].w, 30);
    CHECK_NEAR(app->raster_ops[0].h, 40);
}

TEST(test_wrap_mode_wraps_at_the_canvas_edge) {
    LogoApp *app = new_app();
    run_source(app, "WRAP\nSETXY 490 490\nSETHEADING 180\nFD 30");
    CHECK(app->line_count == 1); // the SETXY jump only; wrapping itself draws no line
    CHECK_NEAR(app->turtles[0].x, 490.0);
    CHECK_NEAR(app->turtles[0].y, 20.0);
}

TEST(test_fence_mode_stops_at_the_canvas_edge_and_reports_error) {
    LogoApp *app = new_app();
    run_source(app, "FENCE\nSETXY 490 490\nSETHEADING 180\nFD 30");
    CHECK_CONTAINS(captured_output, "FENCE: turtle stopped at the canvas edge");
    CHECK_NEAR(app->turtles[0].y, app->canvas_height); // clamped at the edge, not past it
}

TEST(test_window_mode_is_the_default_and_ignores_the_edge) {
    LogoApp *app = new_app();
    run_source(app, "WINDOW\nSETXY 490 490\nSETHEADING 180\nFD 30");
    CHECK_NEAR(app->turtles[0].y, 520.0); // past the canvas edge, freely -- no clamp, no wrap
    CHECK_STREQ(captured_output, "");
}

TEST(test_clean_erases_drawing_but_leaves_turtle_position_alone) {
    LogoApp *app = new_app();
    run_source(app, "FD 50\nRT 90\nCLEAN\nPRINT POS\nPRINT HEADING");
    CHECK(app->line_count == 0);
    CHECK_STREQ(captured_output, "250 200\n90\n");
}

TEST(test_hideturtle_sets_visible_false_and_showturtle_restores_it) {
    LogoApp *app = new_app();
    run_source(app, "HIDETURTLE");
    CHECK(app->turtles[0].visible == 0);
    run_source(app, "SHOWTURTLE");
    CHECK(app->turtles[0].visible != 0);
}

TEST(test_setpencolor_sets_and_clamps_the_pen_color) {
    LogoApp *app = new_app();
    run_source(app, "SETPENCOLOR 300 (-10) 128");
    CHECK_NEAR(app->turtles[0].pen_r, 1.0); // 300 clamped to 255 -> 1.0
    CHECK_NEAR(app->turtles[0].pen_g, 0.0); // -10 clamped to 0
    CHECK_NEAR(app->turtles[0].pen_b, 128.0 / 255.0);
}

TEST(test_setpenwidth_clamps_to_the_valid_range) {
    LogoApp *app = new_app();
    run_source(app, "SETPENWIDTH 0.1");
    CHECK_NEAR(app->turtles[0].pen_width, MIN_PEN_WIDTH);
    run_source(app, "SETPENWIDTH 100");
    CHECK_NEAR(app->turtles[0].pen_width, MAX_PEN_WIDTH);
}

TEST(test_setbackground_sets_the_canvas_background_color) {
    LogoApp *app = new_app();
    run_source(app, "SETBACKGROUND 10 20 30");
    CHECK_NEAR(app->bg_r, 10.0 / 255.0);
    CHECK_NEAR(app->bg_g, 20.0 / 255.0);
    CHECK_NEAR(app->bg_b, 30.0 / 255.0);
}

TEST(test_setcanvassize_resizes_and_resets_turtle_and_drawing) {
    LogoApp *app = new_app();
    run_source(app, "FD 10\nSETCANVASSIZE 800 600");
    CHECK_NEAR(app->canvas_width, 800);
    CHECK_NEAR(app->canvas_height, 600);
    CHECK(app->line_count == 0);
    CHECK_NEAR(app->turtles[0].x, 400);
    CHECK_NEAR(app->turtles[0].y, 300);
}

TEST(test_setcanvassize_out_of_range_reports_error_and_leaves_canvas_unchanged) {
    LogoApp *app = new_app();
    run_source(app, "SETCANVASSIZE 10 10");
    CHECK_CONTAINS(captured_output, "SETCANVASSIZE: width and height must be");
    CHECK_NEAR(app->canvas_width, DEFAULT_CANVAS_WIDTH);
    CHECK_NEAR(app->canvas_height, DEFAULT_CANVAS_HEIGHT);
}

TEST(test_erase_deletes_a_procedure_so_it_can_no_longer_be_called) {
    LogoApp *app = new_app();
    run_source(app, "TO foo\nPRINT 1\nEND\nERASE \"foo\nfoo");
    CHECK_CONTAINS(captured_output, "I don't know how to foo");
}

TEST(test_erase_of_unknown_procedure_reports_error) {
    LogoApp *app = new_app();
    run_source(app, "ERASE \"nosuch");
    CHECK_CONTAINS(captured_output, "ERASE: no such procedure \"nosuch");
}

TEST(test_erase_removes_the_procedure_from_procedures_output) {
    LogoApp *app = new_app();
    run_source(app, "TO a\nEND\nTO b\nEND\nERASE \"a\nPRINT PROCEDURES");
    CHECK_STREQ(captured_output, "b\n");
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

TEST(test_turtle_state_queries) {
    LogoApp *app = new_app();
    run_source(app, "SETXY 30 40\nSETHEADING 45\nPRINT GETX\nPRINT GETY\nPRINT HEADING\nPRINT POS");
    CHECK_STREQ(captured_output, "30\n40\n45\n30 40\n");
}

TEST(test_canvassize_reports_width_and_height) {
    LogoApp *app = new_app();
    run_source(app, "PRINT CANVASSIZE");
    CHECK_STREQ(captured_output, "500 500\n");
}

TEST(test_setx_sety_move_one_axis_only) {
    LogoApp *app = new_app();
    run_source(app, "SETXY 10 20\nSETX 99\nPRINT POS\nSETY 55\nPRINT POS");
    CHECK_STREQ(captured_output, "99 20\n99 55\n");
}

TEST(test_distance_between_two_points) {
    LogoApp *app = new_app();
    run_source(app, "PRINT DISTANCE [0 0] [3 4]");
    CHECK_STREQ(captured_output, "5\n");
}

TEST(test_distance_reports_error_on_non_list_argument) {
    LogoApp *app = new_app();
    run_source(app, "PRINT DISTANCE 5 [1 2]");
    CHECK_STREQ(captured_output, "DISTANCE: expected two 2-element lists\n0\n");
}

TEST(test_towards_computes_compass_bearing_toward_a_point) {
    // Confirmed directly against the running interpreter, including the
    // negative-coordinate case ([0 -100]) that exercises the
    // parse_list_literal leading-sign fidelity fix below directly (a
    // list literal like [0 -100] must stay a 2-element list, not split
    // into 0/-/100).
    LogoApp *app = new_app();
    run_source(app, "HOME\nPRINT TOWARDS [0 100]\nPRINT TOWARDS [100 0]\nPRINT TOWARDS [0 -100]");
    CHECK_STREQ(captured_output, "300.964\n329.036\n324.462\n");
}

TEST(test_towards_reports_error_on_non_list_argument) {
    LogoApp *app = new_app();
    run_source(app, "PRINT TOWARDS 5");
    CHECK_STREQ(captured_output, "TOWARDS: expected a 2-element list\n0\n");
}

TEST(test_setheading_towards_then_forward_reaches_the_point) {
    // A real-shaped usage: SETHEADING TOWARDS point, then FORWARD
    // DISTANCE POS point, should walk straight to point.
    LogoApp *app = new_app();
    run_source(app, "HOME\nSETHEADING TOWARDS [0 100]\nFORWARD DISTANCE POS [0 100]\nPRINT POS");
    CHECK_STREQ(captured_output, "-1.52006e-13 100\n");
}

TEST(test_list_literal_keeps_a_glued_leading_sign_as_one_element) {
    // parse_list_literal fidelity fix: this lexer always splits a
    // leading -/+ off as its own token (needed elsewhere for
    // subtraction/negation), but interpreter.c's own list-literal scan
    // has no concept of tokens at all -- [0 -100] is genuinely the
    // 2-element list `0 -100`, confirmed directly against the running
    // interpreter (COUNT [0 -100] is 2, not 3). A sign with whitespace
    // on both sides (real subtraction, [0 - 100]) must NOT merge.
    LogoApp *app = new_app();
    run_source(app,
        "PRINT COUNT [0 -100]\nPRINT [0 -100]\nPRINT FIRST BUTFIRST [0 -100]\n"
        "PRINT COUNT [0 - 100]\nPRINT [0 - 100]\n"
        "PRINT COUNT [0 +5]\nPRINT [0 +5]");
    CHECK_STREQ(captured_output, "2\n0 -100\n-100\n3\n0 - 100\n2\n0 +5\n");
}

TEST(test_pick_from_a_list_word_array_and_bare_number) {
    LogoApp *app = new_app();
    run_source(app,
        "PRINT PICK [only]\n"
        "PRINT PICK \"a\n"
        "MAKE \"a ARRAY 1\nSETITEM 1 :a \"solo\nPRINT PICK :a\n"
        "PRINT PICK 42");
    CHECK_STREQ(captured_output, "only\na\nsolo\n42\n");
}

TEST(test_pick_reports_error_on_empty_list_or_word) {
    LogoApp *app = new_app();
    run_source(app, "PRINT PICK []\nPRINT PICK BUTFIRST \"a");
    CHECK_STREQ(captured_output, "PICK: empty list\n\nPICK: empty word\n\n");
}

TEST(test_flatten_collects_every_leaf_discarding_nesting) {
    LogoApp *app = new_app();
    run_source(app, "PRINT FLATTEN [1 [2 3] [4 [5 6]] 7]\nPRINT FLATTEN 5");
    CHECK_STREQ(captured_output, "1 2 3 4 5 6 7\n5\n");
}

TEST(test_parse_tokenizes_a_values_printed_text_by_whitespace) {
    // A list's rendered text is not bracket-aware when re-tokenized --
    // PARSE [a b [c d] e] yields 5 flat word tokens (a, b, "[c", "d]",
    // e), not a real nested list back out, matching interpreter.c's own
    // documented PARSE behavior exactly.
    LogoApp *app = new_app();
    run_source(app,
        "PRINT PARSE 'hello world'\nPRINT COUNT PARSE 'hello world'\n"
        "PRINT COUNT PARSE [a b [c d] e]\nPRINT PARSE [a b [c d] e]\n"
        "PRINT PARSE 42");
    CHECK_STREQ(captured_output, "hello world\n2\n5\na b [c d] e\n42\n");
}

TEST(test_subst_replaces_matching_elements_including_a_whole_sublist) {
    LogoApp *app = new_app();
    run_source(app,
        "PRINT SUBST \"b \"z [a b c b]\n"
        "PRINT SUBST [1 2] \"x [a [1 2] c]\n"
        "PRINT SUBST \"a \"z \"a\n"
        "PRINT SUBST \"a \"z \"b");
    CHECK_STREQ(captured_output, "a z c z\na x c\nz\nb\n");
}

TEST(test_dot_product_of_two_numeric_lists) {
    LogoApp *app = new_app();
    run_source(app, "PRINT DOT [1 2 3] [4 5 6]\nPRINT DOT [1 2] [1 2 3]");
    CHECK_STREQ(captured_output, "32\nDOT: lists must be the same length\n0\n");
}

TEST(test_cross_product_of_two_3element_lists) {
    LogoApp *app = new_app();
    run_source(app, "PRINT CROSS [1 0 0] [0 1 0]\nPRINT CROSS [1 2] [1 2 3]");
    CHECK_STREQ(captured_output, "0 0 1\nCROSS: expected two 3-element lists\n\n");
}

TEST(test_thing_reads_a_variable_by_computed_name) {
    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"x 5\nMAKE \"greeting \"hi\nMAKE \"nums [1 2 3]\n"
        "PRINT THING \"x\nPRINT THING WORD \"gree \"ting\nPRINT THING \"nums\nPRINT THING \"nosuch");
    CHECK_STREQ(captured_output, "5\nhi\n1 2 3\n0\n");
}

TEST(test_local_declares_a_call_scoped_variable) {
    LogoApp *app = new_app();
    run_source(app, "TO test\nLOCAL \"x\nMAKE \"x 99\nOUTPUT :x\nEND\nMAKE \"x 1\nPRINT test\nPRINT :x");
    CHECK_STREQ(captured_output, "99\n1\n");
}

TEST(test_local_outside_a_procedure_reports_an_error) {
    LogoApp *app = new_app();
    run_source(app, "LOCAL \"y");
    CHECK_STREQ(captured_output, "LOCAL: can only be used inside a procedure\n");
}

TEST(test_local_twice_with_the_same_name_is_a_no_op) {
    LogoApp *app = new_app();
    run_source(app, "TO test2\nLOCAL \"z\nLOCAL \"z\nMAKE \"z 7\nOUTPUT :z\nEND\nPRINT test2");
    CHECK_STREQ(captured_output, "7\n");
}

TEST(test_names_lists_every_global_variable) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"a 1\nMAKE \"b 2\nPRINT NAMES\nPRINT COUNT NAMES");
    CHECK_STREQ(captured_output, "a b\n2\n");
}

TEST(test_procedures_lists_every_defined_procedure) {
    LogoApp *app = new_app();
    run_source(app, "TO foo\nEND\nTO bar :x\nEND\nPRINT PROCEDURES\nPRINT COUNT PROCEDURES");
    CHECK_STREQ(captured_output, "foo bar\n2\n");
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

TEST(test_for_counts_up_by_default_step) {
    LogoApp *app = new_app();
    run_source(app, "FOR [i 1 3] [PRINT :i]");
    CHECK_STREQ(captured_output, "1\n2\n3\n");
}

TEST(test_for_limit_is_a_full_expression_not_just_a_literal) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"n 2\nFOR [i 1 :n + 1] [PRINT :i]");
    CHECK_STREQ(captured_output, "1\n2\n3\n");
}

TEST(test_for_with_explicit_step) {
    LogoApp *app = new_app();
    run_source(app, "FOR [i 0 10 5] [PRINT :i]");
    CHECK_STREQ(captured_output, "0\n5\n10\n");
}

TEST(test_for_counts_down_when_limit_is_less_than_start) {
    LogoApp *app = new_app();
    run_source(app, "FOR [i 3 1] [PRINT :i]");
    CHECK_STREQ(captured_output, "3\n2\n1\n");
}

TEST(test_for_step_zero_reports_an_error) {
    LogoApp *app = new_app();
    run_source(app, "FOR [i 1 3 0] [PRINT :i]");
    CHECK_STREQ(captured_output, "FOR: step must not be 0\n");
}

TEST(test_forever_runs_until_stop) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"i 0\nFOREVER [MAKE \"i :i + 1 PRINT :i IF :i = 3 [STOP]]");
    CHECK_STREQ(captured_output, "1\n2\n3\n");
}

TEST(test_run_executes_a_stored_list_as_source) {
    LogoApp *app = new_app();
    run_source(app, "RUN [PRINT 1 + 2]");
    CHECK_STREQ(captured_output, "3\n");
}

TEST(test_run_self_referential_is_capped_not_a_crash) {
    LogoApp *app = new_app();
    run_source(app, "MAKE \"x [RUN :x]\nRUN :x\nPRINT 1");
    CHECK_CONTAINS(captured_output, "RUN: too deeply nested, ignored");
    CHECK_CONTAINS(captured_output, "1");
}

TEST(test_apply_calls_a_procedure_with_a_list_of_arguments) {
    LogoApp *app = new_app();
    run_source(app,
        "TO add :a :b\n"
        "  PRINT :a + :b\n"
        "END\n"
        "APPLY \"add [3 4]");
    CHECK_STREQ(captured_output, "7\n");
}

TEST(test_apply_unknown_procedure_reports_an_error) {
    LogoApp *app = new_app();
    run_source(app, "APPLY \"nosuch [1 2]");
    CHECK_CONTAINS(captured_output, "APPLY: no such procedure \"nosuch");
}

TEST(test_apply_wrong_argument_count_reports_an_error) {
    LogoApp *app = new_app();
    run_source(app,
        "TO add :a :b\n"
        "  OUTPUT :a + :b\n"
        "END\n"
        "APPLY \"add [1 2 3]");
    CHECK_CONTAINS(captured_output, "APPLY: wrong number of inputs for procedure \"add");
}

TEST(test_catch_recovers_when_the_thrown_tag_matches) {
    LogoApp *app = new_app();
    run_source(app, "CATCH \"err [PRINT 1 THROW \"err PRINT 2]\nPRINT 3");
    CHECK_STREQ(captured_output, "1\n3\n");
}

TEST(test_throw_with_no_matching_catch_reports_and_recovers_at_top_level) {
    LogoApp *app = new_app();
    run_source(app, "CATCH \"other [THROW \"err]\nPRINT 99");
    CHECK_STREQ(captured_output, "THROW: no CATCH found for \"err\n99\n");
}

TEST(test_uncaught_throw_at_top_level_reports_and_later_statements_still_run) {
    LogoApp *app = new_app();
    run_source(app, "THROW \"nope\nPRINT 1");
    CHECK_STREQ(captured_output, "THROW: no CATCH found for \"nope\n1\n");
}

// --- General file I/O (OPENREAD/OPENWRITE/OPENAPPEND/CLOSE/FILEPRINT/
// READLINE/EOF?/DELETEFILE/DIRECTORY/LOAD) ---
// Real file I/O against build/, same convention as
// tests/test_interpreter.c's own equivalents (already gitignored
// scratch space, no dependency on /tmp being writable). Each test
// cleans up the file it wrote. Distinct filenames from
// test_interpreter.c's own (test_eval_* rather than test_*) even
// though the two binaries never run concurrently, just to make each
// suite's own scratch files unambiguous at a glance.

TEST(test_openwrite_fileprint_close_writes_the_file) {
    const char *path = "build/test_eval_openwrite.txt";
    remove(path);

    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"ch OPENWRITE \"build/test_eval_openwrite.txt\n"
        "FILEPRINT :ch \"hello\n"
        "FILEPRINT :ch \"world\n"
        "CLOSE :ch");

    char *contents = NULL;
    CHECK(g_file_get_contents(path, &contents, NULL, NULL));
    if (contents != NULL) {
        CHECK(strcmp(contents, "hello\nworld\n") == 0);
        g_free(contents);
    }
    remove(path);
}

TEST(test_openread_readline_reads_lines_then_eof) {
    const char *path = "build/test_eval_openread.txt";
    remove(path);
    g_file_set_contents(path, "line one\nline two\n", -1, NULL);

    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"ch OPENREAD \"build/test_eval_openread.txt\n"
        "PRINT READLINE :ch\n"
        "PRINT READLINE :ch\n"
        "PRINT EOF? :ch\n"
        "CLOSE :ch");
    CHECK_STREQ(captured_output, "line one\nline two\nTRUE\n");
    remove(path);
}

TEST(test_openappend_appends_to_existing_content) {
    const char *path = "build/test_eval_openappend.txt";
    remove(path);
    g_file_set_contents(path, "first\n", -1, NULL);

    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"ch OPENAPPEND \"build/test_eval_openappend.txt\n"
        "FILEPRINT :ch \"second\n"
        "CLOSE :ch");

    char *contents = NULL;
    CHECK(g_file_get_contents(path, &contents, NULL, NULL));
    if (contents != NULL) {
        CHECK(strcmp(contents, "first\nsecond\n") == 0);
        g_free(contents);
    }
    remove(path);
}

TEST(test_openread_of_missing_file_returns_negative_one) {
    LogoApp *app = new_app();
    run_source(app, "PRINT OPENREAD \"build/test_eval_does_not_exist.txt");
    CHECK_STREQ(captured_output, "-1\n");
}

TEST(test_close_of_invalid_channel_reports_error) {
    LogoApp *app = new_app();
    run_source(app, "CLOSE 3");
    CHECK_CONTAINS(captured_output, "CLOSE: no such open channel");
}

TEST(test_fileprint_on_a_read_channel_reports_error) {
    const char *path = "build/test_eval_fileprint_wrong_mode.txt";
    remove(path);
    g_file_set_contents(path, "data\n", -1, NULL);

    LogoApp *app = new_app();
    run_source(app,
        "MAKE \"ch OPENREAD \"build/test_eval_fileprint_wrong_mode.txt\n"
        "FILEPRINT :ch \"oops");
    CHECK_CONTAINS(captured_output, "FILEPRINT: channel not open for writing");
    remove(path);
}

TEST(test_readline_and_eof_on_a_closed_channel_are_safe_sentinels) {
    LogoApp *app = new_app();
    run_source(app, "PRINT READLINE 5\nPRINT EOF? 5");
    CHECK_STREQ(captured_output, "\nTRUE\n");
}

TEST(test_deletefile_removes_a_file) {
    const char *path = "build/test_eval_deletefile.txt";
    g_file_set_contents(path, "x", -1, NULL);

    LogoApp *app = new_app();
    run_source(app, "DELETEFILE \"build/test_eval_deletefile.txt");
    CHECK_STREQ(captured_output, "");
    CHECK(!g_file_test(path, G_FILE_TEST_EXISTS));
}

TEST(test_deletefile_of_missing_file_reports_error) {
    LogoApp *app = new_app();
    run_source(app, "DELETEFILE \"build/test_eval_does_not_exist_either.txt");
    CHECK_CONTAINS(captured_output, "DELETEFILE: could not delete");
}

TEST(test_deletefile_without_a_quoted_word_is_a_parse_error_not_runtime) {
    // Unlike interpreter.c's own DELETEFILE (a raw sscanf("%s") check
    // that fails at *runtime*, printing "DELETEFILE: expected a
    // \"path"), this engine's ARG_QUOTED_WORD requires a literal
    // LOGO_TOK_QUOTED_WORD token at *parse* time (see parser.c) -- the
    // same mechanism MAKE/LOCAL's own varname argument already uses,
    // not something new to this batch. A bareword argument here is a
    // parse error, so the script never runs at all (run_source itself
    // reports the parse failure rather than executing DELETEFILE's own
    // runtime error path).
    LogoApp *app = new_app();
    LogoToken tokens[MAX_TEST_TOKENS];
    int n = logo_lex("DELETEFILE path", tokens, MAX_TEST_TOKENS);
    CHECK(n >= 0);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    CHECK(result->error_count > 0);
    free(result);
    (void)app;
}

TEST(test_directory_lists_the_current_working_directory) {
    // Tests run from the repo root (see Makefile's test rule), so
    // Makefile itself is a stable, always-present entry -- no need to
    // create a marker file just to test DIRECTORY.
    LogoApp *app = new_app();
    run_source(app, "PRINT MEMBER? \"Makefile DIRECTORY");
    CHECK_STREQ(captured_output, "TRUE\n");
}

TEST(test_load_runs_a_files_contents_as_logo_source) {
    const char *path = "build/test_eval_load.logo";
    remove(path);
    g_file_set_contents(path,
        "TO greet :name\n"
        "  PRINT WORD \"hello- :name\n"
        "END\n"
        "greet \"world\n"
        "PRINT 1 + 1", -1, NULL);

    LogoApp *app = new_app();
    run_source(app, "PRINT \"before\nLOAD \"build/test_eval_load.logo\nPRINT \"after");
    CHECK_STREQ(captured_output, "before\nhello-world\n2\nafter\n");
    remove(path);
}

TEST(test_load_of_missing_file_reports_error) {
    LogoApp *app = new_app();
    run_source(app, "LOAD \"build/test_eval_does_not_exist_at_all.logo");
    CHECK_CONTAINS(captured_output, "LOAD: could not read file");
}

TEST(test_load_defined_procedure_is_not_callable_from_the_loading_script) {
    // A genuine, documented architectural limitation, not a bug to fix
    // in this batch -- see docs/BYTECODE_VM_DESIGN.md's LOAD/SAVE
    // milestone. interpreter.c parses and executes one statement at a
    // time (eval_logo's own cursor), so by the time LOAD's own eval_logo
    // call registers `greet` into app->procedures[], a later `greet
    // "world` in the SAME top-level script finds it immediately. This
    // engine parses the WHOLE top-level script once, up front, before
    // running anything -- so `greet` isn't a known hoisted procedure
    // yet when *this* script's own parse reaches its call, regardless
    // of what LOAD will later do at runtime. The result is a parse
    // error (not a runtime "I don't know how to greet"), and per this
    // engine's own error-collection design, the whole script simply
    // doesn't run at all -- confirmed directly against interpreter.c,
    // which runs this exact script successfully, before writing this
    // test the other way around.
    const char *path = "build/test_eval_load_uncallable.logo";
    remove(path);
    g_file_set_contents(path, "TO greet :name\n  PRINT WORD \"hello- :name\nEND", -1, NULL);

    LogoApp *app = new_app();
    LogoToken tokens[MAX_TEST_TOKENS];
    int n = logo_lex("LOAD \"build/test_eval_load_uncallable.logo\nPRINT greet \"world", tokens, MAX_TEST_TOKENS);
    CHECK(n >= 0);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    CHECK(result->error_count > 0);
    free(result);
    (void)app;
    remove(path);
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
    RUN(test_who_reports_the_current_turtle_starting_at_zero);
    RUN(test_tell_switches_the_current_turtle_and_creates_it);
    RUN(test_tell_leaves_the_other_turtles_state_untouched);
    RUN(test_tell_out_of_range_reports_error_and_leaves_current_turtle);
    RUN(test_a_script_with_no_tell_behaves_as_a_single_turtle);

    RUN(test_arc_draws_segments_without_moving_the_turtle);
    RUN(test_label_records_position_color_and_text);
    RUN(test_fill_records_a_raster_op_with_the_current_pen_color);
    RUN(test_eraserect_records_a_raster_op_centered_on_the_turtle);
    RUN(test_wrap_mode_wraps_at_the_canvas_edge);
    RUN(test_fence_mode_stops_at_the_canvas_edge_and_reports_error);
    RUN(test_window_mode_is_the_default_and_ignores_the_edge);
    RUN(test_clean_erases_drawing_but_leaves_turtle_position_alone);
    RUN(test_hideturtle_sets_visible_false_and_showturtle_restores_it);
    RUN(test_setpencolor_sets_and_clamps_the_pen_color);
    RUN(test_setpenwidth_clamps_to_the_valid_range);
    RUN(test_setbackground_sets_the_canvas_background_color);
    RUN(test_setcanvassize_resizes_and_resets_turtle_and_drawing);
    RUN(test_setcanvassize_out_of_range_reports_error_and_leaves_canvas_unchanged);
    RUN(test_erase_deletes_a_procedure_so_it_can_no_longer_be_called);
    RUN(test_erase_of_unknown_procedure_reports_error);
    RUN(test_erase_removes_the_procedure_from_procedures_output);
    RUN(test_procedure_with_output);
    RUN(test_procedure_forward_reference);
    RUN(test_recursive_procedure);
    RUN(test_recursion_depth_cap_reports_error_not_a_crash);
    RUN(test_math_operators);
    RUN(test_trig_and_log_operators);
    RUN(test_mod_operator);
    RUN(test_turtle_state_queries);
    RUN(test_canvassize_reports_width_and_height);
    RUN(test_setx_sety_move_one_axis_only);
    RUN(test_distance_between_two_points);
    RUN(test_distance_reports_error_on_non_list_argument);
    RUN(test_towards_computes_compass_bearing_toward_a_point);
    RUN(test_towards_reports_error_on_non_list_argument);
    RUN(test_setheading_towards_then_forward_reaches_the_point);
    RUN(test_list_literal_keeps_a_glued_leading_sign_as_one_element);
    RUN(test_pick_from_a_list_word_array_and_bare_number);
    RUN(test_pick_reports_error_on_empty_list_or_word);
    RUN(test_flatten_collects_every_leaf_discarding_nesting);
    RUN(test_parse_tokenizes_a_values_printed_text_by_whitespace);
    RUN(test_subst_replaces_matching_elements_including_a_whole_sublist);
    RUN(test_dot_product_of_two_numeric_lists);
    RUN(test_cross_product_of_two_3element_lists);
    RUN(test_thing_reads_a_variable_by_computed_name);
    RUN(test_local_declares_a_call_scoped_variable);
    RUN(test_local_outside_a_procedure_reports_an_error);
    RUN(test_local_twice_with_the_same_name_is_a_no_op);
    RUN(test_names_lists_every_global_variable);
    RUN(test_procedures_lists_every_defined_procedure);
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

    RUN(test_for_counts_up_by_default_step);
    RUN(test_for_limit_is_a_full_expression_not_just_a_literal);
    RUN(test_for_with_explicit_step);
    RUN(test_for_counts_down_when_limit_is_less_than_start);
    RUN(test_for_step_zero_reports_an_error);
    RUN(test_forever_runs_until_stop);
    RUN(test_run_executes_a_stored_list_as_source);
    RUN(test_run_self_referential_is_capped_not_a_crash);
    RUN(test_apply_calls_a_procedure_with_a_list_of_arguments);
    RUN(test_apply_unknown_procedure_reports_an_error);
    RUN(test_apply_wrong_argument_count_reports_an_error);
    RUN(test_catch_recovers_when_the_thrown_tag_matches);
    RUN(test_throw_with_no_matching_catch_reports_and_recovers_at_top_level);
    RUN(test_uncaught_throw_at_top_level_reports_and_later_statements_still_run);

    RUN(test_openwrite_fileprint_close_writes_the_file);
    RUN(test_openread_readline_reads_lines_then_eof);
    RUN(test_openappend_appends_to_existing_content);
    RUN(test_openread_of_missing_file_returns_negative_one);
    RUN(test_close_of_invalid_channel_reports_error);
    RUN(test_fileprint_on_a_read_channel_reports_error);
    RUN(test_readline_and_eof_on_a_closed_channel_are_safe_sentinels);
    RUN(test_deletefile_removes_a_file);
    RUN(test_deletefile_of_missing_file_reports_error);
    RUN(test_deletefile_without_a_quoted_word_is_a_parse_error_not_runtime);
    RUN(test_directory_lists_the_current_working_directory);
    RUN(test_load_runs_a_files_contents_as_logo_source);
    RUN(test_load_of_missing_file_reports_error);
    RUN(test_load_defined_procedure_is_not_callable_from_the_loading_script);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
