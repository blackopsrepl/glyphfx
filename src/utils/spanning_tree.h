// Spanning-tree generators, ported from utils/spanningtree/. AldousBroder is
// deliberately not ported (no shipped effect uses it). EffectCharacter.links
// is kept ascending by id, the canonical iteration order.
#ifndef GLYPHFX_SPANNING_TREE_H
#define GLYPHFX_SPANNING_TREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine/ctx.h"

void link_characters(EngineCtx *ctx, CharId a, CharId b);

typedef struct {
    bool limit_to_text_boundary;
    CharId current_char;
    bool has_char_last_linked;
    CharId char_last_linked;
    CharId *char_link_order;
    size_t char_link_order_len;
    size_t char_link_order_cap;
    CharId *edge_chars;
    size_t edge_chars_len;
    size_t edge_chars_cap;
    bool has_edge_last_added;
    CharId edge_last_added;
    bool has_edge_last_popped;
    CharId edge_last_popped;
    bool complete;
} PrimsSimple;

int prims_simple_new(PrimsSimple *g, EngineCtx *ctx, bool has_start, CharId start, bool limit_to_text_boundary);
void prims_simple_free(PrimsSimple *g);
void prims_simple_step(PrimsSimple *g, EngineCtx *ctx);

typedef struct {
    CharId char_a;
    CharId char_b;
    int64_t weight;
} WeightedLink;

typedef struct {
    int64_t weight;
    WeightedLink *links;
    size_t len;
    size_t cap;
} WeightedBucket;

typedef struct {
    bool limit_to_text_boundary;
    int64_t *char_weights;  // indexed by arena slot
    size_t char_weights_len;
    bool has_char_last_linked;
    CharId char_last_linked;
    CharId *char_link_order;
    size_t char_link_order_len;
    size_t char_link_order_cap;
    CharId *neighbors_last_added;
    size_t neighbors_last_added_len;
    size_t neighbors_last_added_cap;
    bool complete;
    WeightedBucket *pending;
    size_t pending_len;
    size_t pending_cap;
} PrimsWeighted;

int prims_weighted_new(PrimsWeighted *g, EngineCtx *ctx, bool has_start, CharId start,
                       bool limit_to_text_boundary);
void prims_weighted_free(PrimsWeighted *g);
void prims_weighted_step(PrimsWeighted *g, EngineCtx *ctx);

typedef struct {
    bool limit_to_text_boundary;
    CharId current_char;
    bool has_char_last_linked;
    CharId char_last_linked;
    CharId *char_link_order;
    size_t char_link_order_len;
    size_t char_link_order_cap;
    CharId *stack;
    size_t stack_len;
    size_t stack_cap;
    bool has_stack_last_popped;
    CharId stack_last_popped;
    bool complete;
} RecursiveBacktracker;

int recursive_backtracker_new(RecursiveBacktracker *g, EngineCtx *ctx, bool has_start, CharId start,
                              bool limit_to_text_boundary);
void recursive_backtracker_free(RecursiveBacktracker *g);
void recursive_backtracker_step(RecursiveBacktracker *g, EngineCtx *ctx);

typedef struct {
    CharId starting_char;
    CharId *frontier;
    size_t frontier_len;
    size_t frontier_cap;
    CharId *explored;  // ascending set
    size_t explored_len;
    size_t explored_cap;
    CharId *explored_last_step;
    size_t explored_last_step_len;
    size_t explored_last_step_cap;
    CharId *char_explore_order;
    size_t char_explore_order_len;
    size_t char_explore_order_cap;
    bool complete;
} BreadthFirst;

int breadth_first_new(BreadthFirst *g, EngineCtx *ctx, bool has_start, CharId start, bool limit_to_text_boundary);
void breadth_first_free(BreadthFirst *g);
void breadth_first_step(BreadthFirst *g, EngineCtx *ctx);

#endif
