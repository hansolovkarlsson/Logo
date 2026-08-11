// test_bytecode.c
//
// Headless tests for bytecode.c's own chunk-level bookkeeping and
// bytecode_disassemble (Stage B of the bytecode save/load/assembler
// initiative -- see docs/ROADMAP.md's own "Bytecode save/load/
// assembler" section). No interpreter.h, no LogoApp, no GTK: bytecode.c
// depends on nothing but ast.h (see bytecode.h's own file comment), and
// compiler.c itself doesn't touch eval.h/interpreter.h either, so a
// real Logo snippet can be lexed/parsed/compiled into a genuine
// BytecodeChunk and disassembled entirely off the VM, matching
// test_parser.c/test_lexer.c's own "no GTK needed" shape rather than
// test_vm.c's shadow-diff one. Run via `make test-bytecode`.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lexer.h"
#include "../src/parser.h"
#include "../src/compiler.h"
#include "../src/bytecode.h"

static int failures = 0;
static const char *current_test = "";

#define TEST(name) static void name(void)
#define RUN(name) do { current_test = #name; name(); } while (0)

#define CHECK(cond) do { \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s: %s (line %d)\n", current_test, #cond, __LINE__); \
    } \
} while (0)

#define CHECK_CONTAINS(haystack, needle) do { \
    if (strstr((haystack), (needle)) == NULL) { \
        failures++; \
        printf("FAIL %s: expected to find %s (line %d)\n  in: %s\n", \
               current_test, #needle, __LINE__, (haystack)); \
    } \
} while (0)

#define MAX_TEST_TOKENS 512

// Lexes/parses/compiles `source` into a genuine BytecodeChunk, heap-
// allocated same as ParseResult (both well over 1MB -- see
// test_parser.c's own parse_source comment for why this never goes on
// the stack).
static BytecodeChunk *compile_source(const char *source) {
    LogoToken tokens[MAX_TEST_TOKENS];
    int n = logo_lex(source, tokens, MAX_TEST_TOKENS);
    if (n < 0) {
        failures++;
        printf("FAIL %s: source needed more than %d tokens\n", current_test, MAX_TEST_TOKENS);
        return NULL;
    }
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    if (result->error_count > 0) {
        failures++;
        printf("FAIL %s: source had %d parse error(s)\n", current_test, result->error_count);
        free(result);
        return NULL;
    }
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    compile_program(&result->pool, result->program, chunk);
    free(result);
    return chunk;
}

// Disassembles `chunk` into a freshly malloc'd NUL-terminated string
// via open_memstream (POSIX, confirmed available on this project's own
// macOS/darwin toolchain) -- the streaming-sink shape append_output
// already uses elsewhere in this codebase, just captured into memory
// instead of routed to a GTK widget. Caller frees the result.
static char *disassemble_to_string(const BytecodeChunk *chunk) {
    char *buf = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&buf, &size);
    bytecode_disassemble(chunk, f);
    fclose(f);
    return buf;
}

TEST(test_opcode_name_covers_a_representative_sample) {
    CHECK(strcmp(bytecode_opcode_name(OP_PUSH_NUMBER), "OP_PUSH_NUMBER") == 0);
    CHECK(strcmp(bytecode_opcode_name(OP_CALL_PROC), "OP_CALL_PROC") == 0);
    CHECK(strcmp(bytecode_opcode_name(OP_MOTION_DELAY), "OP_MOTION_DELAY") == 0);
    CHECK(strcmp(bytecode_opcode_name((OpCode)999), "OP_UNKNOWN") == 0);
}

TEST(test_disassemble_a_number_literal_and_arithmetic) {
    BytecodeChunk *chunk = compile_source("PRINT 1 + 2");
    CHECK(chunk != NULL);
    if (!chunk) return;
    char *text = disassemble_to_string(chunk);
    CHECK_CONTAINS(text, "OP_PUSH_NUMBER 1");
    CHECK_CONTAINS(text, "OP_PUSH_NUMBER 2");
    CHECK_CONTAINS(text, "OP_ADD");
    CHECK_CONTAINS(text, "OP_CALL_BUILTIN PRINT argc=1");
    free(text);
    free(chunk);
}

TEST(test_disassemble_resolves_word_literal_inline) {
    BytecodeChunk *chunk = compile_source("PRINT \"hello");
    CHECK(chunk != NULL);
    if (!chunk) return;
    char *text = disassemble_to_string(chunk);
    CHECK_CONTAINS(text, "OP_PUSH_WORD \"hello\"");
    free(text);
    free(chunk);
}

