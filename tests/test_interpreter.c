// test_interpreter.c
//
// Headless tests for the interpreter core (eval_logo, the expression
// parser, procedures/scoping, conditionals, words, error messages) with
// no GTK widgets involved: LogoApp.output_sink is swapped for a plain
// buffer instead of ui.c's real history-pane sink, so these run in
// milliseconds from the command line. Run via `make test`.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
// (turtle 0 at the canvas center, pen down, default color/width, white
// background) but with no GTK widgets — those fields stay NULL/zero and
// are simply never touched by the interpreter core. Heap-allocated, not
// a local/return-by-value struct: LogoApp is several MB (dominated by
// the list-node pool added for nested lists), comfortably fine on the
// heap — same as the real app's g_new0 in ui.c — but too large to
// safely put on the stack even once, let alone the handful of tests
// that need more than one interpreter alive at the same time. Never
// explicitly freed: this is a short-lived, one-shot test binary, and
// the OS reclaims everything at exit.
static LogoApp* new_app(void) {
    LogoApp *app = calloc(1, sizeof(LogoApp));
    init_turtle(&app->turtles[0]);
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

// --- Comments (';') ---

TEST(test_comment_on_its_own_line_is_ignored) {
    LogoApp *app = new_app();
    eval_logo(app, "; a comment\nPRINT \"hi\n; another comment\nPRINT \"bye");
    CHECK_STREQ(captured_output, "hi\nbye\n");
}

TEST(test_trailing_comment_after_a_command_is_ignored) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT \"hi ; trailing comment\nPRINT \"bye");
    CHECK_STREQ(captured_output, "hi\nbye\n");
}

TEST(test_comment_inside_a_block_is_ignored) {
    LogoApp *app = new_app();
    eval_logo(app, "REPEAT 2 [FD 10 ; move forward\nRT 90]");
    CHECK(app->line_count == 2);
}

TEST(test_semicolon_glued_to_a_word_is_not_a_comment) {
    // Comments are only recognized at a token boundary (wherever
    // whitespace already is) -- a ';' with no preceding space is just an
    // ordinary word character.
    LogoApp *app = new_app();
    eval_logo(app, "PRINT WORD \"hi \";there");
    CHECK_STREQ(captured_output, "hi;there\n");
}

// --- Turtle motion ---

TEST(test_forward_moves_and_draws) {
    LogoApp *app = new_app();
    eval_logo(app, "FD 100");
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 150.0); // angle 0 = up (north): y decreases
    CHECK(app->line_count == 1);
    CHECK_NEAR(app->lines[0].x1, 250.0);
    CHECK_NEAR(app->lines[0].y1, 250.0);
    CHECK_NEAR(app->lines[0].x2, 250.0);
    CHECK_NEAR(app->lines[0].y2, 150.0);
}

TEST(test_penup_moves_without_drawing) {
    LogoApp *app = new_app();
    eval_logo(app, "PENUP FD 100");
    CHECK(app->line_count == 0);
    CHECK_NEAR(app->turtles[0].y, 150.0);
}

TEST(test_repeat_square_returns_to_start) {
    LogoApp *app = new_app();
    eval_logo(app, "REPEAT 4 [FD 100 RT 90]");
    CHECK(app->line_count == 4);
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 250.0);
}

TEST(test_setxy_draws_direct_line) {
    LogoApp *app = new_app();
    eval_logo(app, "SETXY 400 300");
    CHECK(app->line_count == 1);
    CHECK_NEAR(app->turtles[0].x, 400.0);
    CHECK_NEAR(app->turtles[0].y, 300.0);
}

TEST(test_home_returns_and_resets_heading) {
    LogoApp *app = new_app();
    eval_logo(app, "SETXY 400 300 SETHEADING 45 HOME");
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 250.0);
    CHECK_NEAR(app->turtles[0].angle, 0.0);
    CHECK(app->line_count == 2); // the SETXY jump, then HOME's jump back
}

TEST(test_arc_draws_without_moving_the_turtle) {
    LogoApp *app = new_app();
    eval_logo(app, "ARC 90 100");
    // The turtle stays exactly where it was, heading unchanged.
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 250.0);
    CHECK_NEAR(app->turtles[0].angle, 0.0);
    // A 90-degree arc at 5 degrees/segment is 18 segments.
    CHECK(app->line_count == 18);
    // The arc starts exactly where FD 100 would have landed: radius 100
    // north of center, since heading 0 points up (north).
    CHECK_NEAR(app->lines[0].x1, 250.0);
    CHECK_NEAR(app->lines[0].y1, 150.0);
}

TEST(test_setpencolor_and_width_stamp_the_segment) {
    LogoApp *app = new_app();
    eval_logo(app, "SETPENCOLOR 255 0 0 SETPENWIDTH 5 FD 50");
    CHECK(app->line_count == 1);
    CHECK_NEAR(app->lines[0].r, 1.0);
    CHECK_NEAR(app->lines[0].g, 0.0);
    CHECK_NEAR(app->lines[0].b, 0.0);
    CHECK_NEAR(app->lines[0].width, 5.0);
}

TEST(test_setbackground_sets_canvas_color) {
    LogoApp *app = new_app();
    eval_logo(app, "SETBACKGROUND 10 20 30");
    CHECK_NEAR(app->bg_r, 10.0 / 255.0);
    CHECK_NEAR(app->bg_g, 20.0 / 255.0);
    CHECK_NEAR(app->bg_b, 30.0 / 255.0);
}

// --- LABEL ---
// Actual text rendering is Cairo, tested manually in the running app;
// these check the data LABEL records for ui.c's draw_scene to draw.

TEST(test_label_records_position_color_and_text) {
    LogoApp *app = new_app();
    eval_logo(app, "SETPENCOLOR 255 0 0\nFD 100\nLABEL \"hi");
    CHECK(app->label_count == 1);
    CHECK_NEAR(app->labels[0].x, 250.0);
    CHECK_NEAR(app->labels[0].y, 150.0);
    CHECK_NEAR(app->labels[0].r, 1.0);
    CHECK_NEAR(app->labels[0].g, 0.0);
    CHECK_NEAR(app->labels[0].b, 0.0);
    CHECK_STREQ(app->labels[0].text, "hi");
}

TEST(test_label_accepts_a_list_argument) {
    LogoApp *app = new_app();
    eval_logo(app, "LABEL [hello there]");
    CHECK_STREQ(app->labels[0].text, "hello there");
}

TEST(test_clear_erases_labels) {
    LogoApp *app = new_app();
    eval_logo(app, "LABEL \"hi\nCLEAR");
    CHECK(app->label_count == 0);
}

// --- FILL ---
// The actual flood-fill rasterizing is Cairo, in ui.c, tested manually
// in the running app; these check the data FILL records for it.

TEST(test_fill_records_position_and_color) {
    LogoApp *app = new_app();
    eval_logo(app, "SETPENCOLOR 0 200 0\nFD 50\nFILL");
    CHECK(app->fill_count == 1);
    CHECK_NEAR(app->fills[0].x, 250.0);
    CHECK_NEAR(app->fills[0].y, 200.0);
    CHECK_NEAR(app->fills[0].r, 0.0);
    CHECK_NEAR(app->fills[0].g, 200.0 / 255.0);
    CHECK_NEAR(app->fills[0].b, 0.0);
}

TEST(test_clear_erases_fills) {
    LogoApp *app = new_app();
    eval_logo(app, "FILL\nCLEAR");
    CHECK(app->fill_count == 0);
}

