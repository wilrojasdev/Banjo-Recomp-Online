#include "patches.h"
#include "functions.h"
#include "enums.h"

// Network bridge
void recomp_net_send_collectible(u32 type, u32 id, u32 collected, u32 map_id, u32 level_id);
void recomp_net_send_enemy_death(u32 marker_type, u32 spawn_index, u32 map_id, f32 *pos);
u32  recomp_net_pop_world_event(void *out);
u32  recomp_net_is_connected(void);
u32  recomp_net_is_host(void);
void recomp_net_send_enemy_positions(void *buf, u32 count, u32 map_id);
void recomp_net_get_enemy_positions(void *buf, u32 *count);
u32  recomp_net_should_send_full_sync(u8 *out_player_id);
void recomp_net_send_world_state_full(void *data, u32 size, u32 target_player);
u32  recomp_net_pop_full_state(void *out);

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

// Marker spawn index (stable ID across clients)
extern u32 bkrecomp_get_marker_spawn_index(ActorMarker* marker);

// Animation control
extern enum asset_e anctrl_getIndex(AnimCtrl *this);
extern f32 anctrl_getAnimTimer(AnimCtrl *this);
extern void anctrl_setIndex(AnimCtrl *this, enum asset_e index);
extern void anctrl_setAnimTimer(AnimCtrl *this, f32 timer);

// Player marker (for triggering enemy death callbacks)
extern ActorMarker *baMarker_get(void);

// Bundle/item drop system (honeycomb on enemy kill)
extern Actor *__bundle_spawnFromFirstActor(enum bundle_e bundle_id, Actor *actor);
extern Actor *bundle_spawn_f32(enum bundle_e bundle_id, f32 position[3]);
extern void bundle_setYaw(f32 yaw);
extern s32 D_8036E564; // bundle count global, read by bundle system

// Score pointers
extern u8 *jiggyscore_getPtr(void);
extern u8 *func_80321538(void);  // mumboscore ptr
extern u8 *honeycombscore_get_ptr(void);
extern bool honeycombscore_get(s32 indx);
extern void honeycombscore_set(s32 indx, bool val);

// Collectible types (shared between C and C++ via packet)
#define COLLECTIBLE_JIGGY            0
#define COLLECTIBLE_NOTE             1
#define COLLECTIBLE_JINJO            2
#define COLLECTIBLE_MUMBO_TOKEN      3
#define COLLECTIBLE_DESPAWN_ONLY     4  // Non-shared: eggs, feathers, honeycomb
#define COLLECTIBLE_EMPTY_HONEYCOMB  5  // Shared: panel pieces (2 per world)
#define COLLECTIBLE_EXTRA_LIFE       6  // Shared: Banjo trophy

#define EVENT_COLLECTIBLE 0
#define EVENT_ENEMY       1

static bool processing_remote = FALSE;

// === Enemy tracking ===
#define MAX_TRACKED_ENEMIES 128

typedef struct {
    u16 marker_id;
    u16 spawn_index;
    f32 pos_x, pos_y, pos_z;
} TrackedEnemy;

// --- Dying enemies: stop position override so death animation can play ---
#define MAX_DYING_ENEMIES 32
static u16 dying_spawns[MAX_DYING_ENEMIES];
static s32 dying_count = 0;

static bool is_dying(u16 spawn_index) {
    s32 i;
    for (i = 0; i < dying_count; i++) {
        if (dying_spawns[i] == spawn_index) return TRUE;
    }
    return FALSE;
}

static void mark_dying(u16 spawn_index) {
    if (dying_count < MAX_DYING_ENEMIES && !is_dying(spawn_index)) {
        dying_spawns[dying_count] = spawn_index;
        dying_count++;
    }
}

// --- Join dieFunc proxy ---
// Saves original dieFuncs. Proxy sends event to host + calls original for local death.
#define MAX_SAVED_DIEFUNCS 64
static struct {
    u16 spawn_index;
    MarkerCollisionFunc original;
} saved_diefuncs[MAX_SAVED_DIEFUNCS];
static s32 saved_diefunc_count = 0;

static void net_enemy_die_proxy(ActorMarker *self_marker, ActorMarker *other_marker);

