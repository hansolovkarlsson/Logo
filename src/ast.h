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
    AST_PROC_DEF,      // TO name :p1 :p2 ... END -- .text holds the name, .param_count/.param_names hold parameters, first_child = the body AST_BLOCK
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
    char param_names[AST_MAX_PARAMS][32];   // AST_PROC_DEF, without the leading ':' (matching Procedure.param_names' own convention)

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

#endif
