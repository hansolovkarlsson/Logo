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
    CHECK(app->raster_op_count == 1);
    CHECK(app->raster_ops[0].kind == RASTER_OP_FILL);
    CHECK_NEAR(app->raster_ops[0].x, 250.0);
    CHECK_NEAR(app->raster_ops[0].y, 200.0);
    CHECK_NEAR(app->raster_ops[0].r, 0.0);
    CHECK_NEAR(app->raster_ops[0].g, 200.0 / 255.0);
    CHECK_NEAR(app->raster_ops[0].b, 0.0);
}

TEST(test_clear_erases_fills) {
    LogoApp *app = new_app();
    eval_logo(app, "FILL\nCLEAR");
    CHECK(app->raster_op_count == 0);
}

// --- ERASERECT ---
// Same "record plain data, Cairo does the actual rasterizing in ui.c"
// split as FILL -- these check the data ERASERECT records for it.

TEST(test_eraserect_records_position_and_size) {
    LogoApp *app = new_app();
    eval_logo(app, "FD 50\nERASERECT 40 60");
    CHECK(app->raster_op_count == 1);
    CHECK(app->raster_ops[0].kind == RASTER_OP_ERASE_RECT);
    CHECK_NEAR(app->raster_ops[0].x, 250.0);
    CHECK_NEAR(app->raster_ops[0].y, 200.0);
    CHECK_NEAR(app->raster_ops[0].w, 40.0);
    CHECK_NEAR(app->raster_ops[0].h, 60.0);
}

// --- LOADSPRITE / SETSPRITE / STAMPSPRITE ---
// The actual image decoding is gdk-pixbuf, in ui.c, tested manually in
// the running app (load_sprite_image is NULL here, same convention as
// load_background_image); these check interpreter.c's own parsing,
// lookup, and the raster op STAMPSPRITE records for it.

TEST(test_loadsprite_without_quote_name_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "LOADSPRITE turtle \"turtle.png");
    CHECK_CONTAINS(captured_output, "LOADSPRITE: expected a \"name");
}

TEST(test_loadsprite_without_quote_path_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "LOADSPRITE \"turtle path");
    CHECK_CONTAINS(captured_output, "LOADSPRITE: expected a \"path");
}

TEST(test_loadsprite_is_a_safe_no_op_with_no_gui) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT \"before\nLOADSPRITE \"turtle \"turtle.png\nPRINT \"after");
    CHECK_STREQ(captured_output, "before\nafter\n");
}

TEST(test_setsprite_without_quote_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "SETSPRITE name");
    CHECK_CONTAINS(captured_output, "SETSPRITE: expected a \"name");
}

TEST(test_setsprite_of_unknown_name_reports_error_and_leaves_default) {
    // load_sprite_image is NULL in tests, so no sprite is ever actually
    // registered -- SETSPRITE "anything (other than "NONE) should always
    // fail to find one here.
    LogoApp *app = new_app();
    eval_logo(app, "SETSPRITE \"turtle");
    CHECK_CONTAINS(captured_output, "SETSPRITE: no such sprite \"turtle");
    CHECK(app->turtles[0].sprite_index == -1);
}

TEST(test_setsprite_none_is_a_silent_reset_to_default) {
    LogoApp *app = new_app();
    eval_logo(app, "SETSPRITE \"none");
    CHECK_STREQ(captured_output, "");
    CHECK(app->turtles[0].sprite_index == -1);
}

TEST(test_stampsprite_records_position_heading_and_sprite) {
    LogoApp *app = new_app();
    eval_logo(app, "FD 50\nRT 30\nSTAMPSPRITE");
    CHECK(app->raster_op_count == 1);
    CHECK(app->raster_ops[0].kind == RASTER_OP_STAMP);
    CHECK_NEAR(app->raster_ops[0].x, 250.0);
    CHECK_NEAR(app->raster_ops[0].y, 200.0);
    CHECK_NEAR(app->raster_ops[0].angle, 30.0);
    CHECK(app->raster_ops[0].sprite_index == -1); // no sprite set -- default triangle
    CHECK(app->raster_ops[0].sprite_frame == 0);
}

TEST(test_clear_erases_stamps_too) {
    LogoApp *app = new_app();
    eval_logo(app, "STAMPSPRITE\nCLEAR");
    CHECK(app->raster_op_count == 0);
}

// --- LOADSPRITESHEET / SETSPRITEFRAME (sprite-sheet blit) ---
// Same headless limitation as LOADSPRITE/SETSPRITE above: load_sprite_image
// is NULL here, so no sheet is ever actually registered. These check
// interpreter.c's own parsing, validation, and the raster op STAMPSPRITE
// records for the active frame; the actual grid slicing (draw_turtle_shape
// in ui.c) needs a real loaded image and is tested manually in the app.

TEST(test_loadspritesheet_without_quote_name_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "LOADSPRITESHEET walk \"walk.png 4 2");
    CHECK_CONTAINS(captured_output, "LOADSPRITESHEET: expected a \"name");
}

TEST(test_loadspritesheet_without_quote_path_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "LOADSPRITESHEET \"walk path 4 2");
    CHECK_CONTAINS(captured_output, "LOADSPRITESHEET: expected a \"path");
}

TEST(test_loadspritesheet_with_zero_cols_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "LOADSPRITESHEET \"walk \"walk.png 0 2");
    CHECK_CONTAINS(captured_output, "LOADSPRITESHEET: cols and rows must be at least 1");
}

TEST(test_loadspritesheet_with_zero_rows_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "LOADSPRITESHEET \"walk \"walk.png 4 0");
    CHECK_CONTAINS(captured_output, "LOADSPRITESHEET: cols and rows must be at least 1");
}

TEST(test_loadspritesheet_is_a_safe_no_op_with_no_gui) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT \"before\nLOADSPRITESHEET \"walk \"walk.png 4 2\nPRINT \"after");
    CHECK_STREQ(captured_output, "before\nafter\n");
}

TEST(test_setspriteframe_without_a_sprite_set_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "SETSPRITEFRAME 2");
    CHECK_CONTAINS(captured_output, "SETSPRITEFRAME: no sprite set (use SETSPRITE first)");
}

// --- ANIMATESPRITE ---
// Same headless limitation as the rest of the sprite commands: no sprite
// is ever actually registered here, so the frame-cycling loop itself
// (and its pausing) is never reached -- only the error path and the
// guarantee that it doesn't pause when there's nothing to animate are
// testable without a real GTK window.

TEST(test_animatesprite_without_a_sprite_set_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "ANIMATESPRITE 0.5 10");
    CHECK_CONTAINS(captured_output, "ANIMATESPRITE: no sprite set (use SETSPRITE first)");
}

TEST(test_animatesprite_without_a_sprite_set_does_not_pause) {
    LogoApp *app = new_app();
    gint64 start = g_get_monotonic_time();
    // Would take 50 real seconds if the frame-cycling loop ran at all --
    // must bail out on the "no sprite" check before ever pausing.
    eval_logo(app, "ANIMATESPRITE 5 10");
    gint64 elapsed = g_get_monotonic_time() - start;
    CHECK(elapsed < 1000000); // well under 1 second
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

// --- SETCANVASSIZE/CANVASSIZE (resizable canvas) ---
// The actual widget resize is GTK, in ui.c, tested manually in the
// running app (resize_canvas is NULL here, same convention as the other
// GUI-side-effect callbacks); these check the runtime canvas_width/
// height state itself and everything in interpreter.c that depends on
// it (turtle homing, WRAP/FENCE boundaries).

TEST(test_canvassize_defaults_to_500_by_500) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT CANVASSIZE");
    CHECK_STREQ(captured_output, "500 500\n");
}

TEST(test_setcanvassize_changes_the_size) {
    LogoApp *app = new_app();
    eval_logo(app, "SETCANVASSIZE 800 300\nPRINT CANVASSIZE");
    CHECK_STREQ(captured_output, "800 300\n");
}