static void save_diefunc(u16 spawn_index, MarkerCollisionFunc func) {
    if (func == (MarkerCollisionFunc)net_enemy_die_proxy || !func) return;
    s32 i;
    for (i = 0; i < saved_diefunc_count; i++) {
        if (saved_diefuncs[i].spawn_index == spawn_index) return; // already saved
    }
    if (saved_diefunc_count < MAX_SAVED_DIEFUNCS) {
        saved_diefuncs[saved_diefunc_count].spawn_index = spawn_index;
        saved_diefuncs[saved_diefunc_count].original = func;
        saved_diefunc_count++;
    }
}

static MarkerCollisionFunc get_saved_diefunc(u16 spawn_index) {
    s32 i;
    for (i = 0; i < saved_diefunc_count; i++) {
        if (saved_diefuncs[i].spawn_index == spawn_index)
            return saved_diefuncs[i].original;
    }
    return (MarkerCollisionFunc)0;
}

static void net_enemy_die_proxy(ActorMarker *self_marker, ActorMarker *other_marker) {
    if (!self_marker || !suBaddieActorArray) return;

    Actor *actor = &suBaddieActorArray->data[self_marker->actrArrayIdx];
    u16 spawn_index = (u16)bkrecomp_get_marker_spawn_index(self_marker);
    u16 marker_id = (u16)self_marker->id;

    f32 pos[3];
    pos[0] = actor->position[0];
    pos[1] = actor->position[1];
    pos[2] = actor->position[2];

    // Send kill event to host
    recomp_net_send_enemy_death((u32)marker_id, (u32)spawn_index, (u32)map_get(), pos);

    // Stop position override so death animation plays
    mark_dying(spawn_index);

    // Restore + call original dieFunc for natural death sequence
    MarkerCollisionFunc orig = get_saved_diefunc(spawn_index);
    if (orig) {
        self_marker->dieFunc = orig;
        orig(self_marker, other_marker);
    } else {
        marker_despawn(self_marker);
    }
}

static TrackedEnemy prev_enemies[MAX_TRACKED_ENEMIES];
static s32 prev_enemy_count = 0;
static u32 prev_enemy_map = 0xFFFFFFFF;

// Returns TRUE for marker types already handled by collectible sync
static bool is_collectible_marker(u32 id) {
    if (id == MARKER_52_JIGGY) return TRUE;
    if (id == MARKER_39_MUMBO_TOKEN) return TRUE;
    if (id == MARKER_5A_JINJO_BLUE) return TRUE;
    if (id == MARKER_5B_JINJO_GREEN) return TRUE;
    if (id == MARKER_5C_JINJO_ORANGE) return TRUE;
    if (id == MARKER_5D_JINJO_PINK) return TRUE;
    if (id == MARKER_5E_JINJO_YELLOW) return TRUE;
    if (id == MARKER_5F_MUSIC_NOTE) return TRUE;
    if (id == MARKER_53_EMPTY_HONEYCOMB) return TRUE;
    if (id == MARKER_61_EXTRA_LIFE) return TRUE;
    if (id == MARKER_55_HONEYCOMB) return TRUE;
    if (id == MARKER_60_BLUE_EGG_COLLECTIBLE) return TRUE;
    if (id == MARKER_36_ORANGE_COLLECTIBLE) return TRUE;
    if (id == MARKER_37_GOLD_BULLION) return TRUE;
    // UI/system markers
    if (id == MARKER_32_PLAYER_SHADOW) return TRUE;
    if (id == MARKER_62_RED_ARROW) return TRUE;
    if (id == MARKER_63_RED_QUESTION_MARK) return TRUE;
    if (id == MARKER_64_RED_X) return TRUE;
    if (id == MARKER_65_SHRAPNEL) return TRUE;
    return FALSE;
}

