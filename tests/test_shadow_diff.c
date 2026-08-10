// test_shadow_diff.c
//
// The migration-testing strategy from docs/BYTECODE_VM_DESIGN.md,
// actually built: runs the same script through the OLD engine
// (eval_logo, text-driven) and the NEW one (logo_lex -> logo_parse ->
// ast_eval) against two separately-initialized LogoApps, and asserts
// they agree -- both on captured text output and on final turtle
// state (FD/RT/etc. don't print anything, so text-output diffing
// alone would miss a real divergence there). A mechanical way to find
// disagreements, rather than trusting a manual re-read of every
// operator to catch every quirk.
//
// The corpus here is deliberately bounded to what parser.c's
// BUILTIN_SIGNATURES + eval.c currently cover -- this isn't "replay
// all of tests/test_interpreter.c" yet (most of it uses operators the
// new engine doesn't implement, like SETPROP/lists/arrays/TELL, which
// would just report parse errors, not real behavioral disagreements).
// Growing this corpus alongside BUILTIN_SIGNATURES/eval.c's own
// coverage is the natural next step once more of the language lands
// on the new engine.
//
// RANDOM is deliberately excluded from the corpus: both engines share
// the exact same random_below function (see interpreter.h), but
// running old-then-new in the same process means the new engine's
// call draws the *next* value from that one continuing RNG stream --
// a real difference, but not an implementation divergence, so
// byte-for-byte diffing it here would be noise, not signal.
//
// Run via `make test-shadow-diff`.

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

// Matches new_app() in test_interpreter.c/test_eval.c exactly -- both
// engines have to start from the identical default state for a diff
// to mean anything.
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
#define RUN(name) do { current_test = #name; name(); } while (0)

#define CHECK(cond) do { \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s: %s (line %d)\n", current_test, #cond, __LINE__); \
    } \
} while (0)

// The turtle state a text-output diff alone can't see (FD/RT/SETXY/
// etc. never print anything). Covers every turtle 0..turtle_count-1,
// not just turtle 0 -- TELL/multi-turtle means turtle_count itself, and
// any non-current turtle's state, are now real dimensions a script can
// diverge on that a turtle-0-only snapshot would silently miss
// entirely (see the TELL/WHO milestone in docs/BYTECODE_VM_DESIGN.md).
typedef struct {
    int turtle_count;
    double x[MAX_TURTLES], y[MAX_TURTLES], angle[MAX_TURTLES];
    int pen_down[MAX_TURTLES];
    double pen_r[MAX_TURTLES], pen_g[MAX_TURTLES], pen_b[MAX_TURTLES], pen_width[MAX_TURTLES];
    int visible[MAX_TURTLES];
} TurtleSnapshot;

static TurtleSnapshot snapshot_turtle(LogoApp *app) {
    TurtleSnapshot s;
    s.turtle_count = app->turtle_count;
    for (int i = 0; i < app->turtle_count; i++) {
        s.x[i] = app->turtles[i].x;
        s.y[i] = app->turtles[i].y;
        s.angle[i] = app->turtles[i].angle;
        s.pen_down[i] = app->turtles[i].pen_down;
        s.pen_r[i] = app->turtles[i].pen_r;
        s.pen_g[i] = app->turtles[i].pen_g;
        s.pen_b[i] = app->turtles[i].pen_b;
        s.pen_width[i] = app->turtles[i].pen_width;
        s.visible[i] = app->turtles[i].visible;
    }
    return s;
}

// Compares every drawn line segment's endpoints AND color/width, not
// just the turtle's final resting position -- two engines could agree
// on where the turtle ends up while disagreeing about the path it took
// to get there (or how many separate segments PENUP/PENDOWN toggling
// should have produced along the way), which final-position-only
// comparison would miss entirely. Color/width are compared now that
// SETPENCOLOR/SETPENWIDTH are real (see the drawing-primitives
// milestone in docs/BYTECODE_VM_DESIGN.md) -- previously skipped since
// nothing in the corpus varied them, so they'd have trivially matched
// either way.
static int lines_match(LogoApp *old_app, LogoApp *new_app_instance) {
    if (old_app->line_count != new_app_instance->line_count) return 0;
    for (int i = 0; i < old_app->line_count; i++) {
        LineSegment *a = &old_app->lines[i];
        LineSegment *b = &new_app_instance->lines[i];
        if (a->x1 != b->x1 || a->y1 != b->y1 || a->x2 != b->x2 || a->y2 != b->y2 ||
            a->r != b->r || a->g != b->g || a->b != b->b || a->width != b->width) return 0;
    }
    return 1;
}

// LABEL's own recorded data -- position, pen color, and text -- same
// "diff the actual data, not just something derived from it" reasoning
// as lines_match.
static int labels_match(LogoApp *old_app, LogoApp *new_app_instance) {
    if (old_app->label_count != new_app_instance->label_count) return 0;
    for (int i = 0; i < old_app->label_count; i++) {
        Label *a = &old_app->labels[i];
        Label *b = &new_app_instance->labels[i];
        if (a->x != b->x || a->y != b->y || a->r != b->r || a->g != b->g || a->b != b->b ||
            strcmp(a->text, b->text) != 0) return 0;
    }
    return 1;
}

// FILL/ERASERECT's own recorded data. sprite_index/sprite_frame/angle
// are STAMPSPRITE-only fields (see RasterOp in logo_types.h) -- always
// their calloc'd zero/default in this corpus, since STAMPSPRITE isn't
// implemented in either engine's BUILTIN_SIGNATURES here, but compared
// anyway rather than assumed equal, matching lines_match/labels_match's
// own "diff the real data" approach.
static int raster_ops_match(LogoApp *old_app, LogoApp *new_app_instance) {
    if (old_app->raster_op_count != new_app_instance->raster_op_count) return 0;
    for (int i = 0; i < old_app->raster_op_count; i++) {
        RasterOp *a = &old_app->raster_ops[i];
        RasterOp *b = &new_app_instance->raster_ops[i];
        if (a->kind != b->kind || a->x != b->x || a->y != b->y ||
            a->r != b->r || a->g != b->g || a->b != b->b ||
            a->w != b->w || a->h != b->h || a->angle != b->angle ||
            a->sprite_index != b->sprite_index || a->sprite_frame != b->sprite_frame ||
            a->line_count_at_call != b->line_count_at_call) return 0;
    }
    return 1;
}

#define MAX_DIFF_TOKENS 512

