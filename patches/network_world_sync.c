#include "patches.h"
#include "functions.h"
#include "enums.h"

// Network bridge
void recomp_net_send_collectible(u32 type, u32 id, u32 collected, u32 map_id, u32 level_id);
u32  recomp_net_pop_world_event(void *out);
u32  recomp_net_is_connected(void);

extern enum map_e map_get(void);
extern enum level_e level_get(void);
extern s32 item_getCount(enum item_e item);
extern s32 item_adjustByDiffWithHud(enum item_e item, s32 diff);
extern s32 item_adjustByDiffWithoutHud(enum item_e item, s32 diff);
extern void item_inc(enum item_e item);
extern u32 jiggyscore_isCollected(enum jiggy_e jiggy_id);
extern void jiggyscore_setCollected(s32 indx, s32 val);
extern bool mumboscore_get(s32 indx);
extern void mumboscore_set(s32 indx, bool val);
extern void marker_despawn(ActorMarker *marker);
extern ActorArray *suBaddieActorArray;

// Score pointers
extern u8 *jiggyscore_getPtr(void);
extern u8 *func_80321538(void);  // mumboscore ptr

// Collectible types (shared between C and C++ via packet)
#define COLLECTIBLE_JIGGY       0
#define COLLECTIBLE_NOTE        1
#define COLLECTIBLE_JINJO       2
#define COLLECTIBLE_MUMBO_TOKEN 3
#define COLLECTIBLE_DESPAWN_ONLY 4  // Non-shared: eggs, feathers, health, lives

#define EVENT_COLLECTIBLE 0

static bool processing_remote = FALSE;

// === Polling state ===
static s32 prev_jinjo_bits = 0;
static u8  prev_jiggyscore[0xD] = {0};
static u8  prev_mumboscore[16] = {0};
// Note/prop hiding (defined in note_saving.c)
extern void bkrecomp_net_hide_note(u32 note_index);
extern void bkrecomp_net_hide_nearest_prop(u32 asset_id, f32 px, f32 py, f32 pz);
// Non-shared: track to detect local pickups and broadcast despawn-only
static s32 prev_eggs = 0;
static s32 prev_red_feathers = 0;
static s32 prev_gold_feathers = 0;

// === Actor search ===

// Despawn first actor matching marker_id (for unique collectibles like jiggy, mumbo token)
static bool despawn_actor_by_marker_id(u32 marker_id) {
    if (!suBaddieActorArray) return FALSE;
    s32 i;
    for (i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (actor->marker && actor->marker->id == marker_id) {
            marker_despawn(actor->marker);
            return TRUE;
        }
    }
    return FALSE;
}

// Despawn the NEAREST actor matching marker_id to a given position.
// Used for non-unique collectibles (multiple eggs/feathers on same map).
static bool despawn_nearest_actor(u32 marker_id, f32 px, f32 py, f32 pz) {
    if (!suBaddieActorArray) return FALSE;

    f32 best_dist = 999999.0f;
    Actor *best_actor = (Actor*)0;
    s32 i;

    for (i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (!actor->marker || actor->marker->id != marker_id) continue;

        f32 dx = actor->position[0] - px;
        f32 dy = actor->position[1] - py;
        f32 dz = actor->position[2] - pz;
        f32 dist = dx*dx + dy*dy + dz*dz;
        if (dist < best_dist) {
            best_dist = dist;
            best_actor = actor;
        }
    }

    if (best_actor) {
        marker_despawn(best_actor->marker);
        return TRUE;
    }
    return FALSE;
}

// === POLLING: Detect local collectible changes each frame ===