// === Polling state ===
static s32 prev_jinjo_bits = 0;
static u8  prev_jiggyscore[0xD] = {0};
static u8  prev_mumboscore[16] = {0};
// Note/prop hiding (defined in note_saving.c)
extern void bkrecomp_net_hide_note(u32 note_index);
extern void bkrecomp_net_hide_nearest_prop(u32 asset_id, f32 px, f32 py, f32 pz);
static u8  prev_honeycombscore[3] = {0};
static s32 prev_lives = 0;
// Non-shared
static s32 prev_eggs = 0;
static s32 prev_red_feathers = 0;
static s32 prev_gold_feathers = 0;
static s32 prev_health = 0;

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

    // Notes: sent directly from note_saving.c (no polling needed)

    // --- Empty honeycombs (panel pieces, 2 per world) ---
    {
        u8 *score = honeycombscore_get_ptr();
        if (score) {
            s32 i;
            for (i = 0; i < 3; i++) {
                u8 new_bits = score[i] & ~prev_honeycombscore[i];
                if (new_bits) {
                    recomp_printf("[HC-POLL] byte[%d] score=0x%X prev=0x%X new=0x%X\n", i, score[i], prev_honeycombscore[i], new_bits);
                    s32 bit;
                    for (bit = 0; bit < 8; bit++) {
                        if (new_bits & (1 << bit)) {
                            s32 hc_id = i * 8 + bit + 1;
                            if (hc_id > 0 && hc_id < 0x19) {
                                recomp_net_send_collectible(COLLECTIBLE_EMPTY_HONEYCOMB, (u32)hc_id, 1, cur_map, cur_level);
                            }
                        }
                    }
                }
                prev_honeycombscore[i] = score[i];
            }
        }
    }

    // --- Extra life (Banjo trophy) ---
    {
        s32 cur = item_getCount(ITEM_16_LIFE);
        if (cur != prev_lives) {
            recomp_printf("[LIFE-POLL] cur=%d prev=%d diff=%d\n", cur, prev_lives, cur - prev_lives);
            if (cur == prev_lives + 1) {
                recomp_printf("[LIFE-SEND] sending type=%d\n", COLLECTIBLE_EXTRA_LIFE);
                recomp_net_send_collectible(COLLECTIBLE_EXTRA_LIFE, 0, 1, cur_map, cur_level);
            }
        }
        prev_lives = cur;
    }
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
    // Honeycomb (health) — non-shared, despawn only
    {
        s32 cur = item_getCount(ITEM_14_HEALTH);
        if (cur == prev_health + 1) {
            recomp_net_send_collectible(COLLECTIBLE_DESPAWN_ONLY, 0, 1, cur_map, cur_level);
        }
        prev_health = cur;
    }
}

// === POLLING: Detect local enemy deaths each frame ===

