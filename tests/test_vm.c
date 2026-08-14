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
#include <unistd.h> // access(), used by fake_play_sound_file below

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

// TYPE: implemented since Phase 1 (interpreter.c) but never ported to
// src/parser.c's own grammar until 2026-08-12 -- a real,
// previously-undocumented gap (docs/COMMAND_REFERENCE.md's own
// appendix already listed it) caught when an example script using it
// silently failed to parse. Wired into eval.c's own dispatch at the
// same time, so this can shadow-diff against the tree-walker too, not
// just run standalone against the VM.
TEST(test_type_prints_without_a_trailing_newline) {
    shadow_diff_vm("TYPE \"Hello,\nTYPE \" \nPRINT \"world!");
}

// PR: found missing from the VM in the same pass as TYPE (same
// appendix table row grouping) -- a PRINT alias, same never-ported gap.
TEST(test_pr_is_a_print_alias) {
    shadow_diff_vm("PR 42\nPR \"hello");
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
    shadow_diff_vm("ARC 90 50\nLABEL \"hi\nFILL\nPLOT\nERASERECT 10 10\nCLEAN\nHIDETURTLE\nSHOWTURTLE\nPRINT \"ok");
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

// WAIT/WAITKEY (see docs/BYTECODE_VM_DESIGN.md's suspend/resume
// design) can't shadow-diff against ast_eval -- that engine has no
// suspend concept at all, and WAIT/WAITKEY don't even exist there
// (docs/ROADMAP.md explains why: porting a busy-wait shim into a
// second engine before the real mechanism existed would have been
// throwaway work). These tests call vm_run/vm_resume/vm_resume_with_key
// directly instead and assert on the returned VmRunResult plus
// captured output -- the same headless, no-GTK style every other test
// in this file already uses, which is exactly the point: vm.c itself
// never touches a clock or GTK, so all of this is testable without
// either.
typedef struct {
    LogoApp *app;
    ParseResult *result;
    BytecodeChunk *chunk;
    Vm *vm;
} VmTestSession;

static VmTestSession start_vm_session(const char *source, VmRunResult *out_status) {
    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex(source, tokens, MAX_VM_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    captured_output[0] = '\0';
    LogoApp *app = new_app();
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));
    *out_status = vm_run(vm, app, &result->pool, chunk, start_pc);
    VmTestSession s = { app, result, chunk, vm };
    return s;
}

static void end_vm_session(VmTestSession s) {
    free(s.vm);
    free(s.app);
    free(s.chunk);
    parse_result_destroy(s.result);
}

static void expect_status(VmRunResult got, VmRunResult want, const char *label) {
    if (got != want) {
        failures++;
        printf("FAIL %s: %s -- expected VmRunResult %d, got %d\n", current_test, label, want, got);
    }
}

static void expect_output(const char *want) {
    if (strcmp(captured_output, want) != 0) {
        failures++;
        printf("FAIL %s: output -- expected \"%s\", got \"%s\"\n", current_test, want, captured_output);
    }
}

TEST(test_plot_records_a_dot_raster_op_at_the_turtles_position_and_pen_color) {
    // PLOT added 2026-08-12 (the user noticed no single-point command
    // existed; DOT was already taken by the vector dot-product
    // operator) -- checks the actual recorded RasterOp fields directly
    // (kind/position/color/radius), since a filled dot's own visual
    // result can't be asserted through captured text output the way
    // FILL/ERASERECT's shared shadow-diff smoke test above does.
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "SETXY 12 34\nSETPENCOLOR 255 128 0\nSETPENWIDTH 8\nPLOT\nPRINT \"ok", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ok\n");

    if (s.app->raster_op_count != 1) {
        failures++;
        printf("FAIL %s: raster_op_count -- expected 1, got %d\n", current_test, s.app->raster_op_count);
    } else {
        RasterOp *op = &s.app->raster_ops[0];
        if (op->kind != RASTER_OP_DOT) {
            failures++;
            printf("FAIL %s: kind -- expected RASTER_OP_DOT, got %d\n", current_test, op->kind);
        }
        if (op->x != 12 || op->y != 34) {
            failures++;
            printf("FAIL %s: position -- expected (12, 34), got (%g, %g)\n", current_test, op->x, op->y);
        }
        double g_diff = op->g - 128.0 / 255.0;
        if (g_diff < 0) g_diff = -g_diff;
        if (op->r != 1.0 || g_diff > 0.001 || op->b != 0.0) {
            failures++;
            printf("FAIL %s: color -- expected (1, ~0.502, 0), got (%g, %g, %g)\n", current_test, op->r, op->g, op->b);
        }
        if (op->w != 4.0) {
            failures++;
            printf("FAIL %s: radius -- expected 4 (half pen_width), got %g\n", current_test, op->w);
        }
    }
    end_vm_session(s);
}

// CLEARTEXT/LOADPIC/SAVEPIC/MOUSEPOS/MOUSEX/MOUSEY/BUTTON?/BACKTRACE/
// EXECTIME -- ported from interpreter.c 2026-08-12 (docs/ROADMAP.md's
// "Remaining old-engine builtins" entry). Not in eval.c (confirmed:
// never existed there either, only in the oldest interpreter.c), so
// no shadow_diff_vm for these -- start_vm_session-based, same as every
// other VM-only builtin (LAUNCH/ONKEY/...).

TEST(test_cleartext_is_a_silent_noop_headless) {
    // app->clear_history is NULL in headless tests (no history pane to
    // clear) -- confirms it doesn't error or crash, same convention as
    // LOADSPRITE's own NULL-callback headless behavior.
    VmRunResult status;
    VmTestSession s = start_vm_session("CLEARTEXT\nPRINT \"after", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("after\n");
    end_vm_session(s);
}

TEST(test_loadpic_and_savepic_are_silent_noops_headless) {
    // app->load_background_image/save_canvas_image are both NULL
    // headless -- same convention as LOADSPRITE/LOADSPRITESHEET.
    VmRunResult status;
    VmTestSession s = start_vm_session("LOADPIC \"nope.png\nSAVEPIC \"out.png\nPRINT \"after", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("after\n");
    end_vm_session(s);
}

TEST(test_tone_playsound_stopsound_are_silent_noops_headless) {
    // app->play_tone/play_sound_file/stop_sound are all NULL headless
    // -- same convention as LOADPIC/SAVEPIC/LOADSPRITE above. TONE
    // reports nothing on failure even with a real callback (see
    // exec_tone's own comment in vm.c), so there's no separate
    // real-callback TONE test the way PLAYSOUND gets below.
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "TONE 440 0.1\nPLAYSOUND \"nope.wav\nSTOPSOUND\nPRINT \"after", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("after\n");
    end_vm_session(s);
}

// A real (not mocked) play_sound_file stand-in for the two tests below
// that exercise PLAYSOUND's own load-failure path -- ui.c's real
// callback (logo_play_sound_file) decodes through SDL2, which isn't
// linked into this test binary at all (kept out deliberately, see this
// file's own Makefile comment: linking it in made ASan runs of
// test_interpreter hang). access() is a real filesystem check, not a
// guess by filename, so this still exercises exec_playsound's own
// success/failure branches faithfully at the VM level -- same spirit
// as fake_load_sprite_image below, just without a real decode step
// available to call.
static gboolean fake_play_sound_file(LogoApp *app, const char *path) {
    (void)app;
    return access(path, F_OK) == 0;
}

TEST(test_playsound_of_a_real_file_is_silent) {
    // examples/sample.wav -- a real file, run through
    // fake_play_sound_file (see its own comment above) instead of the
    // NULL callback every other test in this file uses.
    captured_output[0] = '\0';
    LogoApp *app = new_app();
    app->play_sound_file = fake_play_sound_file;

    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex("PLAYSOUND \"examples/sample.wav\nPRINT \"ok", tokens, MAX_VM_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));

    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ok\n");

    free(vm);
    free(chunk);
    parse_result_destroy(result);
    free(app);
}

TEST(test_playsound_of_a_missing_file_reports_could_not_load) {
    captured_output[0] = '\0';
    LogoApp *app = new_app();
    app->play_sound_file = fake_play_sound_file;

    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex("PLAYSOUND \"examples/no_such_file.wav", tokens, MAX_VM_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));

    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("PLAYSOUND: could not load \"examples/no_such_file.wav\n");

    free(vm);
    free(chunk);
    parse_result_destroy(result);
    free(app);
}

TEST(test_mousepos_mousex_mousey_button_report_headless_defaults) {
    // app->mouse_x/mouse_y/mouse_button_down are always 0/0/FALSE
    // headless -- no real GTK motion/click controllers to update them,
    // matching interpreter.c's own documented "always 0/FALSE
    // headless" convention.
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "PRINT MOUSEPOS\nPRINT MOUSEX\nPRINT MOUSEY\nPRINT BUTTON?", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("0 0\n0\n0\nFALSE\n");
    end_vm_session(s);
}

TEST(test_backtrace_lists_the_call_stack_innermost_first) {
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "TO inner\n"
        "  BACKTRACE\n"
        "END\n"
        "TO outer\n"
        "  inner\n"
        "END\n"
        "outer", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("BACKTRACE:\n  inner\n  outer\n  (top level)\n");
    end_vm_session(s);
}

