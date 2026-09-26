#include "utils/graphics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/hexterm.h"
#include "utils/pycompat.h"

static uint8_t hex_byte(const char *s) {
    int hi = 0;
    int lo = 0;
    char a = s[0];
    char b = s[1];
    if (a >= '0' && a <= '9') {
        hi = a - '0';
    } else if (a >= 'a' && a <= 'f') {
        hi = a - 'a' + 10;
    } else if (a >= 'A' && a <= 'F') {
        hi = a - 'A' + 10;
    }
    if (b >= '0' && b <= '9') {
        lo = b - '0';
    } else if (b >= 'a' && b <= 'f') {
        lo = b - 'a' + 10;
    } else if (b >= 'A' && b <= 'F') {
        lo = b - 'A' + 10;
    }
    return (uint8_t)(hi * 16 + lo);
}

static void parse_rgb6(const char *s, uint8_t rgb[3]) {
    rgb[0] = hex_byte(s);
    rgb[1] = hex_byte(s + 2);
    rgb[2] = hex_byte(s + 4);
}

bool color_eq(const Color *a, const Color *b) {
    if (a->is_xterm != b->is_xterm) {
        return false;
    }
    if (a->is_xterm) {
        return a->xterm == b->xterm;
    }
    return a->hex_len == b->hex_len && memcmp(a->hex, b->hex, a->hex_len) == 0;
}

Color color_from_xterm(uint8_t code) {
    Color c;
    c.is_xterm = true;
    c.xterm = code;
    const char *h = xterm_to_hex(code);
    size_t len = strlen(h);
    memcpy(c.hex, h, len);
    c.hex[len] = '\0';
    c.hex_len = (uint8_t)len;
    parse_rgb6(h, c.rgb);
    return c;
}

int color_from_hex(const char *hex, Color *out) {
    // trim_matches('#'): leading and trailing '#' removed.
    const char *p = hex;
    while (*p == '#') {
        p++;
    }
    size_t len = strlen(p);
    while (len > 0 && p[len - 1] == '#') {
        len--;
    }
    char stripped[8];
    if (len >= sizeof(stripped)) {
        return -1;
    }
    memcpy(stripped, p, len);
    stripped[len] = '\0';
    if (!is_valid_hex_color(stripped)) {
        return -1;
    }
    Color c;
    c.is_xterm = false;
    c.xterm = 0;
    memcpy(c.hex, stripped, len);
    c.hex[len] = '\0';
    c.hex_len = (uint8_t)len;
    parse_rgb6(stripped, c.rgb);
    *out = c;
    return 0;
}

Color color_from_rgb(uint8_t r, uint8_t g, uint8_t b) {
    static const char digits[] = "0123456789abcdef";
    Color c;
    c.is_xterm = false;
    c.xterm = 0;
    c.hex[0] = digits[(r >> 4) & 0xF];
    c.hex[1] = digits[r & 0xF];
    c.hex[2] = digits[(g >> 4) & 0xF];
    c.hex[3] = digits[g & 0xF];
    c.hex[4] = digits[(b >> 4) & 0xF];
    c.hex[5] = digits[b & 0xF];
    c.hex[6] = '\0';
    c.hex_len = 6;
    c.rgb[0] = r;
    c.rgb[1] = g;
    c.rgb[2] = b;
    return c;
}

void color_rgb_ints(const Color *c, uint8_t rgb[3]) {
    rgb[0] = c->rgb[0];
    rgb[1] = c->rgb[1];
    rgb[2] = c->rgb[2];
}

const char *color_hex_text(const Color *c) {
    return c->hex;
}

// --- gradient -------------------------------------------------------------

static void gradient_push(Gradient *g, size_t *cap, Color c) {
    if (g->len == *cap) {
        size_t ncap = *cap ? *cap * 2 : 16;
        Color *grown = realloc(g->spectrum, ncap * sizeof(Color));
        if (!grown) {
            return;
        }
        g->spectrum = grown;
        *cap = ncap;
    }
    g->spectrum[g->len++] = c;
}

