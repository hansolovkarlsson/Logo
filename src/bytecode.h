#ifndef LOGO_BYTECODE_H
#define LOGO_BYTECODE_H

// bytecode.h
//
// Stage 2's instruction format (see docs/BYTECODE_VM_DESIGN.md): what
// compiler.c produces from Stage 1's AST and vm.c executes. No
// dependency on GTK/GLib/interpreter.h -- same "an X is just data"
// reasoning as ast.h/lexer.h/parser.h, and what keeps this
// independently testable before any VM exists to run it.
//
// Deliberately an array of tagged structs, not packed bytes the way a
// "real" bytecode format usually is: none of this project's own four
// motivations for a VM (see docs/BYTECODE_VM_DESIGN.md's "Why")
// depend on compact encoding, and a flat struct matches AstNode's own
// established "some unused space per node, in exchange for simplicity"
// convention exactly. Every call (builtin or user procedure) always
// leaves exactly one value on vm.c's own value stack -- mirroring
// eval.c's own exec_call, whose *result already defaults to num_val(0)
// unless a real operator overwrites it -- so OP_POP is what a
// statement-position call compiles to afterward, not a special case.
//
// Conditions are ordinary values, not a separate sub-language: this
// language's own AND/OR never short-circuit (confirmed directly in
// eval.c's eval_condition -- both sides are always evaluated), so
// OP_AND/OP_OR/OP_NOT/the OP_CMP_* family are plain pop-N-push-1
// operators pushing num_val(0)/num_val(1), consumed by
// OP_JUMP_IF_FALSE via the same eval_is_truthy every other truthiness
// check in this codebase already uses. No fused compare-and-jump
// instructions needed.

typedef enum {
    OP_PUSH_NUMBER,   // .number -> push num_val(.number)
    OP_PUSH_WORD,     // .text -> push word_val(.text)
    OP_PUSH_VAR,      // .text (variable name) -> push its current value (num_val(0) if unbound, matching eval_expr's own AST_VARREF default)
    OP_SET_VAR,       // .text (variable name); pop 1 -> MAKE-equivalent assignment
    OP_POP,           // discard the top of the value stack

    OP_ADD, OP_SUB, OP_MUL, OP_DIV,   // pop 2, push 1
    OP_NEG,                             // pop 1, push 1

    OP_CMP_LT, OP_CMP_GT, OP_CMP_EQ, OP_CMP_LE, OP_CMP_GE, OP_CMP_NE, // pop 2, push num_val(0/1)
    OP_NOT,                                                             // pop 1, push num_val(0/1)
    OP_AND, OP_OR,                                                     // pop 2, push num_val(0/1) -- no short-circuiting, see the file comment above

    OP_JUMP,           // .a = target instruction index
    OP_JUMP_IF_FALSE,  // pop 1 (via eval_is_truthy); .a = target instruction index if falsy

    OP_CALL_BUILTIN,   // .text = builtin name, .a = argc; pops .a values (in argument order), pushes exactly 1 result (a real value, or a dummy num_val(0) for a void builtin like SETITEM -- vm.c's own call_builtin dispatch tracks which per call, for OP_CHECK_OUTPUT's sake)
    OP_CALL_PROC,      // .a = target pc (this procedure's first instruction, resolved by compiler.c's own backpatching -- see its file comment), .b = argc, .text = procedure name (for OP_CHECK_OUTPUT's own error message); pops .a args, pushes a new VM frame, pushes exactly 1 result once the call returns
    OP_CHECK_OUTPUT,   // .text = the call's own name (builtin or procedure); only ever emitted right after an OP_CALL_BUILTIN/OP_CALL_PROC/OP_VOID_RESULT used in *expression* position -- if that call didn't actually produce a value (vm.c's own last_call_produced_output flag), reports "<name>: didn't output a value" and replaces the top of the value stack with word_val(""), mirroring eval.c's own exec_call/eval_expr wrapper exactly. A statement-position call gets a plain OP_POP instead, never this.

    OP_OUTPUT,   // pop 1 -> return from the current procedure with that value (this call's own OP_CHECK_OUTPUT, if any, sees a real output)
    OP_STOP,     // return from the current procedure with num_val(0) and no real output -- also what the compiler appends after every procedure body's own last statement, so falling off the end behaves exactly like interpreter.c's own call_procedure/eval.c's own call_ast_procedure defaulting to "no OUTPUT was ever called"

    OP_HALT,     // stop the whole run (the very last instruction of the top-level program)

    OP_PUSH_LIST_LITERAL, // .a = the AST_LIST_LITERAL node's own index in the AstPool vm.c was given; pushes eval_build_list_literal(app, pool, .a). A deliberate exception to every other opcode's "self-contained payload" shape: a list literal's contents are recursive, variable-arity raw AST data (untyped words, possibly nested literals -- see ast.h's own AST_LIST_LITERAL comment), not a flat value any Instr field could hold, so the compiler leaves the literal in the AST and the VM (which already needs `pool` for OP_CALL_PROC's own find_proc_def lookups) builds it fresh each time this instruction runs -- same as eval_expr's own AST_LIST_LITERAL case builds it fresh on every visit, not once.

    OP_LOCAL,        // .text = variable name; declares a same-named scope-local variable in the current call (0 if it's genuinely new, otherwise a no-op) -- no stack effect of its own; compile_call always follows this with OP_VOID_RESULT, same as OP_SET_VAR (MAKE)
    OP_VOID_RESULT,  // pushes num_val(0) and clears last_call_produced_output -- the special-form equivalent of a void builtin's OP_CALL_BUILTIN return, for MAKE/LOCAL/WHILE (constructs with their own dedicated opcodes/compiled shape that still need to honor the "every call leaves exactly one value, and OP_CHECK_OUTPUT can tell whether it was a real one" convention every ordinary call follows)
} OpCode;

// AST_MAX_TEXT-sized would be wasteful here (512 bytes per instruction,
// most of which never carry text at all) -- a variable/procedure/
// builtin name only ever needs to fit what AST_MAX_TEXT itself already
// bounds identifiers to in practice; 64 bytes matches the same budget
// interpreter.c's own name buffers (Procedure.name, Variable.name-
// adjacent conventions) already use elsewhere.
#define INSTR_MAX_TEXT 64

typedef struct {
    OpCode op;
    int a, b;              // generic integer operands: jump targets, argc, a procedure's own target pc
    double number;           // OP_PUSH_NUMBER's literal
    char text[INSTR_MAX_TEXT]; // OP_PUSH_WORD/OP_PUSH_VAR/OP_SET_VAR/OP_CALL_BUILTIN/OP_CALL_PROC/OP_CHECK_OUTPUT's own name
} Instr;

// A fixed-size pool of instructions, same "fixed pool, loud error if
// exceeded" discipline as AstPool/list_pool/every other fixed table in
// this codebase, not a new malloc-heavy style. ~700KB (Instr's own
// text[64] dominates) -- heap-allocate this, never a stack local, same
// rule as every other multi-KB-or-larger struct in this project
// (LogoApp, ParseResult, AstPool).
#define MAX_INSTRUCTIONS 8192

typedef struct {
    Instr code[MAX_INSTRUCTIONS];
    int count;
} BytecodeChunk;

// Appends `instr` to `chunk`, returning its own index (the position a
// jump/call target would need to record to point at it), or -1 if the
// chunk is full -- same "loud error, not silent truncation" policy as
// ast_alloc/list_alloc_node.
int bytecode_emit(BytecodeChunk *chunk, Instr instr);

#endif
