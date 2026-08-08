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

// Compares every drawn line segment's endpoints, not just the
// turtle's final resting position -- two engines could agree on where
// the turtle ends up while disagreeing about the path it took to get
// there (or how many separate segments PENUP/PENDOWN toggling should
// have produced along the way), which final-position-only comparison
// would miss entirely. Color/width aren't compared: nothing in the
// current corpus varies the pen color/width, so they'd trivially
// match either way -- worth revisiting once SETPENCOLOR/SETPENWIDTH
// join BUILTIN_SIGNATURES.
static int lines_match(LogoApp *old_app, LogoApp *new_app_instance) {
    if (old_app->line_count != new_app_instance->line_count) return 0;
    for (int i = 0; i < old_app->line_count; i++) {
        LineSegment *a = &old_app->lines[i];
        LineSegment *b = &new_app_instance->lines[i];
        if (a->x1 != b->x1 || a->y1 != b->y1 || a->x2 != b->x2 || a->y2 != b->y2) return 0;
    }
    return 1;
}

#define MAX_DIFF_TOKENS 512

// Runs `source` through both engines and reports any disagreement --
// in output text, final turtle state, or the actual drawn path -- as
// a test failure. This is the whole shadow-diff mechanism in one
// function; every TEST below is just a different script fed through
// it.
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
    if (!lines_match(old_app, new_app_instance)) {
        failures++;
        printf("FAIL %s: drawn lines differ (old: %d segment(s), new: %d segment(s))\n",
               current_test, old_app->line_count, new_app_instance->line_count);
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
    free(result);
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

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