static void poll_shared_collectibles(void) {
    if (!recomp_net_is_connected() || processing_remote) return;

    u32 cur_map = (u32)map_get();
    u32 cur_level = (u32)level_get();

    // --- Jinjos ---
    {
        s32 cur = item_getCount(ITEM_12_JINJOS);
        if (cur != prev_jinjo_bits) {
            s32 new_bits = cur & ~prev_jinjo_bits;
            if (new_bits > 0) {
                recomp_net_send_collectible(COLLECTIBLE_JINJO, (u32)new_bits, 1, cur_map, cur_level);
            }
            prev_jinjo_bits = cur;
        }
    }

    // --- Jigsaws ---
    {
        u8 *score = jiggyscore_getPtr();
        if (score) {
            s32 i;
            for (i = 0; i < 0xD; i++) {
                u8 new_bits = score[i] & ~prev_jiggyscore[i];
                if (new_bits) {
                    s32 bit;
                    for (bit = 0; bit < 8; bit++) {
                        if (new_bits & (1 << bit)) {
                            s32 jiggy_id = i * 8 + bit + 1;
                            if (jiggy_id > 0 && jiggy_id < 0x65) {
                                recomp_net_send_collectible(COLLECTIBLE_JIGGY, (u32)jiggy_id, 1, cur_map, cur_level);
                            }
                        }
                    }
                }
                prev_jiggyscore[i] = score[i];
            }
        }
    }

    // --- Mumbo tokens ---
    {
        u8 *score = func_80321538();
        if (score) {
            s32 i;
            for (i = 0; i < 16; i++) {
                u8 new_bits = score[i] & ~prev_mumboscore[i];
                if (new_bits) {
                    s32 bit;
                    for (bit = 0; bit < 8; bit++) {
                        if (new_bits & (1 << bit)) {
                            s32 token_id = i * 8 + bit + 1;
                            recomp_net_send_collectible(COLLECTIBLE_MUMBO_TOKEN, (u32)token_id, 1, cur_map, cur_level);
                        }
                    }
                }
                prev_mumboscore[i] = score[i];
            }
        }
    }

    // Notes: sent directly from note_saving.c __baMarker_resolveMusicNoteCollision
    // with the specific note_index. No polling needed here.
}

static void poll_nonshared_collectibles(void) {
    if (!recomp_net_is_connected() || processing_remote) return;

    u32 cur_map = (u32)map_get();
    u32 cur_level = (u32)level_get();

    // Non-shared: only detect single increments (actual pickups).
    // Skip large jumps (map init sets eggs=100, feathers=50, etc.)
    // Eggs
    {
        s32 cur = item_getCount(ITEM_D_EGGS);
        if (cur == prev_eggs + 1) {
            recomp_net_send_collectible(COLLECTIBLE_DESPAWN_ONLY, ASSET_36D_SPRITE_BLUE_EGG, 1, cur_map, cur_level);
        }
        prev_eggs = cur;
    }
    // Red feathers
    {
        s32 cur = item_getCount(ITEM_F_RED_FEATHER);
        if (cur == prev_red_feathers + 1) {
            recomp_net_send_collectible(COLLECTIBLE_DESPAWN_ONLY, ASSET_580_SPRITE_RED_FEATHER, 1, cur_map, cur_level);
        }
        prev_red_feathers = cur;
    }
    // Gold feathers
    {
        s32 cur = item_getCount(ITEM_10_GOLD_FEATHER);
        if (cur == prev_gold_feathers + 1) {
            recomp_net_send_collectible(COLLECTIBLE_DESPAWN_ONLY, ASSET_6D1_SPRITE_GOLDFEATHTER, 1, cur_map, cur_level);
        }
        prev_gold_feathers = cur;
    }
}

// === RECEIVE: Apply remote collectible events ===

typedef struct {
    u8  event_type;       // 0x00
    u8  _pad[3];          // 0x01-0x03
    // Collectible data starts at 0x04:
    u8  coll_type;        // 0x04
    u8  _cp;              // 0x05
    u16 coll_id;          // 0x06
    u8  coll_collected;   // 0x08
    u8  _cp2[3];          // 0x09-0x0B
    u32 coll_map_id;      // 0x0C
    u8  coll_level_id;    // 0x10
    u8  _cp3[3];          // 0x11-0x13
    f32 coll_pos_x;       // 0x14
    f32 coll_pos_y;       // 0x18
    f32 coll_pos_z;       // 0x1C
} WorldEventData;