TEST(test_backtrace_at_the_top_level_shows_only_top_level) {
    VmRunResult status;
    VmTestSession s = start_vm_session("BACKTRACE", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("BACKTRACE:\n  (top level)\n");
    end_vm_session(s);
}

TEST(test_exectime_reports_a_plausible_nonnegative_number) {
    // Can't assert an exact value (real elapsed time) -- confirms it's
    // a real number, not an error, and that it works fine in
    // expression position (unlike RUN/APPLY, which are commands).
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT NUMBER? EXECTIME [PRINT 1 + 1]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("2\nTRUE\n");
    end_vm_session(s);
}

TEST(test_exectime_used_as_a_command_still_runs_its_code) {
    VmRunResult status;
    VmTestSession s = start_vm_session("EXECTIME [PRINT \"ran]\nPRINT \"after", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ran\nafter\n");
    end_vm_session(s);
}

TEST(test_exectime_self_referential_is_capped_not_a_crash) {
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "MAKE \"x [EXECTIME :x]\nEXECTIME :x\nPRINT \"survived", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "survived\n") == NULL) {
        failures++;
        printf("FAIL %s: expected to survive and print \"survived\", got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_wait_suspends_with_the_right_duration_then_resumes_and_completes) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT 1\nWAIT 5\nPRINT 2", &status);
    expect_status(status, VM_RUN_SUSPENDED_WAIT, "initial run");
    if (s.vm->suspend_seconds != 5) {
        failures++;
        printf("FAIL %s: suspend_seconds -- expected 5, got %g\n", current_test, s.vm->suspend_seconds);
    }
    status = vm_resume(s.vm, s.app, &s.result->pool, s.chunk);
    expect_status(status, VM_RUN_HALTED, "resume");
    expect_output("1\n2\n");
    end_vm_session(s);
}

TEST(test_wait_with_a_non_positive_duration_never_suspends_at_all) {
    // Matches interpreter.c's own `if (seconds > 0)` guard -- WAIT 0 (or
    // negative) is a no-op, not a suspend point.
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT 1\nWAIT 0\nPRINT 2", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("1\n2\n");
    end_vm_session(s);
}

// The actual point of VM-owned scope storage (see MAX_VM_SCOPE_DEPTH's
// own comment in vm.h): this genuinely can't be a shadow_diff_vm test
// -- 1000 levels comfortably exceeds ast_eval's own real ceiling
// (empirically ~100-186 before its C stack overflows), which is
// exactly the old shared limit (MAX_SCOPE_DEPTH, 200) this feature
// decouples the VM from. VM_RUN_HALTED with no "Recursion too deep"
// message anywhere in the output is the proof: 1000 real, successful
// nested calls, not an early bailout.
TEST(test_vm_recursion_goes_well_past_the_old_shared_200_limit) {
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "TO countdown :n\n"
        "  IF :n = 0 [OUTPUT 0]\n"
        "  OUTPUT countdown :n - 1\n"
        "END\n"
        "PRINT countdown 1000", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("0\n");
    if (strstr(captured_output, "Recursion too deep") != NULL) {
        failures++;
        printf("FAIL %s: hit the recursion cap at only 1000 levels deep, expected MAX_VM_SCOPE_DEPTH (2000) headroom\n", current_test);
    }
    end_vm_session(s);
}

// Regression for the INSTR_MAX_TEXT truncation bug (see
// docs/BYTECODE_VM_DESIGN.md and bytecode.h's own word_literals[]
// comment): a literal word longer than the old INSTR_MAX_TEXT-1 (63
// bytes) used to be silently truncated by compile_expr's own
// snprintf into Instr.text. It now lives in
// BytecodeChunk.word_literals[] instead, sized to AST_MAX_TEXT (512),
// so this 100-byte literal -- deliberately past the old 63-byte
// ceiling, with a trailing marker character to catch any off-by-one
// at that old boundary -- must come back out of PRINT byte-for-byte.
TEST(test_a_word_literal_longer_than_the_old_63_byte_instr_text_limit_is_not_truncated) {
    char long_word[101];
    memset(long_word, 'a', 99);
    long_word[99] = 'Z'; // past the old 63-byte ceiling -- truncation would drop this
    long_word[100] = '\0';

    char source[200];
    snprintf(source, sizeof(source), "PRINT \"%s", long_word);

    char expected[102];
    snprintf(expected, sizeof(expected), "%s\n", long_word);

    VmRunResult status;
    VmTestSession s = start_vm_session(source, &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output(expected);
    end_vm_session(s);
}

TEST(test_waitkey_suspends_then_resumes_with_the_pressed_key_and_completes) {
    VmRunResult status;
    VmTestSession s = start_vm_session("MAKE \"k WAITKEY\nPRINT :k", &status);
    expect_status(status, VM_RUN_SUSPENDED_WAITKEY, "initial run");
    status = vm_resume_with_key(s.vm, s.app, &s.result->pool, s.chunk, "a");
    expect_status(status, VM_RUN_HALTED, "resume");
    expect_output("a\n");
    end_vm_session(s);
}

TEST(test_waitkey_suspends_and_resumes_correctly_through_several_nested_procedure_calls) {
    // The actual payoff of the VmFrame array: WAITKEY suspends 3 real
    // procedure calls deep, and resuming has to correctly unwind back
    // through all 3 OUTPUTs to reach PRINT -- vm/pool/chunk are kept
    // fully intact across the suspend, so this just works.
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "TO f :n\n"
        "  IF :n = 0 [OUTPUT WAITKEY]\n"
        "  OUTPUT f :n - 1\n"
        "END\n"
        "PRINT f 3", &status);
    expect_status(status, VM_RUN_SUSPENDED_WAITKEY, "initial run");
    status = vm_resume_with_key(s.vm, s.app, &s.result->pool, s.chunk, "z");
    expect_status(status, VM_RUN_HALTED, "resume");
    expect_output("z\n");
    end_vm_session(s);
}

TEST(test_waitkey_directly_inside_a_map_template_reports_an_error_instead_of_suspending) {
    // The documented, deliberately-not-fixed gap: a MAP/FILTER/REDUCE/
    // FOREACH template's own recursive vm_run call (see
    // exec_map_compiled) can't correctly propagate a suspend out to
    // this call's own caller, so OP_WAITKEY refuses outright (checked
    // via vm->vm_run_depth) rather than silently losing the suspend or
    // corrupting state -- the whole run still completes normally.
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT MAP [WAITKEY] [1 2]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "WAITKEY: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n") == NULL) {
        failures++;
        printf("FAIL %s: expected the template-refusal message, got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_wait_directly_inside_a_foreach_template_reports_an_error_instead_of_suspending) {
    VmRunResult status;
    VmTestSession s = start_vm_session("FOREACH [WAIT 1] [1 2]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "WAIT: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n") == NULL) {
        failures++;
        printf("FAIL %s: expected the template-refusal message, got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_launch_inside_run_reports_an_error_instead_of_spawning_an_agent) {
    // Same vm_run_depth>1 refusal WAIT/WAITKEY/INPUT/PAUSE/ANIMATESPRITE
    // already use for a MAP/FILTER/REDUCE/FOREACH template (see above),
    // but triggered via RUN instead -- LAUNCH/AWAIT/YIELD are VM-only
    // (no tree-walker equivalent to shadow-diff against), so this is
    // their first-ever automated coverage of any kind.
    VmRunResult status;
    VmTestSession s = start_vm_session("TO foo\nEND\nRUN [LAUNCH \"foo []]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("LAUNCH: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n");
    end_vm_session(s);
}

TEST(test_await_inside_run_reports_an_error_instead_of_blocking) {
    VmRunResult status;
    VmTestSession s = start_vm_session("RUN [AWAIT]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("AWAIT: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n");
    end_vm_session(s);
}

TEST(test_yield_inside_run_reports_an_error_instead_of_yielding) {
    VmRunResult status;
    VmTestSession s = start_vm_session("RUN [YIELD]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("YIELD: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n");
    end_vm_session(s);
}

TEST(test_setspeed_default_is_instant_no_delay_at_all) {
    // The default (0) is unchanged behavior -- FD never suspends unless
    // SETSPEED has actually been called.
    VmRunResult status;
    VmTestSession s = start_vm_session("FD 10\nPRINT 1", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("1\n");
    end_vm_session(s);
}

TEST(test_setspeed_makes_a_motion_command_suspend_then_resume_and_continue) {
    VmRunResult status;
    VmTestSession s = start_vm_session("SETSPEED 0.2\nFD 10\nPRINT 1", &status);
    expect_status(status, VM_RUN_SUSPENDED_MOTION_DELAY, "initial run");
    if (s.vm->suspend_seconds != 0.2) {
        failures++;
        printf("FAIL %s: suspend_seconds -- expected 0.2, got %g\n", current_test, s.vm->suspend_seconds);
    }
    status = vm_resume(s.vm, s.app, &s.result->pool, s.chunk);
    expect_status(status, VM_RUN_HALTED, "resume");
    expect_output("1\n");
    end_vm_session(s);
}

TEST(test_setspeed_does_not_delay_non_motion_commands) {
    // PRINT/SETPENCOLOR/etc never suspend, whatever SETSPEED is set to
    // -- only the turtle-motion commands is_motion_command names.
    VmRunResult status;
    VmTestSession s = start_vm_session("SETSPEED 0.2\nPRINT 1\nSETPENCOLOR 255 0 0\nPRINT 2", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("1\n2\n");
    end_vm_session(s);
}

TEST(test_speed_reports_the_current_setting) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT SPEED\nSETSPEED 0.5\nPRINT SPEED", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("0\n0.5\n");
    end_vm_session(s);
}

TEST(test_setspeed_inside_a_foreach_template_is_a_silent_no_op_not_an_error) {
    // Unlike WAIT/WAITKEY's own explicit refusal message inside a
    // template, an automatic per-step throttle the script never
    // explicitly asked for at this call site just skips quietly (see
    // OP_MOTION_DELAY's own bytecode.h comment) -- no output at all
    // beyond what the script itself prints.
    VmRunResult status;
    VmTestSession s = start_vm_session("SETSPEED 0.2\nFOREACH [FD 1] [1 2]\nPRINT \"done", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("done\n");
    end_vm_session(s);
}

TEST(test_input_suspends_then_resumes_with_the_submitted_line_and_completes) {
    VmRunResult status;
    VmTestSession s = start_vm_session("MAKE \"line INPUT\nPRINT :line", &status);
    expect_status(status, VM_RUN_SUSPENDED_INPUT, "initial run");
    status = vm_resume_with_input(s.vm, s.app, &s.result->pool, s.chunk, "hello there");
    expect_status(status, VM_RUN_HALTED, "resume");
    expect_output("hello there\n");
    end_vm_session(s);
}

TEST(test_input_directly_inside_a_map_template_reports_an_error_instead_of_suspending) {
    // Same documented gap as WAIT/WAITKEY inside a template body -- see
    // vm_run_depth's own comment in vm.h.
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT MAP [INPUT] [1 2]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "INPUT: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n") == NULL) {
        failures++;
        printf("FAIL %s: expected the template-refusal message, got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_pause_suspends_with_the_right_level_then_resumes_and_completes) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT 1\nPAUSE\nPRINT 2", &status);
    expect_status(status, VM_RUN_SUSPENDED_PAUSE, "initial run");
    if (s.vm->pause_level != 1) {
        failures++;
        printf("FAIL %s: pause_level -- expected 1, got %d\n", current_test, s.vm->pause_level);
    }
    status = vm_resume(s.vm, s.app, &s.result->pool, s.chunk);
    expect_status(status, VM_RUN_HALTED, "resume");
    expect_output("1\nPaused (level 1). Type CONTINUE to resume.\n2\n");
    end_vm_session(s);
}

TEST(test_continue_with_nothing_paused_reports_an_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("CONTINUE", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("CONTINUE: nothing is paused\n");
    end_vm_session(s);
}

TEST(test_pause_directly_inside_a_foreach_template_reports_an_error_instead_of_suspending) {
    VmRunResult status;
    VmTestSession s = start_vm_session("FOREACH [PAUSE] [1 2]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "PAUSE: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n") == NULL) {
        failures++;
        printf("FAIL %s: expected the template-refusal message, got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_two_nested_pauses_share_one_pause_depth_and_resume_in_lifo_order) {
    // Mimics what ui.c's own pause stack relies on: two independently
    // compiled/run scripts sharing ONE LogoApp, since variable/
    // pause_depth state is global (app->pause_depth), not per-Vm. The
    // second PAUSE happens while the first is still suspended,
    // capturing a strictly higher level; a CONTINUE (its own tiny
    // script, sharing the same app) drops pause_depth by exactly one,
    // making only the innermost (highest-level) run eligible --
    // resuming it first, then the outer one, confirms LIFO ordering,
    // matching interpreter.c's own nested-PAUSE semantics.
    captured_output[0] = '\0';
    LogoApp *app = new_app();

    LogoToken tokens1[MAX_VM_TEST_TOKENS];
    int n1 = logo_lex("PAUSE\nPRINT \"outerdone", tokens1, MAX_VM_TEST_TOKENS);
    ParseResult *result1 = calloc(1, sizeof(ParseResult));
    logo_parse(tokens1, n1, result1);
    BytecodeChunk *chunk1 = calloc(1, sizeof(BytecodeChunk));
    int start1 = compile_program(&result1->pool, result1->program, chunk1);
    Vm *vm1 = calloc(1, sizeof(Vm));
    VmRunResult status1 = vm_run(vm1, app, &result1->pool, chunk1, start1);
    expect_status(status1, VM_RUN_SUSPENDED_PAUSE, "outer initial run");
    if (vm1->pause_level != 1) {
        failures++;
        printf("FAIL %s: outer pause_level -- expected 1, got %d\n", current_test, vm1->pause_level);
    }

    LogoToken tokens2[MAX_VM_TEST_TOKENS];
    int n2 = logo_lex("PAUSE\nPRINT \"innerdone", tokens2, MAX_VM_TEST_TOKENS);
    ParseResult *result2 = calloc(1, sizeof(ParseResult));
    logo_parse(tokens2, n2, result2);
    BytecodeChunk *chunk2 = calloc(1, sizeof(BytecodeChunk));
    int start2 = compile_program(&result2->pool, result2->program, chunk2);
    Vm *vm2 = calloc(1, sizeof(Vm));
    VmRunResult status2 = vm_run(vm2, app, &result2->pool, chunk2, start2);
    expect_status(status2, VM_RUN_SUSPENDED_PAUSE, "inner initial run");
    if (vm2->pause_level != 2) {
        failures++;
        printf("FAIL %s: inner pause_level -- expected 2, got %d\n", current_test, vm2->pause_level);
    }

    LogoToken tokens3[MAX_VM_TEST_TOKENS];
    int n3 = logo_lex("CONTINUE", tokens3, MAX_VM_TEST_TOKENS);
    ParseResult *result3 = calloc(1, sizeof(ParseResult));
    logo_parse(tokens3, n3, result3);
    BytecodeChunk *chunk3 = calloc(1, sizeof(BytecodeChunk));
    int start3 = compile_program(&result3->pool, result3->program, chunk3);
    Vm *vm3 = calloc(1, sizeof(Vm));
    VmRunResult status3 = vm_run(vm3, app, &result3->pool, chunk3, start3);
    expect_status(status3, VM_RUN_HALTED, "CONTINUE run");
    if (app->pause_depth != 1) {
        failures++;
        printf("FAIL %s: pause_depth after one CONTINUE -- expected 1, got %d\n", current_test, app->pause_depth);
    }

    VmRunResult resumed2 = vm_resume(vm2, app, &result2->pool, chunk2);
    expect_status(resumed2, VM_RUN_HALTED, "inner resume");
    VmRunResult resumed1 = vm_resume(vm1, app, &result1->pool, chunk1);
    expect_status(resumed1, VM_RUN_HALTED, "outer resume");

    const char *inner_pos = strstr(captured_output, "innerdone");
    const char *outer_pos = strstr(captured_output, "outerdone");
    if (inner_pos == NULL || outer_pos == NULL) {
        failures++;
        printf("FAIL %s: expected both innerdone and outerdone in output, got \"%s\"\n", current_test, captured_output);
    } else if (inner_pos > outer_pos) {
        failures++;
        printf("FAIL %s: expected innerdone before outerdone (LIFO), got \"%s\"\n", current_test, captured_output);
    }

    free(vm1); free(chunk1); parse_result_destroy(result1);
    free(vm2); free(chunk2); parse_result_destroy(result2);
    free(vm3); free(chunk3); parse_result_destroy(result3);
    free(app);
}

// Math operators (see docs/ROADMAP.md's own note on the 35-name audit
// this closes the first, highest-value slice of): unlike sprites/
// suspend-resume, these already work identically in ast_eval, so
// ordinary shadow_diff_vm applies -- no filesystem side effects to
// worry about doubling up, unlike the file-I/O batch. RANDOM is the
// one exception: both engines draw from the same process-global RNG
// stream, so running the identical script through both back-to-back
// would consume different draws and spuriously "diverge" -- tested
// separately, single-engine, as a bounds check instead.

TEST(test_math_operators_abs_sqrt_power_round) {
    shadow_diff_vm("PRINT ABS -5\nPRINT SQRT 16\nPRINT POWER 2 10\nPRINT ROUND 3.6\nPRINT ROUND 3.4");
}

TEST(test_mod_takes_the_sign_of_the_divisor) {
    shadow_diff_vm("PRINT MOD 7 3\nPRINT MOD (-7) 3\nPRINT MOD 7 (-3)");
}

TEST(test_trig_operators_use_degrees) {
    shadow_diff_vm("PRINT SIN 90\nPRINT COS 0\nPRINT TAN 45\nPRINT ASIN 1\nPRINT ACOS 1\nPRINT ARCTAN 1");
}

TEST(test_log_and_exp_operators) {
    shadow_diff_vm("PRINT LN 1\nPRINT LOG 100\nPRINT EXP 0");
}

TEST(test_random_stays_within_bounds) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT RANDOM 10", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    char *end;
    double n = strtod(captured_output, &end);
    if (end == captured_output || n < 0 || n >= 10) {
        failures++;
        printf("FAIL %s: expected a number in [0, 10), got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_type_predicates) {
    shadow_diff_vm(
        "PRINT WORD? \"hello\n"
        "PRINT WORD? [1 2]\n"
        "PRINT LIST? [1 2]\n"
        "PRINT LIST? \"hello\n"
        "PRINT NUMBER? 5\n"
        "PRINT NUMBER? \"five\n"
        "PRINT ARRAY? (ARRAY 3)\n"
        "PRINT ARRAY? 5");
}

TEST(test_turtle_command_short_aliases_match_their_full_names) {
    shadow_diff_vm(
        "SETH 90\nPRINT HEADING\n"
        "SETPC 255 0 0\nSETPW 3\nSETBG 0 0 255\n"
        "HT\nST\nPRINT \"done");
}

TEST(test_pick_from_a_list_word_array_and_bare_number) {
    shadow_diff_vm(
        "PRINT PICK [only]\n"
        "PRINT PICK \"a\n"
        "MAKE \"a ARRAY 1\nSETITEM 1 :a \"solo\nPRINT PICK :a\n"
        "PRINT PICK 42");
}

TEST(test_pick_reports_error_on_empty_list_or_word) {
    shadow_diff_vm("PRINT PICK []\nPRINT PICK BUTFIRST \"a");
}

TEST(test_flatten_collects_every_leaf_discarding_nesting) {
    shadow_diff_vm("PRINT FLATTEN [1 [2 3] [4 [5 6]] 7]\nPRINT FLATTEN 5");
}

TEST(test_parse_tokenizes_a_values_printed_text_by_whitespace) {
    shadow_diff_vm(
        "PRINT PARSE 'hello world'\nPRINT COUNT PARSE 'hello world'\n"
        "PRINT COUNT PARSE [a b [c d] e]\nPRINT PARSE [a b [c d] e]\n"
        "PRINT PARSE 42");
}

TEST(test_subst_replaces_matching_elements_including_a_whole_sublist) {
    shadow_diff_vm(
        "PRINT SUBST \"b \"z [a b c b]\n"
        "PRINT SUBST [1 2] \"x [a [1 2] c]\n"
        "PRINT SUBST \"a \"z \"a\n"
        "PRINT SUBST \"a \"z \"b");
}

TEST(test_dot_product_of_two_numeric_lists) {
    shadow_diff_vm("PRINT DOT [1 2 3] [4 5 6]\nPRINT DOT [1 2] [1 2 3]");
}

TEST(test_cross_product_of_two_3element_lists) {
    shadow_diff_vm("PRINT CROSS [1 0 0] [0 1 0]\nPRINT CROSS [1 2] [1 2 3]");
}

TEST(test_memberp_on_list_and_number) {
    shadow_diff_vm(
        "PRINT MEMBER? \"b [a b c]\n"
        "PRINT MEMBER? \"z [a b c]\n"
        "PRINT MEMBER? 2 [1 2 3]\n"
        "PRINT MEMBER? 5 5\n"
        "PRINT MEMBER? 5 6");
}

TEST(test_memberp_on_word_is_substring) {
    shadow_diff_vm("PRINT MEMBER? \"ell \"hello\nPRINT MEMBER? \"xyz \"hello");
}

TEST(test_apply_calls_a_procedure_with_a_list_of_arguments) {
    shadow_diff_vm(
        "TO add :a :b\n"
        "  PRINT :a + :b\n"
        "END\n"
        "APPLY \"add [3 4]");
}

TEST(test_apply_unknown_procedure_reports_an_error) {
    shadow_diff_vm("APPLY \"nosuch [1 2]");
}

TEST(test_apply_wrong_argument_count_reports_an_error) {
    shadow_diff_vm(
        "TO add :a :b\n"
        "  PRINT :a + :b\n"
        "END\n"
        "APPLY \"add [3]");
}

TEST(test_apply_never_hands_back_a_value_even_when_the_procedure_outputs) {
    // The new OP_VOID_DISCARD mechanism's own reason to exist: unlike
    // an ordinary call, APPLY discards whatever the applied procedure
    // itself OUTPUTs -- confirmed here in expression position, where
    // that would otherwise surface as a real value instead of the
    // "didn't output a value" diagnostic.
    shadow_diff_vm(
        "TO five\n"
        "  OUTPUT 5\n"
        "END\n"
        "PRINT APPLY \"five []");
}

// RUN/LOAD (see bytecode.h's own OP_RUN/OP_LOAD comment): the last two
// names from the 35-name audit, and the two genuinely new-architecture
// pieces of this whole batch -- both compile a whole fresh, independent
// BytecodeChunk at runtime and run it via a recursive vm_run call. Both
// are pure (LOAD only ever reads its file, never writes/deletes), so
// -- unlike the earlier file-I/O batch's OPENWRITE/DELETEFILE cases --
// ordinary shadow_diff_vm is safe here: running the same script through
// both engines back-to-back never double-mutates anything on disk.

TEST(test_run_executes_a_stored_list_as_source) {
    shadow_diff_vm("RUN [PRINT 1 + 2]");
}

TEST(test_run_self_referential_is_capped_not_a_crash) {
    shadow_diff_vm("MAKE \"x [RUN :x]\nRUN :x\nPRINT 1");
}

TEST(test_run_bare_stop_at_the_top_level_reports_the_escaping_gap) {
    // The documented, VM-only frame-accounting gap exec_run's own
    // comment describes: a bare STOP inside a RUN'd snippet's own top
    // level (not inside its own TO...END) pops a frame belonging to
    // CALLER, not the RUN'd snippet -- caught after the fact via
    // frame_floor, not prevented. Confirmed here to do exactly what
    // that comment says: the message fires, the statement right after
    // RUN inside caller still executes (the corrupted frame carries on
    // to caller's own next instruction), but caller's own return to the
    // top level is skipped (top-level "after" below never prints) --
    // this has no tree-walker equivalent to shadow-diff against.
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "TO caller\n"
        "  RUN [STOP]\n"
        "  PRINT \"unreachable\n"
        "END\n"
        "caller\n"
        "PRINT \"after", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output(
        "RUN: OUTPUT/STOP escaping the RUN'd snippet's own top level is not fully supported\n"
        "unreachable\n");
    end_vm_session(s);
}

TEST(test_load_runs_a_files_contents_as_logo_source) {
    const char *path = "build/test_vm_load.logo";
    remove(path);
    g_file_set_contents(path,
        "TO greet :name\n"
        "  PRINT WORD \"hello- :name\n"
        "END\n"
        "greet \"world\n"
        "PRINT 1 + 1", -1, NULL);
    shadow_diff_vm("PRINT \"before\nLOAD \"build/test_vm_load.logo\nPRINT \"after");
    remove(path);
}

TEST(test_load_of_missing_file_reports_error) {
    shadow_diff_vm("LOAD \"build/test_vm_does_not_exist_at_all.logo");
}

TEST(test_load_defined_procedure_is_callable_from_the_loading_script) {
    // Confirms the parser's own eager-LOAD-following pre-pass already
    // makes this work at compile time -- exec_load's own recursive
    // vm_run call only has to run the loaded file's own top-level
    // `greet "world` statement, not redefine the procedure itself.
    const char *path = "build/test_vm_load_callable.logo";
    remove(path);
    g_file_set_contents(path, "TO greet :name\n  PRINT WORD \"hello- :name\nEND", -1, NULL);
    shadow_diff_vm("LOAD \"build/test_vm_load_callable.logo\ngreet \"world");
    remove(path);
}

TEST(test_load_of_a_file_with_a_bare_stop_at_its_top_level_reports_the_escaping_gap) {
    // LOAD's own version of exec_run's escaping gap above -- same
    // frame_floor mechanism, same "message fires, corrupted frame
    // carries on to the next statement inside caller, but caller's own
    // return to the top level is skipped" shape, just triggered by a
    // loaded FILE's bare top-level STOP instead of a RUN'd list's.
    const char *path = "build/test_vm_load_escape.logo";
    remove(path);
    g_file_set_contents(path, "STOP\n", -1, NULL);
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "TO caller\n"
        "  LOAD \"build/test_vm_load_escape.logo\n"
        "  PRINT \"after-load\n"
        "END\n"
        "caller\n"
        "PRINT \"after-caller", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output(
        "LOAD: OUTPUT/STOP escaping the loaded file's own top level is not fully supported\n"
        "after-load\n");
    end_vm_session(s);
    remove(path);
}

// Sprites (see docs/BYTECODE_VM_DESIGN.md's suspend/resume design):
// vm.c-only, not shadow-diffed against ast_eval (which never gained
// sprite support at all -- see parser.c's own note). load_sprite_image
// is NULL in new_app() here, same convention as
// tests/test_interpreter.c's own sprite corpus (mirrored below) -- so
// no sprite is ever actually registered via LOADSPRITE/LOADSPRITESHEET
// in these tests, only the parsing/validation/error paths are
// reachable that way. ANIMATESPRITE's real multi-frame suspend/resume
// mechanism (the actual new piece this batch adds) is exercised
// separately, by directly poking app's sprite fields to set up "a
// sprite exists" without going through the GUI-only load path at all.

// A real (not mocked) load_sprite_image stand-in for the two tests
// below that exercise LOADSPRITE/LOADSPRITESHEET's own load-failure
// path -- ui.c's real callback (load_named_sprite_image) is `static`
// and ui.c itself isn't linked into this test binary, but GdkPixbuf is
// still available here (gtk/gtk.h, pulled in transitively via
// logo_types.h, is linked into every test binary), so this decodes the
// same way the real one does rather than faking success/failure by
// filename. Registers into app->sprite_names/sprite_frame_cols/
// sprite_frame_rows/sprite_count on success, matching production
// closely enough for these tests (skips storing an actual
// cairo_surface_t in sprite_images -- nothing at the VM level ever
// reads it, per the ANIMATESPRITE comment above).
static gboolean fake_load_sprite_image(LogoApp *app, const char *name, const char *path, int cols, int rows) {
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, NULL);
    if (pixbuf == NULL) return FALSE;
    g_object_unref(pixbuf);

    if (app->sprite_count >= MAX_TURTLE_SPRITES) return FALSE;
    int idx = app->sprite_count++;
    snprintf(app->sprite_names[idx], sizeof(app->sprite_names[idx]), "%s", name);
    app->sprite_frame_cols[idx] = cols;
    app->sprite_frame_rows[idx] = rows;
    return TRUE;
}

TEST(test_setsprite_of_unknown_name_reports_error_and_leaves_default) {
    VmRunResult status;
    VmTestSession s = start_vm_session("SETSPRITE \"turtle", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "SETSPRITE: no such sprite \"turtle") == NULL) {
        failures++;
        printf("FAIL %s: expected \"no such sprite\", got \"%s\"\n", current_test, captured_output);
    }
    if (s.app->turtles[0].sprite_index != -1) {
        failures++;
        printf("FAIL %s: sprite_index -- expected -1, got %d\n", current_test, s.app->turtles[0].sprite_index);
    }
    end_vm_session(s);
}

TEST(test_setsprite_none_is_a_silent_reset_to_default) {
    VmRunResult status;
    VmTestSession s = start_vm_session("SETSPRITE \"none", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("");
    if (s.app->turtles[0].sprite_index != -1) {
        failures++;
        printf("FAIL %s: sprite_index -- expected -1, got %d\n", current_test, s.app->turtles[0].sprite_index);
    }
    end_vm_session(s);
}

TEST(test_stampsprite_records_default_triangle_when_no_sprite_set) {
    VmRunResult status;
    VmTestSession s = start_vm_session("STAMPSPRITE", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (s.app->raster_op_count != 1 || s.app->raster_ops[0].kind != RASTER_OP_STAMP ||
        s.app->raster_ops[0].sprite_index != -1 || s.app->raster_ops[0].sprite_frame != 0) {
        failures++;
        printf("FAIL %s: expected one RASTER_OP_STAMP with sprite_index -1/frame 0, got count=%d\n",
               current_test, s.app->raster_op_count);
    }
    end_vm_session(s);
}

TEST(test_loadsprite_is_a_safe_no_op_with_no_gui) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT \"before\nLOADSPRITE \"turtle \"turtle.png\nPRINT \"after", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("before\nafter\n");
    end_vm_session(s);
}

TEST(test_loadspritesheet_with_zero_cols_reports_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("LOADSPRITESHEET \"walk \"walk.png 0 2", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("LOADSPRITESHEET: cols and rows must be at least 1\n");
    end_vm_session(s);
}

TEST(test_loadsprite_of_a_real_image_loads_silently_and_registers_it) {
    // examples/ant.png -- a real gdk-pixbuf-decodable file, run through
    // fake_load_sprite_image (see its own comment above) instead of the
    // NULL callback every other test in this file uses. Confirms the
    // success path: no error output, and the sprite actually lands in
    // app->sprite_names/sprite_frame_cols/sprite_frame_rows.
    captured_output[0] = '\0';
    LogoApp *app = new_app();
    app->load_sprite_image = fake_load_sprite_image;

    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex("LOADSPRITE \"ant \"examples/ant.png", tokens, MAX_VM_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));

    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("");
    if (app->sprite_count != 1 || strcasecmp(app->sprite_names[0], "ant") != 0 ||
        app->sprite_frame_cols[0] != 1 || app->sprite_frame_rows[0] != 1) {
        failures++;
        printf("FAIL %s: expected sprite \"ant\" registered as a 1x1 grid, got count=%d name=\"%s\" cols=%d rows=%d\n",
               current_test, app->sprite_count, app->sprite_names[0], app->sprite_frame_cols[0], app->sprite_frame_rows[0]);
    }

    free(vm);
    free(chunk);
    parse_result_destroy(result);
    free(app);
}

TEST(test_loadspritesheet_of_a_real_image_loads_silently_and_registers_the_grid) {
    // examples/walker.png -- documented (docs/TUTORIAL_II.md,
    // examples/spritesheet.logo) as a 4-column, 2-row, 8-frame walk
    // cycle. Same fake_load_sprite_image setup as the LOADSPRITE test
    // above, but also confirms the cols/rows actually get recorded.
    captured_output[0] = '\0';
    LogoApp *app = new_app();
    app->load_sprite_image = fake_load_sprite_image;

    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex("LOADSPRITESHEET \"walker \"examples/walker.png 4 2", tokens, MAX_VM_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));

    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("");
    if (app->sprite_count != 1 || strcasecmp(app->sprite_names[0], "walker") != 0 ||
        app->sprite_frame_cols[0] != 4 || app->sprite_frame_rows[0] != 2) {
        failures++;
        printf("FAIL %s: expected sprite \"walker\" registered as a 4x2 grid, got count=%d name=\"%s\" cols=%d rows=%d\n",
               current_test, app->sprite_count, app->sprite_names[0], app->sprite_frame_cols[0], app->sprite_frame_rows[0]);
    }

    free(vm);
    free(chunk);
    parse_result_destroy(result);
    free(app);
}

TEST(test_loadsprite_of_a_missing_file_reports_could_not_load) {
    // The actual gap this batch closes: with a real (non-NULL)
    // load_sprite_image callback, a path gdk-pixbuf genuinely can't
    // decode must hit LOADSPRITE's "could not load" branch, not just
    // silently no-op the way the NULL-callback tests above do.
    captured_output[0] = '\0';
    LogoApp *app = new_app();
    app->load_sprite_image = fake_load_sprite_image;

    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex("LOADSPRITE \"ant \"examples/no_such_file.png", tokens, MAX_VM_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));

    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("LOADSPRITE: could not load \"examples/no_such_file.png\n");
    if (app->sprite_count != 0) {
        failures++;
        printf("FAIL %s: sprite_count -- expected 0 (nothing registered on failure), got %d\n", current_test, app->sprite_count);
    }

    free(vm);
    free(chunk);
    parse_result_destroy(result);
    free(app);
}

TEST(test_loadspritesheet_of_a_missing_file_reports_could_not_load) {
    captured_output[0] = '\0';
    LogoApp *app = new_app();
    app->load_sprite_image = fake_load_sprite_image;

    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex("LOADSPRITESHEET \"walker \"examples/no_such_file.png 4 2", tokens, MAX_VM_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));

    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("LOADSPRITESHEET: could not load \"examples/no_such_file.png\n");
    if (app->sprite_count != 0) {
        failures++;
        printf("FAIL %s: sprite_count -- expected 0 (nothing registered on failure), got %d\n", current_test, app->sprite_count);
    }

    free(vm);
    free(chunk);
    parse_result_destroy(result);
    free(app);
}

TEST(test_setspriteframe_without_a_sprite_set_reports_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("SETSPRITEFRAME 2", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("SETSPRITEFRAME: no sprite set (use SETSPRITE first)\n");
    end_vm_session(s);
}

TEST(test_setspriteframe_out_of_range_reports_error_and_leaves_the_frame_unchanged) {
    // Same "poke app's own sprite fields directly" setup as
    // test_animatesprite_with_a_positive_delay_advances_one_frame_per_suspend_then_completes
    // below -- load_sprite_image is NULL here (no real GUI), so this is
    // the only way to get "a sprite exists" without going through
    // LOADSPRITE/SETSPRITE.
    captured_output[0] = '\0';
    LogoApp *app = new_app();
    app->sprite_count = 1;
    snprintf(app->sprite_names[0], sizeof(app->sprite_names[0]), "test");
    app->sprite_frame_cols[0] = 2;
    app->sprite_frame_rows[0] = 2; // frame_count = 4, valid frames 0-3
    app->turtles[0].sprite_index = 0;
    app->turtles[0].sprite_frame = 1;

    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex("SETSPRITEFRAME 10", tokens, MAX_VM_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));

    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("SETSPRITEFRAME: frame out of range\n");
    if (app->turtles[0].sprite_frame != 1) {
        failures++;
        printf("FAIL %s: sprite_frame -- expected unchanged (1), got %d\n", current_test, app->turtles[0].sprite_frame);
    }

    free(vm);
    free(chunk);
    parse_result_destroy(result);
    free(app);
}

TEST(test_animatesprite_without_a_sprite_set_reports_error_and_does_not_suspend) {
    // Must bail out on the "no sprite" check before ever suspending --
    // confirmed directly via VmRunResult (VM_RUN_HALTED, not
    // VM_RUN_SUSPENDED_ANIMATESPRITE), not just by not hanging.
    VmRunResult status;
    VmTestSession s = start_vm_session("ANIMATESPRITE 5 10", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ANIMATESPRITE: no sprite set (use SETSPRITE first)\n");
    end_vm_session(s);
}

TEST(test_animatesprite_directly_inside_a_map_template_reports_an_error_instead_of_suspending) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT MAP [ANIMATESPRITE 1 2] [1 2]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "ANIMATESPRITE: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n") == NULL) {
        failures++;
        printf("FAIL %s: expected the template-refusal message, got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_animatesprite_with_a_positive_delay_advances_one_frame_per_suspend_then_completes) {
    // The actual new mechanism this batch adds: unlike every other
    // suspend point, one ANIMATESPRITE call suspends MULTIPLE times
    // (once per remaining frame) before finally completing. Since
    // load_sprite_image is NULL here (no real GUI), "a sprite exists"
    // is set up by directly poking app's own sprite fields instead of
    // going through LOADSPRITE/SETSPRITE -- ANIMATESPRITE only ever
    // reads sprite_frame_cols/rows and Turtle.sprite_index/sprite_frame,
    // never sprite_images itself, so this is a faithful, safe setup.
    captured_output[0] = '\0';
    LogoApp *app = new_app();
    app->sprite_count = 1;
    snprintf(app->sprite_names[0], sizeof(app->sprite_names[0]), "test");
    app->sprite_frame_cols[0] = 2;
    app->sprite_frame_rows[0] = 2; // frame_count = 4
    app->turtles[0].sprite_index = 0;
    app->turtles[0].sprite_frame = 0;

    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex("ANIMATESPRITE 1 3\nPRINT \"done", tokens, MAX_VM_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));

    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);
    expect_status(status, VM_RUN_SUSPENDED_ANIMATESPRITE, "initial run");
    if (vm->suspend_seconds != 1 || vm->suspend_frames_remaining != 2 || app->turtles[0].sprite_frame != 1) {
        failures++;
        printf("FAIL %s: after initial suspend -- expected seconds=1 remaining=2 frame=1, got seconds=%g remaining=%d frame=%d\n",
               current_test, vm->suspend_seconds, vm->suspend_frames_remaining, app->turtles[0].sprite_frame);
    }

    status = vm_resume_animatesprite(vm, app, &result->pool, chunk);
    expect_status(status, VM_RUN_SUSPENDED_ANIMATESPRITE, "resume 1");
    if (app->turtles[0].sprite_frame != 2) {
        failures++;
        printf("FAIL %s: after resume 1 -- expected frame=2, got %d\n", current_test, app->turtles[0].sprite_frame);
    }

    status = vm_resume_animatesprite(vm, app, &result->pool, chunk);
    expect_status(status, VM_RUN_SUSPENDED_ANIMATESPRITE, "resume 2");
    if (app->turtles[0].sprite_frame != 3) {
        failures++;
        printf("FAIL %s: after resume 2 -- expected frame=3, got %d\n", current_test, app->turtles[0].sprite_frame);
    }

    status = vm_resume_animatesprite(vm, app, &result->pool, chunk);
    expect_status(status, VM_RUN_HALTED, "resume 3 (final)");
    expect_output("done\n");
    if (app->turtles[0].sprite_frame != 3) {
        failures++;
        printf("FAIL %s: final frame -- expected 3 (3 total advances for frames=3), got %d\n", current_test, app->turtles[0].sprite_frame);
    }

    free(vm); free(chunk); parse_result_destroy(result); free(app);
}

TEST(test_animatesprite_with_a_non_positive_delay_runs_all_frames_synchronously) {
    // Matches interpreter.c's own `if (delay > 0)` guard -- with no
    // delay, every frame advances in one synchronous burst, no suspend
    // at all.
    captured_output[0] = '\0';
    LogoApp *app = new_app();
    app->sprite_count = 1;
    snprintf(app->sprite_names[0], sizeof(app->sprite_names[0]), "test");
    app->sprite_frame_cols[0] = 2;
    app->sprite_frame_rows[0] = 2; // frame_count = 4
    app->turtles[0].sprite_index = 0;
    app->turtles[0].sprite_frame = 0;

    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex("ANIMATESPRITE 0 5\nPRINT \"done", tokens, MAX_VM_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));

    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("done\n");
    // 5 advances from frame 0, wrapping mod 4: 0->1->2->3->0->1
    if (app->turtles[0].sprite_frame != 1) {
        failures++;
        printf("FAIL %s: final frame -- expected 1 (5 advances mod 4), got %d\n", current_test, app->turtles[0].sprite_frame);
    }

    free(vm); free(chunk); parse_result_destroy(result); free(app);
}

// TEXT/SHOW/SAVE and general file I/O (see docs/BYTECODE_VM_DESIGN.md's
// note on the newly-found gap this closes): unlike sprites/PAUSE, these
// already existed in ast_eval too, but real file I/O against build/
// means running the same script through BOTH engines back-to-back
// (shadow_diff_vm's own convention) would double up non-idempotent
// side effects (a second DELETEFILE/OPENAPPEND on the same real file
// behaves differently the second time) -- so these are direct,
// single-engine VM tests instead, matching tests/test_eval.c's own
// corpus almost verbatim, just pointed at the VM via start_vm_session.

TEST(test_text_on_a_defined_procedure_lists_its_body_words) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO double :x\n  OUTPUT :x * 2\nEND\nPRINT TEXT \"double", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("OUTPUT :x * 2\n");
    end_vm_session(s);
}

TEST(test_text_on_an_undefined_procedure_reports_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT TEXT \"nope", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "TEXT: no such procedure \"nope") == NULL) {
        failures++;
        printf("FAIL %s: expected \"no such procedure\", got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_show_prints_a_procedures_own_definition) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO double :x\n  OUTPUT :x * 2\nEND\nSHOW \"double", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("TO double :x\nOUTPUT :x * 2\n\nEND\n");
    end_vm_session(s);
}

TEST(test_show_of_an_undefined_procedure_reports_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("SHOW \"nope", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "SHOW: no such procedure \"nope") == NULL) {
        failures++;
        printf("FAIL %s: expected \"no such procedure\", got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_save_writes_every_procedure_to_a_file) {
    const char *path = "build/test_vm_save.txt";
    remove(path);
    VmRunResult status;
    VmTestSession s = start_vm_session("TO double :x\n  OUTPUT :x * 2\nEND\nSAVE \"build/test_vm_save.txt", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        failures++;
        printf("FAIL %s: SAVE did not create %s\n", current_test, path);
    } else {
        if (strcmp(contents, "TO double :x\nOUTPUT :x * 2\n\nEND\n\n") != 0) {
            failures++;
            printf("FAIL %s: unexpected SAVE content: \"%s\"\n", current_test, contents);
        }
        g_free(contents);
    }
    remove(path);
    end_vm_session(s);
}

TEST(test_openwrite_fileprint_close_writes_the_file) {
    const char *path = "build/test_vm_openwrite.txt";
    remove(path);
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "MAKE \"ch OPENWRITE \"build/test_vm_openwrite.txt\n"
        "FILEPRINT :ch \"hello\n"
        "FILEPRINT :ch \"world\n"
        "CLOSE :ch", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        failures++;
        printf("FAIL %s: OPENWRITE did not create %s\n", current_test, path);
    } else {
        if (strcmp(contents, "hello\nworld\n") != 0) {
            failures++;
            printf("FAIL %s: unexpected file content: \"%s\"\n", current_test, contents);
        }
        g_free(contents);
    }
    remove(path);
    end_vm_session(s);
}

TEST(test_openread_readline_reads_lines_then_eof) {
    const char *path = "build/test_vm_openread.txt";
    remove(path);
    g_file_set_contents(path, "line one\nline two\n", -1, NULL);
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "MAKE \"ch OPENREAD \"build/test_vm_openread.txt\n"
        "PRINT READLINE :ch\n"
        "PRINT READLINE :ch\n"
        "PRINT EOF? :ch\n"
        "CLOSE :ch", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("line one\nline two\nTRUE\n");
    remove(path);
    end_vm_session(s);
}

TEST(test_openread_readword_reads_whitespace_delimited_words_then_eof) {
    const char *path = "build/test_vm_readword.txt";
    remove(path);
    g_file_set_contents(path, "  hello   world\nfoo\tbar\n", -1, NULL);
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "MAKE \"ch OPENREAD \"build/test_vm_readword.txt\n"
        "PRINT READWORD :ch\n"
        "PRINT READWORD :ch\n"
        "PRINT READWORD :ch\n"
        "PRINT READWORD :ch\n"
        "PRINT READWORD :ch\n"
        "PRINT EOF? :ch\n"
        "CLOSE :ch", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("hello\nworld\nfoo\nbar\n\nTRUE\n");
    remove(path);
    end_vm_session(s);
}

TEST(test_openread_readchar_reads_one_raw_byte_at_a_time_then_eof) {
    const char *path = "build/test_vm_readchar.txt";
    remove(path);
    g_file_set_contents(path, "ab\n", -1, NULL);
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "MAKE \"ch OPENREAD \"build/test_vm_readchar.txt\n"
        "PRINT READCHAR :ch\n"
        "PRINT READCHAR :ch\n"
        "IF READCHAR :ch = CHAR 10 [PRINT \"got-newline]\n"
        "PRINT READCHAR :ch\n"
        "CLOSE :ch", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("a\nb\ngot-newline\n\n");
    remove(path);
    end_vm_session(s);
}

TEST(test_readword_and_readchar_of_an_invalid_channel_report_empty) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT READWORD 99\nPRINT READCHAR 99", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("\n\n");
    end_vm_session(s);
}

TEST(test_openappend_appends_to_existing_content) {
    const char *path = "build/test_vm_openappend.txt";
    remove(path);
    g_file_set_contents(path, "first\n", -1, NULL);
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "MAKE \"ch OPENAPPEND \"build/test_vm_openappend.txt\n"
        "FILEPRINT :ch \"second\n"
        "CLOSE :ch", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        failures++;
        printf("FAIL %s: OPENAPPEND did not create %s\n", current_test, path);
    } else {
        if (strcmp(contents, "first\nsecond\n") != 0) {
            failures++;
            printf("FAIL %s: unexpected file content: \"%s\"\n", current_test, contents);
        }
        g_free(contents);
    }
    remove(path);
    end_vm_session(s);
}

TEST(test_openread_of_missing_file_returns_negative_one) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT OPENREAD \"build/test_vm_does_not_exist.txt", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("-1\n");
    end_vm_session(s);
}

