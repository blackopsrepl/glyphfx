// EffectCharacter storage. The arena keeps a character's slot index and its
// monotonic character_id distinct: the parser allocates an id for every parsed
// character, including ones later overwritten or cropped, so surviving
// characters have id gaps and id-ordered iteration depends on allocation order.
#ifndef GLYPHFX_CHARACTER_H
#define GLYPHFX_CHARACTER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "engine/animation.h"
#include "engine/charid.h"
#include "engine/events.h"
#include "engine/motion.h"
#include "utils/geometry.h"

typedef struct {
    uint32_t character_id;  // Python-compatible allocation id; ordering key
    char *input_symbol;     // owned
    Coord input_coord;
    Motion motion;
    bool is_visible;
    bool has_input_fg_seq;
    char *input_ansi_fg_sequence;  // owned
    bool has_input_bg_seq;
    char *input_ansi_bg_sequence;  // owned
    int64_t layer;
    bool is_fill_character;
    bool uses_input_preexisting_colors;
    Animation animation;
    EventHandler event_handler;
    CharId *links;  // ascending by id, owned
    size_t links_len;
    size_t links_cap;
    CharId north;
    CharId east;
    CharId south;
    CharId west;
} EffectCharacter;

void character_init(EffectCharacter *ch, uint32_t character_id, const char *symbol, int64_t column, int64_t row);
void character_free(EffectCharacter *ch);
// Movement is complete while no path is active; then the animation decides.
bool character_is_active(const EffectCharacter *ch);

typedef struct {
    EffectCharacter *items;
    size_t len;
    size_t cap;
} Arena;

void arena_init(Arena *a);
void arena_free(Arena *a);
// Returns the index of a newly grown, uninitialized slot.
CharId arena_alloc(Arena *a);

#endif