// Runs `old_source` through the old engine and `new_source` through
// the new one, reporting any disagreement -- in output text, final
// turtle state, or the actual drawn path -- as a test failure. The two
// sources are usually identical (see shadow_diff below, the common
// case); this split version exists for the rare, deliberate exception
// where a feature's *syntax* itself genuinely differs between engines
// (see SEND's own test cases further down) -- there, comparing two
// different-but-equivalent scripts is still a real cross-engine check,
// just not a literal same-text diff.
static void shadow_diff_pair(const char *old_source, const char *new_source) {
    captured_output[0] = '\0';
    LogoApp *old_app = new_app();
    eval_logo(old_app, old_source);
    char old_output[4096];
    snprintf(old_output, sizeof(old_output), "%s", captured_output);
    TurtleSnapshot old_turtle = snapshot_turtle(old_app);

    captured_output[0] = '\0';
    LogoApp *new_app_instance = new_app();
    LogoToken tokens[MAX_DIFF_TOKENS];
    int n = logo_lex(new_source, tokens, MAX_DIFF_TOKENS);
    if (n < 0) {
        failures++;
        printf("FAIL %s: source needed more than %d tokens\n", current_test, MAX_DIFF_TOKENS);
        return;
    }
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    if (result->error_count > 0) {
        failures++;
        printf("FAIL %s: new engine reported %d parse error(s), first: %s\n",
               current_test, result->error_count, result->errors[0].message);
        parse_result_destroy(result);
        return;
    }
    ast_eval(new_app_instance, &result->pool, result->program);
    parse_result_destroy(result);
    char new_output[4096];
    snprintf(new_output, sizeof(new_output), "%s", captured_output);
    TurtleSnapshot new_turtle = snapshot_turtle(new_app_instance);

    if (strcmp(old_output, new_output) != 0) {
        failures++;
        printf("FAIL %s: output text differs\n  old: \"%s\"\n  new: \"%s\"\n",
               current_test, old_output, new_output);
    }
    if (old_turtle.turtle_count != new_turtle.turtle_count) {
        failures++;
        printf("FAIL %s: turtle count differs (old: %d, new: %d)\n",
               current_test, old_turtle.turtle_count, new_turtle.turtle_count);
    } else {
        for (int i = 0; i < old_turtle.turtle_count; i++) {
            if (old_turtle.x[i] != new_turtle.x[i] || old_turtle.y[i] != new_turtle.y[i] ||
                old_turtle.angle[i] != new_turtle.angle[i] || old_turtle.pen_down[i] != new_turtle.pen_down[i] ||
                old_turtle.pen_r[i] != new_turtle.pen_r[i] || old_turtle.pen_g[i] != new_turtle.pen_g[i] ||
                old_turtle.pen_b[i] != new_turtle.pen_b[i] || old_turtle.pen_width[i] != new_turtle.pen_width[i] ||
                old_turtle.visible[i] != new_turtle.visible[i]) {
                failures++;
                printf("FAIL %s: turtle %d state differs\n"
                       "  old: x=%g y=%g angle=%g pen=%d color=(%g,%g,%g) width=%g visible=%d\n"
                       "  new: x=%g y=%g angle=%g pen=%d color=(%g,%g,%g) width=%g visible=%d\n",
                       current_test, i,
                       old_turtle.x[i], old_turtle.y[i], old_turtle.angle[i], old_turtle.pen_down[i],
                       old_turtle.pen_r[i], old_turtle.pen_g[i], old_turtle.pen_b[i], old_turtle.pen_width[i], old_turtle.visible[i],
                       new_turtle.x[i], new_turtle.y[i], new_turtle.angle[i], new_turtle.pen_down[i],
                       new_turtle.pen_r[i], new_turtle.pen_g[i], new_turtle.pen_b[i], new_turtle.pen_width[i], new_turtle.visible[i]);
            }
        }
    }
    if (!lines_match(old_app, new_app_instance)) {
        failures++;
        printf("FAIL %s: drawn lines differ (old: %d segment(s), new: %d segment(s))\n",
               current_test, old_app->line_count, new_app_instance->line_count);
    }
    if (!labels_match(old_app, new_app_instance)) {
        failures++;
        printf("FAIL %s: LABEL data differs (old: %d label(s), new: %d label(s))\n",
               current_test, old_app->label_count, new_app_instance->label_count);
    }
    if (!raster_ops_match(old_app, new_app_instance)) {
        failures++;
        printf("FAIL %s: FILL/ERASERECT data differs (old: %d op(s), new: %d op(s))\n",
               current_test, old_app->raster_op_count, new_app_instance->raster_op_count);
    }
    if (old_app->bg_r != new_app_instance->bg_r || old_app->bg_g != new_app_instance->bg_g ||
        old_app->bg_b != new_app_instance->bg_b) {
        failures++;
        printf("FAIL %s: background color differs\n  old: (%g,%g,%g)\n  new: (%g,%g,%g)\n",
               current_test, old_app->bg_r, old_app->bg_g, old_app->bg_b,
               new_app_instance->bg_r, new_app_instance->bg_g, new_app_instance->bg_b);
    }
    if (old_app->canvas_width != new_app_instance->canvas_width ||
        old_app->canvas_height != new_app_instance->canvas_height) {
        failures++;
        printf("FAIL %s: canvas size differs (old: %gx%g, new: %gx%g)\n",
               current_test, old_app->canvas_width, old_app->canvas_height,
               new_app_instance->canvas_width, new_app_instance->canvas_height);
    }
    if (old_app->edge_mode != new_app_instance->edge_mode) {
        failures++;
        printf("FAIL %s: edge_mode differs (old: %d, new: %d)\n",
               current_test, (int)old_app->edge_mode, (int)new_app_instance->edge_mode);
    }
}

// The common case: the same script, run through both engines.
static void shadow_diff(const char *source) {
    shadow_diff_pair(source, source);
}

TEST(test_print_number_and_word) { shadow_diff("PRINT 42\nPRINT \"hello"); }
TEST(test_arithmetic_precedence) { shadow_diff("PRINT 1 + 2 * 3\nPRINT (1 + 2) * 3"); }
TEST(test_make_and_varref) { shadow_diff("MAKE \"x 5\nMAKE \"y :x + 1\nPRINT :y"); }

TEST(test_make_negative_number_fidelity) {
    // The exact fidelity gap this whole migration strategy exists to
    // catch mechanically: MAKE "x -5 sets x to -5 in both engines
    // (confirmed manually already -- this just makes it a standing,
    // automatic check instead of a one-off manual confirmation).
    shadow_diff("MAKE \"x -5\nPRINT :x");
}

TEST(test_if_ifelse) {
    shadow_diff(
        "IF 1 > 0 [PRINT \"a]\n"
        "IF 1 < 0 [PRINT \"b]\n"
        "IFELSE 1 < 0 [PRINT \"c] ELSE [PRINT \"d]");
}

TEST(test_boolean_not_and_or) {
    shadow_diff(
        "IF NOT 1 = 2 [PRINT \"a]\n"
        "IF 1 = 1 AND 2 = 2 [PRINT \"b]\n"
        "IF 1 = 2 OR 3 = 3 [PRINT \"c]");
}

TEST(test_word_equality_case_insensitive) { shadow_diff("IF \"Hello = \"HELLO [PRINT \"matched]"); }

TEST(test_while_loop) { shadow_diff("MAKE \"i 0\nWHILE :i < 5 [PRINT :i\nMAKE \"i :i + 1]"); }
TEST(test_repeat_loop) { shadow_diff("REPEAT 3 [PRINT \"hi]"); }

TEST(test_turtle_motion) { shadow_diff("FD 100\nRT 90\nFD 50\nLT 45\nBK 20"); }
TEST(test_setxy_setheading) { shadow_diff("SETXY 30 40\nSETHEADING 180"); }
TEST(test_penup_pendown) { shadow_diff("PENUP\nPENDOWN\nPENUP"); }
TEST(test_home_and_clear) { shadow_diff("SETXY 10 10\nSETHEADING 45\nHOME\nCLEAR"); }

