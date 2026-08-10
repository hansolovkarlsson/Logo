// test_vm.c
//
// Stage 2's own shadow-diff corpus (see docs/BYTECODE_VM_DESIGN.md's
// Progress log): runs the same script through ast_eval (Stage 1's
// tree-walker, already proven byte-identical to eval_logo by
// test_shadow_diff.c) and through compile_program+vm_run (Stage 2's
// new compiler+VM), against two separately-initialized LogoApps, and
// asserts their captured output agrees -- diffed against ast_eval, not
// eval_logo directly, since Stage 1's own frontend is already proven;
// this isolates "does the compiler+VM match the tree-walker's
// semantics" as its own checkable question.
//
// The corpus here is bounded to exactly the vertical slice's own
// coverage (see compiler.h's own file comment): literals, arithmetic,
// PRINT, IF/IFELSE/WHILE, MAKE, comparisons/AND/OR/NOT, and procedure
// calls with OUTPUT/STOP -- not full BUILTIN_SIGNATURES parity yet.
//
// Run via `make test-vm`.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/compiler.h"
#include "../src/eval.h"
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

// Matches test_eval.c/test_shadow_diff.c's own new_app() exactly --
// all three engines have to start from the identical default state for
// a diff to mean anything.
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

#define MAX_VM_TEST_TOKENS 512

// Runs `source` through both ast_eval and compile_program+vm_run,
// asserting their captured output agrees.
static void shadow_diff_vm(const char *source) {
    // Parsed independently per engine, into two entirely separate
    // ParseResult/AstPool trees -- not one parse shared by both runs.
    // ERASE (see eval_erase_declare/OP_ERASE) mutates the AST it's
    // given as its whole mechanism, so a single shared pool would let
    // whichever engine runs first contaminate the second one's own
    // read of "does this procedure still exist" -- a real bug this
    // test file's own first ERASE test caught directly (ast_eval's own
    // ERASE was blanking the AST_PROC_DEF node the VM's later run then
    // also needed to see intact), not a VM defect.
    LogoToken tree_tokens[MAX_VM_TEST_TOKENS];
    int tree_n = logo_lex(source, tree_tokens, MAX_VM_TEST_TOKENS);
    if (tree_n < 0) {
        failures++;
        printf("FAIL %s: source needed more than %d tokens\n", current_test, MAX_VM_TEST_TOKENS);
        return;
    }
    ParseResult *tree_result = calloc(1, sizeof(ParseResult));
    logo_parse(tree_tokens, tree_n, tree_result);
    if (tree_result->error_count > 0) {
        failures++;
        printf("FAIL %s: parse reported %d error(s), first: %s\n",
               current_test, tree_result->error_count, tree_result->errors[0].message);
        parse_result_destroy(tree_result);
        return;
    }
    captured_output[0] = '\0';
    LogoApp *tree_app = new_app();
    ast_eval(tree_app, &tree_result->pool, tree_result->program);
    char tree_output[4096];
    snprintf(tree_output, sizeof(tree_output), "%s", captured_output);
    free(tree_app);
    parse_result_destroy(tree_result);

    LogoToken vm_tokens[MAX_VM_TEST_TOKENS];
    int vm_n = logo_lex(source, vm_tokens, MAX_VM_TEST_TOKENS);
    ParseResult *vm_result = calloc(1, sizeof(ParseResult));
    logo_parse(vm_tokens, vm_n, vm_result);
    // vm_result's own error_count was already implicitly checked by
    // tree_result's identical parse above (same source, same grammar,
    // so a parse error here would already have been reported and
    // returned on) -- no need to check again.
    captured_output[0] = '\0';
    LogoApp *vm_app = new_app();
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&vm_result->pool, vm_result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));
    vm_run(vm, vm_app, &vm_result->pool, chunk, start_pc);
    char vm_output[4096];
    snprintf(vm_output, sizeof(vm_output), "%s", captured_output);
    free(chunk);
    free(vm);
    free(vm_app);
    parse_result_destroy(vm_result);

    if (strcmp(tree_output, vm_output) != 0) {
        failures++;
        printf("FAIL %s: output differs\n  tree: \"%s\"\n  vm:   \"%s\"\n",
               current_test, tree_output, vm_output);
    }
}

TEST(test_literals_and_print) {
    shadow_diff_vm("PRINT 42\nPRINT \"hello");
}

TEST(test_arithmetic_with_precedence_and_grouping) {
    shadow_diff_vm("PRINT 1 + 2 * 3\nPRINT (1 + 2) * 3\nPRINT -5 + 2");
}

TEST(test_make_and_varref) {
    shadow_diff_vm("MAKE \"x 5\nMAKE \"y :x + 1\nPRINT :y");
}

TEST(test_make_negative_number) {
    shadow_diff_vm("MAKE \"x -5\nPRINT :x");
}

TEST(test_if_true_and_false_branches) {
    shadow_diff_vm("IF 1 > 0 [PRINT \"positive]\nIF 1 < 0 [PRINT \"negative]");
}

