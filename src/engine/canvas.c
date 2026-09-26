#include "engine/canvas.h"

#include <stdlib.h>
#include <string.h>

#include "utils/pycompat.h"

bool anchor_parse(const char *s, Anchor *out) {
    if (strcmp(s, "n") == 0) *out = ANCHOR_N;
    else if (strcmp(s, "ne") == 0) *out = ANCHOR_NE;
    else if (strcmp(s, "e") == 0) *out = ANCHOR_E;
    else if (strcmp(s, "se") == 0) *out = ANCHOR_SE;
    else if (strcmp(s, "s") == 0) *out = ANCHOR_S;
    else if (strcmp(s, "sw") == 0) *out = ANCHOR_SW;
    else if (strcmp(s, "w") == 0) *out = ANCHOR_W;
    else if (strcmp(s, "nw") == 0) *out = ANCHOR_NW;
    else if (strcmp(s, "c") == 0) *out = ANCHOR_C;
    else return false;
    return true;
}

Canvas canvas_new(int64_t top, int64_t right) {
    Canvas c;
    c.top = top;
    c.right = right;
    c.bottom = 1;
    c.left = 1;
    int64_t center_row = py_floor_div(top, 2);
    if (center_row < c.bottom) {
        center_row = c.bottom;
    }
    if (top % 2 != 0 && top > 1) {
        center_row += 1;
    }
    int64_t center_column = py_floor_div(right, 2);
    if (center_column < c.left) {
        center_column = c.left;
    }
    if (right % 2 != 0 && right > 1) {
        center_column += 1;
    }
    c.center_row = center_row;
    c.center_column = center_column;
    c.center = coord_new(center_column, center_row);
    c.width = right;
    c.height = top;
    c.text_left = 0;
    c.text_right = 0;
    c.text_top = 0;
    c.text_bottom = 0;
    c.text_width = 0;
    c.text_height = 0;
    c.text_center_row = 0;
    c.text_center_column = 0;
    c.text_center = coord_new(0, 0);
    return c;
}

bool canvas_coord_is_in_canvas(const Canvas *c, Coord coord) {
    return c->left <= coord.column && coord.column <= c->right && c->bottom <= coord.row && coord.row <= c->top;
}

bool canvas_coord_is_in_text(const Canvas *c, Coord coord) {
    return c->text_left <= coord.column && coord.column <= c->text_right && c->text_bottom <= coord.row &&
           coord.row <= c->text_top;
}

int64_t canvas_random_column(const Canvas *c, Rng *rng, bool within_text_boundary) {
    if (within_text_boundary) {
        return rng_randint(rng, c->text_left, c->text_right);
    }
    return rng_randint(rng, c->left, c->right);
}

int64_t canvas_random_row(const Canvas *c, Rng *rng, bool within_text_boundary) {
    if (within_text_boundary) {
        return rng_randint(rng, c->text_bottom, c->text_top);
    }
    return rng_randint(rng, c->bottom, c->top);
}

Coord canvas_random_coord(const Canvas *c, Rng *rng, bool outside_scope, bool within_text_boundary) {
    if (outside_scope) {
        Coord above = coord_new(canvas_random_column(c, rng, false), c->top + 1);
        Coord below = coord_new(canvas_random_column(c, rng, false), c->bottom - 1);
        Coord left = coord_new(c->left - 1, canvas_random_row(c, rng, false));
        Coord right = coord_new(c->right + 1, canvas_random_row(c, rng, false));
        Coord options[4] = {above, below, left, right};
        return options[rng_choice_index(rng, 4)];
    }
    int64_t column = canvas_random_column(c, rng, within_text_boundary);
    int64_t row = canvas_random_row(c, rng, within_text_boundary);
    return coord_new(column, row);
}

int canvas_anchor_text(Canvas *canvas, Arena *arena, CharId *chars, size_t n, Anchor anchor, CharId **out_kept,
                       size_t *out_n) {
    if (n == 0) {
        return -1;
    }
    int64_t input_width = arena->items[chars[0]].input_coord.column;
    int64_t input_height = arena->items[chars[0]].input_coord.row;
    for (size_t i = 1; i < n; i++) {
        Coord c = arena->items[chars[i]].input_coord;
        if (c.column > input_width) {
            input_width = c.column;
        }
        if (c.row > input_height) {
            input_height = c.row;
        }
    }

    int64_t column_delta = 0;
    int64_t row_delta = 0;
    if (input_width != canvas->width) {
        switch (anchor) {
            case ANCHOR_S:
            case ANCHOR_N:
            case ANCHOR_C:
                column_delta = canvas->center_column - py_floor_div(input_width, 2);
                break;
            case ANCHOR_SE:
            case ANCHOR_E:
            case ANCHOR_NE:
                column_delta = canvas->right - input_width;
                break;
            case ANCHOR_SW:
            case ANCHOR_W:
            case ANCHOR_NW:
                column_delta = canvas->left - 1;
                break;
        }
    }
    if (input_height != canvas->height) {
        switch (anchor) {
            case ANCHOR_W:
            case ANCHOR_E:
            case ANCHOR_C:
                row_delta = canvas->center_row - py_floor_div(input_height, 2);
                break;
            case ANCHOR_NW:
            case ANCHOR_N:
            case ANCHOR_NE:
                row_delta = canvas->top - input_height;
                break;
            case ANCHOR_SW:
            case ANCHOR_S:
            case ANCHOR_SE:
                row_delta = canvas->bottom - 1;
                break;
        }
    }

    for (size_t i = 0; i < n; i++) {
        EffectCharacter *ch = &arena->items[chars[i]];
        Coord anchored = coord_new(ch->input_coord.column + column_delta, ch->input_coord.row + row_delta);
        ch->input_coord = anchored;
        motion_set_coordinate(&ch->motion, anchored);
    }

    CharId *kept = malloc((n ? n : 1) * sizeof(CharId));
    if (!kept) {
        return -1;
    }
    size_t kept_n = 0;
    for (size_t i = 0; i < n; i++) {
        if (canvas_coord_is_in_canvas(canvas, arena->items[chars[i]].input_coord)) {
            kept[kept_n++] = chars[i];
        }
    }

    if (kept_n == 0) {
        free(kept);
        return -1;
    }

    int64_t text_left = arena->items[kept[0]].input_coord.column;
    int64_t text_right = text_left;
    int64_t text_top = arena->items[kept[0]].input_coord.row;
    int64_t text_bottom = text_top;
    for (size_t i = 1; i < kept_n; i++) {
        Coord c = arena->items[kept[i]].input_coord;
        if (c.column < text_left) text_left = c.column;
        if (c.column > text_right) text_right = c.column;
        if (c.row > text_top) text_top = c.row;
        if (c.row < text_bottom) text_bottom = c.row;
    }
    canvas->text_left = text_left;
    canvas->text_right = text_right;
    canvas->text_top = text_top;
    canvas->text_bottom = text_bottom;
    int64_t tw = text_right - text_left + 1;
    canvas->text_width = tw > 1 ? tw : 1;
    int64_t th = text_top - text_bottom + 1;
    canvas->text_height = th > 1 ? th : 1;
    canvas->text_center_row = text_bottom + py_floor_div(text_top - text_bottom, 2);
    canvas->text_center_column = text_left + py_floor_div(text_right - text_left, 2);
    canvas->text_center = coord_new(canvas->text_center_column, canvas->text_center_row);

    *out_kept = kept;
    *out_n = kept_n;
    return 0;
}