TEST(test_close_of_invalid_channel_reports_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("CLOSE 3", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("CLOSE: no such open channel\n");
    end_vm_session(s);
}

TEST(test_deletefile_removes_a_file) {
    const char *path = "build/test_vm_deletefile.txt";
    g_file_set_contents(path, "x", -1, NULL);
    VmRunResult status;
    VmTestSession s = start_vm_session("DELETEFILE \"build/test_vm_deletefile.txt", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("");
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        failures++;
        printf("FAIL %s: %s still exists after DELETEFILE\n", current_test, path);
    }
    end_vm_session(s);
}

TEST(test_deletefile_of_missing_file_reports_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("DELETEFILE \"build/test_vm_does_not_exist_either.txt", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "DELETEFILE: could not delete") == NULL) {
        failures++;
        printf("FAIL %s: expected \"could not delete\", got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_directory_returns_a_list_without_crashing) {
    // Not LIST? DIRECTORY -- LIST? turned out to be a separate,
    // already-known-missing gap (see docs/ROADMAP.md's own note on the
    // broader audit this batch's testing surfaced), not something this
    // batch fixes. COUNT already existed in vm.c before this batch. The
    // exact count is cwd-dependent, so only checked for "a well-formed
    // non-negative number", not an exact value.
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT COUNT DIRECTORY", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    char *end;
    double count = strtod(captured_output, &end);
    if (end == captured_output || count < 0) {
        failures++;
        printf("FAIL %s: expected a non-negative number, got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

// Stage D of the bytecode save/load/assembler initiative
// (docs/ROADMAP.md): SAVEBYTECODE writes the currently-running chunk's
// own bytecode_disassemble text to a real file; a SEPARATE session
// (its own fresh LogoApp/ParseResult/BytecodeChunk/Vm -- start_vm_session
// makes a new one every call, same as every other test in this file)
// then LOADBYTECODEs that file and is checked for the same output --
// this is the actual save-to-disk/load-from-disk scenario the whole
// initiative was for, not just an in-memory round-trip.
TEST(test_savebytecode_then_loadbytecode_round_trips_through_a_real_file) {
    const char *path = "build/test_vm_savebytecode.lgb";
    VmRunResult save_status;
    VmTestSession save_session = start_vm_session(
        "TO FACT :N\nIF :N <= 1 [OUTPUT 1]\nOUTPUT :N * FACT :N - 1\nEND\n"
        "PRINT FACT 6\nSAVEBYTECODE \"build/test_vm_savebytecode.lgb", &save_status);
    expect_status(save_status, VM_RUN_HALTED, "save run");
    if (strstr(captured_output, "720\n") == NULL || strstr(captured_output, "Saved build/test_vm_savebytecode.lgb\n") == NULL) {
        failures++;
        printf("FAIL %s: unexpected save-run output \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(save_session);

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        failures++;
        printf("FAIL %s: %s was not created\n", current_test, path);
        return;
    }

    VmRunResult load_status;
    char load_script[128];
    snprintf(load_script, sizeof(load_script), "LOADBYTECODE \"%s", path);
    VmTestSession load_session = start_vm_session(load_script, &load_status);
    expect_status(load_status, VM_RUN_HALTED, "load run");
    // SAVEBYTECODE saved the WHOLE currently-running chunk, including
    // its own SAVEBYTECODE call (baked into the saved instruction
    // stream right along with the PRINT before it) -- so re-running the
    // loaded chunk correctly re-executes that same statement too,
    // writing the file again and printing "Saved ..." a second time.
    // Not a bug: proof the reloaded program is genuinely running its
    // own real top-level statements, not some trimmed-down subset.
    expect_output("720\nSaved build/test_vm_savebytecode.lgb\n");
    end_vm_session(load_session);

    remove(path);
}

TEST(test_loadbytecode_of_a_missing_file_reports_an_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("LOADBYTECODE \"build/test_vm_does_not_exist.lgb", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("LOADBYTECODE: could not read file\n");
    end_vm_session(s);
}

TEST(test_loadbytecode_of_a_malformed_file_reports_the_assembler_error) {
    const char *path = "build/test_vm_loadbytecode_corrupt.lgb";
    g_file_set_contents(path, "not valid bytecode at all\n", -1, NULL);
    VmRunResult status;
    char script[128];
    snprintf(script, sizeof(script), "LOADBYTECODE \"%s", path);
    VmTestSession s = start_vm_session(script, &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "LOADBYTECODE:") == NULL) {
        failures++;
        printf("FAIL %s: expected a LOADBYTECODE error, got \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
    remove(path);
}

TEST(test_savebytecode_of_an_unwritable_path_reports_an_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("SAVEBYTECODE \"/nonexistent_directory_xyz/out.lgb", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("SAVEBYTECODE: could not write file\n");
    end_vm_session(s);
}

// ONKEY/ONCLICK (docs/ROADMAP.md's "Mouse/keyboard event triggers"):
// registration-time validation only -- exec_onkey/exec_onclick and the
// call_builtin dispatch are the whole part of this feature that lives
// in vm.c and is reachable headlessly. Actually *firing* a registered
// handler (fire_onkey/fire_onclick, the retained-chunk bookkeeping in
// handle_vm_result) is ui.c-only, GTK-event-driven, and untestable here
// the same way WAIT's real timer or WAITKEY's real keypress already
// are -- see this file's own comment on why WAIT/WAITKEY are tested via
// vm_run/vm_resume_with_key directly instead of a live GTK loop.
TEST(test_onkey_registers_a_valid_one_param_handler) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO HANDLER :KEY\nEND\nONKEY \"handler", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("");
    if (strcasecmp(s.app->onkey_handler, "HANDLER") != 0) {
        failures++;
        printf("FAIL %s: onkey_handler -- expected \"HANDLER\", got \"%s\"\n", current_test, s.app->onkey_handler);
    }
    end_vm_session(s);
}

TEST(test_onkey_of_a_missing_procedure_reports_an_error_and_registers_nothing) {
    VmRunResult status;
    VmTestSession s = start_vm_session("ONKEY \"nosuchproc", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ONKEY: no such procedure \"nosuchproc\n");
    if (s.app->onkey_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onkey_handler -- expected empty, got \"%s\"\n", current_test, s.app->onkey_handler);
    }
    end_vm_session(s);
}

TEST(test_onkey_of_a_wrong_arity_procedure_reports_an_error_and_leaves_the_previous_handler) {
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "TO GOOD :KEY\nEND\nTO BAD :A :B\nEND\nONKEY \"good\nONKEY \"bad", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ONKEY: procedure \"bad\" must take exactly one input (the pressed key's name)\n");
    if (strcasecmp(s.app->onkey_handler, "GOOD") != 0) {
        failures++;
        printf("FAIL %s: onkey_handler -- expected the earlier \"GOOD\" to survive, got \"%s\"\n", current_test, s.app->onkey_handler);
    }
    end_vm_session(s);
}

TEST(test_offkey_clears_a_registered_handler) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO HANDLER :KEY\nEND\nONKEY \"handler\nOFFKEY", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (s.app->onkey_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onkey_handler -- expected empty after OFFKEY, got \"%s\"\n", current_test, s.app->onkey_handler);
    }
    end_vm_session(s);
}

TEST(test_onclick_registers_a_valid_three_param_handler) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO HANDLER :X :Y :BUTTON\nEND\nONCLICK \"handler", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("");
    if (strcasecmp(s.app->onclick_handler, "HANDLER") != 0) {
        failures++;
        printf("FAIL %s: onclick_handler -- expected \"HANDLER\", got \"%s\"\n", current_test, s.app->onclick_handler);
    }
    end_vm_session(s);
}

TEST(test_onclick_of_a_missing_procedure_reports_an_error_and_registers_nothing) {
    VmRunResult status;
    VmTestSession s = start_vm_session("ONCLICK \"nosuchproc", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ONCLICK: no such procedure \"nosuchproc\n");
    if (s.app->onclick_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onclick_handler -- expected empty, got \"%s\"\n", current_test, s.app->onclick_handler);
    }
    end_vm_session(s);
}

TEST(test_onclick_of_a_wrong_arity_procedure_reports_an_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO BAD :X :Y\nEND\nONCLICK \"bad", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ONCLICK: procedure \"bad\" must take exactly three inputs (x, y, button)\n");
    if (s.app->onclick_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onclick_handler -- expected empty, got \"%s\"\n", current_test, s.app->onclick_handler);
    }
    end_vm_session(s);
}