TEST(test_ifelse_takes_the_false_branch) {
    shadow_diff_vm("IFELSE 1 < 0 [PRINT \"yes] ELSE [PRINT \"no]");
}

TEST(test_boolean_not_and_or) {
    shadow_diff_vm(
        "IF NOT 1 = 2 [PRINT \"a]\n"
        "IF 1 = 1 AND 2 = 2 [PRINT \"b]\n"
        "IF 1 = 2 OR 3 = 3 [PRINT \"c]");
}

TEST(test_word_equality_is_case_insensitive) {
    shadow_diff_vm("IF \"Hello = \"HELLO [PRINT \"matched]");
}

TEST(test_while_loop) {
    shadow_diff_vm("MAKE \"i 0\nWHILE :i < 5 [PRINT :i\nMAKE \"i :i + 1]");
}

TEST(test_procedure_with_output) {
    shadow_diff_vm(
        "TO double :n\n"
        "  OUTPUT :n * 2\n"
        "END\n"
        "PRINT double 21");
}

TEST(test_procedure_forward_reference) {
    shadow_diff_vm(
        "square 5\n"
        "TO square :n\n"
        "  OUTPUT :n * :n\n"
        "END\n"
        "PRINT square 5");
}

TEST(test_recursive_procedure) {
    shadow_diff_vm(
        "TO countdown :n\n"
        "  IF :n = 0 [STOP]\n"
        "  PRINT :n\n"
        "  countdown :n - 1\n"
        "END\n"
        "countdown 3");
}

TEST(test_recursion_depth_cap_reports_error_not_a_crash) {
    shadow_diff_vm(
        "TO recur\n"
        "  recur\n"
        "END\n"
        "recur");
}

TEST(test_mutually_recursive_procedures) {
    // Exercises the compiler's own backpatching (see compiler.c's file
    // comment): is_even's own body calls is_odd before is_odd has been
    // compiled, since pass 1 walks pool->nodes[] in array order (TO
    // declaration order here), not call order.
    shadow_diff_vm(
        "TO is_even :n\n"
        "  IF :n = 0 [OUTPUT 1]\n"
        "  OUTPUT is_odd :n - 1\n"
        "END\n"
        "TO is_odd :n\n"
        "  IF :n = 0 [OUTPUT 0]\n"
        "  OUTPUT is_even :n - 1\n"
        "END\n"
        "PRINT is_even 10\n"
        "PRINT is_odd 10");
}

TEST(test_procedure_that_never_outputs_reports_error_in_expression_position) {
    shadow_diff_vm(
        "TO silent\n"
        "  PRINT \"hi\n"
        "END\n"
        "PRINT silent");
}

// Lists/arrays/MAKE-adjacent batch (docs/ROADMAP.md's Stage 2
// checklist, item 2) -- scripts ported directly from tests/test_eval.c
// so this corpus rides on the same already-confirmed-correct ground
// truth, rather than fresh guesses.

TEST(test_list_literal_prints_space_separated_without_brackets) {
    shadow_diff_vm("MAKE \"x [1 2 3]\nPRINT :x");
}

TEST(test_nested_list_literal_prints_with_inner_brackets) {
    shadow_diff_vm("MAKE \"x [a [b c] d]\nPRINT :x");
}

TEST(test_first_butfirst_last_butlast_on_a_list) {
    shadow_diff_vm(
        "MAKE \"x [10 20 30]\n"
        "PRINT FIRST :x\n"
        "PRINT BUTFIRST :x\n"
        "PRINT LAST :x\n"
        "PRINT BUTLAST :x");
}

TEST(test_first_butfirst_last_butlast_on_a_word) {
    shadow_diff_vm("PRINT FIRST \"hello\nPRINT BUTFIRST \"hello\nPRINT LAST \"hello\nPRINT BUTLAST \"hello");
}

TEST(test_count_and_empty) {
    shadow_diff_vm(
        "MAKE \"x [1 2 3 4]\n"
        "PRINT COUNT :x\n"
        "PRINT COUNT \"hello\n"
        "PRINT EMPTY? []\n"
        "PRINT EMPTY? :x");
}

TEST(test_fput_and_lput) {
    shadow_diff_vm(
        "MAKE \"x [2 3]\n"
        "PRINT FPUT 1 :x\n"
        "PRINT LPUT 4 :x");
}

TEST(test_word_sentence_list_constructors) {
    shadow_diff_vm(
        "PRINT WORD \"hello \"world\n"
        "PRINT SENTENCE [1 2] [3 4]\n"
        "PRINT LIST [1 2] [3 4]");
}

TEST(test_list_passed_as_procedure_argument_and_output) {
    shadow_diff_vm(
        "TO firstOf :lst\n"
        "  OUTPUT FIRST :lst\n"
        "END\n"
        "PRINT firstOf [7 8 9]");
}

TEST(test_list_equality_in_condition) {
    shadow_diff_vm(
        "MAKE \"x [1 2 3]\n"
        "MAKE \"y [1 2 3]\n"
        "IF :x = :y [PRINT \"same]");
}