TEST(test_disassemble_resolves_list_literal_inline) {
    BytecodeChunk *chunk = compile_source("PRINT [1 2 3]");
    CHECK(chunk != NULL);
    if (!chunk) return;
    char *text = disassemble_to_string(chunk);
    CHECK_CONTAINS(text, "OP_PUSH_LIST_LITERAL [1 2 3]");
    free(text);
    free(chunk);
}

TEST(test_disassemble_jump_targets_use_at_sign_syntax) {
    BytecodeChunk *chunk = compile_source("IF 1 = 1 [PRINT \"yes]");
    CHECK(chunk != NULL);
    if (!chunk) return;
    char *text = disassemble_to_string(chunk);
    CHECK_CONTAINS(text, "OP_JUMP_IF_FALSE @");
    free(text);
    free(chunk);
}

TEST(test_disassemble_lists_a_procedure_in_procs_and_labels_its_entry_point) {
    BytecodeChunk *chunk = compile_source("TO DOUBLE :N\nOUTPUT :N * 2\nEND\nPRINT DOUBLE 5");
    CHECK(chunk != NULL);
    if (!chunk) return;
    char *text = disassemble_to_string(chunk);
    CHECK_CONTAINS(text, "PROCS:");
    CHECK_CONTAINS(text, "DOUBLE start=@");
    CHECK_CONTAINS(text, "params=(N)");
    CHECK_CONTAINS(text, "---- source ----");
    CHECK_CONTAINS(text, "OUTPUT :N * 2");
    CHECK_CONTAINS(text, "DOUBLE:\n");
    CHECK_CONTAINS(text, "OP_CALL_PROC DOUBLE @");
    CHECK_CONTAINS(text, "argc=1");
    free(text);
    free(chunk);
}

TEST(test_disassemble_omits_an_erased_procedure_from_procs) {
    BytecodeChunk *chunk = compile_source("TO FOO\nOUTPUT 1\nEND\nERASE \"FOO");
    CHECK(chunk != NULL);
    if (!chunk) return;
    bytecode_erase_proc(chunk, "FOO");
    char *text = disassemble_to_string(chunk);
    CHECK_CONTAINS(text, "PROCS:\n\n"); // no entries left between the header and the blank line before CODE:
    free(text);
    free(chunk);
}

TEST(test_disassemble_header_summarizes_counts) {
    BytecodeChunk *chunk = compile_source("PRINT \"hi");
    CHECK(chunk != NULL);
    if (!chunk) return;
    char *text = disassemble_to_string(chunk);
    CHECK_CONTAINS(text, "instruction");
    CHECK_CONTAINS(text, "0 proc");
    CHECK_CONTAINS(text, "1 word literal");
    free(text);
    free(chunk);
}

// Asserts two chunks are equal in every way bytecode_assemble is
// supposed to reproduce: same instruction stream (op/a/b/number/text)
// and same proc metadata (name/start_pc/param_count/param_names/
// source_text) -- NOT byte-identical word_literals[]/list_literals[]
// tables, since bytecode_assemble appends into a freshly empty chunk
// via the same bytecode_add_word_literal/bytecode_add_list_literal
// compiler.c itself uses, so indices land in the same order but the
// tables' own unused tail slots differ from a directly-compiled
// chunk's (irrelevant -- nothing ever reads past word_literal_count/
// list_literal_count).
static void check_chunks_equal(const BytecodeChunk *a, const BytecodeChunk *b) {
    CHECK(a->start_pc == b->start_pc);
    CHECK(a->count == b->count);
    int n = a->count < b->count ? a->count : b->count;
    for (int i = 0; i < n; i++) {
        const Instr *x = &a->code[i], *y = &b->code[i];
        if (x->op != y->op || x->a != y->a || x->b != y->b || x->number != y->number || strcmp(x->text, y->text) != 0) {
            failures++;
            printf("FAIL %s: instr %d differs (line %d)\n", current_test, i, __LINE__);
        }
    }
    CHECK(a->proc_count == b->proc_count);
    int pn = a->proc_count < b->proc_count ? a->proc_count : b->proc_count;
    for (int i = 0; i < pn; i++) {
        CHECK(strcmp(a->procs[i].name, b->procs[i].name) == 0);
        CHECK(a->procs[i].start_pc == b->procs[i].start_pc);
        CHECK(a->procs[i].param_count == b->procs[i].param_count);
        CHECK(strcmp(a->procs[i].source_text, b->procs[i].source_text) == 0);
    }
}

