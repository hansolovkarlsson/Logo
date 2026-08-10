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
    LogoToken tokens[MAX_VM_TEST_TOKENS];
    int n = logo_lex(source, tokens, MAX_VM_TEST_TOKENS);
    if (n < 0) {
        failures++;
        printf("FAIL %s: source needed more than %d tokens\n", current_test, MAX_VM_TEST_TOKENS);
        return;
    }
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    if (result->error_count > 0) {
        failures++;
        printf("FAIL %s: parse reported %d error(s), first: %s\n",
               current_test, result->error_count, result->errors[0].message);
        parse_result_destroy(result);
        return;
    }

    captured_output[0] = '\0';
    LogoApp *tree_app = new_app();
    ast_eval(tree_app, &result->pool, result->program);
    char tree_output[4096];
    snprintf(tree_output, sizeof(tree_output), "%s", captured_output);

    captured_output[0] = '\0';
    LogoApp *vm_app = new_app();
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));
    vm_run(vm, vm_app, &result->pool, chunk, start_pc);
    char vm_output[4096];
    snprintf(vm_output, sizeof(vm_output), "%s", captured_output);

    if (strcmp(tree_output, vm_output) != 0) {
        failures++;
        printf("FAIL %s: output differs\n  tree: \"%s\"\n  vm:   \"%s\"\n",
               current_test, tree_output, vm_output);
    }

    free(chunk);
    free(vm);
    free(tree_app);
    free(vm_app);
    parse_result_destroy(result);
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

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
