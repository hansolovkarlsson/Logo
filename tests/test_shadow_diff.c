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
// etc. never print anything).
typedef struct {
    double x, y, angle;
    int pen_down;
} TurtleSnapshot;

static TurtleSnapshot snapshot_turtle(LogoApp *app) {
    TurtleSnapshot s;
    s.x = app->turtles[0].x;
    s.y = app->turtles[0].y;
    s.angle = app->turtles[0].angle;
    s.pen_down = app->turtles[0].pen_down;
    return s;
}

#define MAX_DIFF_TOKENS 512

// Runs `source` through both engines and reports any disagreement --
// in output text, or in final turtle state -- as a test failure. This
// is the whole shadow-diff mechanism in one function; every TEST below
// is just a different script fed through it.
static void shadow_diff(const char *source) {
    captured_output[0] = '\0';
    LogoApp *old_app = new_app();
    eval_logo(old_app, source);
    char old_output[4096];
    snprintf(old_output, sizeof(old_output), "%s", captured_output);
    TurtleSnapshot old_turtle = snapshot_turtle(old_app);

    captured_output[0] = '\0';
    LogoApp *new_app_instance = new_app();
    LogoToken tokens[MAX_DIFF_TOKENS];
    int n = logo_lex(source, tokens, MAX_DIFF_TOKENS);
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
        free(result);
        return;
    }
    ast_eval(new_app_instance, &result->pool, result->program);
    free(result);
    char new_output[4096];
    snprintf(new_output, sizeof(new_output), "%s", captured_output);
    TurtleSnapshot new_turtle = snapshot_turtle(new_app_instance);

    if (strcmp(old_output, new_output) != 0) {
        failures++;
        printf("FAIL %s: output text differs\n  old: \"%s\"\n  new: \"%s\"\n",
               current_test, old_output, new_output);
    }
    if (old_turtle.x != new_turtle.x || old_turtle.y != new_turtle.y ||
        old_turtle.angle != new_turtle.angle || old_turtle.pen_down != new_turtle.pen_down) {
        failures++;
        printf("FAIL %s: turtle state differs\n"
               "  old: x=%g y=%g angle=%g pen=%d\n"
               "  new: x=%g y=%g angle=%g pen=%d\n",
               current_test,
               old_turtle.x, old_turtle.y, old_turtle.angle, old_turtle.pen_down,
               new_turtle.x, new_turtle.y, new_turtle.angle, new_turtle.pen_down);
    }
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
    RUN(test_repeat_with_turtle_motion);
    RUN(test_procedure_with_output);
    RUN(test_procedure_forward_reference);
    RUN(test_recursive_procedure);
    RUN(test_local_scope_shadows_global);
    RUN(test_math_operators);
    RUN(test_recursive_procedure_with_turtle_motion);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