TEST(test_offclick_clears_a_registered_handler) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO HANDLER :X :Y :BUTTON\nEND\nONCLICK \"handler\nOFFCLICK", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (s.app->onclick_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onclick_handler -- expected empty after OFFCLICK, got \"%s\"\n", current_test, s.app->onclick_handler);
    }
    end_vm_session(s);
}

TEST(test_onmousemove_registers_a_valid_two_param_handler) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO HANDLER :X :Y\nEND\nONMOUSEMOVE \"handler", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("");
    if (strcasecmp(s.app->onmousemove_handler, "HANDLER") != 0) {
        failures++;
        printf("FAIL %s: onmousemove_handler -- expected \"HANDLER\", got \"%s\"\n", current_test, s.app->onmousemove_handler);
    }
    end_vm_session(s);
}

TEST(test_onmousemove_of_a_missing_procedure_reports_an_error_and_registers_nothing) {
    VmRunResult status;
    VmTestSession s = start_vm_session("ONMOUSEMOVE \"nosuchproc", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ONMOUSEMOVE: no such procedure \"nosuchproc\n");
    if (s.app->onmousemove_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onmousemove_handler -- expected empty, got \"%s\"\n", current_test, s.app->onmousemove_handler);
    }
    end_vm_session(s);
}

TEST(test_onmousemove_of_a_wrong_arity_procedure_reports_an_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO BAD :X\nEND\nONMOUSEMOVE \"bad", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ONMOUSEMOVE: procedure \"bad\" must take exactly two inputs (x, y)\n");
    if (s.app->onmousemove_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onmousemove_handler -- expected empty, got \"%s\"\n", current_test, s.app->onmousemove_handler);
    }
    end_vm_session(s);
}

