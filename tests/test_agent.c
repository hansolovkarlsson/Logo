// test_agent.c
//
// Phase 6's own first slice (see docs/CONCURRENT_AGENTS_DESIGN.md):
// MultiLogo-style concurrent turtle agents. Fully headless -- agent.c
// never touches GTK by design (see agent.h's own file comment), so
// these tests call scheduler_run directly, the same style test_vm.c's
// own headless suspend/resume tests already use. No shadow-diff here:
// no ast_eval equivalent will ever exist for LAUNCH/AWAIT/YIELD.
//
// Run via `make test-agent`.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/agent.h"
#include "../src/compiler.h"
#include "../src/interpreter.h"
#include "../src/lexer.h"
#include "../src/parser.h"
#include "../src/vm.h"

static int failures = 0;
static const char *current_test = "";
static char captured_output[4096];

static void capture_sink(LogoApp *app, const char *text) {
    (void)app;
    strncat(captured_output, text, sizeof(captured_output) - strlen(captured_output) - 1);
}

// Matches test_vm.c's own new_app() exactly.
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

#define MAX_AGENT_TEST_TOKENS 512

static void expect_output(const char *want) {
    if (strcmp(captured_output, want) != 0) {
        failures++;
        printf("FAIL %s: output -- expected \"%s\", got \"%s\"\n", current_test, want, captured_output);
    }
}

// Compiles and runs `source` against `app`, exactly mirroring
// ui.c's own run_logo_script: an ordinary VM_RUN_HALTED result just
// means nothing was ever LAUNCHed, nothing more to do. A
// VM_RUN_SUSPENDED_LAUNCH result means the script itself needs to
// become the scheduler's own initial Agent -- capturing its own
// already-live scope/throw/run_depth/turtle state (exactly as it
// stood the moment LAUNCH first suspended it) before handing off to
// scheduler_run, which owns and frees the initial Agent (and every
// other Agent it creates) by the time it returns. Any OTHER suspend
// reason at the true top level (WAIT/PAUSE/etc used outside any
// agent) is a test-script bug, not something these tests exercise.
static void run_agent_script(LogoApp *app, const char *source) {
    captured_output[0] = '\0';
    LogoToken tokens[MAX_AGENT_TEST_TOKENS];
    int n = logo_lex(source, tokens, MAX_AGENT_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    if (result->error_count > 0) {
        failures++;
        printf("FAIL %s: parse reported %d error(s), first: %s\n",
               current_test, result->error_count, result->errors[0].message);
        parse_result_destroy(result);
        return;
    }
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));
    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);

    if (status == VM_RUN_SUSPENDED_LAUNCH) {
        Agent *initial_agent = calloc(1, sizeof(Agent));
        initial_agent->vm = *vm; // includes vm's own scopes/scope_depth already -- nothing left to copy separately, see agent.h's own comment
        free(vm);
        initial_agent->turtle_index = app->current_turtle;
        initial_agent->throw_requested = app->throw_requested;
        snprintf(initial_agent->throw_tag, sizeof(initial_agent->throw_tag), "%s", app->throw_tag);
        initial_agent->run_depth = app->run_depth;
        initial_agent->state = AGENT_READY;
        initial_agent->started = TRUE;
        scheduler_run(app, &result->pool, chunk, initial_agent);
    } else if (status != VM_RUN_HALTED) {
        failures++;
        printf("FAIL %s: unexpected top-level suspend (VmRunResult %d), not something this test exercises\n", current_test, status);
        free(vm);
    } else {
        free(vm);
    }
    free(chunk);
    parse_result_destroy(result);
}

TEST(test_launch_runs_a_procedure_to_completion) {
    LogoApp *app = new_app();
    run_agent_script(app,
        "TO walker\n"
        "  PRINT \"walker-ran\n"
        "END\n"
        "LAUNCH \"walker []\n"
        "AWAIT\n"
        "PRINT \"main-done");
    expect_output("walker-ran\nmain-done\n");
    free(app);
}

TEST(test_await_blocks_until_every_launched_agent_finishes) {
    // Order matters here: both agents' own output must appear BEFORE
    // "main-done", confirming AWAIT genuinely blocks the caller rather
    // than letting it race ahead.
    LogoApp *app = new_app();
    run_agent_script(app,
        "TO a\n  PRINT \"a-ran\nEND\n"
        "TO b\n  PRINT \"b-ran\nEND\n"
        "LAUNCH \"a []\n"
        "LAUNCH \"b []\n"
        "AWAIT\n"
        "PRINT \"main-done");
    expect_output("a-ran\nb-ran\nmain-done\n");
    free(app);
}

