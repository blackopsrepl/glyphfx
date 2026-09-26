#include "utils/spanning_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void link_characters(EngineCtx *ctx, CharId a, CharId b) {
    CharId *la = ctx->terminal.arena.items[a].links;
    size_t la_len = ctx->terminal.arena.items[a].links_len;
    // insert sorted, no duplicates (links stored as dynamic vectors)
    size_t pos = 0;
    while (pos < la_len && la[pos] < b) pos++;
    if (pos < la_len && la[pos] == b) {
        return;
    }
    CharId *grown = realloc(la, (la_len + 1) * sizeof(CharId));
    if (!grown) return;
    memmove(grown + pos + 1, grown + pos, (la_len - pos) * sizeof(CharId));
    grown[pos] = b;
    ctx->terminal.arena.items[a].links = grown;
    ctx->terminal.arena.items[a].links_len = la_len + 1;

    CharId *lb = ctx->terminal.arena.items[b].links;
    size_t lb_len = ctx->terminal.arena.items[b].links_len;
    pos = 0;
    while (pos < lb_len && lb[pos] < a) pos++;
    if (pos < lb_len && lb[pos] == a) {
        return;
    }
    grown = realloc(lb, (lb_len + 1) * sizeof(CharId));
    if (!grown) return;
    memmove(grown + pos + 1, grown + pos, (lb_len - pos) * sizeof(CharId));
    grown[pos] = a;
    ctx->terminal.arena.items[b].links = grown;
    ctx->terminal.arena.items[b].links_len = lb_len + 1;
}

// neighbors in dict order (north, east, south, west), optional filters.
static CharId *get_neighbors(const EngineCtx *ctx, CharId id, bool unlinked_only, bool limit_to_text_boundary,
                             size_t *out_len) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    CharId raw[4] = {ch->north, ch->east, ch->south, ch->west};
    CharId *out = malloc(4 * sizeof(CharId));
    size_t n = 0;
    for (int i = 0; i < 4; i++) {
        if (raw[i] == CHAR_ID_NONE) continue;
        if (limit_to_text_boundary &&
            !canvas_coord_is_in_text(&ctx->terminal.canvas, ctx->terminal.arena.items[raw[i]].input_coord)) {
            continue;
        }
        if (unlinked_only && ctx->terminal.arena.items[raw[i]].links_len != 0) {
            continue;
        }
        out[n++] = raw[i];
    }
    *out_len = n;
    return out;
}

static int default_starting_char(EngineCtx *ctx, bool within_text_boundary, CharId *out) {
    Coord coord = canvas_random_coord(&ctx->terminal.canvas, &ctx->rng, false, within_text_boundary);
    CharId id = terminal_get_character_by_input_coord(&ctx->terminal, coord);
    if (id == CHAR_ID_NONE) {
        return -1;
    }
    *out = id;
    return 0;
}

// --- dynamic vector helpers ----------------------------------------------

static void ids_push(CharId **items, size_t *len, size_t *cap, CharId id) {
    if (*len == *cap) {
        size_t c = *cap ? *cap * 2 : 8;
        CharId *grown = realloc(*items, c * sizeof(CharId));
        if (!grown) return;
        *items = grown;
        *cap = c;
    }
    (*items)[(*len)++] = id;
}

// --- PrimsSimple ----------------------------------------------------------

int prims_simple_new(PrimsSimple *g, EngineCtx *ctx, bool has_start, CharId start, bool limit_to_text_boundary) {
    memset(g, 0, sizeof(*g));
    g->limit_to_text_boundary = limit_to_text_boundary;
    if (!has_start && default_starting_char(ctx, limit_to_text_boundary, &start) != 0) {
        return -1;
    }
    g->current_char = start;
    g->has_char_last_linked = true;
    g->char_last_linked = start;
    ids_push(&g->char_link_order, &g->char_link_order_len, &g->char_link_order_cap, start);
    ids_push(&g->edge_chars, &g->edge_chars_len, &g->edge_chars_cap, start);
    g->has_edge_last_added = true;
    g->edge_last_added = start;
    return 0;
}