TEST(test_array_creates_cells_defaulting_to_empty_lists) {
    shadow_diff_vm("MAKE \"a ARRAY 3\nPRINT ITEM 1 :a\nPRINT COUNT :a");
}

TEST(test_setitem_and_fillarray) {
    shadow_diff_vm(
        "MAKE \"a ARRAY 2\nSETITEM 1 :a 10\nSETITEM 2 :a 20\nPRINT :a\n"
        "MAKE \"b ARRAY 2\nFILLARRAY :b [1 2]\nPRINT ITEM 1 :b\nPRINT ITEM 2 :b");
}

TEST(test_array_aliasing_through_make_and_procedure_call) {
    shadow_diff_vm(
        "MAKE \"a ARRAY 2\nMAKE \"b :a\nSETITEM 1 :b 99\nPRINT ITEM 1 :a");
}

TEST(test_setitem_used_in_expression_position_reports_error) {
    // SETITEM is void (like MAKE/LOCAL) -- exercises call_builtin's own
    // *produced=0 path through OP_CHECK_OUTPUT, the same mechanism a
    // never-OUTPUTting procedure exercises above, now for a builtin.
    shadow_diff_vm("MAKE \"a ARRAY 1\nPRINT SETITEM 1 :a 5");
}

TEST(test_thing_reads_a_variable_by_computed_name) {
    shadow_diff_vm(
        "MAKE \"x 5\nMAKE \"greeting \"hi\nMAKE \"nums [1 2 3]\n"
        "PRINT THING \"x\nPRINT THING WORD \"gree \"ting\nPRINT THING \"nums\nPRINT THING \"nosuch");
}

TEST(test_local_declares_a_call_scoped_variable) {
    shadow_diff_vm("TO test\nLOCAL \"x\nMAKE \"x 99\nOUTPUT :x\nEND\nMAKE \"x 1\nPRINT test\nPRINT :x");
}

TEST(test_local_outside_a_procedure_reports_an_error) {
    shadow_diff_vm("LOCAL \"y");
}

TEST(test_local_twice_with_the_same_name_is_a_no_op) {
    shadow_diff_vm("TO test2\nLOCAL \"z\nLOCAL \"z\nMAKE \"z 7\nOUTPUT :z\nEND\nPRINT test2");
}

TEST(test_names_lists_every_global_variable) {
    shadow_diff_vm("MAKE \"a 1\nMAKE \"b 2\nPRINT NAMES\nPRINT COUNT NAMES");
}

// Property lists / NEW (docs/ROADMAP.md's Stage 2 checklist, item 2's
// second batch) -- again ported from already-confirmed test_eval.c
// scripts. SEND is deliberately not here -- see eval.h's own note on
// why it's its own follow-up, not part of this batch.

TEST(test_setprop_getprop_round_trips_a_number_word_and_list) {
    shadow_diff_vm(
        "SETPROP \"turtle1 \"speed 5\n"
        "SETPROP \"turtle1 \"color \"red\n"
        "SETPROP \"turtle1 \"pos [10 20]\n"
        "PRINT GETPROP \"turtle1 \"speed\n"
        "PRINT GETPROP \"turtle1 \"color\n"
        "PRINT GETPROP \"turtle1 \"pos");
}

TEST(test_getprop_of_missing_property_is_the_empty_list) {
    shadow_diff_vm("PRINT GETPROP \"turtle1 \"nosuchkey\nPRINT EMPTY? GETPROP \"turtle1 \"nosuchkey");
}

TEST(test_setprop_on_same_key_overwrites_not_duplicates) {
    shadow_diff_vm(
        "SETPROP \"turtle1 \"color \"red\n"
        "SETPROP \"turtle1 \"color \"blue\n"
        "PRINT GETPROP \"turtle1 \"color\n"
        "PRINT COUNT PROPLIST \"turtle1");
}

TEST(test_removeprop_removes_a_property) {
    shadow_diff_vm(
        "SETPROP \"turtle1 \"color \"red\n"
        "REMOVEPROP \"turtle1 \"color\n"
        "PRINT GETPROP \"turtle1 \"color");
}

TEST(test_proplist_lists_alternating_keys_and_values) {
    shadow_diff_vm(
        "SETPROP \"turtle1 \"speed 5\n"
        "SETPROP \"turtle1 \"color \"red\n"
        "PRINT PROPLIST \"turtle1");
}

TEST(test_new_sets_a_prototype_property) {
    shadow_diff_vm("NEW \"dog \"animal\nPRINT GETPROP \"dog \"prototype");
}

// Turtle/drawing commands (docs/ROADMAP.md's Stage 2 checklist, item
// 2's third batch). shadow_diff_vm only diffs captured text output
// (not full turtle-state snapshots the way test_shadow_diff.c does),
// so these lean on the observable getters (GETX/GETY/HEADING/POS/
// CANVASSIZE/WHO) and on error-message paths to get real coverage out
// of a text-only diff; a handful of purely side-effecting commands
// with no getter at all (PENUP/PENDOWN/WRAP/FENCE/WINDOW/SETPENCOLOR/
// SETPENWIDTH/SETBACKGROUND/LABEL/FILL/ARC/ERASERECT) are still
// exercised for "runs without diverging/crashing," just not verified
// beyond that here.

