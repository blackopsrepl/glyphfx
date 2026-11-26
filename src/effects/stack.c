#include "effects/stack.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "effects/originals.h"
#include "effects/registry.h"
#include "engine/terminal.h"
#include "utils/strtab.h"

// The text built as a game of falling blocks.
//
// The text is cut into tetrominoes of whole characters (pieces of up to four
// connected cells) and the pieces are ordered so that every column fills from
// the bottom up. Each piece drops from above the canvas one row per gravity
// step as solid blocks coloured by its shape, rotating and sliding into line
// with its landing place. At lock the tiles become muted letters. When a row
// is complete, a highlight runs out from its centre along the letter strokes
// and leaves them in their final colours.
//
// Every path is planned at build time against the cells already settled and
// every other piece in flight, so pieces never overlap. Each frame is then a
// pure function of the frame number.

#define STACK_CELLS 4
#define STACK_MOVES 8
#define STACK_TRIES 10
#define STACK_LOCK_FRAMES 8
#define STACK_SHAPES 7

enum { SHAPE_I, SHAPE_O, SHAPE_T, SHAPE_S, SHAPE_Z, SHAPE_J, SHAPE_L };

typedef struct {
    int32_t at;  // frame offset into the flight at which the move happens
    int8_t dx;   // columns to shift
    int8_t rot;  // quarter turns to add
} Move;

typedef struct {
    int n;
    size_t cell[STACK_CELLS];          // cast indices
    int64_t ox[STACK_CELLS], oy[STACK_CELLS];  // home offset from the pivot (rows count upward)
    int64_t pc, pr;                    // pivot home
    int64_t bottom;                    // lowest home row
    int shape;
    uint32_t key;                      // breaks landing-order ties
    int dx0, rot0;
    int nmoves;
    Move moves[STACK_MOVES];
    int64_t lift0;                     // rows above home at spawn
    int64_t spawn, land;
} Piece;

typedef struct {
    StackConfig config;
    OrigCast cast;
    Piece *pieces;
    size_t npieces, cap;
    size_t *order;           // landing order
    int64_t *row_done;       // per text row: frame its last cell lands
    int64_t *wave;           // per cast index: frames from row completion to its clear
    int64_t tl, tb, tw, th;
    int64_t left, right, bottom, top;
    int64_t end;
    Color plain, rim[STACK_SHAPES], resting[STACK_SHAPES];
    SymbolId block;  // a cell of a piece before its row clears
    int64_t frame;
} Stack;

