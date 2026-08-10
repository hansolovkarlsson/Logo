#ifndef LOGO_AST_H
#define LOGO_AST_H

// ast.h
//
// Stage 1's AST (see docs/BYTECODE_VM_DESIGN.md) -- what the parser
// (parser.h/parser.c) builds from lexer.c's token stream. No
// dependency on GTK/GLib/interpreter.h, same as lexer.h: an AST is
// just data, and keeping it decoupled is what makes the parser
// testable on its own (tests/test_parser.c) before any tree-walking
// evaluator exists to consume it.
//
// Node storage mirrors this project's existing style for
// interpreter.c's ListNode/list_pool: a flat, fixed-size pool of nodes
// (AstProgram.nodes below), with children linked via indices
// (first_child/next_sibling) rather than pointers or a variable-length
// inline array -- one node can have any number of children (a
// procedure body's statement count, a call's argument count) without
// needing its own per-node capacity limit.

typedef enum {
    AST_NUMBER,        // .number holds the value
    AST_WORD,          // .text holds the word -- a quoted word, 'raw text', or (inside AST_LIST_LITERAL) one untyped list element, all the same node type: today's interpreter doesn't distinguish their VALUE_WORD-ness by origin either
    AST_VARREF,        // .text holds the variable name
    AST_LIST_LITERAL,  // [ ... ] as a value -- children are AST_WORD/AST_NUMBER leaves or nested AST_LIST_LITERAL, exactly mirroring today's LIST_ELEM_WORD/LIST_ELEM_LIST (list elements are untyped raw text, not expression subtrees -- a list literal's own contents were never operator-precedence-parsed, only whitespace/bracket-split)
    AST_BINOP,         // + - * / -- .binop distinguishes; first_child = left, its next_sibling = right
    AST_NEG,           // unary minus; first_child = operand (unary + is a no-op today -- parse_factor just returns its operand unchanged, so no node is needed for it either)
    AST_COMPARE,       // < > = <= >= <> -- .cmpop distinguishes; first_child = left, its next_sibling = right
    AST_NOT,           // first_child = operand
    AST_AND,           // first_child = left, its next_sibling = right
    AST_OR,            // first_child = left, its next_sibling = right
    AST_CALL,          // .text holds the command/procedure name; children (via first_child/next_sibling) are its arguments, each an expression subtree, an AST_BLOCK (for REPEAT/WHILE's own block argument), or (IF/IFELSE only) omitted entirely for a missing else-block
    AST_IF,            // first_child = condition, next sibling = true-AST_BLOCK, optional next sibling = false-AST_BLOCK (mirrors today's IF/IFELSE sharing one code path -- IF can take an optional second block too, ELSE keyword or not)
    AST_BLOCK,         // children (via first_child/next_sibling) are a sequence of statements
    AST_PROC_DEF,      // TO name :p1 :p2 ... END -- .text holds the name, .param_count/.param_names hold parameters, first_child = the body AST_BLOCK. .body_text/.body_len hold the body's own original source span (see the struct's own comment below) -- TEXT/SHOW/SAVE all need the literal text as typed, not a re-derived rendering of the parsed tree.
    AST_FOR,           // FOR [var start limit step] [block] -- .text holds the loop variable name (no leading ':'); first_child = start expr, next sibling = limit expr, optional next sibling = step expr, last sibling = the body AST_BLOCK. Irregular like AST_IF (a variable-length header, not a fixed ArgKind shape), so it isn't an AST_CALL.
} AstNodeType;

typedef enum { AST_OP_ADD, AST_OP_SUB, AST_OP_MUL, AST_OP_DIV } AstBinOp;
typedef enum { AST_CMP_LT, AST_CMP_GT, AST_CMP_EQ, AST_CMP_LE, AST_CMP_GE, AST_CMP_NE } AstCompareOp;

#define AST_MAX_TEXT 512
#define AST_MAX_PARAMS 8

