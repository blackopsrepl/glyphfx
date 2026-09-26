// Input preprocessing: the mini terminal emulator, ported from the reference
// Terminal._preprocess_input_data. Walks the input codepoint by codepoint (one
// char = one cell, faithfully), tracking SGR color state and cursor movement,
// producing rows of arena character ids.
#ifndef GLYPHFX_INPUT_H
#define GLYPHFX_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine/character.h"
#include "utils/graphics.h"

// Insertion-ordered Color -> count map. Iteration order is behavior.
typedef struct {
    Color color;
    int64_t count;
} ColorFreqEntry;

typedef struct {
    ColorFreqEntry *items;
    size_t len;
    size_t cap;
} ColorFrequency;

void colorfreq_init(ColorFrequency *freq);
void colorfreq_free(ColorFrequency *freq);
void colorfreq_increment(ColorFrequency *freq, const Color *color);

typedef struct {
    CharId *items;
    size_t len;
} CharIdRow;

typedef struct {
    CharIdRow *rows;
    size_t len;
} CharIdRows;

void charidrows_free(CharIdRows *rows);

struct TerminalConfig;

// Error categories mirroring the reference taxonomy.
typedef enum {
    PREPROCESS_OK = 0,
    PREPROCESS_UNSUPPORTED_ANSI,
    PREPROCESS_INVALID_COLOR,
} PreprocessStatus;

typedef struct {
    PreprocessStatus status;
    char message[256];  // for INVALID_COLOR
    char sequence[64];  // for UNSUPPORTED_ANSI
} PreprocessError;

// Runs the emulator. On success returns 0 and fills *out (caller frees).
int preprocess_input(Arena *arena, uint32_t *next_character_id, ColorFrequency *freq,
                     const struct TerminalConfig *config, const char *input_data, CharIdRows *out,
                     PreprocessError *err);

#endif