TEST(test_setcanvassize_too_small_reports_error_and_leaves_size_unchanged) {
    LogoApp *app = new_app();
    eval_logo(app, "SETCANVASSIZE 10 10\nPRINT CANVASSIZE");
    CHECK_CONTAINS(captured_output, "SETCANVASSIZE: width and height must be 50-4000");
    CHECK_CONTAINS(captured_output, "500 500");
}

TEST(test_setcanvassize_too_large_reports_error_and_leaves_size_unchanged) {
    LogoApp *app = new_app();
    eval_logo(app, "SETCANVASSIZE 5000 5000\nPRINT CANVASSIZE");
    CHECK_CONTAINS(captured_output, "SETCANVASSIZE: width and height must be 50-4000");
    CHECK_CONTAINS(captured_output, "500 500");
}

TEST(test_setcanvassize_recenters_turtles_and_clears_drawing) {
    LogoApp *app = new_app();
    eval_logo(app, "FD 50\nRT 45\nFILL\nSETCANVASSIZE 800 600");
    CHECK_NEAR(app->turtles[0].x, 400.0); // new center: 800 / 2
    CHECK_NEAR(app->turtles[0].y, 300.0); // new center: 600 / 2
    CHECK_NEAR(app->turtles[0].angle, 0.0);
    CHECK(app->line_count == 0);
    CHECK(app->raster_op_count == 0);
}

TEST(test_setcanvassize_changes_the_wrap_boundary) {
    LogoApp *app = new_app();
    eval_logo(app, "SETCANVASSIZE 200 200\nWRAP SETXY 250 100");
    CHECK_NEAR(app->turtles[0].x, 50.0); // 250 mod 200, not the old 500
    CHECK_NEAR(app->turtles[0].y, 100.0);
}

// --- PAUSE/CONTINUE, BACKTRACE, EXECTIME (debugger) ---
// PAUSE's real interactive behavior (busy-waiting for a live CONTINUE
// typed into the entry box, reentrant into a nested eval_logo call) is
// GTK-driven and tested manually in the running app -- request_redraw
// is NULL here, same convention as WAIT/ANIMATESPRITE's own GUI-side
// effects, so PAUSE is a silent no-op headless (it never even
// increments pause_depth) rather than busy-waiting forever with no
// live entry box to ever call CONTINUE.

TEST(test_pause_is_a_silent_no_op_with_no_gui) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT \"before\nPAUSE\nPRINT \"after");
    CHECK_STREQ(captured_output, "before\nafter\n");
}

TEST(test_continue_with_nothing_paused_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "CONTINUE");
    CHECK_CONTAINS(captured_output, "CONTINUE: nothing is paused");
}

TEST(test_co_is_an_alias_for_continue) {
    LogoApp *app = new_app();
    eval_logo(app, "CO");
    CHECK_CONTAINS(captured_output, "CONTINUE: nothing is paused");
}

TEST(test_backtrace_at_top_level_shows_no_active_calls) {
    LogoApp *app = new_app();
    eval_logo(app, "BACKTRACE");
    CHECK_STREQ(captured_output, "BACKTRACE:\n  (top level)\n");
}

TEST(test_backtrace_shows_the_call_stack_innermost_first) {
    LogoApp *app = new_app();
    eval_logo(app, "TO inner\nBACKTRACE\nEND\nTO outer\ninner\nEND\nouter");
    CHECK_CONTAINS(captured_output, "BACKTRACE:\n  inner\n  outer\n  (top level)\n");
}

TEST(test_bt_is_an_alias_for_backtrace) {
    LogoApp *app = new_app();
    eval_logo(app, "BT");
    CHECK_STREQ(captured_output, "BACKTRACE:\n  (top level)\n");
}

TEST(test_exectime_measures_at_least_the_wrapped_waits_duration) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT EXECTIME [WAIT 0.02]"); // kept short so the suite stays fast
    double microseconds = atof(captured_output);
    CHECK(microseconds >= 20000); // never faster than the wrapped WAIT itself
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

TEST(test_getx_gety_read_back_single_axes) {
    LogoApp *app = new_app();
    eval_logo(app, "FD 100\nPRINT GETX\nPRINT GETY");
    CHECK_STREQ(captured_output, "250\n150\n");
}

TEST(test_setx_sety_move_along_a_single_axis_only) {
    LogoApp *app = new_app();
    eval_logo(app, "SETXY 400 300\nSETX 10\nPRINT POS\nSETY 20\nPRINT POS");
    CHECK_STREQ(captured_output, "10 300\n10 20\n");
}

TEST(test_setx_sety_draw_a_line_when_pen_is_down) {
    LogoApp *app = new_app();
    eval_logo(app, "SETXY 400 300\nSETX 10\nSETY 20");
    CHECK(app->line_count == 3); // SETXY's jump, then SETX's, then SETY's
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

TEST(test_clean_erases_drawing_but_leaves_the_turtle_alone) {
    LogoApp *app = new_app();
    eval_logo(app, "FD 100 RT 45\nCLEAN");
    CHECK(app->line_count == 0);
    CHECK_NEAR(app->turtles[0].x, 250.0);
    CHECK_NEAR(app->turtles[0].y, 150.0);
    CHECK_NEAR(app->turtles[0].angle, 45.0);
}

TEST(test_cleartext_is_a_safe_no_op_with_no_history_pane) {
    // clear_history is NULL here (no GTK text view in tests, same as
    // request_redraw) -- CLEARTEXT/CT must not crash or print anything
    // when there's nothing to clear.
    LogoApp *app = new_app();
    eval_logo(app, "PRINT \"before\nCLEARTEXT\nCT\nPRINT \"after");
    CHECK_STREQ(captured_output, "before\nafter\n");
}

TEST(test_who_reports_the_current_turtle) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT WHO\nTELL 1\nPRINT WHO\nTELL 0\nPRINT WHO");
    CHECK_STREQ(captured_output, "0\n1\n0\n");
}

TEST(test_procedures_lists_every_defined_procedure) {
    LogoApp *app = new_app();
    eval_logo(app, "TO square :s\nREPEAT 4 [FD :s RT 90]\nEND\nTO tri\nREPEAT 3 [FD 50 RT 120]\nEND\nPRINT PROCEDURES");
    CHECK_STREQ(captured_output, "square tri\n");
}

TEST(test_procedures_is_empty_when_none_are_defined) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT PROCEDURES\nPRINT EMPTY? PROCEDURES");
    CHECK_STREQ(captured_output, "\nTRUE\n");
}

TEST(test_names_lists_every_global_variable_not_locals) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"a 1\n"
        "MAKE \"b 2\n"
        "TO p :x\nLOCAL \"y\nPRINT NAMES\nEND\n"
        "p 3");
    CHECK_STREQ(captured_output, "a b\n"); // parameter :x and LOCAL "y aren't globals
}

TEST(test_names_reflects_reassignment_not_duplicate_entries) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a 1\nMAKE \"a 2\nPRINT NAMES");
    CHECK_STREQ(captured_output, "a\n"); // one entry, even after MAKE "a runs twice
}

// --- Property lists (SETPROP/GETPROP/REMOVEPROP/PROPLIST) ---