void prims_simple_free(PrimsSimple *g) {
    free(g->char_link_order);
    free(g->edge_chars);
    memset(g, 0, sizeof(*g));
}

void prims_simple_step(PrimsSimple *g, EngineCtx *ctx) {
    if (g->edge_chars_len == 0) {
        g->complete = true;
        return;
    }
    size_t idx = (size_t)rng_randrange(&ctx->rng, 0, (int64_t)g->edge_chars_len);
    g->current_char = g->edge_chars[idx];
    memmove(g->edge_chars + idx, g->edge_chars + idx + 1, (g->edge_chars_len - idx - 1) * sizeof(CharId));
    g->edge_chars_len--;
    g->has_edge_last_popped = true;
    g->edge_last_popped = g->current_char;
    size_t un_len = 0;
    CharId *unlinked = get_neighbors(ctx, g->current_char, true, g->limit_to_text_boundary, &un_len);
    if (un_len > 0) {
        size_t nidx = (size_t)rng_randrange(&ctx->rng, 0, (int64_t)un_len);
        CharId next_char = unlinked[nidx];
        memmove(unlinked + nidx, unlinked + nidx + 1, (un_len - nidx - 1) * sizeof(CharId));
        un_len--;
        link_characters(ctx, g->current_char, next_char);
        ids_push(&g->char_link_order, &g->char_link_order_len, &g->char_link_order_cap, next_char);
        g->has_char_last_linked = true;
        g->char_last_linked = next_char;
        if (un_len > 0) {
            ids_push(&g->edge_chars, &g->edge_chars_len, &g->edge_chars_cap, g->current_char);
        }
        size_t next_len = 0;
        CharId *next_neighbors = get_neighbors(ctx, next_char, true, g->limit_to_text_boundary, &next_len);
        if (next_len > 0) {
            ids_push(&g->edge_chars, &g->edge_chars_len, &g->edge_chars_cap, next_char);
            g->has_edge_last_added = true;
            g->edge_last_added = next_char;
        }
        free(next_neighbors);
    }
    free(unlinked);
}

// --- PrimsWeighted --------------------------------------------------------

static void pending_add(PrimsWeighted *g, int64_t weight, WeightedLink link) {
    size_t pos = 0;
    while (pos < g->pending_len && g->pending[pos].weight < weight) pos++;
    if (pos == g->pending_len || g->pending[pos].weight != weight) {
        if (g->pending_len == g->pending_cap) {
            size_t cap = g->pending_cap ? g->pending_cap * 2 : 8;
            g->pending = realloc(g->pending, cap * sizeof(WeightedBucket));
            g->pending_cap = cap;
        }
        memmove(g->pending + pos + 1, g->pending + pos, (g->pending_len - pos) * sizeof(WeightedBucket));
        memset(&g->pending[pos], 0, sizeof(WeightedBucket));
        g->pending[pos].weight = weight;
        g->pending_len++;
    }
    WeightedBucket *b = &g->pending[pos];
    if (b->len == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4;
        b->links = realloc(b->links, cap * sizeof(WeightedLink));
        b->cap = cap;
    }
    b->links[b->len++] = link;
}

static void pending_remove_bucket(PrimsWeighted *g, size_t pos) {
    free(g->pending[pos].links);
    memmove(g->pending + pos, g->pending + pos + 1, (g->pending_len - pos - 1) * sizeof(WeightedBucket));
    g->pending_len--;
}

static bool get_lowest_weight_link(PrimsWeighted *g, EngineCtx *ctx, WeightedLink *out) {
    while (g->pending_len > 0) {
        WeightedBucket *b = &g->pending[0];
        size_t idx = (size_t)rng_randrange(&ctx->rng, 0, (int64_t)b->len);
        WeightedLink link = b->links[idx];
        memmove(b->links + idx, b->links + idx + 1, (b->len - idx - 1) * sizeof(WeightedLink));
        b->len--;
        if (b->len == 0) {
            pending_remove_bucket(g, 0);
        }
        if (ctx->terminal.arena.items[link.char_b].links_len == 0) {
            *out = link;
            return true;
        }
    }
    return false;
}

