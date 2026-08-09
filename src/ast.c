// ast.c
//
// See ast.h for the node shapes and overall rationale. This file is
// just the fixed-pool bookkeeping (allocation, child-chain linking) --
// the parser (parser.c) is what actually decides what tree to build.

#include "ast.h"
#include <stddef.h> // NULL

int ast_alloc(AstPool *pool, AstNodeType type, int line, int col) {
    if (pool->node_count >= MAX_AST_NODES) return -1;
    int idx = pool->node_count++;
    AstNode *node = &pool->nodes[idx];
    node->type = type;
    node->line = line;
    node->col = col;
    node->number = 0;
    node->text[0] = '\0';
    node->param_count = 0;
    node->body_text = NULL;
    node->body_len = 0;
    node->first_child = -1;
    node->next_sibling = -1;
    return idx;
}

void ast_append_child(AstPool *pool, int parent_idx, int child_idx) {
    if (parent_idx < 0 || child_idx < 0) return;
    AstNode *parent = &pool->nodes[parent_idx];
    if (parent->first_child < 0) {
        parent->first_child = child_idx;
        return;
    }
    int cursor = parent->first_child;
    while (pool->nodes[cursor].next_sibling >= 0) {
        cursor = pool->nodes[cursor].next_sibling;
    }
    pool->nodes[cursor].next_sibling = child_idx;
}