TEST(test_turtle_motion_and_queries) {
    shadow_diff_vm("FD 100\nRT 90\nBK 30\nLT 45\nPRINT GETX\nPRINT GETY\nPRINT HEADING\nPRINT POS");
}

TEST(test_setxy_setheading_setx_sety) {
    shadow_diff_vm("SETXY 50 75\nPRINT POS\nSETHEADING 180\nPRINT HEADING\nSETX 10\nSETY 20\nPRINT POS");
}

TEST(test_distance_and_towards) {
    shadow_diff_vm("PRINT DISTANCE [0 0] [3 4]\nPRINT TOWARDS [10 0]");
}

TEST(test_home_and_clear) {
    shadow_diff_vm("FD 50\nHOME\nPRINT POS\nCLEAR\nPRINT POS");
}

TEST(test_who_and_tell) {
    shadow_diff_vm("PRINT WHO\nTELL 2\nPRINT WHO\nFD 10\nPRINT GETX");
}

TEST(test_tell_out_of_range_reports_error) {
    shadow_diff_vm("TELL 9999");
}

TEST(test_canvassize_and_setcanvassize) {
    shadow_diff_vm("PRINT CANVASSIZE\nSETCANVASSIZE 500 500\nPRINT CANVASSIZE");
}

TEST(test_setcanvassize_out_of_range_reports_error) {
    shadow_diff_vm("SETCANVASSIZE 1 1");
}

TEST(test_pen_and_canvas_appearance_smoke) {
    shadow_diff_vm(
        "PENUP\nPENDOWN\nSETPENCOLOR 300 -10 128\nSETPENWIDTH 100\n"
        "SETBACKGROUND 0 0 0\nWRAP\nFENCE\nWINDOW\nPRINT \"ok");
}

TEST(test_drawing_primitives_smoke) {
    shadow_diff_vm("ARC 90 50\nLABEL \"hi\nFILL\nERASERECT 10 10\nCLEAN\nHIDETURTLE\nSHOWTURTLE\nPRINT \"ok");
}

TEST(test_erase_deletes_a_procedure_so_it_can_no_longer_be_called) {
    // Locks in a real gap the vm.c dispatch had before this batch:
    // exec_call_proc's own "unknown procedure" path (reached here
    // because ERASE blanks the AST_PROC_DEF's own text after the
    // compiler already committed to OP_CALL_PROC for this call) needs
    // to print "I don't know how to foo" AND suppress OP_CHECK_OUTPUT's
    // own generic message, matching do_user_procedure_call exactly.
    shadow_diff_vm("TO foo\nPRINT 1\nEND\nERASE \"foo\nfoo");
}

TEST(test_erase_of_unknown_procedure_reports_error) {
    shadow_diff_vm("ERASE \"nosuch");
}

TEST(test_erase_removes_the_procedure_from_procedures_output) {
    shadow_diff_vm("TO a\nEND\nTO b\nEND\nERASE \"a\nPRINT PROCEDURES");
}

// SEND (docs/ROADMAP.md's Stage 2 checklist, item 2's fourth batch --
// deferred twice before this since its callee is only known at
// runtime, unlike every other call construct; see compiler.c's own
// SEND branch and vm.c's own exec_send). Ported directly from
// test_eval.c's own already-confirmed SEND corpus.

TEST(test_send_calls_a_method_registered_directly_on_the_object) {
    shadow_diff_vm(
        "TO dog_bark :self\n"
        "  PRINT SENTENCE :self \"barks\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"bark \"dog_bark\n"
        "SEND \"dog \"bark []");
}

