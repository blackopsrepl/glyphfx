#include "engine/character.h"

#include <stdlib.h>
#include <string.h>

#include "utils/strtab.h"

void character_init(EffectCharacter *ch, uint32_t character_id, const char *symbol, int64_t column, int64_t row) {
    memset(ch, 0, sizeof(*ch));
    ch->character_id = character_id;
    ch->input_symbol = strtab_intern(symbol);
    ch->input_coord = coord_new(column, row);
    motion_init(&ch->motion, coord_new(column, row));
    ch->motion.render_id = (int32_t)character_id;
    ch->north = CHAR_ID_NONE;
    ch->east = CHAR_ID_NONE;
    ch->south = CHAR_ID_NONE;
    ch->west = CHAR_ID_NONE;
    animation_init(&ch->animation, symbol);
    ch->animation.render_id = (int32_t)character_id;
}

void character_free(EffectCharacter *ch) {
    free(ch->input_ansi_fg_sequence);
    free(ch->input_ansi_bg_sequence);
    animation_free(&ch->animation);
    event_handler_free(&ch->event_handler);
    free(ch->links);
    motion_free(&ch->motion);
    memset(ch, 0, sizeof(*ch));
}

bool character_is_active(const EffectCharacter *ch) {
    if (!motion_movement_is_complete(&ch->motion)) {
        return true;
    }
    return !animation_active_scene_is_complete(&ch->animation);
}

void arena_init(Arena *a) {
    a->items = NULL;
    a->len = 0;
    a->cap = 0;
}

void arena_free(Arena *a) {
    for (size_t i = 0; i < a->len; i++) {
        character_free(&a->items[i]);
    }
    free(a->items);
    arena_init(a);
}

CharId arena_alloc(Arena *a) {
    if (a->len == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 64;
        EffectCharacter *grown = realloc(a->items, cap * sizeof(EffectCharacter));
        if (!grown) {
            return CHAR_ID_NONE;
        }
        a->items = grown;
        a->cap = cap;
    }
    CharId id = (CharId)a->len;
    a->len++;
    return id;
}