static void poll_enemy_deaths(void) {
    if (!recomp_net_is_connected() || processing_remote) return;
    // Only host polls — join detects kills via state change in sync_enemy_positions
    if (!recomp_net_is_host()) return;
    if (!suBaddieActorArray) return;

    u32 cur_map = (u32)map_get();

    // Reset tracking on map change (avoids false positives from level unloading)
    if (cur_map != prev_enemy_map) {
        prev_enemy_count = 0;
        prev_enemy_map = cur_map;
        saved_diefunc_count = 0;
        dying_count = 0;
        // Build initial snapshot without sending events
        s32 count = 0;
        s32 i;
        for (i = 0; i < suBaddieActorArray->cnt && count < MAX_TRACKED_ENEMIES; i++) {
            Actor *actor = &suBaddieActorArray->data[i];
            if (!actor->marker) continue;
            u32 mid = actor->marker->id;
            if (is_collectible_marker(mid)) continue;
            prev_enemies[count].marker_id = (u16)mid;
            prev_enemies[count].spawn_index = (u16)bkrecomp_get_marker_spawn_index(actor->marker);
            prev_enemies[count].pos_x = actor->position[0];
            prev_enemies[count].pos_y = actor->position[1];
            prev_enemies[count].pos_z = actor->position[2];
            count++;
        }
        prev_enemy_count = count;
        return;
    }

    // Build current snapshot
    TrackedEnemy cur_enemies[MAX_TRACKED_ENEMIES];
    s32 cur_count = 0;
    s32 i;
    for (i = 0; i < suBaddieActorArray->cnt && cur_count < MAX_TRACKED_ENEMIES; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (!actor->marker) continue;
        u32 mid = actor->marker->id;
        if (is_collectible_marker(mid)) continue;
        cur_enemies[cur_count].marker_id = (u16)mid;
        cur_enemies[cur_count].spawn_index = (u16)bkrecomp_get_marker_spawn_index(actor->marker);
        cur_enemies[cur_count].pos_x = actor->position[0];
        cur_enemies[cur_count].pos_y = actor->position[1];
        cur_enemies[cur_count].pos_z = actor->position[2];
        cur_count++;
    }

    // Detect deaths: entries in prev but not in current
    s32 p;
    for (p = 0; p < prev_enemy_count; p++) {
        bool found = FALSE;
        s32 c;
        for (c = 0; c < cur_count; c++) {
            if (prev_enemies[p].marker_id == cur_enemies[c].marker_id &&
                prev_enemies[p].spawn_index == cur_enemies[c].spawn_index) {
                found = TRUE;
                break;
            }
        }
        if (!found) {
            f32 pos[3];
            pos[0] = prev_enemies[p].pos_x;
            pos[1] = prev_enemies[p].pos_y;
            pos[2] = prev_enemies[p].pos_z;

            recomp_printf("[ENEMY-POLL] death: marker=0x%X spawn=%d pos=(%.1f,%.1f,%.1f)\n",
                prev_enemies[p].marker_id, prev_enemies[p].spawn_index,
                pos[0], pos[1], pos[2]);

            recomp_net_send_enemy_death(
                (u32)prev_enemies[p].marker_id,
                (u32)prev_enemies[p].spawn_index,
                cur_map, pos);
        }
    }

    // Update prev snapshot (manual field copy to avoid compiler-generated memcpy)
    s32 j;
    for (j = 0; j < cur_count; j++) {
        prev_enemies[j].marker_id = cur_enemies[j].marker_id;
        prev_enemies[j].spawn_index = cur_enemies[j].spawn_index;
        prev_enemies[j].pos_x = cur_enemies[j].pos_x;
        prev_enemies[j].pos_y = cur_enemies[j].pos_y;
        prev_enemies[j].pos_z = cur_enemies[j].pos_z;
    }
    prev_enemy_count = cur_count;
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

    u8 ct = evt->coll_type;

    if (ct == COLLECTIBLE_JIGGY) {
        if (!jiggyscore_isCollected(evt->coll_id)) {
            jiggyscore_setCollected(evt->coll_id, TRUE);
            item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, 1);
            { u8 *s = jiggyscore_getPtr(); if (s) { s32 i; for (i=0;i<0xD;i++) prev_jiggyscore[i]=s[i]; } }
            if ((u32)map_get() == evt->coll_map_id) {
                despawn_actor_by_marker_id(MARKER_52_JIGGY);
                bkrecomp_net_hide_nearest_prop(0, evt->coll_pos_x, evt->coll_pos_y, evt->coll_pos_z);
            }
        }
    } else if (ct == COLLECTIBLE_NOTE) {
        item_inc(ITEM_C_NOTE);
        if ((u32)map_get() == evt->coll_map_id) {
            if (evt->coll_id == 0xFFFE) {
                despawn_actor_by_marker_id(MARKER_5F_MUSIC_NOTE);
            } else {
                bkrecomp_net_hide_note(evt->coll_id);
            }
        }
    } else if (ct == COLLECTIBLE_JINJO) {
        item_adjustByDiffWithHud(ITEM_12_JINJOS, (s32)evt->coll_id);
        prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
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
    } else if (ct == COLLECTIBLE_MUMBO_TOKEN) {
        if (!mumboscore_get(evt->coll_id)) {
            mumboscore_set(evt->coll_id, TRUE);
            item_inc(ITEM_1C_MUMBO_TOKEN);
            { u8 *s = func_80321538(); if (s) { s32 i; for (i=0;i<16;i++) prev_mumboscore[i]=s[i]; } }
            if ((u32)map_get() == evt->coll_map_id) {
                despawn_actor_by_marker_id(MARKER_39_MUMBO_TOKEN);
                bkrecomp_net_hide_nearest_prop(0, evt->coll_pos_x, evt->coll_pos_y, evt->coll_pos_z);
            }
        }
    } else if (ct == COLLECTIBLE_DESPAWN_ONLY) {
        if ((u32)map_get() == evt->coll_map_id) {
            bkrecomp_net_hide_nearest_prop(evt->coll_id,
                evt->coll_pos_x, evt->coll_pos_y, evt->coll_pos_z);
        }
    } else if (ct == COLLECTIBLE_EMPTY_HONEYCOMB) {
        if (!honeycombscore_get(evt->coll_id)) {
            honeycombscore_set(evt->coll_id, TRUE);
            item_inc(ITEM_13_EMPTY_HONEYCOMB);
            { u8 *s = honeycombscore_get_ptr(); if (s) { s32 i; for(i=0;i<3;i++) prev_honeycombscore[i]=s[i]; } }
            if ((u32)map_get() == evt->coll_map_id) {
                // Despawn actor AND hide sprite prop
                despawn_actor_by_marker_id(MARKER_53_EMPTY_HONEYCOMB);
                bkrecomp_net_hide_nearest_prop(0, evt->coll_pos_x, evt->coll_pos_y, evt->coll_pos_z);
            }
        }
    } else if (ct == COLLECTIBLE_EXTRA_LIFE) {
        item_inc(ITEM_16_LIFE);
        prev_lives = item_getCount(ITEM_16_LIFE);
        if ((u32)map_get() == evt->coll_map_id) {
            // Despawn actor AND hide sprite prop (some collectibles have both)
            despawn_actor_by_marker_id(MARKER_61_EXTRA_LIFE);
            bkrecomp_net_hide_nearest_prop(0, evt->coll_pos_x, evt->coll_pos_y, evt->coll_pos_z);
        }
    }

    processing_remote = FALSE;
}