TEST(test_offmousemove_clears_a_registered_handler) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO HANDLER :X :Y\nEND\nONMOUSEMOVE \"handler\nOFFMOUSEMOVE", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (s.app->onmousemove_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onmousemove_handler -- expected empty after OFFMOUSEMOVE, got \"%s\"\n", current_test, s.app->onmousemove_handler);
    }
    end_vm_session(s);
}

TEST(test_onkeyup_registers_a_valid_one_param_handler) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO HANDLER :KEY\nEND\nONKEYUP \"handler", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("");
    if (strcasecmp(s.app->onkeyup_handler, "HANDLER") != 0) {
        failures++;
        printf("FAIL %s: onkeyup_handler -- expected \"HANDLER\", got \"%s\"\n", current_test, s.app->onkeyup_handler);
    }
    end_vm_session(s);
}

TEST(test_onkeyup_of_a_missing_procedure_reports_an_error_and_registers_nothing) {
    VmRunResult status;
    VmTestSession s = start_vm_session("ONKEYUP \"nosuchproc", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ONKEYUP: no such procedure \"nosuchproc\n");
    if (s.app->onkeyup_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onkeyup_handler -- expected empty, got \"%s\"\n", current_test, s.app->onkeyup_handler);
    }
    end_vm_session(s);
}