TEST(test_tell_and_who_multiple_turtles) {
    shadow_diff(
        "PRINT WHO\n"
        "FD 60\n"
        "TELL 1\n"
        "PRINT WHO\n"
        "SETXY 350 350\n"
        "RT 90\n"
        "TELL 0\n"
        "PRINT WHO\n"
        "PRINT POS\n"
        // Out-of-range TELL reports an error and leaves the current
        // turtle (and every existing turtle's own state) unchanged.
        "TELL 10\n"
        "TELL -1\n"
        "PRINT WHO\n"
        // HOME only resets whichever turtle is current -- switching
        // back to turtle 1 and HOME-ing it here must leave turtle 0's
        // own state (checked via its own TurtleSnapshot slot) alone.
        "TELL 1\n"
        "HOME\n"
        "PRINT POS");
}

// General file I/O -- against build/, same gitignored-scratch-space
// convention as test_interpreter.c/test_eval.c's own file I/O tests.
// Fully self-contained (create, write, read, delete all within one
// script) so running the identical text through both engines in
// sequence is safe: each engine's own OPENWRITE truncates the file
// fresh, and DELETEFILE cleans up at the end either way.
TEST(test_file_io_write_read_append_and_delete) {
    shadow_diff(
        "MAKE \"ch OPENWRITE \"build/test_shadow_diff_fileio.txt\n"
        "FILEPRINT :ch SENTENCE \"first \"line\n"
        "FILEPRINT :ch \"second\n"
        "CLOSE :ch\n"
        "MAKE \"rd OPENREAD \"build/test_shadow_diff_fileio.txt\n"
        "PRINT READLINE :rd\n"
        "PRINT EOF? :rd\n"
        "PRINT READLINE :rd\n"
        "PRINT EOF? :rd\n"
        "CLOSE :rd\n"
        "MAKE \"ap OPENAPPEND \"build/test_shadow_diff_fileio.txt\n"
        "FILEPRINT :ap \"third\n"
        "CLOSE :ap\n"
        "MAKE \"rd2 OPENREAD \"build/test_shadow_diff_fileio.txt\n"
        "PRINT READLINE :rd2\n"
        "PRINT READLINE :rd2\n"
        "PRINT READLINE :rd2\n"
        "CLOSE :rd2\n"
        "CLOSE 99\n"
        "PRINT READLINE 5\n"
        "PRINT EOF? 5\n"
        "FILEPRINT 5 \"nope\n"
        "PRINT MEMBER? \"Makefile DIRECTORY\n"
        "DELETEFILE \"build/test_shadow_diff_fileio.txt\n"
        "DELETEFILE \"build/test_shadow_diff_does_not_exist.txt");
}

// A loaded file's own statements running, including defining and
// calling ITS OWN procedure from within itself.
TEST(test_load_runs_a_files_own_statements) {
    const char *path = "build/test_shadow_diff_load.logo";
    g_file_set_contents(path,
        "TO greet :name\n"
        "  PRINT WORD \"hello- :name\n"
        "END\n"
        "greet \"world\n"
        "PRINT 1 + 1", -1, NULL);
    shadow_diff("PRINT \"before\nLOAD \"build/test_shadow_diff_load.logo\nPRINT \"after");
    remove(path);
}

// The LOAD-cross-boundary-call fix itself: a procedure a LOAD'd file
// defines, called from the loading script's own top-level code -- see
// docs/BYTECODE_VM_DESIGN.md's LOAD-cross-boundary-call-fix milestone
// and test_eval.c's own test_load_defined_procedure_is_callable_from_
// the_loading_script for the full "why this now works" explanation.
TEST(test_load_defined_procedure_is_callable_from_the_loading_script) {
    const char *path = "build/test_shadow_diff_load_callable.logo";
    g_file_set_contents(path, "TO greet :name\n  PRINT WORD \"hello- :name\nEND", -1, NULL);
    shadow_diff("LOAD \"build/test_shadow_diff_load_callable.logo\ngreet \"world");
    remove(path);
}

TEST(test_load_recursively_follows_a_loaded_files_own_load) {
    const char *inner_path = "build/test_shadow_diff_load_recursive_inner.logo";
    const char *outer_path = "build/test_shadow_diff_load_recursive_outer.logo";
    g_file_set_contents(inner_path, "TO inner\n  PRINT \"inner-ran\nEND", -1, NULL);
    g_file_set_contents(outer_path,
        "LOAD \"build/test_shadow_diff_load_recursive_inner.logo\n"
        "TO outer\n"
        "  inner\n"
        "  PRINT \"outer-ran\n"
        "END", -1, NULL);
    shadow_diff("LOAD \"build/test_shadow_diff_load_recursive_outer.logo\nouter\ninner");
    remove(inner_path);
    remove(outer_path);
}

// `a` (defined first, in a loaded file) calling `b` (defined after it,
// in the SAME loaded file) -- old engine doesn't care about hoisting
// order at all (both TO's have already run by the time `a` is actually
// called), but this exact shape is what caught a real one-pass-design
// bug in this engine's own eager LOAD-following before it ever shipped
// (see build_eager_procedures's own comment in parser.c and
// test_eval.c's test_load_forward_reference_within_a_loaded_file_resolves).
TEST(test_load_forward_reference_within_a_loaded_file_resolves) {
    const char *path = "build/test_shadow_diff_load_forward_ref.logo";
    g_file_set_contents(path,
        "TO a\n"
        "  b\n"
        "  PRINT \"a-ran\n"
        "END\n"
        "TO b\n"
        "  PRINT \"b-ran\n"
        "END", -1, NULL);
    shadow_diff("LOAD \"build/test_shadow_diff_load_forward_ref.logo\na");
    remove(path);
}

TEST(test_drawing_and_canvas_primitives) {
    shadow_diff(
        "ARC 360 80\n"
        "RT 45\n"
        "ARC 90 40\n"
        "SETPENCOLOR 10 20 30\n"
        "LABEL \"hi\n"
        "LABEL 42\n"
        "REPEAT 4 [FD 50 RT 90]\n"
        "SETPENCOLOR 200 50 10\n"
        "FILL\n"
        "ERASERECT 30 40\n"
        "WRAP\n"
        "SETXY 490 490\n"
        "SETHEADING 180\n"
        "FD 30\n"
        "FENCE\n"
        "SETXY 490 490\n"
        "SETHEADING 180\n"
        "FD 30\n"
        "WINDOW\n"
        "SETXY 490 490\n"
        "SETHEADING 180\n"
        "FD 30\n"
        "CLEAN\n"
        "HIDETURTLE\n"
        "PRINT 1\n"
        "SHOWTURTLE\n"
        "PRINT 2\n"
        // A parenthesized negative literal here, not a bare one --
        // the pre-existing "greedy subtraction" ambiguity (already
        // documented for SETXY/SETHEADING/MOD) applies to any
        // multi-argument call, including this one: SETPENCOLOR 300
        // -10 128 would parse "300 - 10" as arg 1's own expression in
        // this engine, leaving too few tokens for arg 3.
        "SETPENCOLOR 300 (-10) 128\n"
        "SETPENWIDTH 0.1\n"
        "SETPENWIDTH 100\n"
        "SETBACKGROUND 10 20 30\n"
        "SETCANVASSIZE 800 600\n"
        "FD 10\n"
        "SETCANVASSIZE 10 10");
}