int gradient_new(const Color *stops, size_t n_stops, const int64_t *steps, size_t n_steps, bool steps_was_int,
                 bool do_loop, Gradient *out) {
    out->spectrum = NULL;
    out->len = 0;
    if (n_stops == 0) {
        return -1;
    }
    if (steps_was_int) {
        for (size_t i = 0; i < n_steps; i++) {
            if (steps[i] < 1) {
                return -1;
            }
        }
    }
    size_t cap = 0;
    if (n_stops == 1) {
        for (int64_t i = 0; i < steps[0]; i++) {
            gradient_push(out, &cap, stops[0]);
        }
        return 0;
    }
    // Build the working stop list (loop appends stops[0]).
    size_t stop_count = n_stops + (do_loop ? 1 : 0);
    Color *work = malloc(stop_count * sizeof(Color));
    if (!work) {
        return -1;
    }
    for (size_t i = 0; i < n_stops; i++) {
        work[i] = stops[i];
    }
    if (do_loop) {
        work[n_stops] = stops[0];
    }
    size_t pair_count = stop_count - 1;
    size_t step_count_n = n_steps < pair_count ? n_steps : pair_count;
    int64_t *step_list = malloc(pair_count * sizeof(int64_t));
    if (!step_list) {
        free(work);
        return -1;
    }
    for (size_t i = 0; i < step_count_n; i++) {
        step_list[i] = steps[i];
    }
    for (size_t i = step_count_n; i < pair_count; i++) {
        step_list[i] = step_list[i - 1];
    }

    int rc = 0;
    for (size_t pair_index = 0; pair_index < pair_count; pair_index++) {
        int64_t step_count = step_list[pair_index];
        if (step_count < 1) {
            rc = -1;
            break;
        }
        const Color *start = &work[pair_index];
        const Color *end = &work[pair_index + 1];
        uint8_t s_rgb[3];
        uint8_t e_rgb[3];
        color_rgb_ints(start, s_rgb);
        color_rgb_ints(end, e_rgb);
        int64_t sr = s_rgb[0], sg = s_rgb[1], sb = s_rgb[2];
        int64_t er = e_rgb[0], eg = e_rgb[1], eb = e_rgb[2];
        int64_t red_delta = py_floor_div(er - sr, step_count);
        int64_t green_delta = py_floor_div(eg - sg, step_count);
        int64_t blue_delta = py_floor_div(eb - sb, step_count);
        int64_t range_start = out->len > 0 ? 1 : 0;
        for (int64_t i = range_start; i < step_count; i++) {
            int64_t red = sr + red_delta * i;
            int64_t green = sg + green_delta * i;
            int64_t blue = sb + blue_delta * i;
            if (red < 0) red = 0;
            if (red > 255) red = 255;
            if (green < 0) green = 0;
            if (green > 255) green = 255;
            if (blue < 0) blue = 0;
            if (blue > 255) blue = 255;
            gradient_push(out, &cap, color_from_rgb((uint8_t)red, (uint8_t)green, (uint8_t)blue));
        }
        gradient_push(out, &cap, *end);
    }
    free(step_list);
    free(work);
    if (rc != 0) {
        free(out->spectrum);
        out->spectrum = NULL;
        out->len = 0;
    }
    return rc;
}

int gradient_with_steps(const Color *stops, size_t n_stops, int64_t steps, bool do_loop, Gradient *out) {
    return gradient_new(stops, n_stops, &steps, 1, true, do_loop, out);
}

void gradient_free(Gradient *g) {
    free(g->spectrum);
    g->spectrum = NULL;
    g->len = 0;
}

const Color *gradient_get_color_at_fraction(const Gradient *g, double fraction) {
    if (!(fraction >= 0.0 && fraction <= 1.0)) {
        return NULL;
    }
    size_t len = g->len;
    if (len == 0) {
        return NULL;
    }
    size_t index = (size_t)(fraction * (double)len);
    if (index >= len) {
        index = len - 1;
    }
    while (index > 0 && fraction <= (double)index / (double)len) {
        index--;
    }
    while (index + 1 < len && fraction > (double)(index + 1) / (double)len) {
        index++;
    }
    return &g->spectrum[index];
}

void coordcolormap_free(CoordColorMap *m) {
    free(m->colors);
    free(m->order);
    m->colors = NULL;
    m->order = NULL;
    m->len = 0;
}