TEST(test_launch_of_unknown_procedure_reports_an_error) {
    LogoApp *app = new_app();
    run_agent_script(app, "LAUNCH \"nosuch []\nPRINT \"main-done");
    if (strstr(captured_output, "LAUNCH: no such procedure \"nosuch") == NULL) {
        failures++;
        printf("FAIL %s: expected \"no such procedure\", got \"%s\"\n", current_test, captured_output);
    }
    free(app);
}

TEST(test_launch_with_wrong_argument_count_reports_an_error) {
    LogoApp *app = new_app();
    run_agent_script(app,
        "TO greet :name\n  PRINT :name\nEND\n"
        "LAUNCH \"greet []\nPRINT \"main-done");
    if (strstr(captured_output, "LAUNCH: wrong number of inputs for procedure \"greet") == NULL) {
        failures++;
        printf("FAIL %s: expected \"wrong number of inputs\", got \"%s\"\n", current_test, captured_output);
    }
    free(app);
}

TEST(test_launch_passes_arguments_to_the_launched_procedure) {
    LogoApp *app = new_app();
    run_agent_script(app,
        "TO greet :name\n  PRINT :name\nEND\n"
        "LAUNCH \"greet [Alice]\n"
        "AWAIT\n"
        "PRINT \"main-done");
    expect_output("Alice\nmain-done\n");
    free(app);
}

TEST(test_launch_binds_multiple_arguments_positionally) {
    LogoApp *app = new_app();
    run_agent_script(app,
        "TO add :a :b\n  PRINT :a + :b\nEND\n"
        "LAUNCH \"add [3 4]\n"
        "AWAIT\n"
        "PRINT \"main-done");
    expect_output("7\nmain-done\n");
    free(app);
}

TEST(test_launch_with_a_bare_scalar_binds_it_as_a_single_argument) {
    // Matches APPLY's own convention (see exec_apply): a non-list
    // second argument is treated as one arg, not an error, same as
    // APPLY "proc 5 already works without needing APPLY "proc [5].
    LogoApp *app = new_app();
    run_agent_script(app,
        "TO double_it :n\n  PRINT :n * 2\nEND\n"
        "LAUNCH \"double_it 21\n"
        "AWAIT\n"
        "PRINT \"main-done");
    expect_output("42\nmain-done\n");
    free(app);
}

// The actual bug found while scoping this feature (see
// docs/CONCURRENT_AGENTS_DESIGN.md's own "Agent arguments" section):
// exec_launch never used to push ANY scope for a zero-argument launch,
// so LOCAL used directly inside a launched procedure's own top level
// (not through a further nested call) fell back to silently creating a
// GLOBAL instead of erroring or working correctly -- leaking across
// every agent. Now that every launch gets a real scope pushed (even a
// zero-argument one, via exec_launch's own eval_push_scope_for_call
// call), LOCAL works there exactly like it already does inside an
// ordinary procedure call.
TEST(test_local_works_directly_inside_a_launched_procedures_own_top_level) {
    LogoApp *app = new_app();
    run_agent_script(app,
        "TO worker\n"
        "  LOCAL \"x\n"
        "  MAKE \"x 42\n"
        "  PRINT :x\n"
        "END\n"
        "LAUNCH \"worker []\n"
        "AWAIT");
    expect_output("42\n");
    // The actual proof: if LOCAL had silently fallen back to a global
    // (the bug this fix closes), app->var_count would be 1 (a leaked
    // global "x") instead of 0 -- checked directly at the C level, not
    // by scraping NAMES' own PRINT-rendered text.
    if (app->var_count != 0) {
        failures++;
        printf("FAIL %s: expected 0 globals, got %d -- LOCAL fell back to a global\n", current_test, app->var_count);
    }
    free(app);
}

// The actual point of decision #1 (docs/CONCURRENT_AGENTS_DESIGN.md):
// two agents each suspend (via YIELD) from *inside* a nested procedure
// call, each with its own real local variable (helper's own :label
// parameter -- genuine app->scopes[] state, not a global) still live
// on its own private scope stack at the moment of interleaving. If the
// save/restore swap were broken, agent_b's own call to helper "B would
// push onto whatever agent_a's own suspended call left behind in the
// shared app->scopes[], corrupting one or both agents' own eventual
// :label reads.
TEST(test_two_agents_dont_leak_local_variables_into_each_other) {
    LogoApp *app = new_app();
    run_agent_script(app,
        "TO helper :label\n"
        "  YIELD\n"
        "  PRINT WORD \"says- :label\n"
        "END\n"
        "TO agent_a\n  helper \"A\nEND\n"
        "TO agent_b\n  helper \"B\nEND\n"
        "LAUNCH \"agent_a []\n"
        "LAUNCH \"agent_b []\n"
        "AWAIT");
    if (strstr(captured_output, "says-A") == NULL || strstr(captured_output, "says-B") == NULL) {
        failures++;
        printf("FAIL %s: expected both says-A and says-B, got \"%s\"\n", current_test, captured_output);
    }
    free(app);
}