// Enemy event layout (matches net_recomp_api.cpp ENEMY case output):
typedef struct {
    u8  event_type;        // 0x00
    u8  _pad[3];           // 0x01-0x03
    u16 enemy_marker_type; // 0x04
    u16 enemy_spawn_index; // 0x06
    u32 enemy_map_id;      // 0x08
    u8  enemy_alive;       // 0x0C
    u8  enemy_health;      // 0x0D
    u8  _pad2[2];          // 0x0E-0x0F
    f32 enemy_pos_x;       // 0x10
    f32 enemy_pos_y;       // 0x14
    f32 enemy_pos_z;       // 0x18
} EnemyEventData;

static void process_enemy_event(EnemyEventData *evt) {
    processing_remote = TRUE;

    u32 cur_map = (u32)map_get();

    if (cur_map != evt->enemy_map_id) {
        processing_remote = FALSE;
        return;
    }

    u16 target_marker = evt->enemy_marker_type;
    u16 target_spawn = evt->enemy_spawn_index;
    f32 pos[3];
    pos[0] = evt->enemy_pos_x;
    pos[1] = evt->enemy_pos_y;
    pos[2] = evt->enemy_pos_z;

    // Stop position override so death animation can play
    mark_dying(target_spawn);

    D_8036E564 = 1;
    bundle_setYaw(0.0f);

    if (suBaddieActorArray) {
        s32 i;
        for (i = 0; i < suBaddieActorArray->cnt; i++) {
            Actor *actor = &suBaddieActorArray->data[i];
            if (!actor->marker) continue;
            if (actor->marker->id != target_marker) continue;
            u16 si = (u16)bkrecomp_get_marker_spawn_index(actor->marker);
            if (si != target_spawn) continue;

            // Spawn honeycomb (killer got theirs from collision, we need ours)
            __bundle_spawnFromFirstActor(BUNDLE_19__HONEYCOMB, actor);

            // Trigger death animation via dieFunc (updateFunc is active, so it plays!)
            MarkerCollisionFunc die = get_saved_diefunc(target_spawn);
            if (!die) die = actor->marker->dieFunc;
            if (die && die != (MarkerCollisionFunc)net_enemy_die_proxy) {
                actor->marker->dieFunc = die; // restore original if proxy
                die(actor->marker, baMarker_get());
            } else {
                marker_despawn(actor->marker);
            }
            goto done;
        }
    }

    // Actor not found — spawn honeycomb at packet position
    bundle_spawn_f32(BUNDLE_19__HONEYCOMB, pos);

done:
    // If host: remove from prev_enemies so poll doesn't re-detect
    if (recomp_net_is_host()) {
        s32 p;
        for (p = 0; p < prev_enemy_count; p++) {
            if (prev_enemies[p].marker_id == target_marker &&
                prev_enemies[p].spawn_index == target_spawn) {
                prev_enemies[p].marker_id = prev_enemies[prev_enemy_count - 1].marker_id;
                prev_enemies[p].spawn_index = prev_enemies[prev_enemy_count - 1].spawn_index;
                prev_enemies[p].pos_x = prev_enemies[prev_enemy_count - 1].pos_x;
                prev_enemies[p].pos_y = prev_enemies[prev_enemy_count - 1].pos_y;
                prev_enemies[p].pos_z = prev_enemies[prev_enemy_count - 1].pos_z;
                prev_enemy_count--;
                break;
            }
        }
    }

    processing_remote = FALSE;
}