// --- WAIT ---

TEST(test_wait_zero_or_negative_returns_immediately) {
    LogoApp *app = new_app();
    eval_logo(app, "WAIT 0\nWAIT -1\nPRINT \"done");
    CHECK_STREQ(captured_output, "done\n");
}

TEST(test_wait_pauses_for_at_least_the_requested_duration) {
    LogoApp *app = new_app();
    gint64 start = g_get_monotonic_time();
    eval_logo(app, "WAIT 0.02"); // kept short so the suite stays fast
    gint64 elapsed = g_get_monotonic_time() - start;
    CHECK(elapsed >= 20000); // microseconds; never faster than requested
}

// --- POS/HEADING (turtle state queries) ---

TEST(test_pos_reads_back_turtle_position) {
    LogoApp *app = new_app();
    eval_logo(app, "FD 100\nPRINT POS");
    CHECK_STREQ(captured_output, "250 150\n");
}

TEST(test_heading_reads_back_turtle_heading_without_wrapping) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT HEADING\nRT 90\nPRINT HEADING\nRT 90 RT 90 RT 90\nPRINT HEADING");
    CHECK_STREQ(captured_output, "0\n90\n360\n"); // no 0-360 normalization, same as RT/LT/SETHEADING
}

TEST(test_heading_can_be_saved_and_restored) {
    LogoApp *app = new_app();
    eval_logo(app, "RT 45\nMAKE \"h HEADING\nSETHEADING 200\nSETHEADING :h\nPRINT HEADING");
    CHECK_STREQ(captured_output, "45\n");
}

TEST(test_pos_and_heading_follow_the_current_turtle) {
    LogoApp *app = new_app();
    eval_logo(app, "TELL 1\nFD 50 RT 90\nPRINT POS\nPRINT HEADING\nTELL 0\nPRINT POS\nPRINT HEADING");
    CHECK_STREQ(captured_output, "250 200\n90\n250 250\n0\n");
}

// --- Multiple turtles ---

TEST(test_tell_creates_and_switches_turtles) {
    LogoApp *app = new_app();
    eval_logo(app, "TELL 1 FD 100");
    CHECK(app->turtle_count == 2); // turtle 0 (default) + turtle 1
    CHECK(app->current_turtle == 1);
    // Turtle 1 moved; turtle 0 stayed put at the default home position.
    CHECK_NEAR(app->turtles[1].y, 150.0);
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 250.0);
}

TEST(test_turtles_move_independently) {
    LogoApp *app = new_app();
    eval_logo(app, "FD 50\nTELL 1\nRT 90\nFD 80\nTELL 0\nFD 20");
    // Turtle 0: two forward moves along its own heading (never turned).
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 250.0 - 50.0 - 20.0);
    // Turtle 1: started fresh at home, turned, then moved.
    CHECK_NEAR(app->turtles[1].x, 250.0 + 80.0);
    CHECK_NEAR(app->turtles[1].y, 250.0);
}

TEST(test_clear_homes_every_turtle) {
    LogoApp *app = new_app();
    eval_logo(app, "TELL 1 FD 100 TELL 0 FD 50 CLEAR");
    CHECK(app->line_count == 0);
    for (int i = 0; i < app->turtle_count; i++) {
        CHECK_NEAR(app->turtles[i].x, 250.0);
        CHECK_NEAR(app->turtles[i].y, 250.0);
        CHECK_NEAR(app->turtles[i].angle, 0.0);
    }
}

TEST(test_tell_out_of_range_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "TELL 99");
    CHECK_CONTAINS(captured_output, "TELL: turtle index must be");
    CHECK(app->current_turtle == 0); // unchanged
}

// --- Visibility & edge modes (HIDETURTLE/SHOWTURTLE, WRAP/FENCE/WINDOW) ---

TEST(test_hideturtle_showturtle_toggles_visibility) {
    LogoApp *app = new_app();
    CHECK(app->turtles[0].visible == 1); // visible by default
    eval_logo(app, "HIDETURTLE");
    CHECK(app->turtles[0].visible == 0);
    eval_logo(app, "SHOWTURTLE");
    CHECK(app->turtles[0].visible == 1);
}

TEST(test_window_mode_is_default_and_allows_offscreen) {
    LogoApp *app = new_app();
    eval_logo(app, "SETXY 600 250"); // no edge mode set -- unchanged pre-existing behavior
    CHECK_NEAR(app->turtles[0].x, 600.0);
    CHECK_NEAR(app->turtles[0].y, 250.0);
    CHECK(app->line_count == 1);
}

TEST(test_wrap_wraps_position_and_lifts_pen) {
    LogoApp *app = new_app();
    eval_logo(app, "WRAP SETXY 600 250");
    CHECK_NEAR(app->turtles[0].x, 100.0); // 600 mod 500
    CHECK_NEAR(app->turtles[0].y, 250.0);
    CHECK(app->line_count == 0); // pen lifts across the wrap, no line recorded
}

TEST(test_fence_clamps_position_and_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "FENCE SETXY 600 250");
    CHECK_NEAR(app->turtles[0].x, 500.0); // clamped to the canvas edge
    CHECK_NEAR(app->turtles[0].y, 250.0);
    CHECK(app->line_count == 1); // drawn up to the clamped edge
    CHECK_CONTAINS(captured_output, "FENCE: turtle stopped at the canvas edge");
}

TEST(test_window_command_resets_from_wrap_or_fence) {
    LogoApp *app = new_app();
    eval_logo(app, "FENCE WINDOW SETXY 600 250");
    CHECK_NEAR(app->turtles[0].x, 600.0); // WINDOW turned the fence back off
}

// --- Procedures & scoping ---

TEST(test_procedure_definition_and_call) {
    LogoApp *app = new_app();
    eval_logo(app, "TO square :size\nREPEAT 4 [FD :size RT 90]\nEND\nsquare 60");
    CHECK(app->line_count == 4);
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 250.0);
}

TEST(test_recursion_with_parameter) {
    LogoApp *app = new_app();
    eval_logo(app, "TO countdown :n\nIF :n > 0 [PRINT :n countdown :n - 1]\nEND\ncountdown 3");
    CHECK_STREQ(captured_output, "3\n2\n1\n");
}

TEST(test_parameter_shadows_outer_global) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"size 999\n"
        "TO square :size\nREPEAT 4 [FD :size RT 90]\nEND\n"
        "square 50\n"
        "PRINT :size");
    CHECK_STREQ(captured_output, "999\n"); // global untouched by the local parameter
}

TEST(test_make_without_local_mutates_outer_global) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"counter 0\n"
        "TO increment\nMAKE \"counter :counter + 1\nEND\n"
        "increment\nincrement\nincrement\n"
        "PRINT :counter");
    CHECK_STREQ(captured_output, "3\n"); // dynamic scoping: no local "counter" to shadow it
}

TEST(test_to_redefinition_overwrites) {
    LogoApp *app = new_app();
    eval_logo(app, "TO t\nPRINT \"one\nEND\nTO t\nPRINT \"two\nEND\nt");
    CHECK_STREQ(captured_output, "two\n");
}

TEST(test_erase_removes_procedure) {
    LogoApp *app = new_app();
    eval_logo(app, "TO t\nPRINT \"hi\nEND\nERASE \"t\nt");
    CHECK_CONTAINS(captured_output, "I don't know how to");
}

// --- OUTPUT, STOP ---