// Same proof, for current_turtle instead of scope storage: two agents
// each move their OWN turtle by a distinct, known amount, YIELDing
// between each step. If turtle isolation were broken (current_turtle
// treated as shared rather than swapped per-turn), one turtle would
// end up having absorbed both agents' own motion while the other never
// moved at all, instead of each ending up the expected distance from
// home.
TEST(test_two_agents_dont_leak_turtle_selection_into_each_other) {
    LogoApp *app = new_app();
    run_agent_script(app,
        "TO mover_a\n"
        "  REPEAT 3 [ FD 10 YIELD ]\n"
        "END\n"
        "TO mover_b\n"
        "  REPEAT 3 [ FD 20 YIELD ]\n"
        "END\n"
        "LAUNCH \"mover_a []\n"
        "LAUNCH \"mover_b []\n"
        "AWAIT");
    if (app->turtle_count != 3) {
        failures++;
        printf("FAIL %s: expected 3 turtles (1 original + 2 launched), got %d\n", current_test, app->turtle_count);
    } else {
        double dist_a = hypot(app->turtles[1].x - home_x(app), app->turtles[1].y - home_y(app));
        double dist_b = hypot(app->turtles[2].x - home_x(app), app->turtles[2].y - home_y(app));
        if (fabs(dist_a - 30.0) > 0.001) {
            failures++;
            printf("FAIL %s: turtle 1 (mover_a) -- expected distance 30 from home, got %g\n", current_test, dist_a);
        }
        if (fabs(dist_b - 60.0) > 0.001) {
            failures++;
            printf("FAIL %s: turtle 2 (mover_b) -- expected distance 60 from home, got %g\n", current_test, dist_b);
        }
    }
    free(app);
}

TEST(test_wait_inside_an_agent_reports_the_deferred_support_error) {
    LogoApp *app = new_app();
    run_agent_script(app,
        "TO waiter\n"
        "  WAIT 1\n"
        "  PRINT \"unreachable\n"
        "END\n"
        "LAUNCH \"waiter []\n"
        "AWAIT\n"
        "PRINT \"main-done");
    if (strstr(captured_output, "not yet supported inside a concurrent agent") == NULL) {
        failures++;
        printf("FAIL %s: expected the deferred-support message, got \"%s\"\n", current_test, captured_output);
    }
    if (strstr(captured_output, "unreachable") != NULL) {
        failures++;
        printf("FAIL %s: agent should have been torn down before reaching PRINT \"unreachable, got \"%s\"\n", current_test, captured_output);
    }
    // AWAIT still resolves once the WAIT-using agent is torn down
    // (AGENT_FINISHED either way) -- the caller isn't stuck forever.
    if (strstr(captured_output, "main-done") == NULL) {
        failures++;
        printf("FAIL %s: expected main-done (AWAIT must still resolve), got \"%s\"\n", current_test, captured_output);
    }
    free(app);
}

// SETSPEED's throttle, unlike WAIT above, doesn't tear the agent down
// -- it's an automatic per-step delay the script never explicitly
// asked for at this call site (see agent.c's own VM_RUN_SUSPENDED_
// MOTION_DELAY case), so a global SETSPEED must not turn FD into a
// silent agent-killer.
TEST(test_setspeed_inside_an_agent_does_not_tear_it_down) {
    LogoApp *app = new_app();
    run_agent_script(app,
        "SETSPEED 0.2\n"
        "TO mover\n"
        "  FD 10\n"
        "  PRINT \"mover-done\n"
        "END\n"
        "LAUNCH \"mover []\n"
        "AWAIT\n"
        "PRINT \"main-done");
    expect_output("mover-done\nmain-done\n");
    free(app);
}

int main(void) {
    RUN(test_launch_runs_a_procedure_to_completion);
    RUN(test_await_blocks_until_every_launched_agent_finishes);
    RUN(test_launch_of_unknown_procedure_reports_an_error);
    RUN(test_launch_with_wrong_argument_count_reports_an_error);
    RUN(test_launch_passes_arguments_to_the_launched_procedure);
    RUN(test_launch_binds_multiple_arguments_positionally);
    RUN(test_launch_with_a_bare_scalar_binds_it_as_a_single_argument);
    RUN(test_local_works_directly_inside_a_launched_procedures_own_top_level);
    RUN(test_two_agents_dont_leak_local_variables_into_each_other);
    RUN(test_two_agents_dont_leak_turtle_selection_into_each_other);
    RUN(test_wait_inside_an_agent_reports_the_deferred_support_error);
    RUN(test_setspeed_inside_an_agent_does_not_tear_it_down);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
