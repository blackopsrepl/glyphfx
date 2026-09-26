#include "engine/character.h"

#include <stdlib.h>
#include <string.h>

static char *dup_cstr(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len + 1);
    return out;
}

void character_init(EffectCharacter *ch, uint32_t character_id, const char *symbol, int64_t column, int64_t row) {
    memset(ch, 0, sizeof(*ch));
    ch->character_id = character_id;
    ch->input_symbol = dup_cstr(symbol);
    ch->input_coord = coord_new(column, row);
    ch->motion_coord = coord_new(column, row);
    ch->north = CHAR_ID_NONE;
    ch->east = CHAR_ID_NONE;
    ch->south = CHAR_ID_NONE;
    ch->west = CHAR_ID_NONE;
    animation_init(&ch->animation, symbol);
}

void character_free(EffectCharacter *ch) {
    free(ch->input_symbol);
    free(ch->input_ansi_fg_sequence);
    free(ch->input_ansi_bg_sequence);
    animation_free(&ch->animation);
    event_handler_free(&ch->event_handler);
    memset(ch, 0, sizeof(*ch));
}

bool character_is_active(const EffectCharacter *ch) {
    bool movement_complete = !ch->has_active_path;
    if (!movement_complete) {
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