TEST(test_setprop_getprop_round_trips_a_number_word_and_list) {
    LogoApp *app = new_app();
    eval_logo(app,
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
    eval_logo(app, "PRINT GETPROP \"turtle1 \"nosuchkey\nPRINT EMPTY? GETPROP \"turtle1 \"nosuchkey");
    CHECK_STREQ(captured_output, "\nTRUE\n");
}

TEST(test_setprop_on_same_key_overwrites_not_duplicates) {
    LogoApp *app = new_app();
    eval_logo(app,
        "SETPROP \"turtle1 \"color \"red\n"
        "SETPROP \"turtle1 \"color \"blue\n"
        "PRINT GETPROP \"turtle1 \"color\n"
        "PRINT COUNT PROPLIST \"turtle1"); // one key/value pair, not two
    CHECK_STREQ(captured_output, "blue\n2\n");
}

TEST(test_different_proplist_names_dont_share_properties) {
    LogoApp *app = new_app();
    eval_logo(app,
        "SETPROP \"turtle1 \"color \"red\n"
        "SETPROP \"turtle2 \"color \"blue\n"
        "PRINT GETPROP \"turtle1 \"color\n"
        "PRINT GETPROP \"turtle2 \"color");
    CHECK_STREQ(captured_output, "red\nblue\n");
}

TEST(test_removeprop_removes_a_property) {
    LogoApp *app = new_app();
    eval_logo(app,
        "SETPROP \"turtle1 \"color \"red\n"
        "REMOVEPROP \"turtle1 \"color\n"
        "PRINT GETPROP \"turtle1 \"color");
    CHECK_STREQ(captured_output, "\n");
}

TEST(test_removeprop_of_missing_property_is_a_silent_no_op) {
    LogoApp *app = new_app();
    eval_logo(app, "REMOVEPROP \"turtle1 \"nosuchkey");
    CHECK_STREQ(captured_output, "");
}

TEST(test_proplist_lists_alternating_keys_and_values) {
    LogoApp *app = new_app();
    eval_logo(app,
        "SETPROP \"turtle1 \"speed 5\n"
        "SETPROP \"turtle1 \"color \"red\n"
        "PRINT PROPLIST \"turtle1");
    CHECK_STREQ(captured_output, "speed 5 color red\n");
}

TEST(test_proplist_of_unknown_name_is_the_empty_list) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT EMPTY? PROPLIST \"nosuchplist");
    CHECK_STREQ(captured_output, "TRUE\n");
}

TEST(test_setprop_can_use_a_computed_plist_name_and_key) {
    LogoApp *app = new_app();
    eval_logo(app,
        "SETPROP WORD \"turtle \"1 WORD \"col \"or \"red\n"
        "PRINT GETPROP \"turtle1 \"color");
    CHECK_STREQ(captured_output, "red\n");
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

// --- Procedure parameters keep their real type (not just numbers) ---
// call_procedure used to bind every argument as VALUE_NUMBER regardless
// of what was actually passed (arg_vals was a plain double[MAX_PARAMS]),
// so a list/word/array argument silently number-coerced at the call
// boundary -- found 2026-08-06 while adding arrays, fixed 2026-08-07.

TEST(test_procedure_parameter_accepts_a_list) {
    LogoApp *app = new_app();
    eval_logo(app, "TO test :x\nPRINT :x\nEND\ntest [1 2 3]");
    CHECK_STREQ(captured_output, "1 2 3\n");
}

TEST(test_procedure_parameter_accepts_a_word) {
    LogoApp *app = new_app();
    eval_logo(app, "TO test :x\nPRINT :x\nEND\ntest \"hello");
    CHECK_STREQ(captured_output, "hello\n");
}

TEST(test_procedure_parameter_still_coerces_arithmetic_to_a_number) {
    LogoApp *app = new_app();
    eval_logo(app, "TO test :x\nPRINT :x\nEND\ntest 5 + 3");
    CHECK_STREQ(captured_output, "8\n");
}

TEST(test_procedure_parameter_accepts_an_array_and_keeps_its_aliasing) {
    // Arrays are a deliberate exception to this language's otherwise
    // immutable values (see set_var_array): mutating through a
    // parameter must be visible to the caller's own variable too, the
    // same as MAKE "b :a then SETITEM ... :b already is.
    LogoApp *app = new_app();
    eval_logo(app,
        "TO mutate :a\nSETITEM 1 :a 777\nEND\n"
        "MAKE \"arr ARRAY 3\n"
        "mutate :arr\n"
        "PRINT ITEM 1 :arr");
    CHECK_STREQ(captured_output, "777\n");
}

TEST(test_procedure_used_as_operator_accepts_a_list_parameter) {
    // The value-producing operator call site (a procedure used inside
    // an expression, via OUTPUT) threads arguments separately from the
    // plain statement call site -- exercise it too.
    LogoApp *app = new_app();
    eval_logo(app, "TO firstof :x\nOUTPUT FIRST :x\nEND\nPRINT firstof [9 8 7]");
    CHECK_STREQ(captured_output, "9\n");
}

TEST(test_apply_passes_a_word_argument_unchanged) {
    // Bracket-list elements are plain bareword tokens (no leading " --
    // that's only for the "word prefix syntax in expression contexts),
    // so [hello] is a one-element list holding the word hello.
    LogoApp *app = new_app();
    eval_logo(app, "TO test :x\nPRINT :x\nEND\nAPPLY \"test [hello]");
    CHECK_STREQ(captured_output, "hello\n");
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

TEST(test_thing_reads_a_variable_by_a_computed_name) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"item1 42\nMAKE \"n 1\nPRINT THING WORD \"item :n");
    CHECK_STREQ(captured_output, "42\n");
}

TEST(test_thing_matches_colon_name_for_words_lists_and_arrays) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"w \"hello\n"
        "MAKE \"l [1 2 3]\n"
        "PRINT THING \"w\n"
        "PRINT THING \"l");
    CHECK_STREQ(captured_output, "hello\n1 2 3\n");
}

TEST(test_thing_of_an_unbound_name_is_zero) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT THING \"nosuchvar");
    CHECK_STREQ(captured_output, "0\n");
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

TEST(test_stop_outside_procedure_ends_the_run_silently) {
    // Unlike OUTPUT (which needs a procedure call to hand its value
    // back to), STOP is legal directly at the REPL/top level -- it's
    // the only way to escape a bare FOREVER typed there. It silently
    // ends the rest of the current run rather than reporting an error.
    LogoApp *app = new_app();
    eval_logo(app, "PRINT \"before\nSTOP\nPRINT \"unreachable");
    CHECK_STREQ(captured_output, "before\n");
}

TEST(test_stop_recovers_cleanly_for_the_next_top_level_run) {
    // A top-level STOP must not leave stop_requested set afterward --
    // otherwise every subsequent command would silently become a no-op.
    LogoApp *app = new_app();
    eval_logo(app, "STOP");
    eval_logo(app, "PRINT \"still-works");
    CHECK_STREQ(captured_output, "still-works\n");
}

TEST(test_forever_stops_via_stop_directly_at_top_level) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"i 0\nFOREVER [MAKE \"i :i + 1 PRINT :i IF :i = 3 [STOP]]");
    CHECK_STREQ(captured_output, "1\n2\n3\n");
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

// --- CATCH, THROW ---

TEST(test_catch_catches_a_matching_throw) {
    LogoApp *app = new_app();
    eval_logo(app, "CATCH \"err [PRINT \"before THROW \"err PRINT \"unreachable]\nPRINT \"after");
    CHECK_STREQ(captured_output, "before\nafter\n");
}

TEST(test_catch_runs_normally_when_nothing_throws) {
    LogoApp *app = new_app();
    eval_logo(app, "CATCH \"tag [PRINT \"hi]\nPRINT \"after");
    CHECK_STREQ(captured_output, "hi\nafter\n");
}

TEST(test_throw_propagates_past_nested_procedure_calls) {
    LogoApp *app = new_app();
    eval_logo(app,
        "TO inner\nTHROW \"boom\nPRINT \"unreachable-inner\nEND\n"
        "TO outer\ninner\nPRINT \"unreachable-outer\nEND\n"
        "CATCH \"boom [outer]\n"
        "PRINT \"after");
    CHECK_STREQ(captured_output, "after\n");
}

TEST(test_throw_with_non_matching_tag_keeps_propagating) {
    LogoApp *app = new_app();
    eval_logo(app,
        "CATCH \"outer_tag [\n"
        "  CATCH \"wrong_tag [THROW \"outer_tag PRINT \"unreachable1]\n"
        "  PRINT \"unreachable2\n"
        "]\n"
        "PRINT \"after");
    CHECK_STREQ(captured_output, "after\n");
}