static void check_roundtrip(const char *source) {
    BytecodeChunk *original = compile_source(source);
    if (!original) return;
    char *text = disassemble_to_string(original);
    BytecodeChunk *reassembled = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    if (!bytecode_assemble(text, reassembled, err, sizeof(err))) {
        failures++;
        printf("FAIL %s: bytecode_assemble failed: %s\n", current_test, err);
    } else {
        check_chunks_equal(original, reassembled);
    }
    free(text);
    free(original);
    free(reassembled);
}

TEST(test_disassemble_then_assemble_roundtrips_arithmetic) {
    check_roundtrip("PRINT 1 + 2 * 3");
}

TEST(test_disassemble_then_assemble_roundtrips_a_nested_list_literal) {
    check_roundtrip("PRINT [1 2 [3 4] \"a :b -5]");
}

TEST(test_disassemble_then_assemble_roundtrips_if_and_its_jump_targets) {
    check_roundtrip("IF 1 = 1 [PRINT \"yes] [PRINT \"no]");
}

TEST(test_disassemble_then_assemble_roundtrips_recursion) {
    check_roundtrip("TO FACT :N\nIF :N <= 1 [OUTPUT 1]\nOUTPUT :N * FACT :N - 1\nEND\nPRINT FACT 5");
}

TEST(test_disassemble_then_assemble_roundtrips_multiple_procedures) {
    check_roundtrip("TO A :X\nOUTPUT :X + 1\nEND\nTO B :Y\nOUTPUT A :Y * 2\nEND\nPRINT B 3");
}

TEST(test_disassemble_then_assemble_roundtrips_a_compiled_map_template) {
    check_roundtrip("PRINT MAP [? * 2] [1 2 3]");
}

TEST(test_assemble_accepts_hand_written_labels_with_a_forward_jump) {
    const char *asm_text =
        "START: @0\n"
        "CODE:\n"
        "  OP_PUSH_NUMBER 5\n"
        "  OP_JUMP_IF_FALSE skip\n"
        "  OP_PUSH_WORD \"yes\"\n"
        "  OP_JUMP done\n"
        "skip:\n"
        "  OP_PUSH_WORD \"no\"\n"
        "done:\n"
        "  OP_HALT\n";
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    int ok = bytecode_assemble(asm_text, chunk, err, sizeof(err));
    CHECK(ok);
    if (ok) {
        CHECK(chunk->count == 6);
        CHECK(chunk->start_pc == 0);
        CHECK(chunk->code[1].a == 4); // OP_JUMP_IF_FALSE skip -> the "skip:" label's own pc
        CHECK(chunk->code[3].a == 5); // OP_JUMP done -> the "done:" label's own pc
    }
    free(chunk);
}

TEST(test_assemble_accepts_a_symbolic_start_label) {
    // "START:" itself may reference a label too, not just "@N".
    const char *asm_text = "START: main\nCODE:\nmain:\n  OP_HALT\n";
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    CHECK(bytecode_assemble(asm_text, chunk, err, sizeof(err)));
    CHECK(chunk->start_pc == 0);
    free(chunk);
}

TEST(test_assemble_omits_the_optional_pc_column) {
    // No "<N>:" address prefix at all -- bytecode_assemble tracks its
    // own running address instead of trusting one.
    const char *asm_text = "START: @0\nCODE:\nOP_PUSH_NUMBER 1\nOP_PUSH_NUMBER 2\nOP_ADD\nOP_HALT\n";
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    CHECK(bytecode_assemble(asm_text, chunk, err, sizeof(err)));
    CHECK(chunk->count == 4);
    free(chunk);
}

TEST(test_assemble_does_not_trust_a_procs_own_stale_start_field) {
    // "start=@99" is wrong on purpose -- the real answer has to come
    // from resolving the "FOO:" label in CODE, exactly so a hand-editor
    // never has to keep this field in sync by hand.
    const char *asm_text =
        "START: @0\n"
        "PROCS:\n"
        "  FOO start=@99 argc=0 params=()\n"
        "  ---- source ----\n"
        "OUTPUT 1\n"
        "  ---- end source ----\n"
        "CODE:\n"
        "  OP_PUSH_NUMBER 7\n"
        "  OP_POP\n"
        "FOO:\n"
        "  OP_PUSH_NUMBER 1\n"
        "  OP_OUTPUT\n"
        "  OP_HALT\n";
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    CHECK(bytecode_assemble(asm_text, chunk, err, sizeof(err)));
    CHECK(chunk->proc_count == 1);
    if (chunk->proc_count == 1) CHECK(chunk->procs[0].start_pc == 2);
    free(chunk);
}