TEST(test_output_returns_a_value_used_in_an_expression) {
    LogoApp *app = new_app();
    eval_logo(app, "TO double :n\nOUTPUT :n * 2\nEND\nPRINT double 5");
    CHECK_STREQ(captured_output, "10\n");
}

TEST(test_output_stops_the_rest_of_the_procedure) {
    LogoApp *app = new_app();
    eval_logo(app, "TO test2\nOUTPUT 1\nPRINT \"unreachable\nEND\nPRINT test2");
    CHECK_STREQ(captured_output, "1\n");
}

TEST(test_output_escapes_nested_blocks) {
    LogoApp *app = new_app();
    eval_logo(app, "TO test3\nIF 1 = 1 [OUTPUT 42]\nPRINT \"unreachable\nEND\nPRINT test3");
    CHECK_STREQ(captured_output, "42\n");
}

TEST(test_stop_ends_a_procedure_early_and_caller_continues) {
    LogoApp *app = new_app();
    eval_logo(app,
        "TO test4\nPRINT \"before\nSTOP\nPRINT \"unreachable\nEND\n"
        "test4\n"
        "PRINT \"after");
    CHECK_STREQ(captured_output, "before\nafter\n");
}

TEST(test_output_used_as_a_plain_statement_discards_the_value) {
    LogoApp *app = new_app();
    eval_logo(app, "TO double :n\nOUTPUT :n * 2\nEND\ndouble 5\nPRINT \"done");
    CHECK_STREQ(captured_output, "done\n"); // called bare (not as an operator) -- no error, no stray output
}

TEST(test_calling_a_procedure_that_never_outputs_as_a_value_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "TO novalue\nPRINT \"hi\nEND\nPRINT novalue");
    CHECK_CONTAINS(captured_output, "hi");
    CHECK_CONTAINS(captured_output, "novalue: didn't output a value");
}

TEST(test_output_outside_procedure_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "OUTPUT 5");
    CHECK_CONTAINS(captured_output, "OUTPUT: can only be used inside a procedure");
}

TEST(test_stop_outside_procedure_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "STOP");
    CHECK_CONTAINS(captured_output, "STOP: can only be used inside a procedure");
}

TEST(test_recursive_procedure_using_output) {
    LogoApp *app = new_app();
    eval_logo(app, "TO fact :n\nIF :n = 0 [OUTPUT 1]\nOUTPUT :n * fact :n - 1\nEND\nPRINT fact 5");
    CHECK_STREQ(captured_output, "120\n");
}

TEST(test_output_value_can_be_a_word_or_list) {
    LogoApp *app = new_app();
    eval_logo(app,
        "TO greet\nOUTPUT \"hello\nEND\n"
        "TO mklist\nOUTPUT [1 2 3]\nEND\n"
        "PRINT greet\n"
        "PRINT mklist");
    CHECK_STREQ(captured_output, "hello\n1 2 3\n");
}

TEST(test_local_scopes_a_variable_without_leaking_to_global) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"x 999\n"
        "TO uselocal\nLOCAL \"x\nMAKE \"x 1\nPRINT :x\nEND\n"
        "uselocal\n"
        "PRINT :x");
    CHECK_STREQ(captured_output, "1\n999\n"); // the outer global is untouched
}

TEST(test_local_initializes_to_zero) {
    LogoApp *app = new_app();
    eval_logo(app, "TO t\nLOCAL \"y\nPRINT :y\nEND\nt");
    CHECK_STREQ(captured_output, "0\n");
}

TEST(test_local_outside_procedure_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "LOCAL \"x");
    CHECK_CONTAINS(captured_output, "LOCAL: can only be used inside a procedure");
}

TEST(test_local_without_quote_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "LOCAL x");
    CHECK_CONTAINS(captured_output, "LOCAL: expected a");
}

// --- RUN, APPLY ---

TEST(test_run_executes_a_list_as_code) {
    LogoApp *app = new_app();
    eval_logo(app, "RUN [FD 100 RT 90]");
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 150.0);
    CHECK_NEAR(app->turtles[0].angle, 90.0);
}

TEST(test_run_executes_a_stored_variable) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"prog [FD 50 RT 90]\nRUN :prog");
    CHECK_NEAR(app->turtles[0].y, 200.0);
    CHECK_NEAR(app->turtles[0].angle, 90.0);
}

TEST(test_run_handles_nested_blocks) {
    // A nested list renders back out through value_to_text with its own
    // brackets (see PRINT of nested lists) -- which happens to mean it's
    // valid Logo source again, so REPEAT's own [block] round-trips.
    LogoApp *app = new_app();
    eval_logo(app, "RUN [REPEAT 4 [FD 10 RT 90]]");
    CHECK(app->line_count == 4);
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 250.0);
}

TEST(test_run_self_reference_is_capped) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"x [RUN :x]\nRUN :x");
    CHECK_CONTAINS(captured_output, "RUN: too deeply nested, ignored");
}

TEST(test_apply_calls_procedure_with_list_args) {
    LogoApp *app = new_app();
    eval_logo(app, "TO add2 :a :b\nPRINT :a + :b\nEND\nAPPLY \"add2 [3 4]");
    CHECK_STREQ(captured_output, "7\n");
}

TEST(test_apply_wrong_arg_count_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "TO add2 :a :b\nPRINT :a + :b\nEND\nAPPLY \"add2 [3]");
    CHECK_CONTAINS(captured_output, "APPLY: wrong number of inputs");
}

TEST(test_apply_unknown_procedure_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "APPLY \"nosuch [1]");
    CHECK_CONTAINS(captured_output, "APPLY: no such procedure");
}

// --- MAP, FOREACH, FILTER, REDUCE ---

TEST(test_map_transforms_each_element) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT MAP [? * 2] [1 2 3]");
    CHECK_STREQ(captured_output, "2 4 6\n");
}

TEST(test_map_on_a_bare_number) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT MAP [? + 1] 5");
    CHECK_STREQ(captured_output, "6\n");
}

TEST(test_map_preserves_nested_list_elements) {
    // A list-typed element must round-trip through the template with its
    // brackets intact, or its tokens would spill into the surrounding
    // expression instead of staying one value.
    LogoApp *app = new_app();
    eval_logo(app, "PRINT MAP [COUNT ?] [[1 2] [3 4 5]]");
    CHECK_STREQ(captured_output, "2 3\n");
}

TEST(test_filter_keeps_matching_elements) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT FILTER [? > 2] [1 2 3 4]");
    CHECK_STREQ(captured_output, "3 4\n");
}

TEST(test_reduce_folds_left_to_right) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT REDUCE [?1 + ?2] [1 2 3 4]");
    CHECK_STREQ(captured_output, "10\n");
}

TEST(test_reduce_of_single_element_list_is_that_element) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT REDUCE [?1 + ?2] [5]");
    CHECK_STREQ(captured_output, "5\n");
}

TEST(test_foreach_runs_template_per_element) {
    LogoApp *app = new_app();
    eval_logo(app, "FOREACH [PRINT ?] [1 2 3]");
    CHECK_STREQ(captured_output, "1\n2\n3\n");
}

// --- Conditionals & booleans ---

TEST(test_if_ifelse_comparisons) {
    LogoApp *app = new_app();
    eval_logo(app, "IF 5 > 3 [PRINT \"yes]");
    CHECK_STREQ(captured_output, "yes\n");
}