// === Enemy position sync (host-authoritative) ===

// Must match MIPS layout expected by net_recomp_api.cpp (28 bytes per entry)
typedef struct {
    u16 spawn_index;  // 0x00
    u16 marker_id;    // 0x02
    f32 x;            // 0x04
    f32 y;            // 0x08
    f32 z;            // 0x0C
    f32 yaw;          // 0x10
    u16 anim_id;      // 0x14
    u16 _pad;         // 0x16
    f32 anim_timer;   // 0x18
} EnemyPosEntry;      // 0x1C = 28 bytes

#define MAX_ENEMY_POS_ENTRIES 64

static void sync_enemy_positions(void) {
    if (!recomp_net_is_connected()) return;
    if (!suBaddieActorArray) return;

    u32 cur_map = (u32)map_get();

    if (recomp_net_is_host()) {
        // HOST: collect enemy positions and send to network
        EnemyPosEntry buf[MAX_ENEMY_POS_ENTRIES];
        s32 count = 0;
        s32 i;

        for (i = 0; i < suBaddieActorArray->cnt && count < MAX_ENEMY_POS_ENTRIES; i++) {
            Actor *actor = &suBaddieActorArray->data[i];
            if (!actor->marker) continue;
            u32 mid = actor->marker->id;
            if (is_collectible_marker(mid)) continue;

            buf[count].spawn_index = (u16)bkrecomp_get_marker_spawn_index(actor->marker);
            buf[count].marker_id = (u16)mid;
            buf[count].x = actor->position[0];
            buf[count].y = actor->position[1];
            buf[count].z = actor->position[2];
            buf[count].yaw = actor->yaw;
            if (actor->anctrl) {
                buf[count].anim_id = (u16)anctrl_getIndex(actor->anctrl);
                buf[count].anim_timer = anctrl_getAnimTimer(actor->anctrl);
            } else {
                buf[count].anim_id = 0;
                buf[count].anim_timer = 0.0f;
            }
            count++;
        }

        if (count > 0) {
            recomp_net_send_enemy_positions(buf, (u32)count, cur_map);
        }
    } else {
        // JOIN: receive interpolated positions and apply to local actors
        EnemyPosEntry buf[MAX_ENEMY_POS_ENTRIES];
        u32 count = 0;
        recomp_net_get_enemy_positions(buf, &count);

        if (count == 0) return;

        // Build set of spawn_indices that host has alive
        u16 host_alive[MAX_ENEMY_POS_ENTRIES];
        s32 host_alive_count = 0;

        s32 e;
        for (e = 0; e < (s32)count; e++) {
            u16 target_spawn = buf[e].spawn_index;
            u16 target_marker = buf[e].marker_id;

            host_alive[host_alive_count] = target_spawn;
            host_alive_count++;

            // Skip dying enemies — let death animation play without override
            if (is_dying(target_spawn)) continue;

            // Find matching actor
            s32 i;
            for (i = 0; i < suBaddieActorArray->cnt; i++) {
                Actor *actor = &suBaddieActorArray->data[i];
                if (!actor->marker) continue;
                if (actor->marker->id != target_marker) continue;

                u16 si = (u16)bkrecomp_get_marker_spawn_index(actor->marker);
                if (si != target_spawn) continue;

                // Apply host position
                actor->position[0] = buf[e].x;
                actor->position[1] = buf[e].y;
                actor->position[2] = buf[e].z;
                actor->yaw = buf[e].yaw;

                // Full animation sync from host (ID + timer every frame)
                if (actor->anctrl && buf[e].anim_id != 0) {
                    anctrl_setIndex(actor->anctrl, (enum asset_e)buf[e].anim_id);
                    anctrl_setAnimTimer(actor->anctrl, buf[e].anim_timer);
                }

                // Zero velocity to prevent physics drift
                actor->velocity[0] = 0.0f;
                actor->velocity[1] = 0.0f;
                actor->velocity[2] = 0.0f;

                // Suppress local AI — host controls position + animation.
                actor->marker->actorUpdateFunc = (ActorUpdateFunc)0;

                // Install dieFunc proxy so join kills send events to host
                save_diefunc(target_spawn, actor->marker->dieFunc);
                actor->marker->dieFunc = (MarkerCollisionFunc)net_enemy_die_proxy;

                break;
            }
        }

        // Despawn local enemies that host doesn't have (killed before join entered)
        // Iterate backwards since marker_despawn uses swap-and-pop
        if (host_alive_count > 0 && suBaddieActorArray) {
            s32 i;
            for (i = suBaddieActorArray->cnt - 1; i >= 0; i--) {
                Actor *actor = &suBaddieActorArray->data[i];
                if (!actor->marker) continue;
                u32 mid = actor->marker->id;
                if (is_collectible_marker(mid)) continue;
                if (is_dying((u16)bkrecomp_get_marker_spawn_index(actor->marker))) continue;

                u16 si = (u16)bkrecomp_get_marker_spawn_index(actor->marker);
                bool in_host = FALSE;
                s32 h;
                for (h = 0; h < host_alive_count; h++) {
                    if (host_alive[h] == si) { in_host = TRUE; break; }
                }
                if (!in_host) {
                    marker_despawn(actor->marker);
                }
            }
        }
    }
}

