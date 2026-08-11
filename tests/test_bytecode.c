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

int main(void) {
    RUN(test_opcode_name_covers_a_representative_sample);
    RUN(test_disassemble_a_number_literal_and_arithmetic);
    RUN(test_disassemble_resolves_word_literal_inline);
    RUN(test_disassemble_resolves_list_literal_inline);
    RUN(test_disassemble_jump_targets_use_at_sign_syntax);
    RUN(test_disassemble_lists_a_procedure_in_procs_and_labels_its_entry_point);
    RUN(test_disassemble_omits_an_erased_procedure_from_procs);
    RUN(test_disassemble_header_summarizes_counts);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