const Color *coordcolormap_get(const CoordColorMap *m, Coord coord) {
    if (coord.column < m->min_column || coord.row < m->min_row) {
        return NULL;
    }
    size_t column = (size_t)(coord.column - m->min_column);
    size_t row = (size_t)(coord.row - m->min_row);
    if (column >= m->width || row >= m->height) {
        return NULL;
    }
    size_t index = m->column_major ? column * m->height + row : row * m->width + column;
    if (index >= m->len) {
        return NULL;
    }
    return &m->colors[index];
}

int gradient_build_coordinate_color_mapping(const Gradient *g, int64_t min_row, int64_t max_row, int64_t min_column,
                                            int64_t max_column, GradientDirection direction, CoordColorMap *out) {
    memset(out, 0, sizeof(*out));
    if (max_row < 1 || max_column < 1 || min_row < 1 || min_column < 1) {
        return -1;
    }
    if (min_row > max_row || min_column > max_column) {
        return -1;
    }
    int64_t row_offset = min_row - 1;
    int64_t column_offset = min_column - 1;
    out->min_row = min_row;
    out->min_column = min_column;
    out->width = (size_t)(max_column - min_column + 1);
    out->height = (size_t)(max_row - min_row + 1);
    out->column_major = direction == GRADIENT_HORIZONTAL;
    out->len = out->width * out->height;
    out->colors = malloc(out->len * sizeof(Color));
    out->order = malloc(out->len * sizeof(Coord));
    if (!out->colors || !out->order) {
        coordcolormap_free(out);
        return -1;
    }
    size_t idx = 0;
    int rc = 0;
    switch (direction) {
        case GRADIENT_VERTICAL:
            for (int64_t row = min_row; row <= max_row; row++) {
                double fraction = (double)(row - row_offset) / (double)(max_row - row_offset);
                const Color *color = gradient_get_color_at_fraction(g, fraction);
                if (!color) {
                    rc = -1;
                    break;
                }
                for (int64_t column = min_column; column <= max_column; column++) {
                    out->colors[idx] = *color;
                    out->order[idx] = coord_new(column, row);
                    idx++;
                }
            }
            break;
        case GRADIENT_HORIZONTAL:
            for (int64_t column = min_column; column <= max_column; column++) {
                double fraction = (double)(column - column_offset) / (double)(max_column - column_offset);
                const Color *color = gradient_get_color_at_fraction(g, fraction);
                if (!color) {
                    rc = -1;
                    break;
                }
                for (int64_t row = min_row; row <= max_row; row++) {
                    out->colors[idx] = *color;
                    out->order[idx] = coord_new(column, row);
                    idx++;
                }
            }
            break;
        case GRADIENT_RADIAL:
            for (int64_t row = min_row; row <= max_row; row++) {
                for (int64_t column = min_column; column <= max_column; column++) {
                    double distance = 0.0;
                    if (find_normalized_distance_from_center(min_row, max_row, min_column, max_column,
                                                             coord_new(column, row), &distance) != 0) {
                        rc = -1;
                        break;
                    }
                    const Color *color = gradient_get_color_at_fraction(g, distance);
                    if (!color) {
                        rc = -1;
                        break;
                    }
                    out->colors[idx] = *color;
                    out->order[idx] = coord_new(column, row);
                    idx++;
                }
                if (rc != 0) {
                    break;
                }
            }
            break;
        case GRADIENT_DIAGONAL:
            for (int64_t row = min_row; row <= max_row; row++) {
                for (int64_t column = min_column; column <= max_column; column++) {
                    double fraction = (double)(((row - row_offset) * 2) + (column - column_offset)) /
                                      (double)(((max_row - row_offset) * 2) + (max_column - column_offset));
                    const Color *color = gradient_get_color_at_fraction(g, fraction);
                    if (!color) {
                        rc = -1;
                        break;
                    }
                    out->colors[idx] = *color;
                    out->order[idx] = coord_new(column, row);
                    idx++;
                }
                if (rc != 0) {
                    break;
                }
            }
            break;
    }
    if (rc != 0) {
        coordcolormap_free(out);
    }
    return rc;
}