static void add_weighted_links(PrimsWeighted *g, EngineCtx *ctx, CharId char_id) {
    g->neighbors_last_added_len = 0;
    size_t n = 0;
    CharId *neighbors = get_neighbors(ctx, char_id, true, g->limit_to_text_boundary, &n);
    for (size_t i = 0; i < n; i++) {
        CharId neighbor = neighbors[i];
        ids_push(&g->neighbors_last_added, &g->neighbors_last_added_len, &g->neighbors_last_added_cap, neighbor);
        int64_t weight = g->char_weights[neighbor];
        WeightedLink link = {char_id, neighbor, weight};
        pending_add(g, weight, link);
    }
    free(neighbors);
}

int prims_weighted_new(PrimsWeighted *g, EngineCtx *ctx, bool has_start, CharId start,
                       bool limit_to_text_boundary) {
    memset(g, 0, sizeof(*g));
    g->limit_to_text_boundary = limit_to_text_boundary;
    if (!has_start && default_starting_char(ctx, limit_to_text_boundary, &start) != 0) {
        return -1;
    }
    size_t arena_len = ctx->terminal.arena.len;
    g->char_weights = malloc((arena_len ? arena_len : 1) * sizeof(int64_t));
    g->char_weights_len = arena_len;
    CharacterFilter filter = {true, true, true, false};
    size_t nn = 0;
    CharId *ordered = terminal_get_characters(&ctx->terminal, NULL, filter, CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &nn);
    // reference: sort by (-row, column) — terminal_get_characters already did
    for (size_t i = 0; i < nn; i++) {
        g->char_weights[ordered[i]] = rng_randint(&ctx->rng, 0, 99);
    }
    free(ordered);
    g->has_char_last_linked = true;
    g->char_last_linked = start;
    ids_push(&g->char_link_order, &g->char_link_order_len, &g->char_link_order_cap, start);
    add_weighted_links(g, ctx, start);
    return 0;
}

void prims_weighted_free(PrimsWeighted *g) {
    free(g->char_weights);
    free(g->char_link_order);
    free(g->neighbors_last_added);
    for (size_t i = 0; i < g->pending_len; i++) {
        free(g->pending[i].links);
    }
    free(g->pending);
    memset(g, 0, sizeof(*g));
}

void prims_weighted_step(PrimsWeighted *g, EngineCtx *ctx) {
    if (g->pending_len > 0) {
        WeightedLink link;
        if (!get_lowest_weight_link(g, ctx, &link)) {
            g->complete = true;
            return;
        }
        link_characters(ctx, link.char_a, link.char_b);
        g->has_char_last_linked = true;
        g->char_last_linked = link.char_b;
        ids_push(&g->char_link_order, &g->char_link_order_len, &g->char_link_order_cap, link.char_b);
        add_weighted_links(g, ctx, link.char_b);
    } else {
        g->complete = true;
        g->has_char_last_linked = false;
        g->neighbors_last_added_len = 0;
    }
}

// --- RecursiveBacktracker -------------------------------------------------

int recursive_backtracker_new(RecursiveBacktracker *g, EngineCtx *ctx, bool has_start, CharId start,
                              bool limit_to_text_boundary) {
    memset(g, 0, sizeof(*g));
    g->limit_to_text_boundary = limit_to_text_boundary;
    if (!has_start && default_starting_char(ctx, limit_to_text_boundary, &start) != 0) {
        return -1;
    }
    g->current_char = start;
    g->has_char_last_linked = true;
    g->char_last_linked = start;
    ids_push(&g->char_link_order, &g->char_link_order_len, &g->char_link_order_cap, start);
    ids_push(&g->stack, &g->stack_len, &g->stack_cap, start);
    return 0;
}

void recursive_backtracker_free(RecursiveBacktracker *g) {
    free(g->char_link_order);
    free(g->stack);
    memset(g, 0, sizeof(*g));
}