TEST(test_onkeyup_of_a_wrong_arity_procedure_reports_an_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO BAD :A :B\nEND\nONKEYUP \"bad", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ONKEYUP: procedure \"bad\" must take exactly one input (the released key's name)\n");
    if (s.app->onkeyup_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onkeyup_handler -- expected empty, got \"%s\"\n", current_test, s.app->onkeyup_handler);
    }
    end_vm_session(s);
}

TEST(test_offkeyup_clears_a_registered_handler) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO HANDLER :KEY\nEND\nONKEYUP \"handler\nOFFKEYUP", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (s.app->onkeyup_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onkeyup_handler -- expected empty after OFFKEYUP, got \"%s\"\n", current_test, s.app->onkeyup_handler);
    }
    end_vm_session(s);
}

TEST(test_onrelease_registers_a_valid_three_param_handler) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO HANDLER :X :Y :BUTTON\nEND\nONRELEASE \"handler", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("");
    if (strcasecmp(s.app->onrelease_handler, "HANDLER") != 0) {
        failures++;
        printf("FAIL %s: onrelease_handler -- expected \"HANDLER\", got \"%s\"\n", current_test, s.app->onrelease_handler);
    }
    end_vm_session(s);
}

TEST(test_onrelease_of_a_missing_procedure_reports_an_error_and_registers_nothing) {
    VmRunResult status;
    VmTestSession s = start_vm_session("ONRELEASE \"nosuchproc", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ONRELEASE: no such procedure \"nosuchproc\n");
    if (s.app->onrelease_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onrelease_handler -- expected empty, got \"%s\"\n", current_test, s.app->onrelease_handler);
    }
    end_vm_session(s);
}

TEST(test_onrelease_of_a_wrong_arity_procedure_reports_an_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO BAD :X :Y\nEND\nONRELEASE \"bad", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ONRELEASE: procedure \"bad\" must take exactly three inputs (x, y, button)\n");
    if (s.app->onrelease_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onrelease_handler -- expected empty, got \"%s\"\n", current_test, s.app->onrelease_handler);
    }
    end_vm_session(s);
}

TEST(test_offrelease_clears_a_registered_handler) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO HANDLER :X :Y :BUTTON\nEND\nONRELEASE \"handler\nOFFRELEASE", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (s.app->onrelease_handler[0] != '\0') {
        failures++;
        printf("FAIL %s: onrelease_handler -- expected empty after OFFRELEASE, got \"%s\"\n", current_test, s.app->onrelease_handler);
    }
    end_vm_session(s);
}

TEST(test_type_exact_output_has_no_trailing_newline) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TYPE \"Hello,\nTYPE 5", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("Hello,5");
    end_vm_session(s);
}

// Loads and runs the REAL example file, not a copy of its text embedded
// in this test -- catches exactly the kind of bug that motivated this
// test in the first place: examples/readword_readchar.logo used TYPE,
// which parsed and ran fine against the OLD tree-walking engine (its
// own examples/type_show.logo has used it since Phase 1) but was never
// ported to src/parser.c's own grammar, so bin/logomotive silently failed to
// parse it -- caught only because a live "does the scratch file get
// cleaned up" launch check isn't enough to distinguish "ran
// successfully" from "failed to parse and never ran at all" (both look
// identical from outside: no crash, no leftover file). A parse-error
// count check here would have caught it immediately.
TEST(test_readword_readchar_example_runs_correctly) {
    char *source = NULL;
    if (!g_file_get_contents("examples/readword_readchar.logo", &source, NULL, NULL)) {
        failures++;
        printf("FAIL %s: could not read examples/readword_readchar.logo\n", current_test);
        return;
    }
    VmRunResult status;
    VmTestSession s = start_vm_session(source, &status);
    g_free(source);
    if (s.result->error_count > 0) {
        failures++;
        printf("FAIL %s: example failed to parse (%d error(s)): %s\n", current_test, s.result->error_count, s.result->errors[0].message);
        end_vm_session(s);
        return;
    }
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output(
        "one-word-at-a-time:\nhello\nworld\ngoodbye\nFALSE\n\nTRUE\n"
        "one-character-at-a-time:\nhello\n");
    end_vm_session(s);
}

// examples/type_show.logo is a Phase 1 relic (predates the bytecode VM
// entirely) that was ALSO silently broken by the same TYPE gap as
// examples/readword_readchar.logo, undetected until now since nothing
// ever loaded and ran the real file against the VM.
TEST(test_type_show_example_runs_correctly) {
    char *source = NULL;
    if (!g_file_get_contents("examples/type_show.logo", &source, NULL, NULL)) {
        failures++;
        printf("FAIL %s: could not read examples/type_show.logo\n", current_test);
        return;
    }
    VmRunResult status;
    VmTestSession s = start_vm_session(source, &status);
    g_free(source);
    if (s.result->error_count > 0) {
        failures++;
        printf("FAIL %s: example failed to parse (%d error(s)): %s\n", current_test, s.result->error_count, s.result->errors[0].message);
        end_vm_session(s);
        return;
    }
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output(
        "Hello,world!\nTO square :size\nREPEAT 4 [FD :size RT 90]\n\nEND\n"
        "SHOW: no such procedure \"nope\n");
    end_vm_session(s);
}

// examples/objects.logo used the OLD tree-walking engine's SEND
// calling convention (SEND obj "message, with trailing positional
// args) -- bin/logomotive's own SEND is deliberately fixed at 3 args (obj,
// message, arglist; see src/parser.c's own BUILTIN_SIGNATURES comment
// on why: the compiler can't know a dynamically-resolved method's
// arity at parse time). The mismatch didn't produce a SEND-shaped
// error at all: the parser treats SEND's missing 3rd argument slot as
// "parse one more expression," which greedily swallows every
// following statement (SEND is expression-capable, so each subsequent
// SEND/NEW call looks like a valid nested expression) until it hits a
// token that can't start an expression -- which turned out to be a
// `TO` two blocks later, an already-confusing, misleading error found
// via [[logo_project_workflow]]'s "audit every examples/*.logo file"
// step, fixed 2026-08-12 by rewriting the file to the current 3-arg
// SEND syntax rather than reverting the VM's own deliberate design.
TEST(test_objects_example_runs_correctly) {
    char *source = NULL;
    if (!g_file_get_contents("examples/objects.logo", &source, NULL, NULL)) {
        failures++;
        printf("FAIL %s: could not read examples/objects.logo\n", current_test);
        return;
    }
    VmRunResult status;
    VmTestSession s = start_vm_session(source, &status);
    g_free(source);
    if (s.result->error_count > 0) {
        failures++;
        printf("FAIL %s: example failed to parse (%d error(s)): %s\n", current_test, s.result->error_count, s.result->errors[0].message);
        end_vm_session(s);
        return;
    }
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("dog Woof\nanimal generic\npuppy\ndog hi alice\ndog\ndog\n");
    end_vm_session(s);
}

// 2026-08-11 Terrapin Logo comparison (docs/ROADMAP.md's "Language
// completeness"): the 24-builtin easy-tier batch, one test per builtin
// (or small logical group), same VM-only headless style as the event
// triggers above.