Color random_color(Rng *rng) {
    int64_t value = rng_randint(rng, 0, 0xFFFFFF);
    return color_from_rgb((uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value);
}

int shift_color_towards(const Color *color, const Color *target, double factor, Color *out) {
    uint8_t c_rgb[3];
    uint8_t t_rgb[3];
    color_rgb_ints(color, c_rgb);
    color_rgb_ints(target, t_rgb);
    double cr = (double)c_rgb[0] / 255.0, cg = (double)c_rgb[1] / 255.0, cb = (double)c_rgb[2] / 255.0;
    double tr = (double)t_rgb[0] / 255.0, tg = (double)t_rgb[1] / 255.0, tb = (double)t_rgb[2] / 255.0;
    double ir = cr + (tr - cr) * factor;
    double ig = cg + (tg - cg) * factor;
    double ib = cb + (tb - cb) * factor;
    int64_t channels[3] = {(int64_t)(ir * 255.0), (int64_t)(ig * 255.0), (int64_t)(ib * 255.0)};
    if (channels[0] >= 0 && channels[0] <= 255 && channels[1] >= 0 && channels[1] <= 255 && channels[2] >= 0 &&
        channels[2] <= 255) {
        *out = color_from_rgb((uint8_t)channels[0], (uint8_t)channels[1], (uint8_t)channels[2]);
        return 0;
    }
    char hex[64];
    size_t pos = 0;
    double interp[3] = {ir, ig, ib};
    for (int i = 0; i < 3; i++) {
        int64_t v = (int64_t)(interp[i] * 255.0);
        int n;
        if (v < 0) {
            n = snprintf(hex + pos, sizeof(hex) - pos, "-%01llx", (unsigned long long)(-v));
        } else {
            n = snprintf(hex + pos, sizeof(hex) - pos, "%02llx", (unsigned long long)v);
        }
        if (n < 0) {
            return -1;
        }
        pos += (size_t)n;
    }
    return color_from_hex(hex, out);
}

Color color_adjust_brightness(const Color *color, double brightness) {
    uint8_t in_rgb[3];
    color_rgb_ints(color, in_rgb);
    double nr = (double)in_rgb[0] / 255.0, ng = (double)in_rgb[1] / 255.0, nb = (double)in_rgb[2] / 255.0;

    double max_val = nr > ng ? nr : ng;
    if (nb > max_val) max_val = nb;
    double min_val = nr < ng ? nr : ng;
    if (nb < min_val) min_val = nb;
    double lightness = (max_val + min_val) / 2.0;

    const double lightness_threshold = 0.5;
    double hue_value;
    double saturation;
    if (max_val == min_val) {
        hue_value = 0.0;
        saturation = 0.0;
    } else {
        double diff = max_val - min_val;
        saturation = lightness > lightness_threshold ? diff / (2.0 - max_val - min_val) : diff / (max_val + min_val);
        if (max_val == nr) {
            hue_value = (ng - nb) / diff + (ng < nb ? 6.0 : 0.0);
        } else if (max_val == ng) {
            hue_value = (nb - nr) / diff + 2.0;
        } else {
            hue_value = (nr - ng) / diff + 4.0;
        }
        hue_value /= 6.0;
    }

    lightness = lightness * brightness;
    if (lightness > 1.0) lightness = 1.0;
    if (lightness < 0.0) lightness = 0.0;

    double red, green, blue;
    if (saturation == 0.0) {
        red = green = blue = lightness;
    } else {
        double color_intensity =
            lightness < lightness_threshold ? lightness * (1.0 + saturation) : lightness + saturation - lightness * saturation;
        double ls = 2.0 * lightness - color_intensity;
        double h1 = hue_value + 1.0 / 3.0;
        double h2 = hue_value;
        double h3 = hue_value - 1.0 / 3.0;
        double h[3] = {h1, h2, h3};
        double outch[3];
        for (int i = 0; i < 3; i++) {
            double hv = h[i];
            if (hv < 0.0) hv += 1.0;
            if (hv > 1.0) hv -= 1.0;
            double v;
            if (hv < 1.0 / 6.0) {
                v = ls + (color_intensity - ls) * 6.0 * hv;
            } else if (hv < 1.0 / 2.0) {
                v = color_intensity;
            } else if (hv < 2.0 / 3.0) {
                v = ls + (color_intensity - ls) * (2.0 / 3.0 - hv) * 6.0;
            } else {
                v = ls;
            }
            outch[i] = v;
        }
        red = outch[0];
        green = outch[1];
        blue = outch[2];
    }
    return color_from_rgb((uint8_t)py_round_half_even(red * 255.0), (uint8_t)py_round_half_even(green * 255.0),
                          (uint8_t)py_round_half_even(blue * 255.0));
}