TEST(test_throw_without_catch_reports_error_and_recovers_to_top_level) {
    LogoApp *app = new_app();
    eval_logo(app, "THROW \"nope\nPRINT \"after");
    CHECK_STREQ(captured_output, "THROW: no CATCH found for \"nope\nafter\n");
}

TEST(test_uncaught_throw_does_not_stick_around_for_next_command) {
    LogoApp *app = new_app();
    eval_logo(app, "THROW \"nope");
    CHECK_CONTAINS(captured_output, "THROW: no CATCH found for \"nope");
    captured_output[0] = '\0';
    eval_logo(app, "PRINT \"stillworks");
    CHECK_STREQ(captured_output, "stillworks\n");
}

TEST(test_stop_inside_catch_still_escapes_the_enclosing_procedure) {
    LogoApp *app = new_app();
    eval_logo(app,
        "TO myproc\nCATCH \"tag [PRINT \"in-catch STOP]\nPRINT \"unreachable\nEND\n"
        "myproc\n"
        "PRINT \"after");
    CHECK_STREQ(captured_output, "in-catch\nafter\n");
}

TEST(test_catch_missing_block_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "CATCH \"tag");
    CHECK_CONTAINS(captured_output, "CATCH: expected [ block ]");
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

TEST(test_map_with_word_elements) {
    // Every bracket-list element is internally a VALUE_WORD regardless
    // of whether it looks numeric (see parse_list_literal) -- this is
    // what a genuinely non-numeric word element exercises: it must
    // come back out of the template quoted, not as bare text that
    // looks like an attempted command.
    LogoApp *app = new_app();
    eval_logo(app, "PRINT MAP [FIRST ?] [foo bar]");
    CHECK_STREQ(captured_output, "f b\n");
}

TEST(test_filter_keeps_matching_elements) {
    // Elements here look numeric (their VALUE_WORD text happens to be
    // "1"/"2"/"3"/"4") but must still compare correctly with > , which
    // requires an actual VALUE_NUMBER on both sides (see
    // parse_comparison) -- a naively-quoted "1" etc. would force
    // VALUE_WORD typing and silently fail every > comparison instead.
    LogoApp *app = new_app();
    eval_logo(app, "PRINT FILTER [? > 2] [1 2 3 4]");
    CHECK_STREQ(captured_output, "3 4\n");
}

TEST(test_filter_with_word_elements) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT FILTER [? = \"b] [a b c b]");
    CHECK_STREQ(captured_output, "b b\n");
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

TEST(test_reduce_concatenates_word_elements) {
    // Exercises value_to_source_text on the accumulator itself (a
    // WORD, not just the current element) across repeated iterations.
    LogoApp *app = new_app();
    eval_logo(app, "PRINT REDUCE [WORD ?1 ?2] [a b c]");
    CHECK_STREQ(captured_output, "abc\n");
}

TEST(test_foreach_runs_template_per_element) {
    LogoApp *app = new_app();
    eval_logo(app, "FOREACH [PRINT ?] [1 2 3]");
    CHECK_STREQ(captured_output, "1\n2\n3\n");
}

TEST(test_foreach_with_word_elements_prints_each_word) {
    // The original bug report: a bare word substituted for ? used to
    // come out unquoted (e.g. `PRINT the`), which tokenizes as its own
    // top-level command rather than PRINT's argument -- `0` printed
    // (parse_factor's number fallback consuming nothing) followed by
    // `I don't know how to the`, for every word, instead of the word
    // itself.
    LogoApp *app = new_app();
    eval_logo(app, "FOREACH [PRINT ?] [the turtle draws a square]");
    CHECK_STREQ(captured_output, "the\nturtle\ndraws\na\nsquare\n");
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

TEST(test_for_loop_counts_up_with_default_step) {
    LogoApp *app = new_app();
    eval_logo(app, "FOR [i 1 3] [PRINT :i]");
    CHECK_STREQ(captured_output, "1\n2\n3\n");
}

TEST(test_for_loop_counts_down_with_default_step) {
    LogoApp *app = new_app();
    eval_logo(app, "FOR [i 3 1] [PRINT :i]");
    CHECK_STREQ(captured_output, "3\n2\n1\n");
}

TEST(test_for_loop_with_explicit_step) {
    LogoApp *app = new_app();
    eval_logo(app, "FOR [i 0 10 5] [PRINT :i]");
    CHECK_STREQ(captured_output, "0\n5\n10\n");
}

TEST(test_for_loop_bounds_can_be_expressions) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"n 2\nFOR [i 1 :n + 1] [PRINT :i]");
    CHECK_STREQ(captured_output, "1\n2\n3\n");
}

TEST(test_for_step_zero_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "FOR [i 1 3 0] [PRINT :i]");
    CHECK_CONTAINS(captured_output, "FOR: step must not be 0");
}

TEST(test_for_unterminated_header_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "FOR [i 1 3\nPRINT \"unreachable");
    CHECK_CONTAINS(captured_output, "FOR: expected [ var start limit step ]");
}

TEST(test_for_unterminated_block_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "FOR [i 1 3] [PRINT :i");
    CHECK_CONTAINS(captured_output, "FOR: expected [ block ]");
}

TEST(test_for_iteration_limit_reports_error) {
    // MAX_WHILE_ITERATIONS is 1,000,000; a range that never ends (step
    // pointed the wrong way relative to an astronomically distant limit
    // isn't needed here -- an ordinary huge range already exercises it)
    // should stop itself rather than hanging forever.
    LogoApp *app = new_app();
    eval_logo(app, "FOR [i 1 999999999] []");
    CHECK_CONTAINS(captured_output, "FOR: stopped after too many iterations");
}

TEST(test_forever_loop_stops_via_stop) {
    LogoApp *app = new_app();
    eval_logo(app,
        "TO countup\n"
        "MAKE \"i 0\n"
        "FOREVER [MAKE \"i :i + 1 PRINT :i IF :i = 3 [STOP]]\n"
        "END\n"
        "countup");
    CHECK_STREQ(captured_output, "1\n2\n3\n");
}

TEST(test_forever_unterminated_block_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "FOREVER [PRINT \"hi");
    CHECK_CONTAINS(captured_output, "FOREVER: expected [ block ]");
}

TEST(test_forever_iteration_limit_reports_error) {
    // Same MAX_WHILE_ITERATIONS ceiling as WHILE/FOR, applied to a loop
    // with no exit condition at all.
    LogoApp *app = new_app();
    eval_logo(app, "FOREVER []");
    CHECK_CONTAINS(captured_output, "FOREVER: stopped after too many iterations");
}

TEST(test_true_false_literals_print_and_store) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT TRUE\nPRINT FALSE\nMAKE \"flag TRUE\nPRINT :flag");
    CHECK_STREQ(captured_output, "TRUE\nFALSE\nTRUE\n");
}

TEST(test_true_false_case_insensitive) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT true\nPRINT false");
    CHECK_STREQ(captured_output, "TRUE\nFALSE\n");
}

TEST(test_if_while_treat_the_word_false_as_falsy) {
    LogoApp *app = new_app();
    eval_logo(app,
        "IF TRUE [PRINT \"a]\n"
        "IF FALSE [PRINT \"should-not-print]\n"
        "MAKE \"flag FALSE\n"
        "IF :flag [PRINT \"also-should-not-print]\n"
        "IF NOT :flag [PRINT \"b]");
    CHECK_STREQ(captured_output, "a\nb\n");
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

// --- FLATTEN, PARSE, SUBST ---

TEST(test_flatten_collapses_every_level_of_nesting) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT FLATTEN [1 [2 3] [4 [5 6]] 7]");
    CHECK_STREQ(captured_output, "1 2 3 4 5 6 7\n");
}

TEST(test_flatten_of_an_already_flat_list_is_unchanged) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT FLATTEN [a b c]");
    CHECK_STREQ(captured_output, "a b c\n");
}

TEST(test_flatten_of_empty_list_is_empty) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT EMPTY? FLATTEN []");
    CHECK_STREQ(captured_output, "TRUE\n");
}