// === Full state sync on join ===

// MIPS layout for WorldStateFull data (must match net_recomp_api.cpp):
// 0x00: u32 map_id
// 0x04: u8  level_id
// 0x08: u8[13] jiggy_score
// 0x15: u8[16] mumbo_score
// 0x25: u8[3]  honeycomb_score
// 0x28: u8  jinjo_bits
// 0x2A: u16 note_count
// 0x2C: u8  lives
// Total: 0x2D (45 bytes)

typedef struct {
    u32 map_id;              // 0x00
    u8  level_id;            // 0x04
    u8  _pad[3];             // 0x05-0x07
    u8  jiggy_score[13];     // 0x08
    u8  mumbo_score[16];     // 0x15
    u8  honeycomb_score[3];  // 0x25
    u8  jinjo_bits;          // 0x28
    u8  _pad2;               // 0x29
    u16 note_count;          // 0x2A
    u8  lives;               // 0x2C
    u8  _pad3;               // 0x2D
} WorldStateFullData;        // 0x2E = 46 bytes

// Host: snapshot and send current state when a new player joins
static void check_full_sync_send(void) {
    if (!recomp_net_is_host()) return;

    u8 target_player;
    if (!recomp_net_should_send_full_sync(&target_player)) return;

    WorldStateFullData data;
    data.map_id = (u32)map_get();
    data.level_id = (u8)level_get();

    // Jiggy score
    u8 *js = jiggyscore_getPtr();
    if (js) { s32 i; for (i = 0; i < 13; i++) data.jiggy_score[i] = js[i]; }
    else { s32 i; for (i = 0; i < 13; i++) data.jiggy_score[i] = 0; }

    // Mumbo token score
    u8 *ms = func_80321538();
    if (ms) { s32 i; for (i = 0; i < 16; i++) data.mumbo_score[i] = ms[i]; }
    else { s32 i; for (i = 0; i < 16; i++) data.mumbo_score[i] = 0; }

    // Honeycomb score
    u8 *hs = honeycombscore_get_ptr();
    if (hs) { s32 i; for (i = 0; i < 3; i++) data.honeycomb_score[i] = hs[i]; }
    else { s32 i; for (i = 0; i < 3; i++) data.honeycomb_score[i] = 0; }

    data.jinjo_bits = (u8)item_getCount(ITEM_12_JINJOS);
    data.note_count = (u16)item_getCount(ITEM_C_NOTE);
    data.lives = (u8)item_getCount(ITEM_16_LIFE);

    recomp_net_send_world_state_full(&data, sizeof(data), (u32)target_player);
    recomp_printf("[STATE-SYNC] sent full state to player %d (map=%d)\n", target_player, data.map_id);
}