void recursive_backtracker_step(RecursiveBacktracker *g, EngineCtx *ctx) {
    g->has_char_last_linked = false;
    g->has_stack_last_popped = false;
    if (g->stack_len == 0) {
        g->complete = true;
        return;
    }
    size_t n = 0;
    CharId *unvisited = get_neighbors(ctx, g->current_char, true, g->limit_to_text_boundary, &n);
    if (n > 0) {
        CharId next_char = unvisited[rng_choice_index(&ctx->rng, n)];
        link_characters(ctx, g->current_char, next_char);
        ids_push(&g->char_link_order, &g->char_link_order_len, &g->char_link_order_cap, next_char);
        g->has_char_last_linked = true;
        g->char_last_linked = next_char;
        ids_push(&g->stack, &g->stack_len, &g->stack_cap, next_char);
        g->current_char = next_char;
    } else {
        g->stack_len--;
        g->has_stack_last_popped = true;
        g->stack_last_popped = g->stack[g->stack_len];
        if (g->stack_len > 0) {
            g->current_char = g->stack[g->stack_len - 1];
        }
    }
    free(unvisited);
}

// --- BreadthFirst ---------------------------------------------------------

static bool explored_contains(const BreadthFirst *g, CharId id) {
    size_t lo = 0, hi = g->explored_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g->explored[mid] < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < g->explored_len && g->explored[lo] == id;
}

static void explored_insert(BreadthFirst *g, CharId id) {
    size_t lo = 0, hi = g->explored_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g->explored[mid] < id) lo = mid + 1;
        else hi = mid;
    }
    if (lo < g->explored_len && g->explored[lo] == id) return;
    ids_push(&g->explored, &g->explored_len, &g->explored_cap, 0);
    memmove(g->explored + lo + 1, g->explored + lo, (g->explored_len - lo - 1) * sizeof(CharId));
    g->explored[lo] = id;
}

int breadth_first_new(BreadthFirst *g, EngineCtx *ctx, bool has_start, CharId start, bool limit_to_text_boundary) {
    memset(g, 0, sizeof(*g));
    if (!has_start && default_starting_char(ctx, limit_to_text_boundary, &start) != 0) {
        return -1;
    }
    g->starting_char = start;
    ids_push(&g->frontier, &g->frontier_len, &g->frontier_cap, start);
    explored_insert(g, start);
    return 0;
}

void breadth_first_free(BreadthFirst *g) {
    free(g->frontier);
    free(g->explored);
    free(g->explored_last_step);
    free(g->char_explore_order);
    memset(g, 0, sizeof(*g));
}

void breadth_first_step(BreadthFirst *g, EngineCtx *ctx) {
    g->explored_last_step_len = 0;
    if (g->frontier_len == 0) {
        g->complete = true;
        return;
    }
    CharId *new_edges = NULL;
    size_t new_edges_len = 0, new_edges_cap = 0;
    while (g->frontier_len > 0) {
        CharId position = g->frontier[0];
        memmove(g->frontier, g->frontier + 1, (g->frontier_len - 1) * sizeof(CharId));
        g->frontier_len--;
        EffectCharacter *ch = &ctx->terminal.arena.items[position];
        size_t links_len = ch->links_len;
        // links ascending; filter not explored/frontier/new_edges
        for (size_t i = 0; i < links_len; i++) {
            CharId n = ch->links[i];
            if (explored_contains(g, n)) continue;
            bool in_frontier = false;
            for (size_t j = 0; j < g->frontier_len; j++) {
                if (g->frontier[j] == n) { in_frontier = true; break; }
            }
            if (in_frontier) continue;
            bool in_new = false;
            for (size_t j = 0; j < new_edges_len; j++) {
                if (new_edges[j] == n) { in_new = true; break; }
            }
            if (in_new) continue;
            explored_insert(g, n);
            ids_push(&g->explored_last_step, &g->explored_last_step_len, &g->explored_last_step_cap, n);
            ids_push(&g->char_explore_order, &g->char_explore_order_len, &g->char_explore_order_cap, n);
            ids_push(&new_edges, &new_edges_len, &new_edges_cap, n);
        }
    }
    free(g->frontier);
    g->frontier = new_edges;
    g->frontier_len = new_edges_len;
    g->frontier_cap = new_edges_cap;
}