TEST(test_erase_deletes_a_procedure) {
    shadow_diff(
        "TO foo\n"
        "  PRINT 1\n"
        "END\n"
        "TO bar\n"
        "END\n"
        "ERASE \"foo\n"
        "foo\n"
        "ERASE \"nosuch\n"
        "PRINT PROCEDURES");
}

TEST(test_text_returns_a_procedures_body_as_a_flat_word_list) {
    shadow_diff(
        "TO square :size\n"
        "  REPEAT 4 [FD :size RT 90]\n"
        "END\n"
        "TO demo\n"
        "  PRINT \"hi\n"
        "END\n"
        "TO empty\n"
        "END\n"
        "PRINT TEXT \"square\n"
        "PRINT COUNT TEXT \"square\n"
        "PRINT ITEM 1 TEXT \"demo\n"
        "PRINT ITEM 2 TEXT \"demo\n"
        "PRINT COUNT TEXT \"empty\n"
        "PRINT TEXT \"nosuch");
}

TEST(test_show_prints_a_procedures_definition) {
    shadow_diff(
        "TO square :size\n"
        "  REPEAT 4 [FD :size RT 90]\n"
        "END\n"
        "TO empty\n"
        "END\n"
        "SHOW \"square\n"
        "SHOW \"empty\n"
        "SHOW \"nosuch");
}

// shadow_diff itself only compares captured PRINT output/turtle state/
// drawn data -- not arbitrary file content -- so SAVE's own file needs
// a dedicated direct comparison instead of the usual shared shadow_diff
// helper (see the general file I/O tests above for the same reasoning
// applied to OPENWRITE/FILEPRINT).
TEST(test_save_writes_byte_identical_output_to_interpreterc) {
    const char *old_path = "build/test_shadow_diff_save_old.logo";
    const char *new_path = "build/test_shadow_diff_save_new.logo";
    remove(old_path);
    remove(new_path);

    const char *body =
        "TO square :size\n"
        "  REPEAT 4 [FD :size RT 90]\n"
        "END\n"
        "TO greet :name\n"
        "  PRINT WORD \"hello- :name\n"
        "END\n"
        "TO empty\n"
        "END\n"
        "SAVE \"";

    char script_old[512], script_new[512];
    snprintf(script_old, sizeof(script_old), "%s%s", body, old_path);
    snprintf(script_new, sizeof(script_new), "%s%s", body, new_path);

    LogoApp *old_app = new_app();
    eval_logo(old_app, script_old);

    LogoApp *new_app_instance = new_app();
    LogoToken tokens[MAX_DIFF_TOKENS];
    int n = logo_lex(script_new, tokens, MAX_DIFF_TOKENS);
    CHECK(n >= 0);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    CHECK(result->error_count == 0);
    ast_eval(new_app_instance, &result->pool, result->program);
    parse_result_destroy(result);

    char *old_contents = NULL, *new_contents = NULL;
    CHECK(g_file_get_contents(old_path, &old_contents, NULL, NULL));
    CHECK(g_file_get_contents(new_path, &new_contents, NULL, NULL));
    if (old_contents != NULL && new_contents != NULL) {
        CHECK(strcmp(old_contents, new_contents) == 0);
    }
    g_free(old_contents);
    g_free(new_contents);
    remove(old_path);
    remove(new_path);
}

TEST(test_repeat_with_turtle_motion) { shadow_diff("REPEAT 4 [FD 50 RT 90]"); }

TEST(test_procedure_with_output) {
    shadow_diff(
        "TO double :n\n"
        "  OUTPUT :n * 2\n"
        "END\n"
        "PRINT double 21");
}

TEST(test_procedure_forward_reference) {
    shadow_diff(
        "TO square :n\n"
        "  OUTPUT :n * :n\n"
        "END\n"
        "PRINT square 5");
}

TEST(test_recursive_procedure) {
    shadow_diff(
        "TO countdown :n\n"
        "  IF :n = 0 [STOP]\n"
        "  PRINT :n\n"
        "  countdown :n - 1\n"
        "END\n"
        "countdown 3");
}

TEST(test_local_scope_shadows_global) {
    // Not named "setx": that's a genuine built-in in the old engine
    // (sets the turtle's X coordinate) that this lexer/parser's
    // BUILTIN_SIGNATURES doesn't have yet -- confirmed by this shadow
    // diff itself, the first time it ran, silently shadowing the
    // user-defined procedure of the same name in the old engine only.
    // Exactly the category of divergence this whole mechanism exists
    // to catch, but not what this specific test is meant to exercise.
    shadow_diff(
        "MAKE \"x 100\n"
        "TO showvalue :x\n"
        "  PRINT :x\n"
        "END\n"
        "showvalue 1\n"
        "PRINT :x");
}

TEST(test_math_operators) {
    shadow_diff("PRINT ABS -5\nPRINT SQRT 16\nPRINT POWER 2 10\nPRINT ROUND 4.6\nPRINT INT 4.9");
}

TEST(test_trig_log_and_mod_operators) {
    shadow_diff(
        "PRINT SIN 90\nPRINT COS 0\nPRINT TAN 45\n"
        "PRINT ARCTAN 1\nPRINT ASIN 1\nPRINT ACOS 1\n"
        "PRINT LN 1\nPRINT LOG 100\nPRINT EXP 0\nPRINT EXP 1\n"
        // Parenthesized negative arguments -- MOD 7 -3 (no parens) hits
        // the same pre-existing "greedy subtraction" call-argument
        // ambiguity already documented for SETXY/SETHEADING, unrelated
        // to MOD itself (see test_mod_operator in test_eval.c).
        "PRINT MOD 7 3\nPRINT MOD 7 (-3)\nPRINT MOD (-7) 3\nPRINT MOD (-7) (-3)\nPRINT MOD 5 0");
}

TEST(test_turtle_state_queries) {
    shadow_diff(
        "SETXY 30 40\nSETHEADING 45\n"
        "PRINT GETX\nPRINT GETY\nPRINT HEADING\nPRINT POS\n"
        "PRINT CANVASSIZE\n"
        "SETX 99\nPRINT POS\nSETY 55\nPRINT POS\n"
        "PRINT DISTANCE [0 0] [3 4]\n"
        "PRINT DISTANCE 5 [1 2]\n"
        "HOME\n"
        "PRINT TOWARDS [0 100]\nPRINT TOWARDS [100 0]\n"
        // [0 -100]: exercises the parse_list_literal leading-sign fix
        // directly -- a list literal with a glued negative number must
        // stay a 2-element list, not split into 0/-/100 (see
        // test_list_literal_keeps_a_glued_leading_sign_as_one_element in
        // test_eval.c).
        "PRINT TOWARDS [0 -100]\n"
        "PRINT TOWARDS 5\n"
        "SETHEADING TOWARDS [0 100]\nFORWARD DISTANCE POS [0 100]\nPRINT POS");
}