TEST(test_assemble_rejects_missing_start_line) {
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    CHECK(!bytecode_assemble("CODE:\n  OP_HALT\n", chunk, err, sizeof(err)));
    CHECK_CONTAINS(err, "START:");
    free(chunk);
}

TEST(test_assemble_rejects_an_unresolvable_start_target) {
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    CHECK(!bytecode_assemble("START: nowhere\nCODE:\n  OP_HALT\n", chunk, err, sizeof(err)));
    CHECK_CONTAINS(err, "nowhere");
    free(chunk);
}

TEST(test_assemble_rejects_an_unknown_opcode) {
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    CHECK(!bytecode_assemble("START: @0\nCODE:\n  OP_BOGUS\n", chunk, err, sizeof(err)));
    CHECK_CONTAINS(err, "OP_BOGUS");
    free(chunk);
}

TEST(test_assemble_rejects_an_undefined_label) {
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    CHECK(!bytecode_assemble("START: @0\nCODE:\n  OP_JUMP nowhere\n", chunk, err, sizeof(err)));
    CHECK_CONTAINS(err, "nowhere");
    free(chunk);
}

TEST(test_assemble_rejects_a_duplicate_label) {
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    CHECK(!bytecode_assemble("CODE:\nfoo:\n  OP_HALT\nfoo:\n  OP_HALT\n", chunk, err, sizeof(err)));
    CHECK_CONTAINS(err, "duplicate label");
    free(chunk);
}

TEST(test_assemble_rejects_missing_code_section) {
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    CHECK(!bytecode_assemble("PROCS:\n", chunk, err, sizeof(err)));
    CHECK_CONTAINS(err, "CODE:");
    free(chunk);
}

TEST(test_assemble_rejects_a_proc_with_no_matching_label) {
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    const char *asm_text =
        "START: @0\n"
        "PROCS:\n"
        "  FOO start=@0 argc=0 params=()\n"
        "  ---- source ----\n"
        "OUTPUT 1\n"
        "  ---- end source ----\n"
        "CODE:\n"
        "  OP_HALT\n";
    CHECK(!bytecode_assemble(asm_text, chunk, err, sizeof(err)));
    CHECK_CONTAINS(err, "FOO");
    free(chunk);
}

TEST(test_assemble_rejects_a_params_count_mismatch) {
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    const char *asm_text =
        "PROCS:\n"
        "  FOO start=@0 argc=2 params=(a)\n"
        "  ---- source ----\n"
        "OUTPUT 1\n"
        "  ---- end source ----\n"
        "CODE:\n"
        "FOO:\n"
        "  OP_HALT\n";
    CHECK(!bytecode_assemble(asm_text, chunk, err, sizeof(err)));
    free(chunk);
}

int main(void) {
    RUN(test_opcode_name_covers_a_representative_sample);
    RUN(test_disassemble_a_number_literal_and_arithmetic);
    RUN(test_disassemble_resolves_word_literal_inline);
    RUN(test_disassemble_resolves_list_literal_inline);
    RUN(test_disassemble_jump_targets_use_at_sign_syntax);
    RUN(test_disassemble_lists_a_procedure_in_procs_and_labels_its_entry_point);
    RUN(test_disassemble_omits_an_erased_procedure_from_procs);
    RUN(test_disassemble_header_summarizes_counts);
    RUN(test_disassemble_then_assemble_roundtrips_arithmetic);
    RUN(test_disassemble_then_assemble_roundtrips_a_nested_list_literal);
    RUN(test_disassemble_then_assemble_roundtrips_if_and_its_jump_targets);
    RUN(test_disassemble_then_assemble_roundtrips_recursion);
    RUN(test_disassemble_then_assemble_roundtrips_multiple_procedures);
    RUN(test_disassemble_then_assemble_roundtrips_a_compiled_map_template);
    RUN(test_assemble_accepts_hand_written_labels_with_a_forward_jump);
    RUN(test_assemble_accepts_a_symbolic_start_label);
    RUN(test_assemble_omits_the_optional_pc_column);
    RUN(test_assemble_does_not_trust_a_procs_own_stale_start_field);
    RUN(test_assemble_rejects_missing_start_line);
    RUN(test_assemble_rejects_an_unresolvable_start_target);
    RUN(test_assemble_rejects_an_unknown_opcode);
    RUN(test_assemble_rejects_an_undefined_label);
    RUN(test_assemble_rejects_a_duplicate_label);
    RUN(test_assemble_rejects_missing_code_section);
    RUN(test_assemble_rejects_a_proc_with_no_matching_label);
    RUN(test_assemble_rejects_a_params_count_mismatch);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