TEST(test_flatten_of_a_bare_word_or_number_is_one_element) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT FLATTEN \"hi\nPRINT COUNT FLATTEN 5");
    CHECK_STREQ(captured_output, "hi\n1\n");
}

TEST(test_parse_tokenizes_a_word_into_a_one_element_list) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT PARSE \"hello\nPRINT COUNT PARSE \"hello");
    CHECK_STREQ(captured_output, "hello\n1\n");
}

TEST(test_parse_splits_the_printed_text_of_a_list_on_whitespace) {
    LogoApp *app = new_app();
    // PARSE tokenizes whatever PRINT would show for the argument --
    // splitting an already-flat list's rendered text back into words is
    // a no-op round trip; COUNT confirms three real elements came out.
    eval_logo(app, "PRINT PARSE [red green blue]\nPRINT COUNT PARSE [red green blue]");
    CHECK_STREQ(captured_output, "red green blue\n3\n");
}

// --- 'raw string' literals ---
// The one way to write a literal space (or other whitespace) directly
// in Logo source -- "word's own reading always stops at the first one.
// A plain VALUE_WORD, not a list, so it prints identically to a
// SENTENCE/PARSE-built list of the same words; COUNT (characters vs.
// elements) is what actually tells the two apart.

TEST(test_single_quote_string_reads_a_raw_multiword_word) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT 'hello world'\nPRINT COUNT 'hello world'");
    CHECK_STREQ(captured_output, "hello world\n11\n"); // 11 characters, not 2 words
}

TEST(test_single_quote_string_composes_with_parse) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT COUNT PARSE 'hello world'");
    CHECK_STREQ(captured_output, "2\n"); // PARSE splits it into [hello world], 2 elements
}

TEST(test_single_quote_string_can_be_empty) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT ''\nPRINT \"after");
    CHECK_STREQ(captured_output, "\nafter\n");
}

TEST(test_single_quote_string_missing_closing_quote_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT 'unterminated");
    CHECK_CONTAINS(captured_output, "'...': missing closing ' or too long");
}

TEST(test_single_quote_string_works_with_make_and_variables) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"greeting 'hello world'\nPRINT :greeting");
    CHECK_STREQ(captured_output, "hello world\n");
}

TEST(test_subst_replaces_every_matching_element) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT SUBST \"b \"x [a b c b]");
    CHECK_STREQ(captured_output, "a x c x\n");
}

TEST(test_subst_recurses_into_sublists) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT SUBST \"b \"x [a [b c] [d [b e]]]");
    CHECK_STREQ(captured_output, "a [x c] [d [x e]]\n");
}

TEST(test_subst_can_replace_a_whole_matching_sublist) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT SUBST [1 2] \"x [[1 2] 3 4]");
    CHECK_STREQ(captured_output, "x 3 4\n");
}

TEST(test_subst_on_a_bare_value_checks_it_directly) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT SUBST \"a \"x \"a\nPRINT SUBST \"a \"x \"b");
    CHECK_STREQ(captured_output, "x\nb\n");
}

// --- DOT, CROSS ---

TEST(test_dot_product_of_equal_length_lists) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT DOT [1 2 3] [4 5 6]");
    CHECK_STREQ(captured_output, "32\n"); // 1*4 + 2*5 + 3*6
}

TEST(test_dot_mismatched_length_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT DOT [1 2] [1 2 3]");
    CHECK_CONTAINS(captured_output, "DOT: lists must be the same length");
}

TEST(test_cross_product_of_3element_lists) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT CROSS [1 0 0] [0 1 0]");
    CHECK_STREQ(captured_output, "0 0 1\n");
}

TEST(test_cross_wrong_length_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT CROSS [1 2] [1 2 3]");
    CHECK_CONTAINS(captured_output, "CROSS: expected two 3-element lists");
}

// --- DISTANCE, TOWARDS ---

TEST(test_distance_between_two_points) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT DISTANCE [0 0] [3 4]");
    CHECK_STREQ(captured_output, "5\n");
}

TEST(test_distance_requires_two_2element_lists) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT DISTANCE [1 2 3] [1 2]");
    CHECK_CONTAINS(captured_output, "DISTANCE: expected two 2-element lists");
}

TEST(test_towards_reports_a_compass_heading_toward_the_point) {
    LogoApp *app = new_app();
    eval_logo(app,
        "SETXY 0 0\n"
        "PRINT TOWARDS [0 -10]\n" // north
        "PRINT TOWARDS [10 0]\n"  // east
        "PRINT TOWARDS [0 10]\n"  // south
        "PRINT TOWARDS [-10 0]"); // west
    CHECK_STREQ(captured_output, "0\n90\n180\n270\n");
}

TEST(test_towards_requires_a_2element_list) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT TOWARDS [1 2 3]");
    CHECK_CONTAINS(captured_output, "TOWARDS: expected a 2-element list");
}

TEST(test_setheading_towards_then_forward_reaches_the_point) {
    // Round-trips DISTANCE/TOWARDS through the turtle's own movement
    // formula rather than checking the angle math in isolation.
    LogoApp *app = new_app();
    eval_logo(app, "SETHEADING TOWARDS [300 200]\nFORWARD DISTANCE POS [300 200]");
    CHECK_NEAR(app->turtles[0].x, 300.0);
    CHECK_NEAR(app->turtles[0].y, 200.0);
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

// --- Arrays ---

TEST(test_array_create_and_read_default_elements) {
    LogoApp *app = new_app();
    // Every slot defaults to an empty list, matching real Logo's ARRAY --
    // printing one is just a blank line.
    eval_logo(app, "MAKE \"a ARRAY 3\nPRINT ITEM 1 :a\nPRINT COUNT :a\nPRINT ARRAY? :a");
    CHECK_STREQ(captured_output, "\n3\nTRUE\n");
}

TEST(test_array_size_must_be_at_least_1) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a ARRAY 0");
    CHECK_CONTAINS(captured_output, "ARRAY: size must be at least 1");
}

TEST(test_setitem_mutates_in_place) {
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"a ARRAY 3\n"
        "SETITEM 1 :a \"x\n"
        "SETITEM 2 :a \"y\n"
        "SETITEM 3 :a \"z\n"
        "PRINT :a");
    CHECK_STREQ(captured_output, "{x y z}\n"); // arrays print with braces, unlike lists
}

TEST(test_setitem_can_store_a_list_element) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a ARRAY 1\nSETITEM 1 :a [1 2 3]\nPRINT ITEM 1 :a");
    CHECK_STREQ(captured_output, "1 2 3\n");
}

TEST(test_setitem_index_out_of_range) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a ARRAY 3\nSETITEM 0 :a \"x");
    CHECK_CONTAINS(captured_output, "SETITEM: index out of range");
    captured_output[0] = '\0';
    eval_logo(app, "SETITEM 4 :a \"x");
    CHECK_CONTAINS(captured_output, "SETITEM: index out of range");
}

TEST(test_setitem_on_non_array_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "SETITEM 1 [1 2 3] \"x");
    CHECK_CONTAINS(captured_output, "SETITEM: expected an array");
}

TEST(test_setitem_rejects_nesting_an_array) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a ARRAY 2\nMAKE \"b ARRAY 2\nSETITEM 1 :a :b");
    CHECK_CONTAINS(captured_output, "SETITEM: can't store an array inside an array");
}

TEST(test_fillarray_sets_every_slot_in_one_call) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a ARRAY 3\nFILLARRAY :a \"x\nPRINT :a");
    CHECK_STREQ(captured_output, "{x x x}\n");
}

TEST(test_fillarray_can_store_a_list_element) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a ARRAY 2\nFILLARRAY :a [1 2]\nPRINT ITEM 1 :a\nPRINT ITEM 2 :a");
    CHECK_STREQ(captured_output, "1 2\n1 2\n");
}

TEST(test_fillarray_on_non_array_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "FILLARRAY [1 2 3] \"x");
    CHECK_CONTAINS(captured_output, "FILLARRAY: expected an array");
}