TEST(test_boolean_and_or_not) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"x 5\n"
        "IF :x > 0 AND :x < 10 [PRINT \"in-range]\n"
        "IF :x < 0 OR :x > 100 [PRINT \"out]\n"
        "IF NOT :x = 0 [PRINT \"nonzero]");
    CHECK_STREQ(captured_output, "in-range\nnonzero\n");
}

TEST(test_while_loop) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"i 0\nWHILE :i < 3 [PRINT :i MAKE \"i :i + 1]");
    CHECK_STREQ(captured_output, "0\n1\n2\n");
}

// --- Words ---

TEST(test_word_variable_and_comparison) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"name \"World\nPRINT :name\nIF :name = \"World [PRINT \"matched]");
    CHECK_STREQ(captured_output, "World\nmatched\n");
}

TEST(test_make_multiword_string_from_bracket_list) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"greeting [hello   there    friend]\nPRINT :greeting");
    CHECK_STREQ(captured_output, "hello there friend\n");
}

TEST(test_multiword_string_comparison) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"greeting [hello there]\n"
        "IF :greeting = [hello there] [PRINT \"matched]\n"
        "IF :greeting = [goodbye] [PRINT \"should-not-print]");
    CHECK_STREQ(captured_output, "matched\n");
}

TEST(test_print_list_joins_words_with_single_spaces) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT [hello   there    friend]");
    CHECK_STREQ(captured_output, "hello there friend\n");
}

// --- List operators (FIRST, BUTFIRST, LAST, COUNT) ---

TEST(test_first_last_butfirst_count_via_print) {
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT FIRST [1 2 3]\n"
        "PRINT LAST [1 2 3]\n"
        "PRINT BUTFIRST [1 2 3]\n"
        "PRINT COUNT [1 2 3]");
    CHECK_STREQ(captured_output, "1\n3\n2 3\n3\n");
}

TEST(test_list_operators_via_make) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"mylist [red green blue]\n"
        "MAKE \"head FIRST :mylist\n"
        "MAKE \"tail BUTFIRST :mylist\n"
        "MAKE \"n COUNT :mylist\n"
        "PRINT :head\n"
        "PRINT :tail\n"
        "PRINT :n");
    CHECK_STREQ(captured_output, "red\ngreen blue\n3\n");
}

TEST(test_nested_list_operators) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT FIRST BUTFIRST [1 2 3]");
    CHECK_STREQ(captured_output, "2\n");
}

TEST(test_count_of_bare_number_is_one) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT COUNT 5");
    CHECK_STREQ(captured_output, "1\n");
}

TEST(test_first_of_empty_list_is_empty) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT FIRST []\nPRINT COUNT []");
    CHECK_STREQ(captured_output, "\n0\n");
}

TEST(test_list_operator_in_comparison) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"mylist [1 2 3]\n"
        "IF FIRST :mylist = 1 [PRINT \"matched]\n"
        "IF COUNT :mylist = 3 [PRINT \"also-matched]");
    CHECK_STREQ(captured_output, "matched\nalso-matched\n");
}

TEST(test_make_a_colon_b_copies_word) {
    // The "MAKE "a :b of a word" limitation is now fixed as part of
    // adding list operators (both went through the same MAKE rewrite).
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a [hello world]\nMAKE \"b :a\nPRINT :b");
    CHECK_STREQ(captured_output, "hello world\n");
}

// --- Substrings (FIRST/LAST/BUTFIRST/COUNT on words) ---

TEST(test_first_last_butfirst_count_on_a_word) {
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT FIRST \"hello\n"
        "PRINT LAST \"hello\n"
        "PRINT BUTFIRST \"hello\n"
        "PRINT COUNT \"hello");
    CHECK_STREQ(captured_output, "h\no\nello\n5\n");
}

TEST(test_substring_operators_on_empty_word) {
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT FIRST \"\n"
        "PRINT LAST \"\n"
        "PRINT BUTFIRST \"\n"
        "PRINT COUNT \"");
    CHECK_STREQ(captured_output, "\n\n\n0\n");
}

TEST(test_numbers_stay_atomic_for_substring_operators) {
    // A NUMBER doesn't decompose into digits the way a WORD does into
    // characters -- COUNT 12345 stays 1, same as before nested lists.
    LogoApp *app = new_app();
    eval_logo(app, "PRINT COUNT 12345\nPRINT FIRST 12345");
    CHECK_STREQ(captured_output, "1\n12345\n");
}

TEST(test_substring_of_word_variable) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"greeting \"hello\nPRINT FIRST :greeting\nPRINT COUNT :greeting");
    CHECK_STREQ(captured_output, "h\n5\n");
}

TEST(test_substring_operators_can_nest) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT BUTFIRST BUTFIRST \"hello");
    CHECK_STREQ(captured_output, "llo\n");
}

TEST(test_fput_lput_on_word_builds_new_word) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT FPUT \"a \"bc\nPRINT LPUT \"d \"bc");
    CHECK_STREQ(captured_output, "abc\nbcd\n");
}

TEST(test_fput_lput_list_onto_word_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT FPUT [1 2] \"bc");
    CHECK_CONTAINS(captured_output, "FPUT: can't add a list to a word");
    captured_output[0] = '\0';
    eval_logo(app, "PRINT LPUT [1 2] \"bc");
    CHECK_CONTAINS(captured_output, "LPUT: can't add a list to a word");
}

// --- List construction (WORD, SENTENCE/SE/LIST, FPUT, LPUT) ---

TEST(test_word_concatenates_with_no_space) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT WORD \"hello \"world");
    CHECK_STREQ(captured_output, "helloworld\n");
}

TEST(test_sentence_joins_with_a_space) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT SENTENCE \"a \"b\nPRINT SE [1 2] [3 4]\nPRINT LIST \"x \"y");
    CHECK_STREQ(captured_output, "a b\n1 2 3 4\nx y\n");
}

TEST(test_fput_prepends_and_lput_appends) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"colors [green blue]\n"
        "PRINT FPUT \"red :colors\n"
        "PRINT LPUT \"purple :colors");
    CHECK_STREQ(captured_output, "red green blue\ngreen blue purple\n");
}

TEST(test_list_construction_via_make_and_nesting) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"greeting WORD \"hello \"world\n"
        "PRINT :greeting\n"
        "PRINT FIRST FPUT \"a [b c]");
    CHECK_STREQ(captured_output, "helloworld\na\n");
}

// --- True nested lists ---

TEST(test_print_nested_list_literal) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT [a [b c] d]");
    CHECK_STREQ(captured_output, "a [b c] d\n");
}

TEST(test_count_of_nested_list_counts_top_level_only) {
    // A regression test for a real bug in the pre-nested-lists code: the
    // old flatten-by-whitespace tokenizer silently miscounted this as 4
    // ("a", "[b", "c]", "d") instead of the correct 3 top-level elements.
    LogoApp *app = new_app();
    eval_logo(app, "PRINT COUNT [a [b c] d]");
    CHECK_STREQ(captured_output, "3\n");
}

TEST(test_first_and_last_can_return_a_sublist) {
    // A sublist FIRST/LAST returns prints unbracketed if it's PRINT's own
    // top-level value (same "no brackets at the top level" rule as any
    // other list) -- but bracketed once it's nested as an element inside
    // something larger, here via LIST.
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT FIRST [[1 2] 3]\n"
        "PRINT LAST [1 2 [3 4]]\n"
        "PRINT LIST FIRST [[1 2] 3] \"x");
    CHECK_STREQ(captured_output, "1 2\n3 4\n[1 2] x\n");
}