static void process_collectible_event(WorldEventData *evt) {
    processing_remote = TRUE;

    switch (evt->coll_type) {
        case COLLECTIBLE_JIGGY:
            if (!jiggyscore_isCollected(evt->coll_id)) {
                jiggyscore_setCollected(evt->coll_id, TRUE);
                item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, 1);
                // Update polling state
                { u8 *s = jiggyscore_getPtr(); if (s) { s32 i; for (i=0;i<0xD;i++) prev_jiggyscore[i]=s[i]; } }
                // Despawn jiggy actor if on same map
                if ((u32)map_get() == evt->coll_map_id) {
                    despawn_actor_by_marker_id(MARKER_52_JIGGY);
                }
            }
            break;

        case COLLECTIBLE_NOTE:
            item_inc(ITEM_C_NOTE);
            // Hide the specific note by its note_index if on same map
            if ((u32)map_get() == evt->coll_map_id) {
                if (evt->coll_id == 0xFFFE) {
                    // Dynamic note — try actor despawn
                    despawn_actor_by_marker_id(MARKER_5F_MUSIC_NOTE);
                } else {
                    // Static note — hide by note_index via prop system
                    bkrecomp_net_hide_note(evt->coll_id);
                }
            }
            break;

        case COLLECTIBLE_JINJO: {
            item_adjustByDiffWithHud(ITEM_12_JINJOS, (s32)evt->coll_id);
            prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
            // Despawn correct color jinjo
            if ((u32)map_get() == evt->coll_map_id) {
                u32 bit = evt->coll_id;
                u32 marker_id = 0;
                if (bit & 0x01) marker_id = MARKER_5A_JINJO_BLUE;
                else if (bit & 0x02) marker_id = MARKER_5B_JINJO_GREEN;
                else if (bit & 0x04) marker_id = MARKER_5C_JINJO_ORANGE;
                else if (bit & 0x08) marker_id = MARKER_5D_JINJO_PINK;
                else if (bit & 0x10) marker_id = MARKER_5E_JINJO_YELLOW;
                if (marker_id) despawn_actor_by_marker_id(marker_id);
            }
            break;
        }

        case COLLECTIBLE_MUMBO_TOKEN:
            if (!mumboscore_get(evt->coll_id)) {
                mumboscore_set(evt->coll_id, TRUE);
                item_inc(ITEM_1C_MUMBO_TOKEN);
                { u8 *s = func_80321538(); if (s) { s32 i; for (i=0;i<16;i++) prev_mumboscore[i]=s[i]; } }
                if ((u32)map_get() == evt->coll_map_id) {
                    despawn_actor_by_marker_id(MARKER_39_MUMBO_TOKEN);
                }
            }
            break;

        case COLLECTIBLE_DESPAWN_ONLY:
            // Non-shared: hide nearest sprite prop by asset_id + position.
            // Eggs/feathers are sprite props (not actors), so we use the
            // cube/prop system via note_saving.c's bkrecomp_net_hide_nearest_prop.
            if ((u32)map_get() == evt->coll_map_id) {
                bkrecomp_net_hide_nearest_prop(evt->coll_id,
                    evt->coll_pos_x, evt->coll_pos_y, evt->coll_pos_z);
            }
            break;
    }

    processing_remote = FALSE;
}

// Called every frame
RECOMP_EXPORT void bkrecomp_net_process_world_events(void) {
    if (!recomp_net_is_connected()) return;

    poll_shared_collectibles();
    poll_nonshared_collectibles();

    WorldEventData evt;
    while (recomp_net_pop_world_event(&evt)) {
        if (evt.event_type == EVENT_COLLECTIBLE) {
            process_collectible_event(&evt);
        }
    }
}