TEST(test_remaining_word_and_list_operators) {
    shadow_diff(
        "PRINT FLATTEN [1 [2 3] [4 [5 6]] 7]\nPRINT FLATTEN 5\n"
        "PRINT PARSE 'hello world'\nPRINT COUNT PARSE 'hello world'\n"
        "PRINT COUNT PARSE [a b [c d] e]\nPRINT PARSE [a b [c d] e]\n"
        "PRINT PARSE 42\n"
        "PRINT SUBST \"b \"z [a b c b]\n"
        "PRINT SUBST [1 2] \"x [a [1 2] c]\n"
        "PRINT SUBST \"a \"z \"a\nPRINT SUBST \"a \"z \"b\n"
        "PRINT DOT [1 2 3] [4 5 6]\nPRINT DOT [1 2] [1 2 3]\n"
        "PRINT CROSS [1 0 0] [0 1 0]\nPRINT CROSS [1 2] [1 2 3]\n"
        // PICK draws from the shared RNG stream, so only single-element
        // containers are used here -- the outcome is deterministic
        // regardless of which draw either engine's random_below call
        // happens to make, unlike a real multi-element PICK (see the
        // file comment on RANDOM's own exclusion from this corpus).
        "PRINT PICK [only]\nPRINT PICK \"a\n"
        "PRINT PICK []\nPRINT PICK BUTFIRST \"a\n"
        "MAKE \"arr ARRAY 1\nSETITEM 1 :arr \"solo\nPRINT PICK :arr\n"
        "PRINT PICK 42");
}

TEST(test_variable_and_procedure_introspection) {
    shadow_diff(
        "MAKE \"x 5\nMAKE \"greeting \"hi\nMAKE \"nums [1 2 3]\n"
        "PRINT THING \"x\nPRINT THING WORD \"gree \"ting\nPRINT THING \"nums\nPRINT THING \"nosuch\n"
        "TO uselocal\nLOCAL \"x\nMAKE \"x 99\nOUTPUT :x\nEND\n"
        "PRINT uselocal\nPRINT :x\n"
        "LOCAL \"y\n"
        "TO usetwolocal\nLOCAL \"z\nLOCAL \"z\nMAKE \"z 7\nOUTPUT :z\nEND\n"
        "PRINT usetwolocal\n"
        "MAKE \"a 1\nMAKE \"b 2\nPRINT NAMES\nPRINT COUNT NAMES\n"
        "TO foo\nEND\nTO bar :n\nEND\nPRINT PROCEDURES\nPRINT COUNT PROCEDURES");
}

TEST(test_recursive_procedure_with_turtle_motion) {
    // A fractal-shaped script (real Logo usage, not a synthetic
    // grammar exercise): recursion, arithmetic, and turtle motion
    // together in one script -- if any of those three interact badly
    // in one engine but not the other, this is where it would show.
    shadow_diff(
        "TO spiral :size\n"
        "  IF :size > 100 [STOP]\n"
        "  FD :size\n"
        "  RT 30\n"
        "  spiral :size + 5\n"
        "END\n"
        "spiral 10");
}

TEST(test_nested_repeat_loops) {
    shadow_diff("REPEAT 3 [REPEAT 2 [FD 10 RT 90]\nRT 30]");
}

TEST(test_nested_if_inside_while) {
    shadow_diff(
        "MAKE \"i 0\n"
        "WHILE :i < 6 [\n"
        "  IF :i / 2 * 2 = :i [PRINT \"even] ELSE [PRINT \"odd]\n"
        "  MAKE \"i :i + 1\n"
        "]");
}

TEST(test_multiple_procedures_calling_each_other) {
    // Mutual reference in both directions -- isEven calls isOdd, which
    // is defined *after* it (forward reference), and isOdd calls
    // isEven right back (backward reference in the same call).
    shadow_diff(
        "TO isEven :n\n"
        "  IF :n = 0 [OUTPUT \"TRUE]\n"
        "  OUTPUT isOdd :n - 1\n"
        "END\n"
        "TO isOdd :n\n"
        "  IF :n = 0 [OUTPUT \"FALSE]\n"
        "  OUTPUT isEven :n - 1\n"
        "END\n"
        "PRINT isEven 10\n"
        "PRINT isOdd 10");
}

TEST(test_procedure_with_multiple_parameters) {
    shadow_diff(
        "TO addthree :a :b :c\n"
        "  OUTPUT :a + :b + :c\n"
        "END\n"
        "PRINT addthree 1 2 3");
}

TEST(test_procedure_with_no_parameters) {
    shadow_diff(
        "TO greet\n"
        "  PRINT \"hi\n"
        "END\n"
        "greet\ngreet");
}

TEST(test_nested_procedure_calls_as_arguments) {
    shadow_diff(
        "TO double :n\n"
        "  OUTPUT :n * 2\n"
        "END\n"
        "PRINT double double 5");
}

TEST(test_true_false_literals_in_conditions) {
    shadow_diff("IF TRUE [PRINT \"a]\nIF FALSE [PRINT \"b]\nIF NOT FALSE [PRINT \"c]");
}

TEST(test_comparison_operators_le_ge_ne) {
    shadow_diff(
        "IF 5 <= 5 [PRINT \"a]\n"
        "IF 5 >= 6 [PRINT \"b]\n"
        "IF 5 <> 6 [PRINT \"c]\n"
        "IF 5 <> 5 [PRINT \"d]");
}