TEST(test_butfirst_preserves_a_sublist_element) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT BUTFIRST [a [b c] d]");
    CHECK_STREQ(captured_output, "[b c] d\n");
}

// --- ITEM, BUTLAST ---

TEST(test_item_on_list_and_word) {
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT ITEM 1 [a b c]\n"
        "PRINT ITEM 2 [a b c]\n"
        "PRINT ITEM 3 [a b c]\n"
        "PRINT ITEM 1 \"hi\n"
        "PRINT ITEM 3 \"hello");
    CHECK_STREQ(captured_output, "a\nb\nc\nh\nl\n");
}

TEST(test_item_out_of_range_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT ITEM 0 [a b c]");
    CHECK_CONTAINS(captured_output, "ITEM: index out of range");
    captured_output[0] = '\0';
    eval_logo(app, "PRINT ITEM 5 [a b c]");
    CHECK_CONTAINS(captured_output, "ITEM: index out of range");
    captured_output[0] = '\0';
    eval_logo(app, "PRINT ITEM 10 \"hi");
    CHECK_CONTAINS(captured_output, "ITEM: index out of range");
}

TEST(test_item_on_bare_number) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT ITEM 1 5");
    CHECK_STREQ(captured_output, "5\n");
    captured_output[0] = '\0';
    eval_logo(app, "PRINT ITEM 2 5");
    CHECK_CONTAINS(captured_output, "ITEM: index out of range");
}

TEST(test_item_of_nested_sublist_element) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT ITEM 2 [a [b c] d]");
    CHECK_STREQ(captured_output, "b c\n"); // the sublist, printed as PRINT's own top-level value
}

TEST(test_butlast_on_list_and_word) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT BUTLAST [a b c]\nPRINT BUTLAST \"hello");
    CHECK_STREQ(captured_output, "a b\nhell\n");
}

TEST(test_butlast_of_empty_or_single_element_is_empty) {
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT BUTLAST []\n"
        "PRINT BUTLAST [a]\n"
        "PRINT BUTLAST \"\n"
        "PRINT BUTLAST \"a");
    CHECK_STREQ(captured_output, "\n\n\n\n");
}

TEST(test_butlast_preserves_a_sublist_element) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT BUTLAST [a [b c] d]");
    CHECK_STREQ(captured_output, "a [b c]\n");
}

TEST(test_sentence_splices_but_list_wraps) {
    // The whole point of true nesting: SENTENCE and LIST finally differ.
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT SENTENCE [1 2] [3 4]\n"
        "PRINT LIST [1 2] [3 4]");
    CHECK_STREQ(captured_output, "1 2 3 4\n[1 2] [3 4]\n");
}

TEST(test_fput_a_list_creates_genuine_nesting) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT FPUT [1 2] [3 4]");
    CHECK_STREQ(captured_output, "[1 2] 3 4\n");
}

TEST(test_lput_a_list_creates_genuine_nesting) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT LPUT [3 4] [1 2]");
    CHECK_STREQ(captured_output, "1 2 [3 4]\n");
}

TEST(test_make_aliases_a_nested_list) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a [1 [2 3]]\nMAKE \"b :a\nPRINT :b");
    CHECK_STREQ(captured_output, "1 [2 3]\n");
}

TEST(test_word_on_a_list_argument_reports_an_error) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT WORD [1 2] \"x");
    CHECK_CONTAINS(captured_output, "WORD: expected words, not a list");
}

TEST(test_list_pool_exhaustion_reports_an_error) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"x []\nREPEAT 9000 [MAKE \"x FPUT 1 :x]");
    CHECK_CONTAINS(captured_output, "list storage full");
}

// --- Words/lists in numeric contexts ---

TEST(test_forward_with_list_operator_argument) {
    LogoApp *app = new_app();
    eval_logo(app, "FD FIRST [100 50]");
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 150.0); // moved 100, same as FD 100
}

TEST(test_repeat_count_from_list_operator) {
    LogoApp *app = new_app();
    eval_logo(app, "REPEAT FIRST [3 9] [FD 10 RT 90]");
    CHECK(app->line_count == 3);
}

TEST(test_make_arithmetic_with_word_variable) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"colors [100 50]\n"
        "MAKE \"x FIRST :colors + 1\n"
        "PRINT :x");
    CHECK_STREQ(captured_output, "101\n");
}

TEST(test_non_numeric_word_coerces_to_zero_in_arithmetic) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"greeting \"hello\nPRINT :greeting + 1");
    CHECK_STREQ(captured_output, "1\n");
}

TEST(test_unary_minus_on_list_operator) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT -FIRST [5 6]");
    CHECK_STREQ(captured_output, "-5\n");
}

TEST(test_setpenwidth_from_word_typed_variable) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"w LAST [1 2 3]\nSETPENWIDTH :w\nFD 10");
    CHECK_NEAR(app->lines[0].width, 3.0);
}

// --- Fuller arithmetic (MOD, POWER, SQRT, trig, RANDOM, ROUND, ABS) ---

TEST(test_mod_and_power) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT MOD 7 3\nPRINT MOD -7 3\nPRINT POWER 2 10");
    CHECK_STREQ(captured_output, "1\n2\n1024\n"); // MOD takes the sign of the divisor
}

TEST(test_sqrt_and_abs) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT SQRT 16\nPRINT ABS -5\nPRINT ABS 5");
    CHECK_STREQ(captured_output, "4\n5\n5\n");
}

TEST(test_round_to_nearest_integer) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT ROUND 2.4\nPRINT ROUND 2.6\nPRINT ROUND -2.6");
    CHECK_STREQ(captured_output, "2\n3\n-3\n");
}

TEST(test_sin_cos_arctan_in_degrees) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT SIN 90\nPRINT COS 0\nPRINT ARCTAN 1");
    CHECK_STREQ(captured_output, "1\n1\n45\n");
}

TEST(test_random_stays_in_range) {
    LogoApp *app = new_app();
    for (int i = 0; i < 200; i++) {
        eval_logo(app, "PRINT RANDOM 10");
    }
    const char *p = captured_output;
    int checked = 0;
    while (p != NULL && *p) {
        int n;
        CHECK(sscanf(p, "%d", &n) == 1);
        CHECK(n >= 0 && n < 10);
        checked++;
        p = strchr(p, '\n');
        if (p != NULL) p++;
    }
    CHECK(checked == 200);
}

// --- Type/membership predicates (MEMBER?, EMPTY?, WORD?, LIST?, NUMBER?) ---

TEST(test_word_list_number_predicates) {
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT WORD? \"hi\n"
        "PRINT WORD? [1 2]\n"
        "PRINT WORD? 5\n"
        "PRINT LIST? [1 2]\n"
        "PRINT LIST? \"hi\n"
        "PRINT LIST? 5\n"
        "PRINT NUMBER? 5\n"
        "PRINT NUMBER? \"hi\n"
        "PRINT NUMBER? [1 2]");
    CHECK_STREQ(captured_output, "1\n0\n0\n1\n0\n0\n1\n0\n0\n");
}

TEST(test_empty_predicate) {
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT EMPTY? []\n"
        "PRINT EMPTY? [1]\n"
        "PRINT EMPTY? \"\n"
        "PRINT EMPTY? \"a\n"
        "PRINT EMPTY? 5");
    CHECK_STREQ(captured_output, "1\n0\n1\n0\n0\n");
}