TEST(test_fillarray_rejects_nesting_an_array) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a ARRAY 2\nMAKE \"b ARRAY 2\nFILLARRAY :a :b");
    CHECK_CONTAINS(captured_output, "FILLARRAY: can't store an array inside an array");
}

TEST(test_array_item_out_of_range_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a ARRAY 3\nPRINT ITEM 0 :a");
    CHECK_CONTAINS(captured_output, "ITEM: index out of range");
    captured_output[0] = '\0';
    eval_logo(app, "PRINT ITEM 4 :a");
    CHECK_CONTAINS(captured_output, "ITEM: index out of range");
}

TEST(test_array_predicates) {
    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"a ARRAY 3\nPRINT ARRAY? :a\nPRINT ARRAY? [1 2 3]\nPRINT ARRAY? 5");
    CHECK_STREQ(captured_output, "TRUE\nFALSE\nFALSE\n");
}

TEST(test_make_b_a_aliases_the_same_mutable_array) {
    // The one deliberate exception to this language's otherwise
    // immutable values: assigning an array to another variable shares
    // the same underlying storage, so SETITEM through either is visible
    // from both.
    LogoApp *app = new_app();
    eval_logo(app,
        "MAKE \"a ARRAY 2\n"
        "MAKE \"b :a\n"
        "SETITEM 1 :b 42\n"
        "PRINT ITEM 1 :a");
    CHECK_STREQ(captured_output, "42\n");
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

TEST(test_tan_in_degrees) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT TAN 0\nPRINT TAN 45");
    CHECK_STREQ(captured_output, "0\n1\n");
}

TEST(test_asin_acos_report_degrees) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT ASIN 1\nPRINT ACOS 1\nPRINT ACOS 0");
    CHECK_STREQ(captured_output, "90\n0\n90\n");
}

TEST(test_log_is_base_10_ln_is_natural_log) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT LOG 100\nPRINT LN 1\nPRINT EXP 0\nPRINT LN EXP 3");
    CHECK_STREQ(captured_output, "2\n0\n1\n3\n");
}

TEST(test_int_truncates_towards_zero) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT INT 2.9\nPRINT INT -2.9\nPRINT INT 3");
    CHECK_STREQ(captured_output, "2\n-2\n3\n");
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

TEST(test_pick_returns_an_element_actually_in_the_list) {
    LogoApp *app = new_app();
    for (int i = 0; i < 100; i++) {
        eval_logo(app, "PRINT PICK [10 20 30]");
    }
    const char *p = captured_output;
    int checked = 0;
    while (p != NULL && *p) {
        int n;
        CHECK(sscanf(p, "%d", &n) == 1);
        CHECK(n == 10 || n == 20 || n == 30);
        checked++;
        p = strchr(p, '\n');
        if (p != NULL) p++;
    }
    CHECK(checked == 100);
}

TEST(test_pick_on_word_and_array) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT PICK \"a"); // single-character word: only one possible pick
    CHECK_STREQ(captured_output, "a\n");
    captured_output[0] = '\0';
    eval_logo(app, "MAKE \"arr ARRAY 1\nSETITEM 1 :arr \"z\nPRINT PICK :arr");
    CHECK_STREQ(captured_output, "z\n");
}

TEST(test_pick_on_empty_list_or_word_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT PICK []");
    CHECK_CONTAINS(captured_output, "PICK: empty list");
    captured_output[0] = '\0';
    eval_logo(app, "PRINT PICK \"");
    CHECK_CONTAINS(captured_output, "PICK: empty word");
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
    CHECK_STREQ(captured_output, "TRUE\nFALSE\nFALSE\nTRUE\nFALSE\nFALSE\nTRUE\nFALSE\nFALSE\n");
}

TEST(test_empty_predicate) {
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT EMPTY? []\n"
        "PRINT EMPTY? [1]\n"
        "PRINT EMPTY? \"\n"
        "PRINT EMPTY? \"a\n"
        "PRINT EMPTY? 5");
    CHECK_STREQ(captured_output, "TRUE\nFALSE\nTRUE\nFALSE\nFALSE\n");
}

TEST(test_member_on_list_and_number) {
    LogoApp *app = new_app();
    eval_logo(app,
        "PRINT MEMBER? \"b [a b c]\n"
        "PRINT MEMBER? \"z [a b c]\n"
        "PRINT MEMBER? 2 [1 2 3]\n"
        "PRINT MEMBER? 5 5\n"
        "PRINT MEMBER? 5 6");
    CHECK_STREQ(captured_output, "TRUE\nFALSE\nTRUE\nTRUE\nFALSE\n");
}

TEST(test_member_on_word_is_substring) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT MEMBER? \"ell \"hello\nPRINT MEMBER? \"xyz \"hello");
    CHECK_STREQ(captured_output, "TRUE\nFALSE\n");
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

// --- TYPE, SHOW ---

TEST(test_type_prints_without_trailing_newline) {
    LogoApp *app = new_app();
    eval_logo(app, "TYPE \"a\nTYPE \"b\nPRINT \"c");
    CHECK_STREQ(captured_output, "abc\n");
}

TEST(test_show_prints_a_procedures_definition) {
    // A trailing blank line before END is expected here: proc->body
    // captures everything up to END, including its own trailing
    // newline, and append_procedure_text (shared with SAVE) always adds
    // one more before "END" -- pre-existing SAVE-format behavior, not
    // something SHOW introduces.
    LogoApp *app = new_app();
    eval_logo(app, "TO square :size\nREPEAT 4 [FD :size RT 90]\nEND\nSHOW \"square");
    CHECK_STREQ(captured_output, "TO square :size\nREPEAT 4 [FD :size RT 90]\n\nEND\n");
}

TEST(test_show_undefined_procedure_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "SHOW \"nope");
    CHECK_CONTAINS(captured_output, "SHOW: no such procedure \"nope");
}

TEST(test_text_returns_a_procedures_body_as_a_flat_word_list) {
    LogoApp *app = new_app();
    eval_logo(app,
        "TO square :size\nREPEAT 4 [FD :size RT 90]\nEND\n"
        "PRINT TEXT \"square\n"
        "PRINT COUNT TEXT \"square");
    // Printed text round-trips exactly, but it's 6 separate word
    // tokens, not one real nested sublist for [FD :size RT 90] -- see
    // the next test for that distinction made concrete.
    CHECK_STREQ(captured_output, "REPEAT 4 [FD :size RT 90]\n6\n");
}

TEST(test_text_tokens_keep_quote_and_bracket_punctuation_literal) {
    LogoApp *app = new_app();
    eval_logo(app,
        "TO demo\nPRINT \"hi\nEND\n"
        "PRINT ITEM 1 TEXT \"demo\n"
        "PRINT ITEM 2 TEXT \"demo");
    // TEXT is a plain whitespace tokenize of the source, not a real
    // parse: the "hi token keeps its literal quote character rather
    // than being reinterpreted as the quoted word hi.
    CHECK_STREQ(captured_output, "PRINT\n\"hi\n");
}

TEST(test_text_undefined_procedure_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT TEXT \"nope");
    CHECK_CONTAINS(captured_output, "TEXT: no such procedure \"nope");
}

TEST(test_text_without_quoted_name_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT TEXT 5");
    CHECK_CONTAINS(captured_output, "TEXT: expected a \"name");
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

TEST(test_loadpic_without_quote_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "LOADPIC path");
    CHECK_CONTAINS(captured_output, "LOADPIC: expected a");
}

TEST(test_savepic_without_quote_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "SAVEPIC path");
    CHECK_CONTAINS(captured_output, "SAVEPIC: expected a");
}

TEST(test_loadpic_savepic_are_a_safe_no_op_with_no_gui) {
    // load_background_image/save_canvas_image are NULL here (no GTK/
    // gdk-pixbuf image decoding in tests, same convention as
    // clear_history) -- a well-formed LOADPIC/SAVEPIC call must not
    // crash or print anything when there's no callback to do the work.
    LogoApp *app = new_app();
    eval_logo(app, "PRINT \"before\nLOADPIC \"bg.png\nSAVEPIC \"out.png\nPRINT \"after");
    CHECK_STREQ(captured_output, "before\nafter\n");
}