TEST(test_send_finds_a_method_through_the_prototype_chain) {
    shadow_diff_vm(
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

TEST(test_send_inherits_methods_but_not_data_fields) {
    shadow_diff_vm(
        "TO animal_speak :self\n"
        "  PRINT SENTENCE :self GETPROP :self \"sound\n"
        "END\n"
        "NEW \"animal \"nothing\n"
        "SETPROP \"animal \"speak \"animal_speak\n"
        "NEW \"dog \"animal\n"
        "SETPROP \"dog \"sound \"Woof\n"
        "NEW \"puppy \"dog\n"
        "SEND \"puppy \"speak []");
}

TEST(test_send_passes_extra_message_arguments_after_self) {
    shadow_diff_vm(
        "TO animal_greet :self :name\n"
        "  PRINT SENTENCE :self SENTENCE \"hi :name\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"greet \"animal_greet\n"
        "SEND \"dog \"greet [alice]");
}

TEST(test_send_operator_form_captures_output) {
    shadow_diff_vm(
        "TO dog_getname :self\n"
        "  OUTPUT :self\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"getname \"dog_getname\n"
        "PRINT SEND \"dog \"getname []\n"
        "MAKE \"n SEND \"dog \"getname []\n"
        "PRINT :n");
}

TEST(test_send_operator_form_errors_if_method_never_outputs) {
    shadow_diff_vm(
        "TO dog_bark :self\n"
        "  PRINT \"Woof\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"bark \"dog_bark\n"
        "PRINT SEND \"dog \"bark []");
}

TEST(test_send_to_unknown_message_reports_does_not_understand) {
    shadow_diff_vm("NEW \"dog \"nothing\nSEND \"dog \"bark []");
}

TEST(test_send_on_a_data_property_reports_not_a_method) {
    shadow_diff_vm("NEW \"dog \"nothing\nSETPROP \"dog \"sound \"Woof\nSEND \"dog \"sound []");
}

TEST(test_send_method_without_self_param_reports_error) {
    shadow_diff_vm(
        "TO badmethod\n"
        "  PRINT \"oops\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"bark \"badmethod\n"
        "SEND \"dog \"bark []");
}

TEST(test_send_cyclic_prototype_chain_is_bounded_not_infinite) {
    shadow_diff_vm("NEW \"a \"b\nNEW \"b \"a\nSEND \"a \"speak []");
}

TEST(test_send_wrong_argument_count_reports_error) {
    shadow_diff_vm(
        "TO animal_greet :self :name\n"
        "  PRINT SENTENCE :self SENTENCE \"hi :name\n"
        "END\n"
        "NEW \"dog \"nothing\n"
        "SETPROP \"dog \"greet \"animal_greet\n"
        "SEND \"dog \"greet []");
}

// THROW/CATCH (docs/ROADMAP.md's Stage 2 checklist -- the other
// genuinely hard piece, alongside suspend/resume: THROW's own
// cooperative unwind has to propagate through however many nested
// blocks/loops/procedure calls sit between it and whichever CATCH (or
// the top level) actually stops it. See compiler.c's own compile_block
// and bytecode.h's own OP_CHECK_THROW family for the mechanism
// (forward jumps standing in for the tree-walker's own recursive
// "if (throw_requested) break" checks, since this VM has no C call
// stack to unwind through). The first three cases are ported from
// test_eval.c's own confirmed corpus; the rest are new, specifically
// targeting cross-frame/cross-construct propagation the existing
// tree-walker corpus doesn't happen to exercise.

TEST(test_catch_recovers_when_the_thrown_tag_matches) {
    shadow_diff_vm("CATCH \"err [PRINT 1 THROW \"err PRINT 2]\nPRINT 3");
}

TEST(test_throw_with_no_matching_catch_reports_and_recovers_at_top_level) {
    shadow_diff_vm("CATCH \"other [THROW \"err]\nPRINT 99");
}

TEST(test_uncaught_throw_at_top_level_reports_and_later_statements_still_run) {
    shadow_diff_vm("THROW \"nope\nPRINT 1");
}

TEST(test_throw_propagates_through_nested_procedure_calls) {
    // The core cross-frame case: THROW inside `inner`, called from
    // `outer`, called from CATCH's own block -- has to unwind two VM
    // frames (inner's, then outer's) before CATCH ever sees it.
    shadow_diff_vm(
        "TO inner\n"
        "  THROW \"boom\n"
        "END\n"
        "TO outer\n"
        "  inner\n"
        "  PRINT \"unreachable\n"
        "END\n"
        "CATCH \"boom [outer PRINT \"also_unreachable]\n"
        "PRINT \"done");
}

TEST(test_throw_breaks_a_while_loop_early) {
    shadow_diff_vm(
        "MAKE \"i 0\n"
        "CATCH \"stop [WHILE :i < 10 [PRINT :i MAKE \"i :i + 1 IF :i = 3 [THROW \"stop]]]\n"
        "PRINT \"after");
}

TEST(test_throw_inside_an_if_branch_is_caught_by_enclosing_catch) {
    shadow_diff_vm("CATCH \"x [IF 1 = 1 [THROW \"x] PRINT \"never]\nPRINT \"ok");
}

TEST(test_nested_catch_with_non_matching_inner_tag_propagates_to_outer) {
    shadow_diff_vm("CATCH \"outer_tag [CATCH \"inner_tag [THROW \"outer_tag] PRINT \"never]\nPRINT \"done");
}

TEST(test_procedure_that_throws_in_expression_position_cascades_both_diagnostics) {
    // A procedure used for its value (PRINT risky) that throws instead
    // of outputting: the immediate caller's own OP_CHECK_OUTPUT still
    // fires ("risky: didn't output a value") since resolved stays 1 on
    // this path (same asymmetry OP_CALL_PROC's own recursion-too-deep
    // case has -- see vm.c's own comment), *and* the throw itself keeps
    // propagating past that point, uncaught, to the top level's own
    // recovery.
    shadow_diff_vm(
        "TO risky\n"
        "  THROW \"oops\n"
        "END\n"
        "PRINT risky\n"
        "PRINT \"after");
}

// No "CATCH used in expression position" test: parser.c's own
// try_parse_call deliberately rejects any ARG_BLOCK/ARG_CONDITION
// builtin (CATCH, WHILE, REPEAT, FOREVER) from ever being resolved as
// a value-producing call in the first place (confirmed directly --
// `PRINT CATCH ...` is a genuine parse error, "unknown word: CATCH",
// in both engines, not a VM-specific gap). compile_call's own
// want_value handling for CATCH (and WHILE's, already shipped in an
// earlier batch) is therefore unreachable in practice, kept only for
// structural consistency with every other call form's own uniform
// finish_call tail -- not exercised by this corpus, and can't be,
// short of the parser itself changing.

// REPEAT/FOREVER/FOR (docs/ROADMAP.md's Stage 2 checklist -- a known
// gap noted, not fixed, back in the THROW/CATCH batch). All three need
// persistent loop-control state (a remaining count, an iteration
// counter, FOR's own limit/step) kept on the VM's own value stack via
// the new OP_PEEK, specifically so it survives a *recursive* call
// within the loop body -- see bytecode.h's own OP_PEEK comment. The
// first six cases are ported from test_eval.c's own confirmed corpus;
// the rest are new, targeting fractional counts (REPEAT's own upfront
// truncation), THROW propagating out of FOR, and -- the most important
// property of this whole design -- recursion safety.

TEST(test_repeat_loop) {
    shadow_diff_vm("REPEAT 3 [PRINT \"hi]");
}

TEST(test_forever_runs_until_stop) {
    shadow_diff_vm("MAKE \"i 0\nFOREVER [MAKE \"i :i + 1 PRINT :i IF :i = 3 [STOP]]");
}

TEST(test_for_counts_up_by_default_step) {
    shadow_diff_vm("FOR [i 1 3] [PRINT :i]");
}

TEST(test_for_limit_is_a_full_expression_not_just_a_literal) {
    shadow_diff_vm("MAKE \"n 2\nFOR [i 1 :n + 1] [PRINT :i]");
}

TEST(test_for_with_explicit_step) {
    shadow_diff_vm("FOR [i 0 10 5] [PRINT :i]");
}

TEST(test_for_counts_down_when_limit_is_less_than_start) {
    shadow_diff_vm("FOR [i 3 1] [PRINT :i]");
}

TEST(test_for_step_zero_reports_an_error) {
    shadow_diff_vm("FOR [i 1 3 0] [PRINT :i]");
}

TEST(test_repeat_truncates_a_fractional_count) {
    // REPEAT 3.5 must run exactly 3 times, matching do_repeat's own
    // upfront (int) cast -- not 4, which a naive "decrement until <=0"
    // loop would produce without truncating first.
    shadow_diff_vm("REPEAT 3.5 [PRINT \"x]");
}

TEST(test_throw_breaks_a_for_loop_early) {
    shadow_diff_vm("CATCH \"stop [FOR [i 1 10] [PRINT :i IF :i = 3 [THROW \"stop]]]\nPRINT \"after");
}

TEST(test_repeat_count_survives_a_recursive_call_in_its_own_body) {
    // The core recursion-safety case this whole design (a value-stack
    // slot via OP_PEEK, not a hidden Logo variable) exists for: the
    // SAME compiled REPEAT statement runs recursively (countdown 1's
    // own REPEAT, invoked from inside countdown 2's own REPEAT body),
    // and each invocation's own remaining-count has to stay correct
    // across the other's entire run.
    shadow_diff_vm(
        "TO countdown :n\n"
        "  IF :n = 0 [STOP]\n"
        "  REPEAT 2 [\n"
        "    PRINT :n\n"
        "    countdown :n - 1\n"
        "  ]\n"
        "END\n"
        "countdown 2");
}

TEST(test_for_limit_and_step_survive_a_recursive_call_in_its_own_body) {
    // Same property as above, for FOR's own two persistent stack slots
    // (limit and step) instead of REPEAT's one.
    shadow_diff_vm(
        "TO nested :n\n"
        "  IF :n = 0 [STOP]\n"
        "  FOR [i 1 2] [\n"
        "    PRINT LIST :n :i\n"
        "    nested :n - 1\n"
        "  ]\n"
        "END\n"
        "nested 2");
}

TEST(test_map_transforms_each_element) {
    shadow_diff_vm("PRINT MAP [? * 2] [1 2 3]");
}

TEST(test_map_on_a_bare_number) {
    shadow_diff_vm("PRINT MAP [? + 1] 5");
}

TEST(test_map_preserves_nested_list_elements) {
    // A list-typed element must round-trip through the template with
    // its own structure intact -- compile_template_call's own fast
    // path binds the element's REAL EvalValue directly (no text
    // round-trip at all), so this is a much lower-risk case for the VM
    // than it was for eval.c's own runtime text substitution, but it's
    // still worth confirming byte-for-byte against ast_eval.
    shadow_diff_vm("PRINT MAP [COUNT ?] [[1 2] [3 4 5]]");
}

TEST(test_map_with_word_elements) {
    shadow_diff_vm("PRINT MAP [FIRST ?] [foo bar]");
}

TEST(test_filter_keeps_matching_elements) {
    shadow_diff_vm("PRINT FILTER [? > 2] [1 2 3 4]");
}

TEST(test_filter_with_word_elements) {
    shadow_diff_vm("PRINT FILTER [? = \"b] [a b c b]");
}

TEST(test_reduce_folds_left_to_right) {
    shadow_diff_vm("PRINT REDUCE [?1 + ?2] [1 2 3 4]");
}

TEST(test_reduce_of_single_element_list_is_that_element) {
    shadow_diff_vm("PRINT REDUCE [?1 + ?2] [5]");
}

TEST(test_reduce_concatenates_word_elements) {
    shadow_diff_vm("PRINT REDUCE [WORD ?1 ?2] [a b c]");
}

TEST(test_foreach_runs_template_for_each_element) {
    shadow_diff_vm("FOREACH [PRINT WORD ? \"!] [a b c]");
}

TEST(test_foreach_on_a_bare_number) {
    shadow_diff_vm("FOREACH [PRINT ?] 5");
}

TEST(test_foreach_accumulates_via_make) {
    shadow_diff_vm("MAKE \"sum 0\nFOREACH [MAKE \"sum :sum + ?] [1 2 3 4]\nPRINT :sum");
}

TEST(test_foreach_with_quoted_word_template) {
    shadow_diff_vm("FOREACH [IF ? = \"b [PRINT \"match]] [a b c]");
}

TEST(test_foreach_preserves_nested_list_elements) {
    shadow_diff_vm("FOREACH [PRINT FIRST ?] [[10 20] [30 40]]");
}

TEST(test_map_template_calling_a_user_procedure) {
    // Exercises compile_template_call's own "graft into the main
    // AstPool" mechanism: the template body's own OP_CALL_PROC has to
    // resolve `double` against chunk->procs[], which only exists
    // because the grafted node lives in the same AstPool/chunk as the
    // rest of the program, not a throwaway scratch one.
    shadow_diff_vm(
        "TO double :x\n"
        "  OUTPUT :x * 2\n"
        "END\n"
        "PRINT MAP [double ?] [1 2 3]");
}

TEST(test_map_with_a_runtime_computed_template_uses_the_dynamic_fallback) {
    // The template isn't a literal `[...]` visible at compile time (it's
    // read out of a variable instead), so compile_call's own literal
    // check fails and this takes the ordinary OP_CALL_BUILTIN path
    // straight to eval_map_value at runtime -- compile_template_call's
    // own fast path is never even attempted.
    shadow_diff_vm("MAKE \"tmpl [? * 10]\nPRINT MAP :tmpl [1 2 3]");
}

TEST(test_filter_with_a_runtime_computed_template_uses_the_dynamic_fallback) {
    shadow_diff_vm("MAKE \"tmpl [? > 2]\nPRINT FILTER :tmpl [1 2 3 4]");
}

TEST(test_map_with_a_malformed_literal_template_falls_back_to_the_dynamic_path) {
    // "* 2" alone isn't a valid expression (a binary operator with no
    // left operand) -- compile_template_call discovers this at compile
    // time (the rendered/lexed/parsed template comes back with a parse
    // error) and falls through to the generic dispatch instead of
    // taking its own fast path, landing on exactly the same
    // eval_map_value runtime path a dynamic template would, which
    // reproduces ast_eval's own defensive per-element behavior on a
    // template that's broken.
    shadow_diff_vm("PRINT MAP [* 2] [1 2 3]");
}

TEST(test_nested_reduce_inside_map_each_get_their_own_placeholder) {
    // Exercises compile_template_call's own per-call-site-unique
    // placeholder naming across nesting: the outer MAP's own "?" must
    // never be confused with the inner REDUCE's own "?1"/"?2", even
    // though both compile into the very same chunk. Sums each row.
    shadow_diff_vm("PRINT MAP [REDUCE [?1 + ?2] ?] [[1 2 3] [4 5]]");
}

TEST(test_map_template_placeholder_survives_a_recursive_call_in_its_own_body) {
    // The interesting case this whole design exists for: the SAME
    // compiled MAP call site (same instr->a, same placeholder variable
    // name) is reached again, recursively, while the outer activation's
    // own loop is still mid-iteration (paused inside its own template's
    // evaluation, about to read "?" AFTER the recursive call returns).
    // A single shared placeholder slot that's merely written-then-read
    // isn't enough here -- see vm.c's own save_var/restore_var comment
    // for why (and for the earlier, wrong version of this code that
    // unconditionally deleted the placeholder instead of restoring it,
    // which broke exactly this case).
    // f(0) = 0 (a plain NUMBER, no MAP at all). f(1) sums
    // MAP [(f 0) + ?] [1 2 3] = [1 2 3] -> 6. f(2) reuses the exact
    // same MAP call site again, one level up, over the SAME list, with
    // f(1)'s own result (6) added to each element before f(1) itself
    // has returned to f(2)'s own paused iteration -- REDUCE [+]
    // [7 8 9] = 24.
    shadow_diff_vm(
        "TO f :n\n"
        "  IF :n = 0 [OUTPUT 0]\n"
        "  OUTPUT REDUCE [?1 + ?2] MAP [(f :n - 1) + ?] [1 2 3]\n"
        "END\n"
        "PRINT f 2");
}

int main(void) {
    RUN(test_literals_and_print);
    RUN(test_arithmetic_with_precedence_and_grouping);
    RUN(test_make_and_varref);
    RUN(test_make_negative_number);
    RUN(test_if_true_and_false_branches);
    RUN(test_ifelse_takes_the_false_branch);
    RUN(test_boolean_not_and_or);
    RUN(test_word_equality_is_case_insensitive);
    RUN(test_while_loop);
    RUN(test_procedure_with_output);
    RUN(test_procedure_forward_reference);
    RUN(test_recursive_procedure);
    RUN(test_recursion_depth_cap_reports_error_not_a_crash);
    RUN(test_mutually_recursive_procedures);
    RUN(test_procedure_that_never_outputs_reports_error_in_expression_position);
    RUN(test_list_literal_prints_space_separated_without_brackets);
    RUN(test_nested_list_literal_prints_with_inner_brackets);
    RUN(test_first_butfirst_last_butlast_on_a_list);
    RUN(test_first_butfirst_last_butlast_on_a_word);
    RUN(test_count_and_empty);
    RUN(test_fput_and_lput);
    RUN(test_word_sentence_list_constructors);
    RUN(test_list_passed_as_procedure_argument_and_output);
    RUN(test_list_equality_in_condition);
    RUN(test_array_creates_cells_defaulting_to_empty_lists);
    RUN(test_setitem_and_fillarray);
    RUN(test_array_aliasing_through_make_and_procedure_call);
    RUN(test_setitem_used_in_expression_position_reports_error);
    RUN(test_thing_reads_a_variable_by_computed_name);
    RUN(test_local_declares_a_call_scoped_variable);
    RUN(test_local_outside_a_procedure_reports_an_error);
    RUN(test_local_twice_with_the_same_name_is_a_no_op);
    RUN(test_names_lists_every_global_variable);
    RUN(test_setprop_getprop_round_trips_a_number_word_and_list);
    RUN(test_getprop_of_missing_property_is_the_empty_list);
    RUN(test_setprop_on_same_key_overwrites_not_duplicates);
    RUN(test_removeprop_removes_a_property);
    RUN(test_proplist_lists_alternating_keys_and_values);
    RUN(test_new_sets_a_prototype_property);
    RUN(test_turtle_motion_and_queries);
    RUN(test_setxy_setheading_setx_sety);
    RUN(test_distance_and_towards);
    RUN(test_home_and_clear);
    RUN(test_who_and_tell);
    RUN(test_tell_out_of_range_reports_error);
    RUN(test_canvassize_and_setcanvassize);
    RUN(test_setcanvassize_out_of_range_reports_error);
    RUN(test_pen_and_canvas_appearance_smoke);
    RUN(test_drawing_primitives_smoke);
    RUN(test_erase_deletes_a_procedure_so_it_can_no_longer_be_called);
    RUN(test_erase_of_unknown_procedure_reports_error);
    RUN(test_erase_removes_the_procedure_from_procedures_output);
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
    RUN(test_catch_recovers_when_the_thrown_tag_matches);
    RUN(test_throw_with_no_matching_catch_reports_and_recovers_at_top_level);
    RUN(test_uncaught_throw_at_top_level_reports_and_later_statements_still_run);
    RUN(test_throw_propagates_through_nested_procedure_calls);
    RUN(test_throw_breaks_a_while_loop_early);
    RUN(test_throw_inside_an_if_branch_is_caught_by_enclosing_catch);
    RUN(test_nested_catch_with_non_matching_inner_tag_propagates_to_outer);
    RUN(test_procedure_that_throws_in_expression_position_cascades_both_diagnostics);
    RUN(test_repeat_loop);
    RUN(test_forever_runs_until_stop);
    RUN(test_for_counts_up_by_default_step);
    RUN(test_for_limit_is_a_full_expression_not_just_a_literal);
    RUN(test_for_with_explicit_step);
    RUN(test_for_counts_down_when_limit_is_less_than_start);
    RUN(test_for_step_zero_reports_an_error);
    RUN(test_repeat_truncates_a_fractional_count);
    RUN(test_throw_breaks_a_for_loop_early);
    RUN(test_repeat_count_survives_a_recursive_call_in_its_own_body);
    RUN(test_for_limit_and_step_survive_a_recursive_call_in_its_own_body);
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
    RUN(test_foreach_preserves_nested_list_elements);
    RUN(test_map_template_calling_a_user_procedure);
    RUN(test_map_with_a_runtime_computed_template_uses_the_dynamic_fallback);
    RUN(test_filter_with_a_runtime_computed_template_uses_the_dynamic_fallback);
    RUN(test_map_with_a_malformed_literal_template_falls_back_to_the_dynamic_path);
    RUN(test_nested_reduce_inside_map_each_get_their_own_placeholder);
    RUN(test_map_template_placeholder_survives_a_recursive_call_in_its_own_body);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