TEST(test_member_on_list_and_number) {
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT MEMBER? \"b [a b c]\n"
        "PRINT MEMBER? \"z [a b c]\n"
        "PRINT MEMBER? 2 [1 2 3]\n"
        "PRINT MEMBER? 5 5\n"
        "PRINT MEMBER? 5 6");
    CHECK_STREQ(captured_output, "1\n0\n1\n1\n0\n");
}

TEST(test_member_on_word_is_substring) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT MEMBER? \"ell \"hello\nPRINT MEMBER? \"xyz \"hello");
    CHECK_STREQ(captured_output, "1\n0\n");
}

TEST(test_predicates_usable_in_conditions) {
    LogoApp *app = new_app();
    eval_logo(app,
        "IF EMPTY? [] [PRINT \"a]\n"
        "IF WORD? \"hi [PRINT \"b]\n"
        "IF LIST? [1 2] [PRINT \"c]\n"
        "IF MEMBER? \"x [x y z] [PRINT \"d]\n"
        "IF NOT NUMBER? \"hi [PRINT \"e]");
    CHECK_STREQ(captured_output, "a\nb\nc\nd\ne\n");
}

// --- Turtle command aliases & clamping ---

TEST(test_back_moves_opposite_of_forward) {
    LogoApp *app = new_app();
    eval_logo(app, "BK 100");
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 350.0); // opposite of FD 100's y=150
}

TEST(test_left_turns_opposite_of_right) {
    LogoApp *app = new_app();
    eval_logo(app, "LT 90 FD 100");
    CHECK_NEAR(app->turtles[0].x, 150.0); // opposite of RT 90 FD 100's x=350
    CHECK_NEAR(app->turtles[0].y, 250.0);
}

TEST(test_setpencolor_clamps_out_of_range_channels) {
    LogoApp *app = new_app();
    eval_logo(app, "SETPENCOLOR -10 300 128\nFD 10");
    CHECK_NEAR(app->lines[0].r, 0.0);
    CHECK_NEAR(app->lines[0].g, 1.0);
    CHECK_NEAR(app->lines[0].b, 128.0 / 255.0);
}

TEST(test_setpenwidth_clamps_to_min_and_max) {
    LogoApp *app = new_app();
    eval_logo(app, "SETPENWIDTH 0.1\nFD 10");
    CHECK_NEAR(app->lines[0].width, 0.5); // MIN_PEN_WIDTH
    eval_logo(app, "SETPENWIDTH 100\nFD 10");
    CHECK_NEAR(app->lines[1].width, 20.0); // MAX_PEN_WIDTH
}

// --- Comparison operators (<= >= <> = <) ---

TEST(test_full_comparison_operator_set) {
    LogoApp *app = new_app();
    eval_logo(app,
        "IF 5 <= 5 [PRINT \"le]\n"
        "IF 6 <= 5 [PRINT \"should-not-print]\n"
        "IF 5 >= 5 [PRINT \"ge]\n"
        "IF 5 >= 6 [PRINT \"should-not-print]\n"
        "IF 5 <> 6 [PRINT \"ne]\n"
        "IF 5 <> 5 [PRINT \"should-not-print]\n"
        "IF 5 < 6 [PRINT \"lt]\n"
        "IF 5 = 5 [PRINT \"eq]");
    CHECK_STREQ(captured_output, "le\nge\nne\nlt\neq\n");
}

TEST(test_not_can_nest) {
    LogoApp *app = new_app();
    eval_logo(app, "IF NOT NOT 1 = 1 [PRINT \"yes]\nIF NOT NOT 1 = 2 [PRINT \"should-not-print]");
    CHECK_STREQ(captured_output, "yes\n");
}

// --- IF/IFELSE branch selection ---

TEST(test_if_false_condition_runs_nothing) {
    LogoApp *app = new_app();
    eval_logo(app, "IF 1 = 2 [PRINT \"should-not-print]\nPRINT \"after");
    CHECK_STREQ(captured_output, "after\n");
}

TEST(test_if_with_literal_else_keyword) {
    LogoApp *app = new_app();
    eval_logo(app, "IF 1 = 2 [PRINT \"a] ELSE [PRINT \"b]");
    CHECK_STREQ(captured_output, "b\n");
}

TEST(test_ifelse_selects_true_and_false_branches) {
    LogoApp *app = new_app();
    eval_logo(app, "IFELSE 1 = 1 [PRINT \"a] [PRINT \"b]\nIFELSE 1 = 2 [PRINT \"a] [PRINT \"b]");
    CHECK_STREQ(captured_output, "a\nb\n");
}

// --- REPL input completeness (is_input_complete) ---

TEST(test_is_input_complete_simple_command) {
    CHECK(is_input_complete("FD 100"));
}

TEST(test_is_input_complete_unbalanced_bracket) {
    CHECK(!is_input_complete("REPEAT 4 [FD 100"));
}

TEST(test_is_input_complete_balanced_bracket) {
    CHECK(is_input_complete("REPEAT 4 [FD 100 RT 90]"));
}

TEST(test_is_input_complete_to_without_end) {
    CHECK(!is_input_complete("TO square :size\nREPEAT 4 [FD :size RT 90]"));
}

TEST(test_is_input_complete_to_with_end) {
    CHECK(is_input_complete("TO square :size\nREPEAT 4 [FD :size RT 90]\nEND"));
}

// --- Files (LOAD/SAVE) ---
//
// Real file I/O against build/ (already gitignored scratch space the
// Makefile creates for object files) rather than a fixed system path, so
// these don't depend on /tmp being writable or collide with anything
// else. Each test cleans up the file it wrote.

TEST(test_save_and_load_round_trip) {
    const char *path = "build/test_save_roundtrip.logo";
    remove(path);

    LogoApp *saver = new_app();
    eval_logo(saver, "TO square :size\nREPEAT 4 [FD :size RT 90]\nEND\nSAVE \"build/test_save_roundtrip.logo");
    CHECK_CONTAINS(captured_output, "Saved build/test_save_roundtrip.logo");

    LogoApp *loader = new_app();
    captured_output[0] = '\0'; // new_app doesn't reset the shared buffer between apps
    eval_logo(loader, "LOAD \"build/test_save_roundtrip.logo\nsquare 60");
    CHECK(loader->line_count == 4);
    CHECK_NEAR(loader->turtles[0].x, 250.0);
    CHECK_NEAR(loader->turtles[0].y, 250.0);

    remove(path);
}

TEST(test_save_with_no_procedures_writes_empty_file) {
    const char *path = "build/test_save_empty.logo";
    remove(path);

    LogoApp *app = new_app();
    eval_logo(app, "SAVE \"build/test_save_empty.logo");

    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        CHECK(ftell(f) == 0);
        fclose(f);
    }

    remove(path);
}

TEST(test_load_nonexistent_file_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "LOAD \"build/definitely_does_not_exist.logo");
    CHECK_CONTAINS(captured_output, "LOAD: could not read file");
}

TEST(test_save_to_unwritable_path_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "SAVE \"build/no_such_subdir/whatever.logo");
    CHECK_CONTAINS(captured_output, "SAVE: could not write file");
}

TEST(test_load_without_quote_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "LOAD path");
    CHECK_CONTAINS(captured_output, "LOAD: expected a");
}

TEST(test_save_without_quote_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "SAVE path");
    CHECK_CONTAINS(captured_output, "SAVE: expected a");
}

// --- Errors ---

TEST(test_unknown_command_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "FOOBAR");
    CHECK_STREQ(captured_output, "I don't know how to FOOBAR\n");
}