TEST(test_pi_reports_the_constant) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT PI", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("3.14159\n");
    end_vm_session(s);
}

TEST(test_rerandom_makes_random_reproducible) {
    VmRunResult status1;
    VmTestSession s1 = start_vm_session("RERANDOM\nPRINT RANDOM 1000000\nPRINT RANDOM 1000000", &status1);
    expect_status(status1, VM_RUN_HALTED, "run 1");
    char first_run[64];
    snprintf(first_run, sizeof(first_run), "%s", captured_output);
    end_vm_session(s1);

    VmRunResult status2;
    VmTestSession s2 = start_vm_session("RERANDOM\nPRINT RANDOM 1000000\nPRINT RANDOM 1000000", &status2);
    expect_status(status2, VM_RUN_HALTED, "run 2");
    if (strcmp(first_run, captured_output) != 0) {
        failures++;
        printf("FAIL %s: RERANDOM -- expected the same sequence both times, got \"%s\" then \"%s\"\n", current_test, first_run, captured_output);
    }
    end_vm_session(s2);
}

TEST(test_ascii_and_char_round_trip) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT ASCII \"A\nPRINT CHAR 65", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("65\nA\n");
    end_vm_session(s);
}

TEST(test_ascii_of_more_than_one_character_reports_an_error) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT ASCII \"AB", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("ASCII: expected a single character\n0\n");
    end_vm_session(s);
}

TEST(test_uppercase_and_lowercase) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT UPPERCASE \"MixedCase\nPRINT LOWERCASE \"MixedCase", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("MIXEDCASE\nmixedcase\n");
    end_vm_session(s);
}

TEST(test_bitwise_operators) {
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "PRINT BITAND 12 10\nPRINT BITOR 12 10\nPRINT BITXOR 12 10\nPRINT BITNOT 0\nPRINT LSHIFT 1 4\nPRINT RSHIFT 16 4", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("8\n14\n6\n-1\n16\n1\n");
    end_vm_session(s);
}

TEST(test_shift_operators_tolerate_a_negative_count_by_flipping_direction) {
    // LSHIFT 16 (-4), not LSHIFT 16 -4 -- unparenthesized, the trailing
    // -4 greedily continues as binary subtraction (16 - 4 = 12) instead
    // of starting a fresh second argument, the same general "unary
    // minus needs disambiguating" quirk docs/LANGUAGE.md's own MOD
    // example sidesteps by only ever showing a negative *first*
    // argument.
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT LSHIFT 16 (-4)\nPRINT RSHIFT 1 (-4)", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("1\n16\n");
    end_vm_session(s);
}

TEST(test_arctan2_and_the_rest_of_the_trig_family) {
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "PRINT ARCTAN2 1 1\nPRINT SEC 60\nPRINT CSC 90\nPRINT COT 45\nPRINT ASEC 2\nPRINT ACSC 1\nPRINT ACOT 1", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("45\n2\n1\n1\n60\n90\n45\n");
    end_vm_session(s);
}

TEST(test_time_date_milliseconds_report_plausible_values) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT TIME\nPRINT DATE\nPRINT MILLISECONDS", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    // TIME is HH:MM:SS, DATE is YYYY-MM-DD -- checked structurally
    // (colon/dash positions, right lengths), not against a specific
    // clock reading, which would make this test flaky. MILLISECONDS is
    // checked as a plausible epoch value directly in C (relational
    // operators like > only work in ARG_CONDITION position, e.g. IF's
    // own argument -- not in PRINT's plain ARG_EXPR position).
    char time_part[16], date_part[16], ms_part[32];
    if (sscanf(captured_output, "%15[^\n]\n%15[^\n]\n%31s", time_part, date_part, ms_part) != 3 ||
        strlen(time_part) != 8 || time_part[2] != ':' || time_part[5] != ':' ||
        strlen(date_part) != 10 || date_part[4] != '-' || date_part[7] != '-' ||
        atof(ms_part) < 1.7e12) {
        failures++;
        printf("FAIL %s: unexpected TIME/DATE/MILLISECONDS output \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_definedp_reports_whether_a_procedure_exists) {
    VmRunResult status;
    VmTestSession s = start_vm_session("TO FOO\nEND\nPRINT DEFINED? \"foo\nPRINT DEFINED? \"bar", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("TRUE\nFALSE\n");
    end_vm_session(s);
}

TEST(test_turtles_reports_the_active_turtle_count) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT TURTLES", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("1\n");
    end_vm_session(s);
}

// REPCOUNT: the innermost REPEAT's 1-indexed pass number
// (OP_REPCOUNT_PUSH/INCR/POP, see bytecode.h's own comment). Follow-up
// to the 2026-08-11 Terrapin comparison's easy tier -- flagged there as
// needing real VM support, unlike that batch's own plain wrappers.

TEST(test_repcount_is_minus_one_outside_any_repeat) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT REPCOUNT", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("-1\n");
    end_vm_session(s);
}

TEST(test_repcount_counts_up_from_one_each_pass) {
    VmRunResult status;
    VmTestSession s = start_vm_session("REPEAT 5 [PRINT REPCOUNT]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("1\n2\n3\n4\n5\n");
    end_vm_session(s);
}

TEST(test_repcount_reverts_to_minus_one_after_the_loop_ends) {
    VmRunResult status;
    VmTestSession s = start_vm_session("REPEAT 3 [PRINT REPCOUNT]\nPRINT REPCOUNT", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("1\n2\n3\n-1\n");
    end_vm_session(s);
}

TEST(test_repcount_is_minus_one_for_a_zero_pass_repeat) {
    VmRunResult status;
    VmTestSession s = start_vm_session("REPEAT 0 [PRINT REPCOUNT]\nPRINT REPCOUNT", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("-1\n");
    end_vm_session(s);
}

TEST(test_repcount_in_nested_repeats_reports_the_innermost_one) {
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "REPEAT 2 [\n"
        "  PRINT SENTENCE \"outer: REPCOUNT\n"
        "  REPEAT 2 [PRINT SENTENCE \"inner: REPCOUNT]\n"
        "  PRINT SENTENCE \"outer-again: REPCOUNT\n"
        "]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output(
        "outer: 1\ninner: 1\ninner: 2\nouter-again: 1\n"
        "outer: 2\ninner: 1\ninner: 2\nouter-again: 2\n");
    end_vm_session(s);
}

TEST(test_repcount_propagates_into_a_procedure_called_from_inside_repeat) {
    // Dynamic, not lexical: a procedure called from inside a REPEAT
    // sees the CALLER's REPCOUNT, same as Terrapin's own documented
    // behavior -- vm->repcount_stack is genuine per-Vm runtime state,
    // unaffected by the called procedure's own separate VmFrame/scope.
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "TO REPORT\n"
        "  PRINT REPCOUNT\n"
        "END\n"
        "REPEAT 3 [report]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("1\n2\n3\n");
    end_vm_session(s);
}

TEST(test_repcount_pop_still_balances_after_an_uncaught_throw_exits_the_loop_early) {
    // THROW inside the loop body jumps straight to the loop's own end
    // (OP_CHECK_THROW), skipping OP_REPCOUNT_INCR -- but must still
    // reach OP_REPCOUNT_POP, or repcount_depth would leak and every
    // REPEAT for the rest of the script would report the wrong count.
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "REPEAT 5 [\n"
        "  IF REPCOUNT = 2 [THROW \"stop]\n"
        "  PRINT REPCOUNT\n"
        "]\n"
        "REPEAT 2 [PRINT REPCOUNT]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "1\n") == NULL || strstr(captured_output, "THROW: no CATCH found") == NULL ||
        strstr(captured_output, "1\n2\n") == NULL) {
        failures++;
        printf("FAIL %s: unexpected output \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_repcount_survives_100_levels_of_recursive_nesting) {
    // Real recursion depth (not just lexical nesting) exercising
    // repcount_stack/repcount_depth at real scale -- each level pushes
    // its own REPEAT's REPCOUNT entry via a recursive call, same
    // stress-testing spirit as this file's own other recursion-depth
    // checks.
    VmRunResult status;
    VmTestSession s = start_vm_session(
        "TO DEEPREP :N\n"
        "  IF :N = 0 [STOP]\n"
        "  REPEAT 3 [\n"
        "    IF REPCOUNT = 3 [deeprep :N - 1]\n"
        "  ]\n"
        "END\n"
        "deeprep 100\n"
        "PRINT REPCOUNT", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("-1\n");
    end_vm_session(s);
}

// EVAL: runs each element of a list independently and collects the
// results into a new list (Terrapin's own "runs list and collects
// outputs"). Follow-up to the 2026-08-11 Terrapin comparison's easy
// tier -- the last item left there, flagged as needing real design
// work rather than a plain wrapper.

TEST(test_eval_runs_each_sublist_as_code_and_collects_the_results) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT EVAL [[3 * 4] [SQRT 16] [1 + 2]]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("12 4 3\n");
    end_vm_session(s);
}

TEST(test_eval_passes_a_plain_value_element_through_unchanged) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT EVAL [5 hello [3 * 4]]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("5 hello 12\n");
    end_vm_session(s);
}

TEST(test_eval_of_an_empty_list_is_an_empty_list) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT EVAL []", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("\n");
    end_vm_session(s);
}

TEST(test_eval_of_a_non_list_passes_it_through_unchanged) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT EVAL 5", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("5\n");
    end_vm_session(s);
}

TEST(test_eval_output_length_always_matches_input_length) {
    // A code element that runs but doesn't itself OUTPUT a value (a
    // command, not an operator) reports the same "didn't output a
    // value" error any other expression-position misuse already does
    // elsewhere in this language -- not an EVAL-specific error path --
    // and still contributes one (empty) slot to the result, keeping
    // the output list the same length as the input.
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT COUNT EVAL [[PRINT 5] [3 * 4] [PRINT 6]]", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    if (strstr(captured_output, "5\n") == NULL || strstr(captured_output, "6\n") == NULL ||
        strstr(captured_output, "3\n") == NULL) {
        failures++;
        printf("FAIL %s: unexpected output \"%s\"\n", current_test, captured_output);
    }
    end_vm_session(s);
}

TEST(test_range_counts_up_or_down_by_one) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT RANGE 1 5\nPRINT RANGE 5 1", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("1 2 3 4 5\n5 4 3 2 1\n");
    end_vm_session(s);
}

TEST(test_spacedrange_generates_equally_spaced_numbers) {
    VmRunResult status;
    VmTestSession s = start_vm_session("PRINT SPACEDRANGE 0 10 3", &status);
    expect_status(status, VM_RUN_HALTED, "run");
    expect_output("0 5 10\n");
    end_vm_session(s);
}

// Stage C of the bytecode save/load/assembler initiative
// (docs/ROADMAP.md): disassembles a normally-compiled chunk, feeds the
// resulting text straight back through bytecode_assemble into a FRESH
// chunk, then runs that reassembled chunk against a completely empty
// AstPool (node_count=0 -- no original AST at all) and checks its
// output still matches the original run exactly. tests/test_bytecode.c
// already covers structural round-tripping (same instructions/proc
// metadata) without needing GTK; this is the one behavioral check that
// genuinely needs the full Vm/LogoApp stack -- proving a hand-
// assembled or reloaded-from-disk chunk can actually execute standalone,
// not merely look structurally identical.
TEST(test_a_disassembled_then_reassembled_chunk_runs_standalone_without_the_original_ast) {
    const char *source = "TO FACT :N\nIF :N <= 1 [OUTPUT 1]\nOUTPUT :N * FACT :N - 1\nEND\nPRINT FACT 6";
    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex(source, tokens, MAX_VM_TEST_TOKENS);
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);

    captured_output[0] = '\0';
    LogoApp *app1 = new_app();
    BytecodeChunk *chunk1 = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk1);
    Vm *vm1 = calloc(1, sizeof(Vm));
    vm_run(vm1, app1, &result->pool, chunk1, start_pc);
    char original_output[4096];
    snprintf(original_output, sizeof(original_output), "%s", captured_output);

    char *text = bytecode_disassemble_to_string(chunk1, NULL);

    BytecodeChunk *chunk2 = calloc(1, sizeof(BytecodeChunk));
    char asm_err[256];
    if (!bytecode_assemble(text, chunk2, asm_err, sizeof(asm_err))) {
        failures++;
        printf("FAIL %s: bytecode_assemble failed: %s\n", current_test, asm_err);
    } else if (chunk2->start_pc != start_pc) {
        failures++;
        printf("FAIL %s: reassembled chunk's own start_pc %d != original %d\n", current_test, chunk2->start_pc, start_pc);
    } else {
        captured_output[0] = '\0';
        LogoApp *app2 = new_app();
        AstPool *empty_pool = calloc(1, sizeof(AstPool));
        Vm *vm2 = calloc(1, sizeof(Vm));
        // Uses chunk2's OWN recovered start_pc, not the original's --
        // proving bytecode_assemble's "START:" line round-trips this
        // correctly, not just that reusing the original value happens
        // to work.
        vm_run(vm2, app2, empty_pool, chunk2, chunk2->start_pc);
        if (strcmp(captured_output, original_output) != 0) {
            failures++;
            printf("FAIL %s: output differs\n  original:   \"%s\"\n  reassembled: \"%s\"\n",
                   current_test, original_output, captured_output);
        }
        free(vm2);
        free(app2);
        free(empty_pool);
    }

    free(text);
    free(chunk1);
    free(vm1);
    free(app1);
    free(chunk2);
    parse_result_destroy(result);
}