// Join: apply received full state from host
static void check_full_sync_receive(void) {
    if (recomp_net_is_host()) return;

    WorldStateFullData data;
    if (!recomp_net_pop_full_state(&data)) return;

    recomp_printf("[STATE-SYNC] applying full state from host (map=%d)\n", data.map_id);

    processing_remote = TRUE;

    // Apply jiggy scores
    {
        s32 i;
        for (i = 0; i < 13; i++) {
            s32 bit;
            for (bit = 0; bit < 8; bit++) {
                if (data.jiggy_score[i] & (1 << bit)) {
                    s32 jiggy_id = i * 8 + bit + 1;
                    if (jiggy_id > 0 && jiggy_id < 0x65) {
                        if (!jiggyscore_isCollected(jiggy_id)) {
                            jiggyscore_setCollected(jiggy_id, TRUE);
                            item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, 1);
                        }
                    }
                }
            }
        }
        // Sync prev tracking
        u8 *s = jiggyscore_getPtr();
        if (s) { s32 j; for (j = 0; j < 0xD; j++) prev_jiggyscore[j] = s[j]; }
    }

    // Apply mumbo token scores
    {
        s32 i;
        for (i = 0; i < 16; i++) {
            s32 bit;
            for (bit = 0; bit < 8; bit++) {
                if (data.mumbo_score[i] & (1 << bit)) {
                    s32 token_id = i * 8 + bit + 1;
                    if (!mumboscore_get(token_id)) {
                        mumboscore_set(token_id, TRUE);
                        item_inc(ITEM_1C_MUMBO_TOKEN);
                    }
                }
            }
        }
        u8 *s = func_80321538();
        if (s) { s32 j; for (j = 0; j < 16; j++) prev_mumboscore[j] = s[j]; }
    }

    // Apply honeycomb scores
    {
        s32 i;
        for (i = 0; i < 3; i++) {
            s32 bit;
            for (bit = 0; bit < 8; bit++) {
                if (data.honeycomb_score[i] & (1 << bit)) {
                    s32 hc_id = i * 8 + bit + 1;
                    if (hc_id > 0 && hc_id < 0x19) {
                        if (!honeycombscore_get(hc_id)) {
                            honeycombscore_set(hc_id, TRUE);
                            item_inc(ITEM_13_EMPTY_HONEYCOMB);
                        }
                    }
                }
            }
        }
        u8 *s = honeycombscore_get_ptr();
        if (s) { s32 j; for (j = 0; j < 3; j++) prev_honeycombscore[j] = s[j]; }
    }

    // Apply jinjo bits
    {
        s32 cur = item_getCount(ITEM_12_JINJOS);
        s32 target = (s32)data.jinjo_bits;
        s32 new_bits = target & ~cur;
        if (new_bits > 0) {
            item_adjustByDiffWithHud(ITEM_12_JINJOS, new_bits);
        }
        prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
    }

    // Apply note count
    {
        s32 cur = item_getCount(ITEM_C_NOTE);
        s32 target = (s32)data.note_count;
        if (target > cur) {
            item_adjustByDiffWithoutHud(ITEM_C_NOTE, target - cur);
        }
    }

    // Apply lives
    {
        s32 cur = item_getCount(ITEM_16_LIFE);
        s32 target = (s32)data.lives;
        if (target > cur) {
            item_adjustByDiffWithoutHud(ITEM_16_LIFE, target - cur);
        }
        prev_lives = item_getCount(ITEM_16_LIFE);
    }

    processing_remote = FALSE;
}

// Called every frame
RECOMP_EXPORT void bkrecomp_net_process_world_events(void) {
    if (!recomp_net_is_connected()) return;

    // Full state sync (runs once when new player joins)
    check_full_sync_send();
    check_full_sync_receive();

    poll_shared_collectibles();
    poll_nonshared_collectibles();
    poll_enemy_deaths();
    sync_enemy_positions();

    WorldEventData evt;
    while (recomp_net_pop_world_event(&evt)) {
        if (evt.event_type == EVENT_COLLECTIBLE) {
            process_collectible_event(&evt);
        } else if (evt.event_type == EVENT_ENEMY) {
            process_enemy_event((EnemyEventData*)&evt);
        }
    }
}