TEST(test_malformed_repeat_does_not_crash) {
    LogoApp *app = new_app();
    eval_logo(app, "REPEAT 4 FD 10\nPRINT \"still-here");
    CHECK_STREQ(captured_output, "REPEAT: expected [ block ]\nstill-here\n");
}

TEST(test_make_without_quote_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE x 5");
    CHECK_CONTAINS(captured_output, "MAKE: expected a");
}

// --- Robustness (fixed-size buffer limits) ---
//
// extract_block (shared by REPEAT/WHILE/IF/IFELSE and bracketed list
// literals) used to treat an unterminated `[` as if it silently extended
// to the end of the input; it now detects that and reports an error
// instead of running mangled state.

TEST(test_repeat_unterminated_block_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "REPEAT 4 [FD 10 RT 90");
    CHECK_CONTAINS(captured_output, "REPEAT: expected [ block ]");
    CHECK(app->line_count == 0); // the body never ran
}

TEST(test_if_unterminated_block_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "IF 1 = 1 [PRINT \"hi");
    CHECK_CONTAINS(captured_output, "IF: expected [ block ]");
}

TEST(test_while_unterminated_block_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"i 0\nWHILE :i < 3 [PRINT :i");
    CHECK_CONTAINS(captured_output, "WHILE: expected [ block ]");
}

TEST(test_to_body_too_long_reports_error) {
    LogoApp *app = new_app();
    char body[9000];
    size_t i = 0;
    while (i + 8 < sizeof(body)) {
        memcpy(body + i, "PRINT 1\n", 8);
        i += 8;
    }
    body[i] = '\0';
    char code[10000];
    snprintf(code, sizeof(code), "TO longproc\n%sEND\n", body);
    eval_logo(app, code);
    CHECK_CONTAINS(captured_output, "longproc: procedure body too long, not defined");
}

TEST(test_to_missing_end_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "TO square\nPRINT \"never-runs");
    CHECK_CONTAINS(captured_output, "square: missing END");
}

TEST(test_ifelse_missing_second_block_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "IFELSE 1 = 1 [PRINT \"a]");
    CHECK_CONTAINS(captured_output, "IFELSE: expected two [ block ]s");
}

TEST(test_erase_without_quote_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "ERASE t");
    CHECK_CONTAINS(captured_output, "ERASE: expected a");
}

TEST(test_erase_nonexistent_procedure_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "ERASE \"nosuch");
    CHECK_CONTAINS(captured_output, "ERASE: no such procedure \"nosuch");
}

TEST(test_too_many_procedures_reports_error) {
    // MAX_PROCEDURES is 50; define 51 and confirm the last one is refused
    // rather than silently overwriting or corrupting the table.
    LogoApp *app = new_app();
    char code[4096] = {0};
    for (int i = 0; i < 51; i++) {
        char one[32];
        snprintf(one, sizeof(one), "TO p%d\nEND\n", i);
        strncat(code, one, sizeof(code) - strlen(code) - 1);
    }
    eval_logo(app, code);
    CHECK_CONTAINS(captured_output, "Too many procedures defined, TO ignored");
    CHECK(app->proc_count == 50);
}

// --- Table-exhaustion boundaries (variables, parameters) ---

TEST(test_too_many_variables_reports_error) {
    // MAX_VARIABLES is 100; define 100 distinct globals, then confirm a
    // 101st (new) name is refused loudly rather than silently dropped --
    // find_or_create_var used to return NULL with no error at all here,
    // unlike the analogous MAX_PROCEDURES case above -- while an
    // existing variable can still be updated.
    LogoApp *app = new_app();
    char code[4096] = {0};
    for (int i = 0; i < 100; i++) {
        char one[24];
        snprintf(one, sizeof(one), "MAKE \"v%d %d\n", i, i);
        strncat(code, one, sizeof(code) - strlen(code) - 1);
    }
    eval_logo(app, code);
    CHECK(app->var_count == 100);

    eval_logo(app, "MAKE \"v100 999");
    CHECK_CONTAINS(captured_output, "MAKE: too many variables defined, not set");
    CHECK(app->var_count == 100); // refused, table unchanged

    eval_logo(app, "MAKE \"v0 42\nPRINT :v0");
    CHECK_CONTAINS(captured_output, "42\n"); // updating an existing var still works
}

TEST(test_too_many_parameters_reports_error) {
    // MAX_PARAMS is 8; a 9th declared parameter is dropped (with a loud
    // error) rather than corrupting the body -- the previous behavior
    // left the excess :param token dangling right where body capture
    // was about to start, since the parameter loop simply stopped early
    // instead of still consuming (and discarding) it.
    LogoApp *app = new_app();
    eval_logo(app,
        "TO addall :a :b :c :d :e :f :g :h :i\n"
        "PRINT :a + :b + :c + :d + :e + :f + :g + :h\n"
        "END\n"
        "addall 1 2 3 4 5 6 7 8");
    CHECK_CONTAINS(captured_output, "addall: too many parameters, extra parameters ignored");
    CHECK_CONTAINS(captured_output, "36\n"); // 1+2+...+8, using the first 8 params
}

// --- Safety limits (recursion depth, WHILE iteration cap) ---

TEST(test_recursion_depth_limit_reports_error) {
    // MAX_SCOPE_DEPTH is 200; a procedure that always recurses should hit
    // the cap and report an error rather than overflowing the scope stack.
    LogoApp *app = new_app();
    eval_logo(app, "TO recurse\nrecurse\nEND\nrecurse");
    CHECK_CONTAINS(captured_output, "Recursion too deep, call ignored");
}

TEST(test_while_iteration_limit_reports_error) {
    // MAX_WHILE_ITERATIONS is 1,000,000; a condition that never goes
    // false should stop itself rather than hanging forever.
    LogoApp *app = new_app();
    eval_logo(app, "WHILE 1 = 1 []");
    CHECK_CONTAINS(captured_output, "WHILE: stopped after too many iterations");
}