// A flat struct covering every node type's payload (not a real C
// union) -- matches interpreter.c's own Value/PlistEntry style, at the
// cost of some unused space per node in exchange for simplicity.
typedef struct {
    AstNodeType type;
    int line, col;    // where this node's own token started, for error messages

    double number;                    // AST_NUMBER
    char text[AST_MAX_TEXT];           // AST_WORD/AST_VARREF/AST_CALL/AST_PROC_DEF's name
    AstBinOp binop;                     // AST_BINOP
    AstCompareOp cmpop;                  // AST_COMPARE

    int param_count;                       // AST_PROC_DEF
    char param_names[AST_MAX_PARAMS][32];   // AST_PROC_DEF, without the leading ':' -- NOT the same convention interpreter.c's own Procedure.param_names uses (its sscanf-based capture keeps the ':', confirmed directly in interpreter.c's own TO-parsing and append_procedure_text). This engine's own LOGO_TOK_VARREF already strips it at the lexer level (same as an ordinary :varref), and call_ast_procedure binds scope variable names from these directly with no colon involved anywhere -- self-consistent within this engine, just a real difference from interpreter.c worth knowing about the one place it actually matters (do_save/eval.c, reconstructing "TO name :params" text, has to add the ':' back explicitly).

    // AST_PROC_DEF only: the body's literal source text, from right
    // after the last parameter (or the name, if there are none) up to
    // (not including) the closing END -- a pointer *into the original
    // source buffer* passed to logo_lex, same convention LogoToken.text
    // itself already uses ("the source text outlives the parse"), not
    // a copy. Deliberately NOT embedded as a fixed char[] the way
    // .text/.param_names are: interpreter.c's own Procedure.body is
    // char[8192], and inlining a buffer that size into every AstNode
    // (most of which are nowhere near AST_PROC_DEF) would balloon
    // AstPool's already-multi-MB fixed array by roughly MAX_AST_NODES *
    // 8KB for no benefit. NULL/0 for every other node type, and for an
    // AST_PROC_DEF whose own END was never found (a parse error either
    // way, so nothing needs to read it).
    const char *body_text;
    int body_len;

    int first_child;   // -1 if none
    int next_sibling;   // -1 if this is its parent's last child
} AstNode;

#define MAX_AST_NODES 8192

typedef struct {
    AstNode nodes[MAX_AST_NODES];
    int node_count;
} AstPool;

// Allocates a fresh node of `type` from `pool`, with every field
// zeroed/defaulted (first_child/next_sibling both -1) except `type`
// and the token position. Returns its index, or -1 if the pool is
// full -- same "loud error, not silent corruption" policy as
// list_alloc_node in interpreter.c.
int ast_alloc(AstPool *pool, AstNodeType type, int line, int col);

// Appends `child_idx` to `parent_idx`'s child chain (as the new last
// child) -- the one place the first_child/next_sibling linking logic
// lives, rather than every call site in parser.c re-walking the chain
// to find its tail by hand. A no-op if either index is -1 (parsing
// a child that failed shouldn't corrupt its parent's otherwise-valid
// chain).
void ast_append_child(AstPool *pool, int parent_idx, int child_idx);

// Finds the AST_PROC_DEF node named `name` anywhere in `pool` (a
// linear scan, not just program_node's own top-level children --
// matches parser.c's own hoist_procedures reach), or -1 if there isn't
// one. Lives here rather than eval.c -- despite eval.c being its main
// caller -- because it touches nothing but `pool` itself: an AST-only
// operation belongs in the AST-only module, same "an X is just data"
// reasoning as the rest of this file, and it lets compiler.c (Stage 2,
// deliberately GTK/GLib/interpreter.h-free) call it directly rather
// than duplicating the scan or pulling in eval.h's own interpreter.h
// dependency just for this one pool-only function.
int find_proc_def(AstPool *pool, const char *name);

#endif