int main(void) {
    RUN(test_literals_and_print);
    RUN(test_type_prints_without_a_trailing_newline);
    RUN(test_pr_is_a_print_alias);
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
    RUN(test_cleartext_is_a_silent_noop_headless);
    RUN(test_loadpic_and_savepic_are_silent_noops_headless);
    RUN(test_tone_playsound_stopsound_are_silent_noops_headless);
    RUN(test_playsound_of_a_real_file_is_silent);
    RUN(test_playsound_of_a_missing_file_reports_could_not_load);
    RUN(test_mousepos_mousex_mousey_button_report_headless_defaults);
    RUN(test_backtrace_lists_the_call_stack_innermost_first);
    RUN(test_backtrace_at_the_top_level_shows_only_top_level);
    RUN(test_exectime_reports_a_plausible_nonnegative_number);
    RUN(test_exectime_used_as_a_command_still_runs_its_code);
    RUN(test_exectime_self_referential_is_capped_not_a_crash);
    RUN(test_pen_and_canvas_appearance_smoke);
    RUN(test_drawing_primitives_smoke);
    RUN(test_plot_records_a_dot_raster_op_at_the_turtles_position_and_pen_color);
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
    RUN(test_wait_suspends_with_the_right_duration_then_resumes_and_completes);
    RUN(test_wait_with_a_non_positive_duration_never_suspends_at_all);
    RUN(test_vm_recursion_goes_well_past_the_old_shared_200_limit);
    RUN(test_a_word_literal_longer_than_the_old_63_byte_instr_text_limit_is_not_truncated);
    RUN(test_waitkey_suspends_then_resumes_with_the_pressed_key_and_completes);
    RUN(test_waitkey_suspends_and_resumes_correctly_through_several_nested_procedure_calls);
    RUN(test_waitkey_directly_inside_a_map_template_reports_an_error_instead_of_suspending);
    RUN(test_wait_directly_inside_a_foreach_template_reports_an_error_instead_of_suspending);
    RUN(test_launch_inside_run_reports_an_error_instead_of_spawning_an_agent);
    RUN(test_await_inside_run_reports_an_error_instead_of_blocking);
    RUN(test_yield_inside_run_reports_an_error_instead_of_yielding);
    RUN(test_setspeed_default_is_instant_no_delay_at_all);
    RUN(test_setspeed_makes_a_motion_command_suspend_then_resume_and_continue);
    RUN(test_setspeed_does_not_delay_non_motion_commands);
    RUN(test_speed_reports_the_current_setting);
    RUN(test_setspeed_inside_a_foreach_template_is_a_silent_no_op_not_an_error);
    RUN(test_input_suspends_then_resumes_with_the_submitted_line_and_completes);
    RUN(test_input_directly_inside_a_map_template_reports_an_error_instead_of_suspending);
    RUN(test_pause_suspends_with_the_right_level_then_resumes_and_completes);
    RUN(test_continue_with_nothing_paused_reports_an_error);
    RUN(test_pause_directly_inside_a_foreach_template_reports_an_error_instead_of_suspending);
    RUN(test_two_nested_pauses_share_one_pause_depth_and_resume_in_lifo_order);
    RUN(test_math_operators_abs_sqrt_power_round);
    RUN(test_mod_takes_the_sign_of_the_divisor);
    RUN(test_trig_operators_use_degrees);
    RUN(test_log_and_exp_operators);
    RUN(test_random_stays_within_bounds);
    RUN(test_type_predicates);
    RUN(test_turtle_command_short_aliases_match_their_full_names);
    RUN(test_pick_from_a_list_word_array_and_bare_number);
    RUN(test_pick_reports_error_on_empty_list_or_word);
    RUN(test_flatten_collects_every_leaf_discarding_nesting);
    RUN(test_parse_tokenizes_a_values_printed_text_by_whitespace);
    RUN(test_subst_replaces_matching_elements_including_a_whole_sublist);
    RUN(test_dot_product_of_two_numeric_lists);
    RUN(test_cross_product_of_two_3element_lists);
    RUN(test_memberp_on_list_and_number);
    RUN(test_memberp_on_word_is_substring);
    RUN(test_apply_calls_a_procedure_with_a_list_of_arguments);
    RUN(test_apply_unknown_procedure_reports_an_error);
    RUN(test_apply_wrong_argument_count_reports_an_error);
    RUN(test_apply_never_hands_back_a_value_even_when_the_procedure_outputs);
    RUN(test_run_executes_a_stored_list_as_source);
    RUN(test_run_self_referential_is_capped_not_a_crash);
    RUN(test_run_bare_stop_at_the_top_level_reports_the_escaping_gap);
    RUN(test_load_runs_a_files_contents_as_logo_source);
    RUN(test_load_of_missing_file_reports_error);
    RUN(test_load_defined_procedure_is_callable_from_the_loading_script);
    RUN(test_load_of_a_file_with_a_bare_stop_at_its_top_level_reports_the_escaping_gap);
    RUN(test_setsprite_of_unknown_name_reports_error_and_leaves_default);
    RUN(test_setsprite_none_is_a_silent_reset_to_default);
    RUN(test_stampsprite_records_default_triangle_when_no_sprite_set);
    RUN(test_loadsprite_is_a_safe_no_op_with_no_gui);
    RUN(test_loadspritesheet_with_zero_cols_reports_error);
    RUN(test_loadsprite_of_a_real_image_loads_silently_and_registers_it);
    RUN(test_loadspritesheet_of_a_real_image_loads_silently_and_registers_the_grid);
    RUN(test_loadsprite_of_a_missing_file_reports_could_not_load);
    RUN(test_loadspritesheet_of_a_missing_file_reports_could_not_load);
    RUN(test_setspriteframe_without_a_sprite_set_reports_error);
    RUN(test_setspriteframe_out_of_range_reports_error_and_leaves_the_frame_unchanged);
    RUN(test_animatesprite_without_a_sprite_set_reports_error_and_does_not_suspend);
    RUN(test_animatesprite_directly_inside_a_map_template_reports_an_error_instead_of_suspending);
    RUN(test_animatesprite_with_a_positive_delay_advances_one_frame_per_suspend_then_completes);
    RUN(test_animatesprite_with_a_non_positive_delay_runs_all_frames_synchronously);
    RUN(test_text_on_a_defined_procedure_lists_its_body_words);
    RUN(test_text_on_an_undefined_procedure_reports_error);
    RUN(test_show_prints_a_procedures_own_definition);
    RUN(test_show_of_an_undefined_procedure_reports_error);
    RUN(test_save_writes_every_procedure_to_a_file);
    RUN(test_openwrite_fileprint_close_writes_the_file);
    RUN(test_openread_readline_reads_lines_then_eof);
    RUN(test_openread_readword_reads_whitespace_delimited_words_then_eof);
    RUN(test_openread_readchar_reads_one_raw_byte_at_a_time_then_eof);
    RUN(test_readword_and_readchar_of_an_invalid_channel_report_empty);
    RUN(test_openappend_appends_to_existing_content);
    RUN(test_openread_of_missing_file_returns_negative_one);
    RUN(test_close_of_invalid_channel_reports_error);
    RUN(test_deletefile_removes_a_file);
    RUN(test_deletefile_of_missing_file_reports_error);
    RUN(test_directory_returns_a_list_without_crashing);
    RUN(test_savebytecode_then_loadbytecode_round_trips_through_a_real_file);
    RUN(test_loadbytecode_of_a_missing_file_reports_an_error);
    RUN(test_loadbytecode_of_a_malformed_file_reports_the_assembler_error);
    RUN(test_savebytecode_of_an_unwritable_path_reports_an_error);
    RUN(test_onkey_registers_a_valid_one_param_handler);
    RUN(test_onkey_of_a_missing_procedure_reports_an_error_and_registers_nothing);
    RUN(test_onkey_of_a_wrong_arity_procedure_reports_an_error_and_leaves_the_previous_handler);
    RUN(test_offkey_clears_a_registered_handler);
    RUN(test_onclick_registers_a_valid_three_param_handler);
    RUN(test_onclick_of_a_missing_procedure_reports_an_error_and_registers_nothing);
    RUN(test_onclick_of_a_wrong_arity_procedure_reports_an_error);
    RUN(test_offclick_clears_a_registered_handler);
    RUN(test_onmousemove_registers_a_valid_two_param_handler);
    RUN(test_onmousemove_of_a_missing_procedure_reports_an_error_and_registers_nothing);
    RUN(test_onmousemove_of_a_wrong_arity_procedure_reports_an_error);
    RUN(test_offmousemove_clears_a_registered_handler);
    RUN(test_onkeyup_registers_a_valid_one_param_handler);
    RUN(test_onkeyup_of_a_missing_procedure_reports_an_error_and_registers_nothing);
    RUN(test_onkeyup_of_a_wrong_arity_procedure_reports_an_error);
    RUN(test_offkeyup_clears_a_registered_handler);
    RUN(test_onrelease_registers_a_valid_three_param_handler);
    RUN(test_onrelease_of_a_missing_procedure_reports_an_error_and_registers_nothing);
    RUN(test_onrelease_of_a_wrong_arity_procedure_reports_an_error);
    RUN(test_offrelease_clears_a_registered_handler);
    RUN(test_type_exact_output_has_no_trailing_newline);
    RUN(test_readword_readchar_example_runs_correctly);
    RUN(test_type_show_example_runs_correctly);
    RUN(test_objects_example_runs_correctly);
    RUN(test_pi_reports_the_constant);
    RUN(test_rerandom_makes_random_reproducible);
    RUN(test_ascii_and_char_round_trip);
    RUN(test_ascii_of_more_than_one_character_reports_an_error);
    RUN(test_uppercase_and_lowercase);
    RUN(test_bitwise_operators);
    RUN(test_shift_operators_tolerate_a_negative_count_by_flipping_direction);
    RUN(test_arctan2_and_the_rest_of_the_trig_family);
    RUN(test_time_date_milliseconds_report_plausible_values);
    RUN(test_definedp_reports_whether_a_procedure_exists);
    RUN(test_turtles_reports_the_active_turtle_count);
    RUN(test_repcount_is_minus_one_outside_any_repeat);
    RUN(test_repcount_counts_up_from_one_each_pass);
    RUN(test_repcount_reverts_to_minus_one_after_the_loop_ends);
    RUN(test_repcount_is_minus_one_for_a_zero_pass_repeat);
    RUN(test_repcount_in_nested_repeats_reports_the_innermost_one);
    RUN(test_repcount_propagates_into_a_procedure_called_from_inside_repeat);
    RUN(test_repcount_pop_still_balances_after_an_uncaught_throw_exits_the_loop_early);
    RUN(test_repcount_survives_100_levels_of_recursive_nesting);
    RUN(test_eval_runs_each_sublist_as_code_and_collects_the_results);
    RUN(test_eval_passes_a_plain_value_element_through_unchanged);
    RUN(test_eval_of_an_empty_list_is_an_empty_list);
    RUN(test_eval_of_a_non_list_passes_it_through_unchanged);
    RUN(test_eval_output_length_always_matches_input_length);
    RUN(test_range_counts_up_or_down_by_one);
    RUN(test_spacedrange_generates_equally_spaced_numbers);
    RUN(test_a_disassembled_then_reassembled_chunk_runs_standalone_without_the_original_ast);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