void stack_config_defaults(void *cfg_ptr) {
    StackConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->gravity_frames = 2;
    cfg->stack_frames = 220;
    cfg->max_moves = 3;
    cfg->clear_frames = 24;
    // I O T S Z J L
    static const char *const shapes[STACK_SHAPES] = {"5ee7f0", "f5e15b", "b57cf8", "82fb9c",
                                                     "f8727c", "6f8cf8", "f8a95c"};
    for (int i = 0; i < STACK_SHAPES; i++) {
        orig_push_color(&cfg->piece_colors, shapes[i]);
    }
    color_from_hex("ffffff", &cfg->flash_color);
    orig_push_color(&cfg->final_gradient_stops, "82fb9c");
    orig_push_color(&cfg->final_gradient_stops, "7cf8f7");
    orig_push_color(&cfg->final_gradient_stops, "8a7cf8");
    orig_push_int(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_HORIZONTAL;
}

void stack_free_config(void *cfg_ptr) {
    StackConfig *cfg = cfg_ptr;
    free(cfg->piece_colors.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

// --- geometry --------------------------------------------------------------

static void rotate(int64_t x, int64_t y, int rot, int64_t *rx, int64_t *ry) {
    for (int r = 0; r < (rot & 3); r++) {
        int64_t t = x;
        x = -y;
        y = t;
    }
    *rx = x;
    *ry = y;
}

static int64_t flight_frames(const Stack *st, const Piece *p) {
    return p->lift0 * st->config.gravity_frames;
}

// The piece's shift, turn and lift at a frame offset into its flight.
static void piece_state(const Stack *st, const Piece *p, int64_t off, int *dx, int *rot, int64_t *lift) {
    int d = p->dx0, r = p->rot0;
    for (int m = 0; m < p->nmoves && p->moves[m].at <= off; m++) {
        d += p->moves[m].dx;
        r += p->moves[m].rot;
    }
    *dx = d;
    *rot = r & 3;
    int64_t l = p->lift0 - off / st->config.gravity_frames;
    *lift = l < 0 ? 0 : l;
}

static void cell_at(const Piece *p, int k, int dx, int rot, int64_t lift, int64_t *col, int64_t *row) {
    int64_t rx, ry;
    rotate(p->ox[k], p->oy[k], rot, &rx, &ry);
    *col = p->pc + dx + rx;
    *row = p->pr + lift + ry;
}

// --- cutting the text into pieces ----------------------------------------

static Piece *new_piece(Stack *st, Rng *rng) {
    if (st->npieces == st->cap) {
        size_t cap = st->cap ? st->cap * 2 : 64;
        Piece *grown = realloc(st->pieces, cap * sizeof(Piece));
        if (!grown) {
            return NULL;
        }
        st->pieces = grown;
        st->cap = cap;
    }
    Piece *p = &st->pieces[st->npieces++];
    memset(p, 0, sizeof(*p));
    p->key = (uint32_t)rng_randbelow(rng, UINT32_MAX);
    return p;
}

static int64_t cell_col(const EngineCtx *ctx, const Stack *st, size_t i) {
    return ctx->terminal.arena.items[st->cast.ids[i]].input_coord.column;
}

static int64_t cell_row(const EngineCtx *ctx, const Stack *st, size_t i) {
    return ctx->terminal.arena.items[st->cast.ids[i]].input_coord.row;
}

// Grows pieces of four connected cells from the bottom row up, then lets
// leftover small pieces join a neighbour when the two still fit in four.
static int cut_pieces(Stack *st, EngineCtx *ctx, const int64_t *grid, int64_t *owner) {
    size_t n = st->cast.len;
    for (size_t i = 0; i < n; i++) {
        owner[i] = -1;
    }
    static const int64_t NX[4] = {1, -1, 0, 0}, NY[4] = {0, 0, 1, -1};
    for (int64_t y = 0; y < st->th; y++) {
        for (int64_t x = 0; x < st->tw; x++) {
            int64_t s = grid[y * st->tw + x];
            if (!s || owner[s - 1] >= 0) {
                continue;
            }
            Piece *p = new_piece(st, &ctx->rng);
            if (!p) {
                return -1;
            }
            int64_t id = (int64_t)(st->npieces - 1);
            p->cell[p->n++] = (size_t)(s - 1);
            owner[s - 1] = id;
            while (p->n < STACK_CELLS) {
                int64_t frontier[STACK_CELLS * 4];
                int nf = 0;
                for (int k = 0; k < p->n; k++) {
                    int64_t cx = cell_col(ctx, st, p->cell[k]) - st->tl, cy = cell_row(ctx, st, p->cell[k]) - st->tb;
                    for (int d = 0; d < 4; d++) {
                        int64_t nx = cx + NX[d], ny = cy + NY[d];
                        if (nx < 0 || ny < 0 || nx >= st->tw || ny >= st->th) {
                            continue;
                        }
                        int64_t c = grid[ny * st->tw + nx];
                        if (c && owner[c - 1] < 0) {
                            bool dup = false;
                            for (int q = 0; q < nf; q++) {
                                dup |= frontier[q] == c;
                            }
                            if (!dup) {
                                frontier[nf++] = c;
                            }
                        }
                    }
                }
                if (nf == 0) {
                    break;
                }
                int64_t c = frontier[rng_randbelow(&ctx->rng, (uint64_t)nf)];
                p->cell[p->n++] = (size_t)(c - 1);
                owner[c - 1] = id;
            }
        }
    }
    // Merge small neighbours.
    for (size_t a = 0; a < st->npieces; a++) {
        Piece *p = &st->pieces[a];
        if (p->n == 0 || p->n == STACK_CELLS) {
            continue;
        }
        for (int k = 0; k < p->n && p->n < STACK_CELLS; k++) {
            int64_t cx = cell_col(ctx, st, p->cell[k]) - st->tl, cy = cell_row(ctx, st, p->cell[k]) - st->tb;
            for (int d = 0; d < 4; d++) {
                int64_t nx = cx + NX[d], ny = cy + NY[d];
                if (nx < 0 || ny < 0 || nx >= st->tw || ny >= st->th) {
                    continue;
                }
                int64_t c = grid[ny * st->tw + nx];
                if (!c) {
                    continue;
                }
                int64_t b = owner[c - 1];
                if (b < 0 || (size_t)b == a) {
                    continue;
                }
                Piece *q = &st->pieces[b];
                if (q->n == 0 || p->n + q->n > STACK_CELLS) {
                    continue;
                }
                for (int j = 0; j < q->n; j++) {
                    p->cell[p->n++] = q->cell[j];
                    owner[q->cell[j]] = (int64_t)a;
                }
                q->n = 0;
            }
        }
    }
    // Compact away the emptied pieces.
    size_t w = 0;
    for (size_t a = 0; a < st->npieces; a++) {
        if (st->pieces[a].n) {
            st->pieces[w] = st->pieces[a];
            for (int k = 0; k < st->pieces[w].n; k++) {
                owner[st->pieces[w].cell[k]] = (int64_t)w;
            }
            w++;
        }
    }
    st->npieces = w;
    return 0;
}

// Orders the pieces so every column fills from the bottom up: a piece lands
// after every piece below it in any of its columns. Two pieces that each sit
// above the other in some column can never be ordered; the lower of them is
// split into single cells and the order is retried.
static int order_pieces(Stack *st, EngineCtx *ctx, const int64_t *grid, int64_t *owner) {
    for (;;) {
        size_t np = st->npieces;
        size_t *indeg = calloc(np, sizeof(size_t));
        size_t *count = calloc(np + 1, sizeof(size_t));
        size_t ecap = (size_t)(st->tw * st->th) + 1;
        size_t *ef = malloc(ecap * sizeof(size_t)), *et = malloc(ecap * sizeof(size_t));
        size_t ne = 0;
        if (!indeg || !count || !ef || !et) {
            free(indeg), free(count), free(ef), free(et);
            return -1;
        }
        for (int64_t x = 0; x < st->tw; x++) {
            int64_t prev = -1;
            for (int64_t y = 0; y < st->th; y++) {
                int64_t c = grid[y * st->tw + x];
                if (!c) {
                    continue;
                }
                int64_t o = owner[c - 1];
                if (prev >= 0 && prev != o) {
                    ef[ne] = (size_t)prev;
                    et[ne] = (size_t)o;
                    ne++;
                }
                prev = o;
            }
        }
        size_t *adj = malloc((ne + 1) * sizeof(size_t));
        size_t *fill = calloc(np + 1, sizeof(size_t));
        bool *done = calloc(np, sizeof(bool));
        if (!adj || !fill || !done) {
            free(indeg), free(count), free(ef), free(et), free(adj), free(fill), free(done);
            return -1;
        }
        for (size_t e = 0; e < ne; e++) {
            count[ef[e] + 1]++;
            indeg[et[e]]++;
        }
        for (size_t a = 0; a < np; a++) {
            count[a + 1] += count[a];
        }
        for (size_t e = 0; e < ne; e++) {
            adj[count[ef[e]] + fill[ef[e]]++] = et[e];
        }
        free(st->order);
        st->order = malloc((np ? np : 1) * sizeof(size_t));
        if (!st->order) {
            free(indeg), free(count), free(ef), free(et), free(adj), free(fill), free(done);
            return -1;
        }
        size_t placed = 0;
        int64_t stuck = -1;
        while (placed < np) {
            // The ready piece lowest on the canvas lands next; ties break at
            // random, so a row fills in a scatter rather than a scan.
            int64_t best = -1;
            for (size_t a = 0; a < np; a++) {
                if (done[a] || indeg[a]) {
                    continue;
                }
                if (best < 0 || st->pieces[a].bottom < st->pieces[best].bottom ||
                    (st->pieces[a].bottom == st->pieces[best].bottom && st->pieces[a].key < st->pieces[best].key)) {
                    best = (int64_t)a;
                }
            }
            if (best < 0) {
                for (size_t a = 0; a < np; a++) {
                    if (!done[a] && st->pieces[a].n > 1 &&
                        (stuck < 0 || st->pieces[a].bottom < st->pieces[stuck].bottom)) {
                        stuck = (int64_t)a;
                    }
                }
                break;
            }
            done[best] = true;
            st->order[placed++] = (size_t)best;
            for (size_t e = count[best]; e < count[best + 1]; e++) {
                indeg[adj[e]]--;
            }
        }
        free(indeg), free(count), free(ef), free(et), free(adj), free(fill), free(done);
        if (placed == np) {
            return 0;
        }
        if (stuck < 0) {
            return -1;  // unreachable: single cells never form a cycle
        }
        Piece split = st->pieces[stuck];
        st->pieces[stuck].n = 1;
        for (int k = 1; k < split.n; k++) {
            Piece *q = new_piece(st, &ctx->rng);
            if (!q) {
                return -1;
            }
            q->n = 1;
            q->cell[0] = split.cell[k];
            owner[split.cell[k]] = (int64_t)(st->npieces - 1);
        }
        // Splitting changes pieces' bottoms; recompute them.
        for (size_t a = 0; a < st->npieces; a++) {
            Piece *p = &st->pieces[a];
            p->bottom = INT64_MAX;
            for (int k = 0; k < p->n; k++) {
                int64_t r = cell_row(ctx, st, p->cell[k]);
                p->bottom = r < p->bottom ? r : p->bottom;
            }
        }
    }
}

// Pivot, home offsets and shape of a piece.
static void shape_piece(Stack *st, EngineCtx *ctx, Piece *p) {
    double mx = 0.0, my = 0.0;
    for (int k = 0; k < p->n; k++) {
        mx += (double)cell_col(ctx, st, p->cell[k]);
        my += (double)cell_row(ctx, st, p->cell[k]);
    }
    mx /= p->n;
    my /= p->n;
    int pivot = 0;
    double best = INFINITY;
    for (int k = 0; k < p->n; k++) {
        double ddx = (double)cell_col(ctx, st, p->cell[k]) - mx, ddy = (double)cell_row(ctx, st, p->cell[k]) - my;
        double dist = ddx * ddx + ddy * ddy;
        if (dist < best) {
            best = dist;
            pivot = k;
        }
    }
    p->pc = cell_col(ctx, st, p->cell[pivot]);
    p->pr = cell_row(ctx, st, p->cell[pivot]);
    p->bottom = INT64_MAX;
    for (int k = 0; k < p->n; k++) {
        p->ox[k] = cell_col(ctx, st, p->cell[k]) - p->pc;
        p->oy[k] = cell_row(ctx, st, p->cell[k]) - p->pr;
        int64_t r = cell_row(ctx, st, p->cell[k]);
        p->bottom = r < p->bottom ? r : p->bottom;
    }
    if (p->n == 1) {
        p->shape = SHAPE_O;
        return;
    }
    // Normalised bitmask of the shape in a 4x4 box, under each quarter turn.
    static const uint16_t CANON[STACK_SHAPES] = {
        0x000F,  // I: (0,0)(1,0)(2,0)(3,0)
        0x0033,  // O: (0,0)(1,0)(0,1)(1,1)
        0x0027,  // T: (0,0)(1,0)(2,0)(1,1)
        0x0063,  // S: (0,0)(1,0)(1,1)(2,1)
        0x0036,  // Z: (1,0)(2,0)(0,1)(1,1)
        0x0017,  // J: (0,0)(1,0)(2,0)(0,1)
        0x0047,  // L: (0,0)(1,0)(2,0)(2,1)
    };
    bool straight = true;
    for (int k = 0; k < p->n; k++) {
        straight &= p->ox[k] == p->ox[0] || p->oy[k] == p->oy[0];
    }
    if (p->n < 4) {
        bool row = true, col = true;
        for (int k = 0; k < p->n; k++) {
            row &= p->oy[k] == p->oy[0];
            col &= p->ox[k] == p->ox[0];
        }
        p->shape = (row || col) ? SHAPE_I : SHAPE_J;
        return;
    }
    for (int r = 0; r < 4; r++) {
        int64_t xs[STACK_CELLS], ys[STACK_CELLS], minx = INT64_MAX, miny = INT64_MAX;
        for (int k = 0; k < 4; k++) {
            rotate(p->ox[k], p->oy[k], r, &xs[k], &ys[k]);
            minx = xs[k] < minx ? xs[k] : minx;
            miny = ys[k] < miny ? ys[k] : miny;
        }
        uint16_t mask = 0;
        for (int k = 0; k < 4; k++) {
            mask |= (uint16_t)(1u << ((ys[k] - miny) * 4 + (xs[k] - minx)));
        }
        for (int s = 0; s < STACK_SHAPES; s++) {
            if (mask == CANON[s]) {
                p->shape = s;
                return;
            }
        }
    }
    p->shape = straight ? SHAPE_I : SHAPE_T;
}

// --- planning the flights ------------------------------------------------

typedef struct {
    uint8_t *bits;  // frames x canvas cells
    int64_t frames;
    int64_t cells;
} Timeline;

static bool timeline_reserve(Timeline *tl, int64_t frames) {
    if (frames <= tl->frames) {
        return true;
    }
    int64_t want = tl->frames ? tl->frames : 256;
    while (want < frames) {
        want *= 2;
    }
    size_t row = (size_t)((tl->cells + 7) / 8);
    uint8_t *grown = realloc(tl->bits, (size_t)want * row);
    if (!grown) {
        return false;
    }
    memset(grown + (size_t)tl->frames * row, 0, (size_t)(want - tl->frames) * row);
    tl->bits = grown;
    tl->frames = want;
    return true;
}

static bool timeline_get(const Timeline *tl, int64_t f, int64_t cell) {
    if (f >= tl->frames) {
        return false;
    }
    size_t row = (size_t)((tl->cells + 7) / 8);
    return (tl->bits[(size_t)f * row + (size_t)(cell >> 3)] >> (cell & 7)) & 1;
}

static void timeline_set(Timeline *tl, int64_t f, int64_t cell) {
    size_t row = (size_t)((tl->cells + 7) / 8);
    tl->bits[(size_t)f * row + (size_t)(cell >> 3)] |= (uint8_t)(1u << (cell & 7));
}

// Canvas cell index of a coordinate, or -1 when it is off the canvas.
static int64_t canvas_cell(const Stack *st, int64_t col, int64_t row) {
    if (col < st->left || col > st->right || row < st->bottom || row > st->top) {
        return -1;
    }
    return (row - st->bottom) * (st->right - st->left + 1) + (col - st->left);
}

// Draws a random opening turn and shift and spreads the moves that undo them
// over the part of the fall the viewer sees. Attempt -1 is the plain drop.
static bool plan_moves(Stack *st, EngineCtx *ctx, Piece *p, bool plain) {
    const StackConfig *cfg = &st->config;
    p->nmoves = 0;
    p->dx0 = 0;
    p->rot0 = 0;
    if (!plain && cfg->max_moves > 0) {
        int budget = (int)(cfg->max_moves < STACK_MOVES ? cfg->max_moves : STACK_MOVES);
        int turns = (p->shape == SHAPE_O || p->n == 1) ? 0 : (int)rng_randint(&ctx->rng, 0, 3);
        int turn_moves = turns == 2 ? 2 : (turns ? 1 : 0);
        if (turn_moves > budget) {
            turns = 0;
            turn_moves = 0;
        }
        int shift_max = budget - turn_moves;
        int shift = shift_max ? (int)rng_randint(&ctx->rng, -shift_max, shift_max) : 0;
        p->rot0 = turns;
        p->dx0 = shift;
        Move m[STACK_MOVES];
        int nm = 0;
        if (turns == 2) {
            m[nm++] = (Move){0, 0, 1};
            m[nm++] = (Move){0, 0, 1};
        } else if (turns) {
            m[nm++] = (Move){0, 0, (int8_t)(turns == 3 ? 1 : -1)};
        }
        for (int k = 0; k < abs(shift); k++) {
            m[nm++] = (Move){0, (int8_t)(shift > 0 ? -1 : 1), 0};
        }
        // Shuffle so turns and shifts interleave.
        for (int k = nm - 1; k > 0; k--) {
            int j = (int)rng_randbelow(&ctx->rng, (uint64_t)k + 1);
            Move t = m[k];
            m[k] = m[j];
            m[j] = t;
        }
        p->nmoves = nm;
        memcpy(p->moves, m, (size_t)nm * sizeof(Move));
    }
    // Lift at spawn: every cell of the opening pose just above the canvas.
    int64_t lowest = INT64_MAX;
    for (int k = 0; k < p->n; k++) {
        int64_t col, row;
        cell_at(p, k, p->dx0, p->rot0, 0, &col, &row);
        lowest = row < lowest ? row : lowest;
    }
    p->lift0 = st->top + 1 - lowest;
    if (p->lift0 < 0) {
        p->lift0 = 0;
    }
    if (p->nmoves == 0) {
        return true;
    }
    // Moves happen while the piece is in view, before its last two rows.
    int64_t g = cfg->gravity_frames;
    int64_t first = g;                         // one row into the canvas
    int64_t last = (p->lift0 - 2) * g;         // two rows above home
    int64_t span = last - first;
    if (span < (int64_t)p->nmoves * 2) {
        return false;
    }
    for (int k = 0; k < p->nmoves; k++) {
        int64_t slot = first + span * k / p->nmoves;
        int64_t width = span / p->nmoves;
        p->moves[k].at = (int32_t)(slot + (width > 1 ? (int64_t)rng_randbelow(&ctx->rng, (uint64_t)width) : 0));
    }
    return true;
}

static bool flight_clear(const Stack *st, const Piece *p, int64_t spawn, const bool *settled, const Timeline *tl) {
    int64_t flight = flight_frames(st, p);
    for (int64_t off = 0; off < flight; off++) {
        int dx, rot;
        int64_t lift;
        piece_state(st, p, off, &dx, &rot, &lift);
        bool home_pose = dx == 0 && rot == 0;
        for (int k = 0; k < p->n; k++) {
            int64_t col, row;
            cell_at(p, k, dx, rot, lift, &col, &row);
            if (row > st->top) {
                continue;  // still above the canvas
            }
            int64_t c = canvas_cell(st, col, row);
            if (c < 0) {
                if (!home_pose) {
                    return false;  // a turn or shift must not leave the canvas
                }
                continue;
            }
            if (settled[c] || timeline_get(tl, spawn + off, c)) {
                return false;
            }
        }
    }
    return true;
}

static void flight_reserve(const Stack *st, const Piece *p, bool *settled, Timeline *tl) {
    int64_t flight = flight_frames(st, p);
    for (int64_t off = 0; off < flight; off++) {
        int dx, rot;
        int64_t lift;
        piece_state(st, p, off, &dx, &rot, &lift);
        for (int k = 0; k < p->n; k++) {
            int64_t col, row;
            cell_at(p, k, dx, rot, lift, &col, &row);
            int64_t c = canvas_cell(st, col, row);
            if (c >= 0) {
                timeline_set(tl, p->spawn + off, c);
            }
        }
    }
    for (int k = 0; k < p->n; k++) {
        int64_t col, row;
        cell_at(p, k, 0, 0, 0, &col, &row);
        int64_t c = canvas_cell(st, col, row);
        if (c >= 0) {
            settled[c] = true;
        }
    }
}

static int plan_flights(Stack *st, EngineCtx *ctx) {
    const StackConfig *cfg = &st->config;
    int64_t cells = (st->right - st->left + 1) * (st->top - st->bottom + 1);
    bool *settled = calloc((size_t)(cells > 0 ? cells : 1), sizeof(bool));
    Timeline tl = {NULL, 0, cells > 0 ? cells : 1};
    if (!settled) {
        return -1;
    }
    size_t np = st->npieces;
    int64_t prev_land = 0;
    for (size_t k = 0; k < np; k++) {
        Piece *p = &st->pieces[st->order[k]];
        int64_t target = np > 1 ? (int64_t)k * cfg->stack_frames / (int64_t)(np - 1) : 0;
        int64_t land = target > prev_land ? target : prev_land;
        for (bool placed = false; !placed; land++) {
            if (land > target + 100000) {
                free(settled);
                free(tl.bits);
                return -1;  // unreachable: a plain drop always clears in time
            }
            for (int attempt = 0; attempt <= STACK_TRIES && !placed; attempt++) {
                bool plain = attempt == STACK_TRIES;
                if (!plan_moves(st, ctx, p, plain)) {
                    continue;
                }
                int64_t spawn = land - flight_frames(st, p);
                if (spawn < 0) {
                    continue;
                }
                if (!timeline_reserve(&tl, land + 1)) {
                    free(settled);
                    free(tl.bits);
                    return -1;
                }
                if (flight_clear(st, p, spawn, settled, &tl)) {
                    p->spawn = spawn;
                    p->land = land;
                    flight_reserve(st, p, settled, &tl);
                    placed = true;
                }
            }
            if (placed) {
                break;
            }
        }
        prev_land = p->land;
    }
    free(settled);
    free(tl.bits);
    return 0;
}

// --- build ---------------------------------------------------------------

static int stack_build(Effect *self, EngineCtx *ctx) {
    Stack *st = self->state;
    StackConfig *cfg = &st->config;
    if (orig_cast_build(ctx, &cfg->final_gradient_stops, &cfg->final_gradient_steps,
                        cfg->final_gradient_direction, &st->cast) != 0) {
        return -1;
    }
    const Canvas *cv = &ctx->terminal.canvas;
    st->left = cv->left;
    st->right = cv->right;
    st->bottom = cv->bottom;
    st->top = cv->top;
    Color ink = color_from_rgb(26, 28, 40);
    for (size_t s = 0; s < STACK_SHAPES; s++) {
        const Color *base = &cfg->piece_colors.items[s % cfg->piece_colors.len];
        st->rim[s] = orig_mix(base, &cfg->flash_color, 0.38);
        st->resting[s] = orig_mix(base, &ink, 0.38);
    }
    color_from_hex("c8c8d0", &st->plain);
    st->block = strtab_handle(ctx->terminal.config.no_color ? "█" : "▔");

    size_t n = st->cast.len;
    int64_t l = INT64_MAX, r = INT64_MIN, b = INT64_MAX, t = INT64_MIN;
    for (size_t i = 0; i < n; i++) {
        int64_t c = cell_col(ctx, st, i), w = cell_row(ctx, st, i);
        l = c < l ? c : l;
        r = c > r ? c : r;
        b = w < b ? w : b;
        t = w > t ? w : t;
    }
    if (n == 0) {
        l = r = st->left;
        b = t = st->bottom;
    }
    st->tl = l;
    st->tb = b;
    st->tw = r - l + 1;
    st->th = t - b + 1;

    int64_t *grid = calloc((size_t)(st->tw * st->th), sizeof(int64_t));
    int64_t *owner = calloc(n ? n : 1, sizeof(int64_t));
    st->row_done = calloc((size_t)st->th, sizeof(int64_t));
    st->wave = calloc(n ? n : 1, sizeof(int64_t));
    if (!grid || !owner || !st->row_done || !st->wave) {
        free(grid);
        free(owner);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        grid[(cell_row(ctx, st, i) - st->tb) * st->tw + (cell_col(ctx, st, i) - st->tl)] = (int64_t)i + 1;
    }
    int rc = cut_pieces(st, ctx, grid, owner);
    for (size_t a = 0; rc == 0 && a < st->npieces; a++) {
        shape_piece(st, ctx, &st->pieces[a]);
    }
    if (rc == 0) {
        rc = order_pieces(st, ctx, grid, owner);
    }
    for (size_t a = 0; rc == 0 && a < st->npieces; a++) {
        shape_piece(st, ctx, &st->pieces[a]);  // splits leave single cells to shape
    }
    free(grid);
    free(owner);
    if (rc != 0 || plan_flights(st, ctx) != 0) {
        return -1;
    }

    // Each row clears once its last cell has landed, from its centre out.
    int64_t last_land = 0;
    for (size_t a = 0; a < st->npieces; a++) {
        const Piece *p = &st->pieces[a];
        last_land = p->land > last_land ? p->land : last_land;
        for (int k = 0; k < p->n; k++) {
            int64_t row = cell_row(ctx, st, p->cell[k]) - st->tb;
            st->row_done[row] = p->land > st->row_done[row] ? p->land : st->row_done[row];
        }
    }
    double cx = (double)(st->tl + r) / 2.0, half = (double)st->tw / 2.0;
    st->end = last_land + STACK_LOCK_FRAMES;
    for (size_t i = 0; i < n; i++) {
        double d = fabs((double)cell_col(ctx, st, i) - cx) / (half > 1.0 ? half : 1.0);
        st->wave[i] = (int64_t)floor(d * (double)cfg->clear_frames * 0.5 + 0.5);
        int64_t end = st->row_done[cell_row(ctx, st, i) - st->tb] + st->wave[i] + cfg->clear_frames;
        st->end = end > st->end ? end : st->end;
    }

    return 0;
}

// --- frames --------------------------------------------------------------

static const Color *piece_color(const Stack *st, const Piece *p) {
    const ColorList *pc = &st->config.piece_colors;
    return &pc->items[(size_t)p->shape % pc->len];
}

static void draw_landed(Stack *st, EngineCtx *ctx, const Piece *p, size_t i, int64_t f) {
    const StackConfig *cfg = &st->config;
    CharId id = st->cast.ids[i];
    int64_t since = f - p->land;
    if (since == 0) orig_place(ctx, id, ctx->terminal.arena.items[id].input_coord);
    int64_t row = ctx->terminal.arena.items[id].input_coord.row - st->tb;
    int64_t start = st->row_done[row] + st->wave[i];
    if ((since > STACK_LOCK_FRAMES && f < start) || f - start > cfg->clear_frames) return;
    const Color *base = &st->resting[p->shape];
    Color c = since < STACK_LOCK_FRAMES ? orig_mix(&st->rim[p->shape], base, (double)since / STACK_LOCK_FRAMES) : *base;
    SymbolId sym = orig_input_symbol(ctx, id);
    const Color *fin = st->cast.has_final[i] ? &st->cast.final[i] : &st->plain;
    if (f >= start) {
        double q = (double)(f - start) / (double)cfg->clear_frames;
        if (q >= 1.0) {
            orig_look(ctx, id, orig_input_symbol(ctx, id), st->cast.has_final[i] ? fin : NULL, NULL);
            return;
        }
        // The clear follows the strokes of the already seated letters.
        if (q < 0.25) {
            c = orig_mix(&c, &cfg->flash_color, q / 0.25);
        } else {
            c = orig_mix(&cfg->flash_color, fin, (q - 0.25) / 0.75);
        }
    }
    orig_look(ctx, id, sym, &c, NULL);
}

static const char *stack_next_frame(Effect *self, EngineCtx *ctx) {
    Stack *st = self->state;
    int64_t f = st->frame;
    if (f > st->end) {
        return NULL;
    }
    for (size_t a = 0; a < st->npieces; a++) {
        const Piece *p = &st->pieces[a];
        if (f < p->spawn) {
            continue;
        }
        if (f >= p->land) {
            for (int k = 0; k < p->n; k++) {
                draw_landed(st, ctx, p, p->cell[k], f);
            }
            continue;
        }
        int dx, rot;
        int64_t lift;
        piece_state(st, p, f - p->spawn, &dx, &rot, &lift);
        const Color *base = piece_color(st, p);
        for (int k = 0; k < p->n; k++) {
            int64_t col, row;
            cell_at(p, k, dx, rot, lift, &col, &row);
            CharId id = st->cast.ids[p->cell[k]];
            orig_place(ctx, id, coord_new(col, row));
            if (f == p->spawn) orig_look(ctx, id, st->block, &st->rim[p->shape], base);
        }
    }
    st->frame++;
    return engine_frame(ctx);
}

static void stack_destroy(Effect *self) {
    Stack *st = self->state;
    if (!st) {
        return;
    }
    orig_cast_free(&st->cast);
    free(st->pieces);
    free(st->order);
    free(st->row_done);
    free(st->wave);
    free(st);
    free(self);
}

static const EffectOps STACK_OPS = {stack_build, stack_next_frame, stack_destroy, NULL};

Effect *stack_make(const void *cfg) {
    Stack *st = calloc(1, sizeof(Stack));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const StackConfig *)cfg;
    effect->ops = &STACK_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry stack_entry = {
    "stack",
    (const EffOptSpec[]){
        EF_SPEC("gravity-frames", 0, EF_POS_INT, offsetof(StackConfig, gravity_frames)),
        EF_SPEC("stack-frames", 0, EF_NONNEG_INT, offsetof(StackConfig, stack_frames)),
        EF_SPEC("max-moves", 0, EF_NONNEG_INT, offsetof(StackConfig, max_moves)),
        EF_SPEC("clear-frames", 0, EF_POS_INT, offsetof(StackConfig, clear_frames)),
        EF_SPEC("piece-colors", 0, EF_COLOR_LIST, offsetof(StackConfig, piece_colors)),
        EF_SPEC("flash-color", 0, EF_COLOR, offsetof(StackConfig, flash_color)),
        EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(StackConfig, final_gradient_stops)),
        EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(StackConfig, final_gradient_steps)),
        EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(StackConfig, final_gradient_direction)),
    },
    9,
    sizeof(StackConfig),
    stack_config_defaults,
    stack_free_config,
    stack_make,
};