TEST(test_boolean_grouping_with_parens) {
    // The resolved "fix boolean grouping" decision from
    // docs/BYTECODE_VM_DESIGN.md -- today's interpreter has no way to
    // write this at all (no grouping for NOT/AND/OR), so there's
    // nothing to diff against here; this just confirms the new
    // engine's own added capability actually runs correctly, the same
    // way test_parser.c already confirms it parses correctly.
    LogoApp *app = new_app();
    captured_output[0] = '\0';
    LogoToken tokens[MAX_DIFF_TOKENS];
    int n = logo_lex("IF NOT (1 = 2 OR 3 = 3) [PRINT \"a] ELSE [PRINT \"b]", tokens, MAX_DIFF_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    CHECK(result->error_count == 0);
    ast_eval(app, &result->pool, result->program);
    parse_result_destroy(result);
    CHECK(strcmp(captured_output, "b\n") == 0);
}

TEST(test_stop_without_output_exits_early) {
    shadow_diff(
        "TO firstpositive :a :b\n"
        "  IF :a > 0 [PRINT :a\n  STOP]\n"
        "  PRINT :b\n"
        "END\n"
        "firstpositive -1 99\n"
        "firstpositive 5 99");
}

TEST(test_zero_and_boundary_values) {
    shadow_diff(
        "REPEAT 0 [PRINT \"never]\n"
        "WHILE FALSE [PRINT \"never]\n"
        "PRINT 0 - 0\n"
        "PRINT 0 * 100\n"
        "IF 0 = 0 [PRINT \"zero-equal]");
}

TEST(test_deep_but_safe_recursion) {
    // Well under MAX_SCOPE_DEPTH (200), but NOT well under the old
    // engine's real ASan stack-safety edge for THIS specific pattern --
    // calling yourself as an expression (OUTPUT sumTo ...) recurses
    // through a longer per-level C-stack chain (parse_factor ->
    // parse_term -> parse_expr -> eval_logo, the "procedure as
    // operator" fallback) than a plain statement-position self-call
    // does. Bisected directly: this old-engine recursion pattern
    // overflows the real stack under AddressSanitizer somewhere
    // between depth 34 and 36 -- a materially lower ceiling than the
    // ~102-186 level range previously documented for statement-
    // position recursion (see the Makefile's own -O1 comment). 20 has
    // real margin below that newly-found edge.
    shadow_diff(
        "TO sumTo :n :acc\n"
        "  IF :n = 0 [OUTPUT :acc]\n"
        "  OUTPUT sumTo :n - 1 :acc + :n\n"
        "END\n"
        "PRINT sumTo 20 0");
}

TEST(test_setxy_negative_coordinates_and_large_heading) {
    // Two adjacent negative-number arguments need the second one
    // parenthesized: SETXY -50 -75 (no parens) greedily parses as
    // SETXY's *first* argument being -50 - 75 = -125 (unary then
    // binary minus, the same grammar both engines share), leaving
    // whatever statement follows to become the *second* argument's
    // expression instead -- harmless in the old engine (a bareword
    // there just silently reads as 0), but exercises this evaluator's
    // already-documented AST_CALL-in-expression-position gap if that
    // next statement happens to be a callable built-in (confirmed
    // directly: this exact unparenthesized script reports "SETHEADING:
    // didn't output a value" here, for precisely that reason). Not
    // what this test is meant to exercise, so sidestepped with parens
    // rather than silently working around it.
    shadow_diff("SETXY -50 (-75)\nSETHEADING 450\nFD 20");
}

TEST(test_list_literal_print_flat_and_nested) {
    shadow_diff("MAKE \"x [1 2 3]\nPRINT :x\nMAKE \"y [a [b c] d]\nPRINT :y");
}

TEST(test_first_butfirst_last_butlast_list_and_word) {
    shadow_diff(
        "MAKE \"x [10 20 30]\n"
        "PRINT FIRST :x\n"
        "PRINT BUTFIRST :x\n"
        "PRINT LAST :x\n"
        "PRINT BUTLAST :x\n"
        "PRINT FIRST \"hello\n"
        "PRINT BUTFIRST \"hello\n"
        "PRINT LAST \"hello\n"
        "PRINT BUTLAST \"hello");
}

TEST(test_count_and_empty_list_and_word) {
    shadow_diff(
        "MAKE \"x [1 2 3 4]\n"
        "PRINT COUNT :x\n"
        "PRINT COUNT \"hello\n"
        "PRINT EMPTY? []\n"
        "PRINT EMPTY? :x");
}

TEST(test_fput_lput_word_sentence_list) {
    shadow_diff(
        "MAKE \"x [2 3]\n"
        "PRINT FPUT 1 :x\n"
        "PRINT LPUT 4 :x\n"
        "PRINT WORD \"hello \"world\n"
        "PRINT SENTENCE [1 2] [3 4]\n"
        "PRINT LIST [1 2] [3 4]");
}

TEST(test_list_as_procedure_argument_and_output) {
    shadow_diff(
        "TO firstOf :lst\n"
        "  OUTPUT FIRST :lst\n"
        "END\n"
        "PRINT firstOf [7 8 9]");
}

TEST(test_list_equality_comparison) {
    shadow_diff(
        "MAKE \"x [1 2 3]\n"
        "MAKE \"y [1 2 3]\n"
        "MAKE \"z [1 2 4]\n"
        "IF :x = :y [PRINT \"same]\n"
        "IF :x = :z [PRINT \"never]\n"
        "IF :x <> :z [PRINT \"different]");
}

TEST(test_list_operators_combined_with_recursion) {
    // A real, common pattern: recursing over a list, one element at a
    // time via FIRST/BUTFIRST, accumulating a result via OUTPUT.
    shadow_diff(
        "TO sumList :lst :acc\n"
        "  IF EMPTY? :lst [OUTPUT :acc]\n"
        "  OUTPUT sumList BUTFIRST :lst :acc + FIRST :lst\n"
        "END\n"
        "PRINT sumList [1 2 3 4 5] 0");
}

TEST(test_setprop_getprop_round_trip) {
    shadow_diff(
        "SETPROP \"turtle1 \"speed 5\n"
        "SETPROP \"turtle1 \"color \"red\n"
        "SETPROP \"turtle1 \"pos [10 20]\n"
        "PRINT GETPROP \"turtle1 \"speed\n"
        "PRINT GETPROP \"turtle1 \"color\n"
        "PRINT GETPROP \"turtle1 \"pos\n"
        "PRINT GETPROP \"turtle1 \"nosuchkey");
}

TEST(test_setprop_overwrite_and_removeprop) {
    shadow_diff(
        "SETPROP \"turtle1 \"color \"red\n"
        "SETPROP \"turtle1 \"color \"blue\n"
        "PRINT GETPROP \"turtle1 \"color\n"
        "REMOVEPROP \"turtle1 \"color\n"
        "PRINT GETPROP \"turtle1 \"color");
}

TEST(test_proplist_and_separate_plist_names) {
    shadow_diff(
        "SETPROP \"turtle1 \"speed 5\n"
        "SETPROP \"turtle1 \"color \"red\n"
        "SETPROP \"turtle2 \"color \"blue\n"
        "PRINT PROPLIST \"turtle1\n"
        "PRINT GETPROP \"turtle2 \"color");
}

TEST(test_property_list_used_as_lightweight_object_state) {
    // A real-shaped use: property lists as per-object state records,
    // combined with a procedure -- the same pattern that motivated
    // this project's own prototype-object feature (NEW/SEND), just
    // via plain SETPROP/GETPROP instead.
    shadow_diff(
        "TO describe :name\n"
        "  PRINT SENTENCE :name SENTENCE \"is GETPROP :name \"color\n"
        "END\n"
        "SETPROP \"apple \"color \"red\n"
        "SETPROP \"banana \"color \"yellow\n"
        "describe \"apple\n"
        "describe \"banana");
}

// --- Prototype-style objects (NEW/SEND) -----------------------------
// SEND's own syntax is a deliberate exception to "same script, both
// engines" (see BUILTIN_SIGNATURES's comment on SEND in parser.c): the
// old engine takes positional arguments (SEND obj "message arg1 ...),
// the new one takes an explicit argument list (SEND obj "message
// [arg1 ...]) because real static parsing can't resolve the old
// engine's mid-parse, value-dependent arity trick. Each test below is
// the same *behavior* expressed as the two engines' own real syntax,
// via shadow_diff_pair, not shadow_diff's usual literal same-text
// comparison.

TEST(test_send_method_registered_directly_on_object) {
    shadow_diff_pair(
        "TO dog_bark :self\n"
        "  PRINT SENTENCE :self \"barks\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"bark \"dog_bark\n"
        "SEND \"dog \"bark",
        "TO dog_bark :self\n"
        "  PRINT SENTENCE :self \"barks\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"bark \"dog_bark\n"
        "SEND \"dog \"bark []");
}

TEST(test_send_finds_method_through_prototype_chain) {
    shadow_diff_pair(
        "TO animal_speak :self\n"
        "  PRINT SENTENCE :self GETPROP :self \"sound\n"
        "END\n"
        "NEW \"animal \"nothing\n"
        "SETPROP \"animal \"speak \"animal_speak\n"
        "SETPROP \"animal \"sound \"generic\n"
        "NEW \"dog \"animal\n"
        "SETPROP \"dog \"sound \"Woof\n"
        "SEND \"dog \"speak\n"
        "SEND \"animal \"speak",
        "TO animal_speak :self\n"
        "  PRINT SENTENCE :self GETPROP :self \"sound\n"
        "END\n"
        "NEW \"animal \"nothing\n"
        "SETPROP \"animal \"speak \"animal_speak\n"
        "SETPROP \"animal \"sound \"generic\n"
        "NEW \"dog \"animal\n"
        "SETPROP \"dog \"sound \"Woof\n"
        "SEND \"dog \"speak []\n"
        "SEND \"animal \"speak []");
}

TEST(test_send_passes_extra_message_arguments) {
    shadow_diff_pair(
        "TO animal_greet :self :name\n"
        "  PRINT SENTENCE :self SENTENCE \"hi :name\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"greet \"animal_greet\n"
        "SEND \"dog \"greet \"alice",
        "TO animal_greet :self :name\n"
        "  PRINT SENTENCE :self SENTENCE \"hi :name\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"greet \"animal_greet\n"
        "SEND \"dog \"greet [alice]");
}

TEST(test_send_operator_form_captures_output) {
    shadow_diff_pair(
        "TO dog_getname :self\n"
        "  OUTPUT :self\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"getname \"dog_getname\n"
        "PRINT SEND \"dog \"getname",
        "TO dog_getname :self\n"
        "  OUTPUT :self\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"getname \"dog_getname\n"
        "PRINT SEND \"dog \"getname []");
}

TEST(test_send_to_unknown_message_reports_does_not_understand) {
    shadow_diff_pair(
        "NEW \"dog \"nothing\nSEND \"dog \"bark",
        "NEW \"dog \"nothing\nSEND \"dog \"bark []");
}

TEST(test_send_object_used_as_lightweight_class_hierarchy) {
    // A real-shaped multi-level hierarchy: animal -> dog -> puppy,
    // combining SEND with regular arithmetic and a second message.
    shadow_diff_pair(
        "TO animal_speak :self\n"
        "  PRINT SENTENCE :self GETPROP :self \"sound\n"
        "END\n"
        "TO animal_legs :self\n"
        "  OUTPUT 4\n"
        "END\n"
        "NEW \"animal \"nothing\n"
        "SETPROP \"animal \"speak \"animal_speak\n"
        "SETPROP \"animal \"sound \"generic\n"
        "SETPROP \"animal \"legs \"animal_legs\n"
        "NEW \"dog \"animal\n"
        "SETPROP \"dog \"sound \"Woof\n"
        "NEW \"puppy \"dog\n"
        "SEND \"puppy \"speak\n"
        "PRINT SEND \"puppy \"legs",
        "TO animal_speak :self\n"
        "  PRINT SENTENCE :self GETPROP :self \"sound\n"
        "END\n"
        "TO animal_legs :self\n"
        "  OUTPUT 4\n"
        "END\n"
        "NEW \"animal \"nothing\n"
        "SETPROP \"animal \"speak \"animal_speak\n"
        "SETPROP \"animal \"sound \"generic\n"
        "SETPROP \"animal \"legs \"animal_legs\n"
        "NEW \"dog \"animal\n"
        "SETPROP \"dog \"sound \"Woof\n"
        "NEW \"puppy \"dog\n"
        "SEND \"puppy \"speak []\n"
        "PRINT SEND \"puppy \"legs []");
}

TEST(test_array_create_setitem_fillarray_item) {
    shadow_diff(
        "MAKE \"a ARRAY 3\n"
        "SETITEM 1 :a \"x\n"
        "SETITEM 2 :a 42\n"
        "SETITEM 3 :a [1 2 3]\n"
        "PRINT :a\n"
        "PRINT ITEM 1 :a\n"
        "PRINT ITEM 2 :a\n"
        "PRINT ITEM 3 :a\n"
        "MAKE \"b ARRAY 2\n"
        "FILLARRAY :b \"z\n"
        "PRINT :b");
}

TEST(test_array_aliasing_through_make_and_procedure_arguments) {
    // Arrays are this language's one mutable value -- MAKE "b :a and
    // passing an array into a procedure parameter both alias the same
    // storage rather than copying it, in both engines.
    shadow_diff(
        "TO mutate :a\n"
        "  SETITEM 1 :a 777\n"
        "END\n"
        "MAKE \"arr ARRAY 3\n"
        "MAKE \"alias :arr\n"
        "SETITEM 1 :alias 99\n"
        "PRINT ITEM 1 :arr\n"
        "mutate :arr\n"
        "PRINT ITEM 1 :alias");
}

TEST(test_array_errors_match_between_engines) {
    shadow_diff("MAKE \"a ARRAY 0");
    shadow_diff("MAKE \"a ARRAY 3\nSETITEM 0 :a \"x");
    shadow_diff("MAKE \"a ARRAY 3\nPRINT ITEM 5 :a");
    shadow_diff("SETITEM 1 [1 2 3] \"x");
    shadow_diff("MAKE \"a ARRAY 2\nMAKE \"b ARRAY 2\nSETITEM 1 :a :b");
    shadow_diff("FILLARRAY [1 2 3] \"x");
    shadow_diff("MAKE \"a ARRAY 2\nMAKE \"b ARRAY 2\nFILLARRAY :a :b");
}

TEST(test_type_predicates_on_every_value_kind) {
    shadow_diff(
        "MAKE \"w \"hi\n"
        "MAKE \"n 5\n"
        "MAKE \"l [1 2 3]\n"
        "MAKE \"a ARRAY 2\n"
        "PRINT WORD? :w\nPRINT LIST? :w\nPRINT NUMBER? :w\nPRINT ARRAY? :w\n"
        "PRINT WORD? :n\nPRINT LIST? :n\nPRINT NUMBER? :n\nPRINT ARRAY? :n\n"
        "PRINT WORD? :l\nPRINT LIST? :l\nPRINT NUMBER? :l\nPRINT ARRAY? :l\n"
        "PRINT WORD? :a\nPRINT LIST? :a\nPRINT NUMBER? :a\nPRINT ARRAY? :a");
}

TEST(test_memberp_on_list_word_and_number) {
    shadow_diff(
        "PRINT MEMBER? \"b [a b c]\n"
        "PRINT MEMBER? \"z [a b c]\n"
        "PRINT MEMBER? 2 [1 2 3]\n"
        "PRINT MEMBER? 5 5\n"
        "PRINT MEMBER? 5 6\n"
        "PRINT MEMBER? \"ell \"hello\n"
        "PRINT MEMBER? \"xyz \"hello");
}

TEST(test_map_filter_reduce_over_numbers_and_words) {
    shadow_diff(
        "PRINT MAP [? * 2] [1 2 3]\n"
        "PRINT MAP [? + 1] 5\n"
        "PRINT MAP [COUNT ?] [[1 2] [3 4 5]]\n"
        "PRINT MAP [FIRST ?] [foo bar]\n"
        "PRINT FILTER [? > 2] [1 2 3 4]\n"
        // Not parenthesized -- FILTER's template goes through the same
        // parse_condition grammar as IF/WHILE, and the old engine has
        // no ( ... ) boolean grouping at all (see the resolved boolean-
        // grouping decision in docs/BYTECODE_VM_DESIGN.md), so a
        // grouped version here would diverge for a reason that has
        // nothing to do with FILTER itself.
        "PRINT FILTER [? > 2 AND ? < 5] [1 2 3 4 5 6]\n"
        // Exercises parse_list_literal's quoted-word fidelity fix: "b
        // inside this template must keep its literal " (see
        // test_filter_with_word_elements in test_eval.c) for the
        // element-substitution round-trip to compare correctly at all.
        "PRINT FILTER [? = \"b] [a b c b]\n"
        "PRINT REDUCE [?1 + ?2] [1 2 3 4]\n"
        "PRINT REDUCE [?1 + ?2] [5]\n"
        "PRINT REDUCE [WORD ?1 ?2] [a b c]");
}

TEST(test_foreach_runs_a_statement_template_per_element) {
    shadow_diff(
        "FOREACH [PRINT WORD ? \"!] [a b c]\n"
        "FOREACH [PRINT ?] 5\n"
        // Exercises parse_list_literal's :varref fidelity fix directly
        // -- :sum inside the template must keep its leading : across
        // each re-parse, or the running total reads back as an
        // unrelated bareword that's always 0 (see
        // test_foreach_accumulates_via_make in test_eval.c).
        "MAKE \"sum 0\n"
        "FOREACH [MAKE \"sum :sum + ?] [1 2 3 4]\n"
        "PRINT :sum\n"
        "FOREACH [IF ? = \"b [PRINT \"match]] [a b c]\n"
        "FOREACH [IF ? = 3 [STOP] PRINT ?] [1 2 3 4 5]\n"
        "FOREACH [PRINT FIRST ?] [[10 20] [30 40]]");
}

TEST(test_for_and_forever_loops) {
    shadow_diff(
        "FOR [i 1 3] [PRINT :i]\n"
        // Exercises FOR's header accepting a full expression, not just
        // a literal, for its limit -- confirmed directly against
        // interpreter.c that this is genuinely supported (see
        // test_for_limit_is_a_full_expression_not_just_a_literal in
        // test_eval.c).
        "MAKE \"n 2\n"
        "FOR [i 1 :n + 1] [PRINT :i]\n"
        "FOR [i 0 10 5] [PRINT :i]\n"
        "FOR [i 3 1] [PRINT :i]\n"
        "FOR [i 1 3 0] [PRINT :i]\n"
        "MAKE \"j 0\n"
        "FOREVER [MAKE \"j :j + 1 PRINT :j IF :j = 3 [STOP]]");
}

TEST(test_run_and_apply_deferred_execution) {
    shadow_diff(
        "RUN [PRINT 1 + 2]\n"
        "MAKE \"x [RUN :x]\n"
        "RUN :x\n" // self-referential -- capped by run_depth, not a crash
        "PRINT 1\n"
        "TO add2 :a :b\n"
        "  PRINT :a + :b\n"
        "END\n"
        "APPLY \"add2 [3 4]\n"
        "APPLY \"nosuch [1 2]\n"
        "APPLY \"add2 [1 2 3]");
}

TEST(test_catch_and_throw_unwind_correctly) {
    shadow_diff(
        "CATCH \"err [PRINT 1 THROW \"err PRINT 2]\n"
        "PRINT 3\n"
        // A non-matching tag propagates past this CATCH, and (since
        // nothing else catches it either) is recovered at the genuine
        // top level -- reported, then execution keeps going with the
        // rest of the script, matching interpreter.c's own eval_depth
        // == 1 recovery exactly.
        "CATCH \"other [THROW \"err]\n"
        "PRINT 99\n"
        "THROW \"nope\n"
        "PRINT 4");
}

int main(void) {
    RUN(test_print_number_and_word);
    RUN(test_arithmetic_precedence);
    RUN(test_make_and_varref);
    RUN(test_make_negative_number_fidelity);
    RUN(test_if_ifelse);
    RUN(test_boolean_not_and_or);
    RUN(test_word_equality_case_insensitive);
    RUN(test_while_loop);
    RUN(test_repeat_loop);
    RUN(test_turtle_motion);
    RUN(test_setxy_setheading);
    RUN(test_penup_pendown);
    RUN(test_home_and_clear);
    RUN(test_tell_and_who_multiple_turtles);
    RUN(test_file_io_write_read_append_and_delete);
    RUN(test_load_runs_a_files_own_statements);
    RUN(test_load_defined_procedure_is_callable_from_the_loading_script);
    RUN(test_load_recursively_follows_a_loaded_files_own_load);
    RUN(test_load_forward_reference_within_a_loaded_file_resolves);
    RUN(test_drawing_and_canvas_primitives);
    RUN(test_erase_deletes_a_procedure);
    RUN(test_text_returns_a_procedures_body_as_a_flat_word_list);
    RUN(test_show_prints_a_procedures_definition);
    RUN(test_save_writes_byte_identical_output_to_interpreterc);
    RUN(test_repeat_with_turtle_motion);
    RUN(test_procedure_with_output);
    RUN(test_procedure_forward_reference);
    RUN(test_recursive_procedure);
    RUN(test_local_scope_shadows_global);
    RUN(test_math_operators);
    RUN(test_trig_log_and_mod_operators);
    RUN(test_turtle_state_queries);
    RUN(test_remaining_word_and_list_operators);
    RUN(test_variable_and_procedure_introspection);
    RUN(test_recursive_procedure_with_turtle_motion);
    RUN(test_nested_repeat_loops);
    RUN(test_nested_if_inside_while);
    RUN(test_multiple_procedures_calling_each_other);
    RUN(test_procedure_with_multiple_parameters);
    RUN(test_procedure_with_no_parameters);
    RUN(test_nested_procedure_calls_as_arguments);
    RUN(test_true_false_literals_in_conditions);
    RUN(test_comparison_operators_le_ge_ne);
    RUN(test_boolean_grouping_with_parens);
    RUN(test_stop_without_output_exits_early);
    RUN(test_zero_and_boundary_values);
    RUN(test_deep_but_safe_recursion);
    RUN(test_setxy_negative_coordinates_and_large_heading);
    RUN(test_list_literal_print_flat_and_nested);
    RUN(test_first_butfirst_last_butlast_list_and_word);
    RUN(test_count_and_empty_list_and_word);
    RUN(test_fput_lput_word_sentence_list);
    RUN(test_list_as_procedure_argument_and_output);
    RUN(test_list_equality_comparison);
    RUN(test_list_operators_combined_with_recursion);
    RUN(test_setprop_getprop_round_trip);
    RUN(test_setprop_overwrite_and_removeprop);
    RUN(test_proplist_and_separate_plist_names);
    RUN(test_property_list_used_as_lightweight_object_state);
    RUN(test_send_method_registered_directly_on_object);
    RUN(test_send_finds_method_through_prototype_chain);
    RUN(test_send_passes_extra_message_arguments);
    RUN(test_send_operator_form_captures_output);
    RUN(test_send_to_unknown_message_reports_does_not_understand);
    RUN(test_send_object_used_as_lightweight_class_hierarchy);
    RUN(test_array_create_setitem_fillarray_item);
    RUN(test_array_aliasing_through_make_and_procedure_arguments);
    RUN(test_array_errors_match_between_engines);
    RUN(test_type_predicates_on_every_value_kind);
    RUN(test_memberp_on_list_word_and_number);
    RUN(test_map_filter_reduce_over_numbers_and_words);
    RUN(test_foreach_runs_a_statement_template_per_element);
    RUN(test_for_and_forever_loops);
    RUN(test_run_and_apply_deferred_execution);
    RUN(test_catch_and_throw_unwind_correctly);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