// --- General file I/O (OPENREAD/OPENWRITE/OPENAPPEND/CLOSE/FILEPRINT/
// READLINE/EOF?/DELETEFILE/DIRECTORY) ---
// Real file I/O against build/, same convention as the LOAD/SAVE tests
// above (already gitignored scratch space, no dependency on /tmp being
// writable). Each test cleans up the file it wrote.

TEST(test_openwrite_fileprint_close_writes_the_file) {
    const char *path = "build/test_openwrite.txt";
    remove(path);

    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"ch OPENWRITE \"build/test_openwrite.txt\n"
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
    const char *path = "build/test_openread.txt";
    remove(path);
    g_file_set_contents(path, "line one\nline two\n", -1, NULL);

    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"ch OPENREAD \"build/test_openread.txt\n"
                   "PRINT READLINE :ch\n"
                   "PRINT READLINE :ch\n"
                   "PRINT EOF? :ch\n"
                   "CLOSE :ch");
    CHECK_STREQ(captured_output, "line one\nline two\nTRUE\n");

    remove(path);
}

TEST(test_openappend_appends_to_existing_content) {
    const char *path = "build/test_openappend.txt";
    remove(path);
    g_file_set_contents(path, "first\n", -1, NULL);

    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"ch OPENAPPEND \"build/test_openappend.txt\n"
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
    eval_logo(app, "PRINT OPENREAD \"build/definitely_does_not_exist_either.txt");
    CHECK_STREQ(captured_output, "-1\n");
}

TEST(test_close_of_invalid_channel_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "CLOSE 3");
    CHECK_CONTAINS(captured_output, "CLOSE: no such open channel");
}

TEST(test_fileprint_on_a_read_channel_reports_error) {
    const char *path = "build/test_fileprint_wrong_mode.txt";
    remove(path);
    g_file_set_contents(path, "data\n", -1, NULL);

    LogoApp *app = new_app();
    eval_logo(app, "MAKE \"ch OPENREAD \"build/test_fileprint_wrong_mode.txt\n"
                   "FILEPRINT :ch \"oops");
    CHECK_CONTAINS(captured_output, "FILEPRINT: channel not open for writing");

    remove(path);
}

TEST(test_readline_and_eof_on_a_closed_channel_are_safe_sentinels) {
    LogoApp *app = new_app();
    eval_logo(app, "PRINT READLINE 5\nPRINT EOF? 5");
    CHECK_STREQ(captured_output, "\nTRUE\n");
}

TEST(test_deletefile_removes_a_file) {
    const char *path = "build/test_deletefile.txt";
    g_file_set_contents(path, "x", -1, NULL);

    LogoApp *app = new_app();
    eval_logo(app, "DELETEFILE \"build/test_deletefile.txt");
    CHECK_STREQ(captured_output, "");
    CHECK(!g_file_test(path, G_FILE_TEST_EXISTS));
}

TEST(test_deletefile_of_missing_file_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "DELETEFILE \"build/definitely_does_not_exist_at_all.txt");
    CHECK_CONTAINS(captured_output, "DELETEFILE: could not delete");
}

TEST(test_deletefile_without_quote_reports_error) {
    LogoApp *app = new_app();
    eval_logo(app, "DELETEFILE path");
    CHECK_CONTAINS(captured_output, "DELETEFILE: expected a");
}