int main(void) {
    RUN(test_comment_on_its_own_line_is_ignored);
    RUN(test_trailing_comment_after_a_command_is_ignored);
    RUN(test_comment_inside_a_block_is_ignored);
    RUN(test_semicolon_glued_to_a_word_is_not_a_comment);

    RUN(test_forward_moves_and_draws);
    RUN(test_penup_moves_without_drawing);
    RUN(test_repeat_square_returns_to_start);
    RUN(test_setxy_draws_direct_line);
    RUN(test_home_returns_and_resets_heading);
    RUN(test_arc_draws_without_moving_the_turtle);
    RUN(test_setpencolor_and_width_stamp_the_segment);
    RUN(test_setbackground_sets_canvas_color);

    RUN(test_label_records_position_color_and_text);
    RUN(test_label_accepts_a_list_argument);
    RUN(test_clear_erases_labels);

    RUN(test_fill_records_position_and_color);
    RUN(test_clear_erases_fills);

    RUN(test_wait_zero_or_negative_returns_immediately);
    RUN(test_wait_pauses_for_at_least_the_requested_duration);

    RUN(test_pos_reads_back_turtle_position);
    RUN(test_heading_reads_back_turtle_heading_without_wrapping);
    RUN(test_heading_can_be_saved_and_restored);
    RUN(test_pos_and_heading_follow_the_current_turtle);

    RUN(test_tell_creates_and_switches_turtles);
    RUN(test_turtles_move_independently);
    RUN(test_clear_homes_every_turtle);
    RUN(test_tell_out_of_range_reports_error);

    RUN(test_hideturtle_showturtle_toggles_visibility);
    RUN(test_window_mode_is_default_and_allows_offscreen);
    RUN(test_wrap_wraps_position_and_lifts_pen);
    RUN(test_fence_clamps_position_and_reports_error);
    RUN(test_window_command_resets_from_wrap_or_fence);

    RUN(test_procedure_definition_and_call);
    RUN(test_recursion_with_parameter);
    RUN(test_parameter_shadows_outer_global);
    RUN(test_make_without_local_mutates_outer_global);
    RUN(test_to_redefinition_overwrites);
    RUN(test_erase_removes_procedure);

    RUN(test_output_returns_a_value_used_in_an_expression);
    RUN(test_output_stops_the_rest_of_the_procedure);
    RUN(test_output_escapes_nested_blocks);
    RUN(test_stop_ends_a_procedure_early_and_caller_continues);
    RUN(test_output_used_as_a_plain_statement_discards_the_value);
    RUN(test_calling_a_procedure_that_never_outputs_as_a_value_reports_error);
    RUN(test_output_outside_procedure_reports_error);
    RUN(test_stop_outside_procedure_reports_error);
    RUN(test_recursive_procedure_using_output);
    RUN(test_output_value_can_be_a_word_or_list);

    RUN(test_local_scopes_a_variable_without_leaking_to_global);
    RUN(test_local_initializes_to_zero);
    RUN(test_local_outside_procedure_reports_error);
    RUN(test_local_without_quote_reports_error);

    RUN(test_run_executes_a_list_as_code);
    RUN(test_run_executes_a_stored_variable);
    RUN(test_run_handles_nested_blocks);
    RUN(test_run_self_reference_is_capped);
    RUN(test_apply_calls_procedure_with_list_args);
    RUN(test_apply_wrong_arg_count_reports_error);
    RUN(test_apply_unknown_procedure_reports_error);

    RUN(test_map_transforms_each_element);
    RUN(test_map_on_a_bare_number);
    RUN(test_map_preserves_nested_list_elements);
    RUN(test_filter_keeps_matching_elements);
    RUN(test_reduce_folds_left_to_right);
    RUN(test_reduce_of_single_element_list_is_that_element);
    RUN(test_foreach_runs_template_per_element);

    RUN(test_if_ifelse_comparisons);
    RUN(test_boolean_and_or_not);
    RUN(test_while_loop);

    RUN(test_word_variable_and_comparison);
    RUN(test_make_multiword_string_from_bracket_list);
    RUN(test_multiword_string_comparison);
    RUN(test_print_list_joins_words_with_single_spaces);

    RUN(test_first_last_butfirst_count_via_print);
    RUN(test_list_operators_via_make);
    RUN(test_nested_list_operators);
    RUN(test_count_of_bare_number_is_one);
    RUN(test_first_of_empty_list_is_empty);
    RUN(test_list_operator_in_comparison);
    RUN(test_make_a_colon_b_copies_word);

    RUN(test_first_last_butfirst_count_on_a_word);
    RUN(test_substring_operators_on_empty_word);
    RUN(test_numbers_stay_atomic_for_substring_operators);
    RUN(test_substring_of_word_variable);
    RUN(test_substring_operators_can_nest);
    RUN(test_fput_lput_on_word_builds_new_word);
    RUN(test_fput_lput_list_onto_word_reports_error);

    RUN(test_word_concatenates_with_no_space);
    RUN(test_sentence_joins_with_a_space);
    RUN(test_fput_prepends_and_lput_appends);
    RUN(test_list_construction_via_make_and_nesting);

    RUN(test_print_nested_list_literal);
    RUN(test_count_of_nested_list_counts_top_level_only);
    RUN(test_first_and_last_can_return_a_sublist);
    RUN(test_butfirst_preserves_a_sublist_element);

    RUN(test_item_on_list_and_word);
    RUN(test_item_out_of_range_reports_error);
    RUN(test_item_on_bare_number);
    RUN(test_item_of_nested_sublist_element);
    RUN(test_butlast_on_list_and_word);
    RUN(test_butlast_of_empty_or_single_element_is_empty);
    RUN(test_butlast_preserves_a_sublist_element);

    RUN(test_sentence_splices_but_list_wraps);
    RUN(test_fput_a_list_creates_genuine_nesting);
    RUN(test_lput_a_list_creates_genuine_nesting);
    RUN(test_make_aliases_a_nested_list);
    RUN(test_word_on_a_list_argument_reports_an_error);
    RUN(test_list_pool_exhaustion_reports_an_error);

    RUN(test_forward_with_list_operator_argument);
    RUN(test_repeat_count_from_list_operator);
    RUN(test_make_arithmetic_with_word_variable);
    RUN(test_non_numeric_word_coerces_to_zero_in_arithmetic);
    RUN(test_unary_minus_on_list_operator);
    RUN(test_setpenwidth_from_word_typed_variable);

    RUN(test_mod_and_power);
    RUN(test_sqrt_and_abs);
    RUN(test_round_to_nearest_integer);
    RUN(test_sin_cos_arctan_in_degrees);
    RUN(test_random_stays_in_range);

    RUN(test_word_list_number_predicates);
    RUN(test_empty_predicate);
    RUN(test_member_on_list_and_number);
    RUN(test_member_on_word_is_substring);
    RUN(test_predicates_usable_in_conditions);

    RUN(test_back_moves_opposite_of_forward);
    RUN(test_left_turns_opposite_of_right);
    RUN(test_setpencolor_clamps_out_of_range_channels);
    RUN(test_setpenwidth_clamps_to_min_and_max);

    RUN(test_full_comparison_operator_set);
    RUN(test_not_can_nest);

    RUN(test_if_false_condition_runs_nothing);
    RUN(test_if_with_literal_else_keyword);
    RUN(test_ifelse_selects_true_and_false_branches);

    RUN(test_is_input_complete_simple_command);
    RUN(test_is_input_complete_unbalanced_bracket);
    RUN(test_is_input_complete_balanced_bracket);
    RUN(test_is_input_complete_to_without_end);
    RUN(test_is_input_complete_to_with_end);

    RUN(test_save_and_load_round_trip);
    RUN(test_save_with_no_procedures_writes_empty_file);
    RUN(test_load_nonexistent_file_reports_error);
    RUN(test_save_to_unwritable_path_reports_error);
    RUN(test_load_without_quote_reports_error);
    RUN(test_save_without_quote_reports_error);

    RUN(test_unknown_command_reports_error);
    RUN(test_malformed_repeat_does_not_crash);
    RUN(test_make_without_quote_reports_error);

    RUN(test_repeat_unterminated_block_reports_error);
    RUN(test_if_unterminated_block_reports_error);
    RUN(test_while_unterminated_block_reports_error);
    RUN(test_to_body_too_long_reports_error);
    RUN(test_to_missing_end_reports_error);
    RUN(test_ifelse_missing_second_block_reports_error);
    RUN(test_erase_without_quote_reports_error);
    RUN(test_erase_nonexistent_procedure_reports_error);
    RUN(test_too_many_procedures_reports_error);

    RUN(test_too_many_variables_reports_error);
    RUN(test_too_many_parameters_reports_error);

    RUN(test_recursion_depth_limit_reports_error);
    RUN(test_while_iteration_limit_reports_error);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d failure(s).\n", failures);
    return 1;
}