TEST(test_directory_lists_the_current_working_directory) {
    // Tests run from the repo root (see Makefile's test rule), so
    // Makefile itself is a stable, always-present entry -- no need to
    // create a marker file just to test DIRECTORY.
    LogoApp *app = new_app();
    eval_logo(app, "PRINT MEMBER? \"Makefile DIRECTORY");
    CHECK_STREQ(captured_output, "TRUE\n");
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
    RUN(test_eraserect_records_position_and_size);
    RUN(test_loadsprite_without_quote_name_reports_error);
    RUN(test_loadsprite_without_quote_path_reports_error);
    RUN(test_loadsprite_is_a_safe_no_op_with_no_gui);
    RUN(test_setsprite_without_quote_reports_error);
    RUN(test_setsprite_of_unknown_name_reports_error_and_leaves_default);
    RUN(test_setsprite_none_is_a_silent_reset_to_default);
    RUN(test_stampsprite_records_position_heading_and_sprite);
    RUN(test_clear_erases_stamps_too);
    RUN(test_loadspritesheet_without_quote_name_reports_error);
    RUN(test_loadspritesheet_without_quote_path_reports_error);
    RUN(test_loadspritesheet_with_zero_cols_reports_error);
    RUN(test_loadspritesheet_with_zero_rows_reports_error);
    RUN(test_loadspritesheet_is_a_safe_no_op_with_no_gui);
    RUN(test_setspriteframe_without_a_sprite_set_reports_error);
    RUN(test_animatesprite_without_a_sprite_set_reports_error);
    RUN(test_animatesprite_without_a_sprite_set_does_not_pause);

    RUN(test_wait_zero_or_negative_returns_immediately);
    RUN(test_wait_pauses_for_at_least_the_requested_duration);

    RUN(test_canvassize_defaults_to_500_by_500);
    RUN(test_setcanvassize_changes_the_size);
    RUN(test_setcanvassize_too_small_reports_error_and_leaves_size_unchanged);
    RUN(test_setcanvassize_too_large_reports_error_and_leaves_size_unchanged);
    RUN(test_setcanvassize_recenters_turtles_and_clears_drawing);
    RUN(test_setcanvassize_changes_the_wrap_boundary);

    RUN(test_pause_is_a_silent_no_op_with_no_gui);
    RUN(test_continue_with_nothing_paused_reports_error);
    RUN(test_co_is_an_alias_for_continue);
    RUN(test_backtrace_at_top_level_shows_no_active_calls);
    RUN(test_backtrace_shows_the_call_stack_innermost_first);
    RUN(test_bt_is_an_alias_for_backtrace);
    RUN(test_exectime_measures_at_least_the_wrapped_waits_duration);

    RUN(test_pos_reads_back_turtle_position);
    RUN(test_heading_reads_back_turtle_heading_without_wrapping);
    RUN(test_heading_can_be_saved_and_restored);
    RUN(test_pos_and_heading_follow_the_current_turtle);
    RUN(test_getx_gety_read_back_single_axes);
    RUN(test_setx_sety_move_along_a_single_axis_only);
    RUN(test_setx_sety_draw_a_line_when_pen_is_down);

    RUN(test_tell_creates_and_switches_turtles);
    RUN(test_turtles_move_independently);
    RUN(test_clear_homes_every_turtle);
    RUN(test_clean_erases_drawing_but_leaves_the_turtle_alone);
    RUN(test_cleartext_is_a_safe_no_op_with_no_history_pane);
    RUN(test_who_reports_the_current_turtle);
    RUN(test_procedures_lists_every_defined_procedure);
    RUN(test_procedures_is_empty_when_none_are_defined);
    RUN(test_names_lists_every_global_variable_not_locals);
    RUN(test_names_reflects_reassignment_not_duplicate_entries);
    RUN(test_setprop_getprop_round_trips_a_number_word_and_list);
    RUN(test_getprop_of_missing_property_is_the_empty_list);
    RUN(test_setprop_on_same_key_overwrites_not_duplicates);
    RUN(test_different_proplist_names_dont_share_properties);
    RUN(test_removeprop_removes_a_property);
    RUN(test_removeprop_of_missing_property_is_a_silent_no_op);
    RUN(test_proplist_lists_alternating_keys_and_values);
    RUN(test_proplist_of_unknown_name_is_the_empty_list);
    RUN(test_setprop_can_use_a_computed_plist_name_and_key);
    RUN(test_tell_out_of_range_reports_error);

    RUN(test_hideturtle_showturtle_toggles_visibility);
    RUN(test_window_mode_is_default_and_allows_offscreen);
    RUN(test_wrap_wraps_position_and_lifts_pen);
    RUN(test_fence_clamps_position_and_reports_error);
    RUN(test_window_command_resets_from_wrap_or_fence);

    RUN(test_procedure_definition_and_call);
    RUN(test_recursion_with_parameter);
    RUN(test_procedure_parameter_accepts_a_list);
    RUN(test_procedure_parameter_accepts_a_word);
    RUN(test_procedure_parameter_still_coerces_arithmetic_to_a_number);
    RUN(test_procedure_parameter_accepts_an_array_and_keeps_its_aliasing);
    RUN(test_procedure_used_as_operator_accepts_a_list_parameter);
    RUN(test_apply_passes_a_word_argument_unchanged);
    RUN(test_parameter_shadows_outer_global);
    RUN(test_make_without_local_mutates_outer_global);
    RUN(test_thing_reads_a_variable_by_a_computed_name);
    RUN(test_thing_matches_colon_name_for_words_lists_and_arrays);
    RUN(test_thing_of_an_unbound_name_is_zero);
    RUN(test_to_redefinition_overwrites);
    RUN(test_erase_removes_procedure);

    RUN(test_output_returns_a_value_used_in_an_expression);
    RUN(test_output_stops_the_rest_of_the_procedure);
    RUN(test_output_escapes_nested_blocks);
    RUN(test_stop_ends_a_procedure_early_and_caller_continues);
    RUN(test_output_used_as_a_plain_statement_discards_the_value);
    RUN(test_calling_a_procedure_that_never_outputs_as_a_value_reports_error);
    RUN(test_output_outside_procedure_reports_error);
    RUN(test_stop_outside_procedure_ends_the_run_silently);
    RUN(test_stop_recovers_cleanly_for_the_next_top_level_run);
    RUN(test_forever_stops_via_stop_directly_at_top_level);
    RUN(test_recursive_procedure_using_output);
    RUN(test_output_value_can_be_a_word_or_list);
    RUN(test_catch_catches_a_matching_throw);
    RUN(test_catch_runs_normally_when_nothing_throws);
    RUN(test_throw_propagates_past_nested_procedure_calls);
    RUN(test_throw_with_non_matching_tag_keeps_propagating);
    RUN(test_throw_without_catch_reports_error_and_recovers_to_top_level);
    RUN(test_uncaught_throw_does_not_stick_around_for_next_command);
    RUN(test_stop_inside_catch_still_escapes_the_enclosing_procedure);
    RUN(test_catch_missing_block_reports_error);

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
    RUN(test_map_with_word_elements);
    RUN(test_filter_keeps_matching_elements);
    RUN(test_filter_with_word_elements);
    RUN(test_reduce_folds_left_to_right);
    RUN(test_reduce_of_single_element_list_is_that_element);
    RUN(test_reduce_concatenates_word_elements);
    RUN(test_foreach_runs_template_per_element);
    RUN(test_foreach_with_word_elements_prints_each_word);

    RUN(test_if_ifelse_comparisons);
    RUN(test_boolean_and_or_not);
    RUN(test_while_loop);
    RUN(test_for_loop_counts_up_with_default_step);
    RUN(test_for_loop_counts_down_with_default_step);
    RUN(test_for_loop_with_explicit_step);
    RUN(test_for_loop_bounds_can_be_expressions);
    RUN(test_for_step_zero_reports_error);
    RUN(test_for_unterminated_header_reports_error);
    RUN(test_for_unterminated_block_reports_error);
    RUN(test_for_iteration_limit_reports_error);
    RUN(test_forever_loop_stops_via_stop);
    RUN(test_forever_unterminated_block_reports_error);
    RUN(test_forever_iteration_limit_reports_error);
    RUN(test_true_false_literals_print_and_store);
    RUN(test_true_false_case_insensitive);
    RUN(test_if_while_treat_the_word_false_as_falsy);

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
    RUN(test_flatten_collapses_every_level_of_nesting);
    RUN(test_flatten_of_an_already_flat_list_is_unchanged);
    RUN(test_flatten_of_empty_list_is_empty);
    RUN(test_flatten_of_a_bare_word_or_number_is_one_element);
    RUN(test_parse_tokenizes_a_word_into_a_one_element_list);
    RUN(test_parse_splits_the_printed_text_of_a_list_on_whitespace);
    RUN(test_single_quote_string_reads_a_raw_multiword_word);
    RUN(test_single_quote_string_composes_with_parse);
    RUN(test_single_quote_string_can_be_empty);
    RUN(test_single_quote_string_missing_closing_quote_reports_error);
    RUN(test_single_quote_string_works_with_make_and_variables);
    RUN(test_subst_replaces_every_matching_element);
    RUN(test_subst_recurses_into_sublists);
    RUN(test_subst_can_replace_a_whole_matching_sublist);
    RUN(test_subst_on_a_bare_value_checks_it_directly);
    RUN(test_dot_product_of_equal_length_lists);
    RUN(test_dot_mismatched_length_reports_error);
    RUN(test_cross_product_of_3element_lists);
    RUN(test_cross_wrong_length_reports_error);
    RUN(test_distance_between_two_points);
    RUN(test_distance_requires_two_2element_lists);
    RUN(test_towards_reports_a_compass_heading_toward_the_point);
    RUN(test_towards_requires_a_2element_list);
    RUN(test_setheading_towards_then_forward_reaches_the_point);

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
    RUN(test_array_create_and_read_default_elements);
    RUN(test_array_size_must_be_at_least_1);
    RUN(test_setitem_mutates_in_place);
    RUN(test_setitem_can_store_a_list_element);
    RUN(test_setitem_index_out_of_range);
    RUN(test_setitem_on_non_array_reports_error);
    RUN(test_setitem_rejects_nesting_an_array);
    RUN(test_fillarray_sets_every_slot_in_one_call);
    RUN(test_fillarray_can_store_a_list_element);
    RUN(test_fillarray_on_non_array_reports_error);
    RUN(test_fillarray_rejects_nesting_an_array);
    RUN(test_array_item_out_of_range_reports_error);
    RUN(test_array_predicates);
    RUN(test_make_b_a_aliases_the_same_mutable_array);
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
    RUN(test_tan_in_degrees);
    RUN(test_asin_acos_report_degrees);
    RUN(test_log_is_base_10_ln_is_natural_log);
    RUN(test_int_truncates_towards_zero);
    RUN(test_random_stays_in_range);
    RUN(test_pick_returns_an_element_actually_in_the_list);
    RUN(test_pick_on_word_and_array);
    RUN(test_pick_on_empty_list_or_word_reports_error);

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
    RUN(test_type_prints_without_trailing_newline);
    RUN(test_show_prints_a_procedures_definition);
    RUN(test_show_undefined_procedure_reports_error);
    RUN(test_text_returns_a_procedures_body_as_a_flat_word_list);
    RUN(test_text_tokens_keep_quote_and_bracket_punctuation_literal);
    RUN(test_text_undefined_procedure_reports_error);
    RUN(test_text_without_quoted_name_reports_error);

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
    RUN(test_loadpic_without_quote_reports_error);
    RUN(test_savepic_without_quote_reports_error);
    RUN(test_loadpic_savepic_are_a_safe_no_op_with_no_gui);

    RUN(test_openwrite_fileprint_close_writes_the_file);
    RUN(test_openread_readline_reads_lines_then_eof);
    RUN(test_openappend_appends_to_existing_content);
    RUN(test_openread_of_missing_file_returns_negative_one);
    RUN(test_close_of_invalid_channel_reports_error);
    RUN(test_fileprint_on_a_read_channel_reports_error);
    RUN(test_readline_and_eof_on_a_closed_channel_are_safe_sentinels);
    RUN(test_deletefile_removes_a_file);
    RUN(test_deletefile_of_missing_file_reports_error);
    RUN(test_deletefile_without_quote_reports_error);
    RUN(test_directory_lists_the_current_working_directory);

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
