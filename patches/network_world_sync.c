#include "patches.h"
#include "functions.h"
#include "enums.h"

// Network bridge
void recomp_net_send_collectible(u32 type, u32 id, u32 collected, u32 map_id, u32 level_id);
void recomp_net_send_enemy_death(u32 marker_type, u32 spawn_index, u32 map_id, f32 *pos);
u32  recomp_net_pop_world_event(void *out);
u32  recomp_net_is_connected(void);
u32  recomp_net_is_host(void);
u32  recomp_net_am_i_world_owner(u32 level_id);
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

// Flag data access for full state sync
extern void fileProgressFlag_getSizeAndPtr(s32 *size, u8 **addr);
extern struct { s32 unk0; s32 unk4; u8 unk8[0x25]; } gFileProgressFlags;
extern struct { u32 unk0; u32 unk4; u8 unk8[8]; } D_80383320;
extern struct { s32 unk0; s32 unk4; u8 unk8[0x19]; } gVolatileFlags;
extern u32 D_80367000;  // mapSpecificFlags

// Animation control
extern enum asset_e anctrl_getIndex(AnimCtrl *this);
extern f32 anctrl_getAnimTimer(AnimCtrl *this);
extern void anctrl_setIndex(AnimCtrl *this, enum asset_e index);
extern void anctrl_setAnimTimer(AnimCtrl *this, f32 timer);

// Player marker (for triggering enemy death callbacks)
extern ActorMarker *baMarker_get(void);
extern void player_getPosition(f32 pos[3]);

// Jiggy spawn (for jinjo completion)
extern void jiggy_spawn(enum jiggy_e jiggy_id, f32 pos[3]);

// Bundle/item drop system (honeycomb on enemy kill)
extern Actor *__bundle_spawnFromFirstActor(enum bundle_e bundle_id, Actor *actor);
extern Actor *bundle_spawn_f32(enum bundle_e bundle_id, f32 position[3]);
extern void bundle_setYaw(f32 yaw);
extern s32 D_8036E564; // bundle count global, read by bundle system

static void net_enemy_die_proxy(ActorMarker *self_marker, ActorMarker *other_marker);

// Killable enemies have dieFunc set (death handler from marker_setCollisionScripts).
// NPCs like Bottles, platforms, pads do NOT have dieFunc.
static bool is_killable_enemy(Actor *actor) {
    if (!actor || !actor->marker) return FALSE;
    MarkerCollisionFunc df = actor->marker->dieFunc;
    if (!df) return FALSE;
    if (df == (MarkerCollisionFunc)net_enemy_die_proxy) return TRUE;
    return TRUE;
}

// Jiggy actor local ID
extern enum jiggy_e chjiggy_getJiggyId(Actor *this);

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
#define EVENT_FLAG        2

// Flag sync (from network_flag_sync.c)
extern void bkrecomp_net_process_flag_event(void *data);

// Jigsaw sync (from network_jigsaw_sync.c)
extern void bkrecomp_net_jigsaw_check_disconnects(void);
extern void bkrecomp_net_apply_flag_bulk(
    u8 *file_progress, s32 fp_size,
    u8 *level_specific, s32 ls_size,
    u8 *volatile_flags, s32 vf_size,
    u32 map_flags,
    u8 *abilities, s32 ab_size);
extern void ability_getSizeAndPtr(s32 *size, u8 **addr);

static bool processing_remote = FALSE;

// === Enemy tracking ===
#define MAX_TRACKED_ENEMIES 128

// Join: pending kills received before entering the map — applied when map matches
#define MAX_PENDING_KILLS 32
static struct {
    u16 marker_type;
    u16 spawn_index;
    u32 map_id;
} pending_kills[MAX_PENDING_KILLS];
static s32 pending_kill_count = 0;

// World owner: track killed enemies in current level to re-send to late joiners
#define MAX_KILLED_ON_MAP 64
static struct {
    u16 marker_type;
    u16 spawn_index;
    f32 pos_x, pos_y, pos_z;
    u32 map_id;    // Which specific map this enemy was on
} killed_on_map[MAX_KILLED_ON_MAP];
static s32 killed_on_map_count = 0;
static u32 killed_on_level_id = 0xFFFFFFFF;

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

// --- Non-owner saved functions ---
// Saves original callbacks for enemy AI suppression.
// Restored when enemy dies so death animation can play.
#define MAX_SAVED_DIEFUNCS 64
static struct {
    u16 spawn_index;
    MarkerCollisionFunc original_die;
    MarkerCollisionFunc original_collision;
    ActorUpdateFunc original_update;
} saved_diefuncs[MAX_SAVED_DIEFUNCS];
static s32 saved_diefunc_count = 0;

static void net_enemy_hit_proxy(ActorMarker *self_marker, ActorMarker *other_marker);

static void save_enemy_funcs(u16 spawn_index, MarkerCollisionFunc die_func,
                              MarkerCollisionFunc coll_func, ActorUpdateFunc update_func) {
    if (die_func == (MarkerCollisionFunc)net_enemy_die_proxy || !die_func) return;
    s32 i;
    for (i = 0; i < saved_diefunc_count; i++) {
        if (saved_diefuncs[i].spawn_index == spawn_index) return; // already saved
    }
    if (saved_diefunc_count < MAX_SAVED_DIEFUNCS) {
        saved_diefuncs[saved_diefunc_count].spawn_index = spawn_index;
        saved_diefuncs[saved_diefunc_count].original_die = die_func;
        saved_diefuncs[saved_diefunc_count].original_collision = coll_func;
        saved_diefuncs[saved_diefunc_count].original_update = update_func;
        saved_diefunc_count++;
    }
}

static MarkerCollisionFunc get_saved_diefunc(u16 spawn_index) {
    s32 i;
    for (i = 0; i < saved_diefunc_count; i++) {
        if (saved_diefuncs[i].spawn_index == spawn_index)
            return saved_diefuncs[i].original_die;
    }
    return (MarkerCollisionFunc)0;
}

static ActorUpdateFunc get_saved_updatefunc(u16 spawn_index) {
    s32 i;
    for (i = 0; i < saved_diefunc_count; i++) {
        if (saved_diefuncs[i].spawn_index == spawn_index)
            return saved_diefuncs[i].original_update;
    }
    return (ActorUpdateFunc)0;
}

static MarkerCollisionFunc get_saved_collisionfunc(u16 spawn_index) {
    s32 i;
    for (i = 0; i < saved_diefunc_count; i++) {
        if (saved_diefuncs[i].spawn_index == spawn_index)
            return saved_diefuncs[i].original_collision;
    }
    return (MarkerCollisionFunc)0;
}

// Collision hit proxy: when non-owner player hits an enemy, force-kill it.
// Most BK enemies die in 1 hit, so treating any hit as a kill is acceptable.
static void net_enemy_hit_proxy(ActorMarker *self_marker, ActorMarker *other_marker) {
    if (!self_marker) return;

    // Delegate to the die proxy which handles kill tracking + death sequence
    net_enemy_die_proxy(self_marker, other_marker);
}

static void net_enemy_die_proxy(ActorMarker *self_marker, ActorMarker *other_marker) {
    if (!self_marker || !suBaddieActorArray) return;

    // Bounds check on actrArrayIdx before accessing data array
    s32 arr_idx = self_marker->actrArrayIdx;
    if (arr_idx < 0 || arr_idx >= suBaddieActorArray->cnt) {
        // Index out of range — just despawn safely
        marker_despawn(self_marker);
        return;
    }

    Actor *actor = &suBaddieActorArray->data[arr_idx];
    u16 spawn_index = (u16)bkrecomp_get_marker_spawn_index(self_marker);
    u16 marker_id = (u16)self_marker->id;

    f32 pos[3];
    pos[0] = actor->position[0];
    pos[1] = actor->position[1];
    pos[2] = actor->position[2];

    // Track this REAL kill (collision-confirmed, not distance culling)
    if (killed_on_map_count < MAX_KILLED_ON_MAP) {
        s32 ki = killed_on_map_count;
        killed_on_map[ki].marker_type = marker_id;
        killed_on_map[ki].spawn_index = spawn_index;
        killed_on_map[ki].pos_x = pos[0];
        killed_on_map[ki].pos_y = pos[1];
        killed_on_map[ki].pos_z = pos[2];
        killed_on_map[ki].map_id = (u32)map_get();
        killed_on_map_count = ki + 1;
    }

    // Send kill event to all other players immediately (both owner and non-owner)
    recomp_net_send_enemy_death((u32)marker_id, (u32)spawn_index, (u32)map_get(), pos);

    // Stop position override so death animation plays
    mark_dying(spawn_index);

    // Restore ALL original functions so death animation state machine runs
    ActorUpdateFunc upd = get_saved_updatefunc(spawn_index);
    if (upd) {
        self_marker->actorUpdateFunc = upd;
    }
    MarkerCollisionFunc coll = get_saved_collisionfunc(spawn_index);
    if (coll) {
        self_marker->collisionFunc = coll;
    }

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
static u32 prev_enemy_level = 0xFFFFFFFF;  // Track by level_id (stable across sub-areas)
static u32 prev_enemy_map = 0xFFFFFFFF;    // Track map_id to rebuild snapshot on sub-area change

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
extern bool is_note_collected(s32 map_id, s32 level_id, u8 note_index);
extern void set_note_collected(s32 map_id, s32 level_id, u8 note_index);
static u8  prev_honeycombscore[3] = {0};
static s32 prev_lives = 0;
// Debug: track jiggy total changes from ANY source
static s32 dbg_prev_jiggy_total = -1;
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

// Despawn the jiggy actor matching a specific jiggy ID (e.g. JIGGY_A_MM_CONGA).
// Unlike marker-based despawn, this checks each jiggy actor's local ID to find
// the correct one, avoiding despawning unrelated jiggies.
static bool despawn_jiggy_by_id(u16 jiggy_id) {
    if (!suBaddieActorArray) return FALSE;
    s32 i;
    for (i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (!actor->marker || actor->marker->id != MARKER_52_JIGGY) continue;
        if ((u16)chjiggy_getJiggyId(actor) == jiggy_id) {
            marker_despawn(actor->marker);
            return TRUE;
        }
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
    // BK uses unusual bit indexing: byte=(id-1)/8, bit=id&7
    // (bit 0 of each byte = id that is a multiple of 8, NOT id*8+1)
    // Iterate by jiggy_id to match the game's own layout.
    {
        u8 *score = jiggyscore_getPtr();
        if (score) {
            s32 jid;
            for (jid = 1; jid < 0x65; jid++) {
                s32 byte_idx = (jid - 1) / 8;
                u8 bit_mask = 1 << (jid & 7);
                if ((score[byte_idx] & bit_mask) && !(prev_jiggyscore[byte_idx] & bit_mask)) {
                    recomp_net_send_collectible(COLLECTIBLE_JIGGY, (u32)jid, 1, cur_map, cur_level);
                }
            }
            { s32 i; for (i = 0; i < 0xD; i++) prev_jiggyscore[i] = score[i]; }
        }
    }

    // --- Mumbo tokens --- (same bit layout as jiggies: byte=(id-1)/8, bit=id&7)
    {
        u8 *score = func_80321538();
        if (score) {
            s32 tid;
            for (tid = 1; tid < 126; tid++) {
                s32 byte_idx = (tid - 1) / 8;
                u8 bit_mask = 1 << (tid & 7);
                if ((score[byte_idx] & bit_mask) && !(prev_mumboscore[byte_idx] & bit_mask)) {
                    recomp_net_send_collectible(COLLECTIBLE_MUMBO_TOKEN, (u32)tid, 1, cur_map, cur_level);
                }
            }
            { s32 i; for (i = 0; i < 16; i++) prev_mumboscore[i] = score[i]; }
        }
    }

    // Notes: sent directly from note_saving.c (no polling needed)

    // --- Empty honeycombs (panel pieces, 2 per world) ---
    // Same bit layout: byte=(id-1)/8, bit=id&7
    {
        u8 *score = honeycombscore_get_ptr();
        if (score) {
            s32 hid;
            for (hid = 1; hid < 0x19; hid++) {
                s32 byte_idx = (hid - 1) / 8;
                u8 bit_mask = 1 << (hid & 7);
                if ((score[byte_idx] & bit_mask) && !(prev_honeycombscore[byte_idx] & bit_mask)) {
                    recomp_net_send_collectible(COLLECTIBLE_EMPTY_HONEYCOMB, (u32)hid, 1, cur_map, cur_level);
                }
            }
            { s32 i; for (i = 0; i < 3; i++) prev_honeycombscore[i] = score[i]; }
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
    if (!suBaddieActorArray) return;

    u32 cur_map = (u32)map_get();
    u32 cur_level = (u32)level_get();

    // Only world owner polls — others receive via sync_enemy_positions
    if (!recomp_net_am_i_world_owner(cur_level)) return;

    // Reset tracking only on LEVEL change (not sub-area map transitions).
    // level_get() stays constant when entering interiors/sub-areas within the same world.
    if (cur_level != prev_enemy_level) {
        prev_enemy_count = 0;
        prev_enemy_level = cur_level;
        prev_enemy_map = cur_map;
        saved_diefunc_count = 0;
        dying_count = 0;
        killed_on_map_count = 0;
        killed_on_level_id = cur_level;
        // Build initial snapshot without sending events
        s32 count = 0;
        s32 i;
        for (i = 0; i < suBaddieActorArray->cnt && count < MAX_TRACKED_ENEMIES; i++) {
            Actor *actor = &suBaddieActorArray->data[i];
            if (!actor->marker) continue;
            if (!is_killable_enemy(actor)) continue;
            u32 mid = actor->marker->id;
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

    // Sub-area change within same level: rebuild snapshot without sending death events
    // or resetting killed_on_map (enemies killed in the main area stay tracked).
    if (cur_map != prev_enemy_map) {
        prev_enemy_map = cur_map;
        saved_diefunc_count = 0;
        dying_count = 0;
        s32 count = 0;
        s32 i;
        for (i = 0; i < suBaddieActorArray->cnt && count < MAX_TRACKED_ENEMIES; i++) {
            Actor *actor = &suBaddieActorArray->data[i];
            if (!actor->marker) continue;
            if (!is_killable_enemy(actor)) continue;
            prev_enemies[count].marker_id = (u16)actor->marker->id;
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
        if (!is_killable_enemy(actor)) continue;
        u32 mid = actor->marker->id;
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

            // Only track as real kill if enemy was close to player (not distance culling)
            {
                f32 ppos[3];
                player_getPosition(ppos);
                f32 dx = pos[0] - ppos[0];
                f32 dy = pos[1] - ppos[1];
                f32 dz = pos[2] - ppos[2];
                f32 dist_sq = dx*dx + dy*dy + dz*dz;
                // 2000 units radius = likely a real kill, not culling
                if (dist_sq < 2000.0f * 2000.0f) {
                    if (killed_on_map_count < MAX_KILLED_ON_MAP) {
                        s32 ki = killed_on_map_count;
                        killed_on_map[ki].marker_type = prev_enemies[p].marker_id;
                        killed_on_map[ki].spawn_index = prev_enemies[p].spawn_index;
                        killed_on_map[ki].pos_x = pos[0];
                        killed_on_map[ki].pos_y = pos[1];
                        killed_on_map[ki].pos_z = pos[2];
                        killed_on_map[ki].map_id = cur_map;
                        killed_on_map_count = ki + 1;
                    }
                }
            }

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
    u8  sender_id;        // 0x01 — 0xFE = resync (silent), else real-time collect
    u8  _pad[2];          // 0x02-0x03
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
    bool is_resync = (evt->sender_id == 0xFE);

    // Resync: silently apply scores + despawn actors, no HUD effects
    if (is_resync) {
        u32 cur_map = (u32)map_get();

        if (ct == COLLECTIBLE_JINJO) {
            // Apply jinjo bits silently
            s32 cur = item_getCount(ITEM_12_JINJOS);
            s32 new_bits = (s32)evt->coll_id & ~cur;
            if (new_bits > 0) {
                item_adjustByDiffWithoutHud(ITEM_12_JINJOS, new_bits);
            }
            prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
            // Despawn jinjo actor
            if (cur_map == evt->coll_map_id) {
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
                item_adjustByDiffWithoutHud(ITEM_1C_MUMBO_TOKEN, 1);
                { u8 *s = func_80321538(); if (s) { s32 i; for(i=0;i<16;i++) prev_mumboscore[i]=s[i]; } }
            }
            // Despawn mumbo token actors whose uid matches a collected token.
            // Can't use despawn_actor_by_marker_id (despawns wrong one).
            // Must check each actor's local uid against mumboscore bitfield.
            if (cur_map == evt->coll_map_id && suBaddieActorArray) {
                s32 i;
                for (i = 0; i < suBaddieActorArray->cnt; i++) {
                    Actor *actor = &suBaddieActorArray->data[i];
                    if (!actor->marker) continue;
                    if (actor->marker->id != MARKER_39_MUMBO_TOKEN) continue;
                    s32 uid = *(s32*)&actor->local;  // ActorLocal_MumboToken.uid at offset 0
                    if (mumboscore_get(uid)) {
                        marker_despawn(actor->marker);
                    }
                }
            }
        } else if (ct == COLLECTIBLE_JIGGY) {
            if (!jiggyscore_isCollected(evt->coll_id)) {
                recomp_printf("[JIGGY-DEBUG] RESYNC adding jiggy %d (was NOT collected), total before=%d\n",
                    evt->coll_id, item_getCount(ITEM_26_JIGGY_TOTAL));
                jiggyscore_setCollected(evt->coll_id, TRUE);
                item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, 1);
                dbg_prev_jiggy_total = item_getCount(ITEM_26_JIGGY_TOTAL);
                { u8 *s = jiggyscore_getPtr(); if (s) { s32 i; for(i=0;i<0xD;i++) prev_jiggyscore[i]=s[i]; } }
            } else {
                recomp_printf("[JIGGY-DEBUG] RESYNC skip jiggy %d (already collected)\n", evt->coll_id);
            }
            if (cur_map == evt->coll_map_id) {
                despawn_jiggy_by_id(evt->coll_id);
            }
        } else if (ct == COLLECTIBLE_EMPTY_HONEYCOMB) {
            if (!honeycombscore_get(evt->coll_id)) {
                honeycombscore_set(evt->coll_id, TRUE);
                item_adjustByDiffWithoutHud(ITEM_13_EMPTY_HONEYCOMB, 1);
                { u8 *s = honeycombscore_get_ptr(); if (s) { s32 i; for(i=0;i<3;i++) prev_honeycombscore[i]=s[i]; } }
            }
            if (cur_map == evt->coll_map_id) {
                despawn_actor_by_marker_id(MARKER_53_EMPTY_HONEYCOMB);
            }
        } else if (ct == COLLECTIBLE_NOTE) {
            // Notes are per-level — only apply + hide if on the same map
            if (cur_map == evt->coll_map_id) {
                // Only increment counter if note wasn't already collected
                // (prevents host double-counting its own notes via resync)
                if (evt->coll_id != 0xFFFE
                    && !is_note_collected((s32)cur_map, (s32)level_get(), (u8)evt->coll_id)) {
                    item_adjustByDiffWithoutHud(ITEM_C_NOTE, 1);
                }
                if (evt->coll_id == 0xFFFE) {
                    // Dynamic note — no specific index, just despawn actor
                    despawn_actor_by_marker_id(MARKER_5F_MUSIC_NOTE);
                } else {
                    // Static note — hide by index (also marks as collected)
                    bkrecomp_net_hide_note(evt->coll_id);
                }
            }
        } else if (ct == COLLECTIBLE_EXTRA_LIFE) {
            // Don't adjust lives on resync — just despawn
            if (cur_map == evt->coll_map_id) {
                despawn_actor_by_marker_id(MARKER_61_EXTRA_LIFE);
            }
        }

        processing_remote = FALSE;
        return;
    }

    // Real-time collectible event
    // For remote events: only set global bitfields (save file).
    // Don't call item_inc/item_adjustByDiff for level-tracked items — these use
    // level_get() internally and would assign stats to the RECEIVER's current level
    // instead of the SENDER's level. Counters recalculate from bitfields on level entry.
    // Use WithHud only when on the same map (player can see the collection).
    {
        u32 cur_map = (u32)map_get();
        u32 cur_lvl = (u32)level_get();
        bool same_map = (cur_map == evt->coll_map_id);
        bool same_level = (cur_lvl == (u32)evt->coll_level_id);

        if (ct == COLLECTIBLE_JIGGY) {
            if (!jiggyscore_isCollected(evt->coll_id)) {
                recomp_printf("[JIGGY-DEBUG] REALTIME adding jiggy %d from player %d, total before=%d, same_lvl=%d\n",
                    evt->coll_id, evt->sender_id, item_getCount(ITEM_26_JIGGY_TOTAL), same_level);
                jiggyscore_setCollected(evt->coll_id, TRUE);
                // Jiggy total is global — always adjust.
                // Show HUD counter when on same level, silent when different.
                if (same_level) {
                    item_adjustByDiffWithHud(ITEM_26_JIGGY_TOTAL, 1);
                } else {
                    item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, 1);
                }
                dbg_prev_jiggy_total = item_getCount(ITEM_26_JIGGY_TOTAL);
                { u8 *s = jiggyscore_getPtr(); if (s) { s32 i; for (i=0;i<0xD;i++) prev_jiggyscore[i]=s[i]; } }
                if (same_map) {
                    despawn_jiggy_by_id(evt->coll_id);
                }
            }
        } else if (ct == COLLECTIBLE_NOTE) {
            // Notes are per-level, only apply if on the same level.
            // Always mark the bitfield so the note stays collected across map transitions.
            if (same_level && evt->coll_id != 0xFFFE) {
                bool already = is_note_collected((s32)evt->coll_map_id, (s32)evt->coll_level_id, (u8)evt->coll_id);
                if (!already) {
                    set_note_collected((s32)evt->coll_map_id, (s32)evt->coll_level_id, (u8)evt->coll_id);
                    item_inc(ITEM_C_NOTE);
                }
            } else if (same_level && evt->coll_id == 0xFFFE) {
                item_inc(ITEM_C_NOTE);
            }
            // Visual despawn only when on the same map
            if (same_map) {
                if (evt->coll_id == 0xFFFE) {
                    despawn_actor_by_marker_id(MARKER_5F_MUSIC_NOTE);
                } else {
                    bkrecomp_net_hide_note(evt->coll_id);
                }
            }
        } else if (ct == COLLECTIBLE_JINJO) {
            // Jinjos are per-level — ONLY apply if on the same level.
            if (same_level) {
                s32 result = item_adjustByDiffWithHud(ITEM_12_JINJOS, (s32)evt->coll_id);
                prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
                // All 5 jinjos collected (0x1f) — spawn the jinjo jiggy on this side too.
                // The collecting player spawns it via the jinjo actor callback,
                // but remote players need it spawned explicitly here.
                if (result == 0x1f && same_map) {
                    f32 jiggy_pos[3];
                    jiggy_pos[0] = evt->coll_pos_x;
                    jiggy_pos[1] = evt->coll_pos_y + 50.0f;
                    jiggy_pos[2] = evt->coll_pos_z;
                    jiggy_spawn(10 * (s32)level_get() - 9, jiggy_pos);
                }
            }
            if (same_map) {
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
            // Mumbo tokens: global bitfield + counter always (not per-level)
            if (!mumboscore_get(evt->coll_id)) {
                mumboscore_set(evt->coll_id, TRUE);
                if (same_level) {
                    item_inc(ITEM_1C_MUMBO_TOKEN);
                } else {
                    item_adjustByDiffWithoutHud(ITEM_1C_MUMBO_TOKEN, 1);
                }
                { u8 *s = func_80321538(); if (s) { s32 i; for (i=0;i<16;i++) prev_mumboscore[i]=s[i]; } }
                if (same_map && suBaddieActorArray) {
                    s32 i;
                    for (i = 0; i < suBaddieActorArray->cnt; i++) {
                        Actor *actor = &suBaddieActorArray->data[i];
                        if (!actor->marker) continue;
                        if (actor->marker->id != MARKER_39_MUMBO_TOKEN) continue;
                        s32 uid = *(s32*)&actor->local;
                        if (uid == (s32)evt->coll_id) {
                            marker_despawn(actor->marker);
                            break;
                        }
                    }
                }
            }
        } else if (ct == COLLECTIBLE_DESPAWN_ONLY) {
            if (same_map) {
                bkrecomp_net_hide_nearest_prop(evt->coll_id,
                    evt->coll_pos_x, evt->coll_pos_y, evt->coll_pos_z);
            }
        } else if (ct == COLLECTIBLE_EMPTY_HONEYCOMB) {
            // Empty honeycombs: global bitfield + counter always (not per-level)
            if (!honeycombscore_get(evt->coll_id)) {
                honeycombscore_set(evt->coll_id, TRUE);
                if (same_level) {
                    item_inc(ITEM_13_EMPTY_HONEYCOMB);
                } else {
                    item_adjustByDiffWithoutHud(ITEM_13_EMPTY_HONEYCOMB, 1);
                }
                { u8 *s = honeycombscore_get_ptr(); if (s) { s32 i; for(i=0;i<3;i++) prev_honeycombscore[i]=s[i]; } }
                if (same_map && suBaddieActorArray) {
                    s32 i;
                    for (i = 0; i < suBaddieActorArray->cnt; i++) {
                        Actor *actor = &suBaddieActorArray->data[i];
                        if (!actor->marker) continue;
                        if (actor->marker->id != MARKER_53_EMPTY_HONEYCOMB) continue;
                        s32 uid = *(s32*)&actor->local;
                        if (uid == (s32)evt->coll_id) {
                            marker_despawn(actor->marker);
                            break;
                        }
                    }
                }
            }
        } else if (ct == COLLECTIBLE_EXTRA_LIFE) {
            item_inc(ITEM_16_LIFE);
            prev_lives = item_getCount(ITEM_16_LIFE);
            if (same_map) {
                despawn_actor_by_marker_id(MARKER_61_EXTRA_LIFE);
                bkrecomp_net_hide_nearest_prop(0, evt->coll_pos_x, evt->coll_pos_y, evt->coll_pos_z);
            }
        }
    }

    processing_remote = FALSE;
}

// Enemy event layout (matches net_recomp_api.cpp ENEMY case output):
typedef struct {
    u8  event_type;        // 0x00
    u8  sender_id;         // 0x01 — 0xFF = resync (silent kill, no animation/honeycomb)
    u8  _pad[2];           // 0x02-0x03
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
        // Save for later — join might not be on this map yet
        if (pending_kill_count < MAX_PENDING_KILLS) {
            pending_kills[pending_kill_count].marker_type = evt->enemy_marker_type;
            pending_kills[pending_kill_count].spawn_index = evt->enemy_spawn_index;
            pending_kills[pending_kill_count].map_id = evt->enemy_map_id;
            pending_kill_count++;
            recomp_printf("[ENEMY-EVT] saved pending kill: marker=0x%X spawn=%d map=%d (cur=%d)\n",
                evt->enemy_marker_type, evt->enemy_spawn_index, evt->enemy_map_id, cur_map);
        }
        processing_remote = FALSE;
        return;
    }

    u16 target_marker = evt->enemy_marker_type;
    u16 target_spawn = evt->enemy_spawn_index;
    bool is_resync = (evt->sender_id == 0xFF);

    if (suBaddieActorArray) {
        s32 i;
        for (i = 0; i < suBaddieActorArray->cnt; i++) {
            Actor *actor = &suBaddieActorArray->data[i];
            if (!actor->marker) continue;
            if (actor->marker->id != target_marker) continue;
            u16 si = (u16)bkrecomp_get_marker_spawn_index(actor->marker);
            if (si != target_spawn) continue;

            if (is_resync) {
                // Silent despawn — enemy was already killed, just remove on re-entry
                marker_despawn(actor->marker);
            } else {
                // Real-time kill: honeycomb + death animation
                {
                    f32 drop_pos[3];
                    drop_pos[0] = actor->position[0];
                    drop_pos[1] = actor->position[1] + 50.0f;
                    drop_pos[2] = actor->position[2];
                    bundle_setYaw(actor->yaw);
                    D_8036E564 = 1;
                    bundle_spawn_f32(BUNDLE_14__HONEYCOMB, drop_pos);
                }

                mark_dying(target_spawn);

                MarkerCollisionFunc die = actor->marker->dieFunc;
                if (die == (MarkerCollisionFunc)net_enemy_die_proxy) {
                    die = get_saved_diefunc(target_spawn);
                }
                if (die) {
                    actor->marker->dieFunc = die;
                    die(actor->marker, baMarker_get());
                } else {
                    marker_despawn(actor->marker);
                }
            }
            break;
        }
    }
    // If world owner: remove from prev_enemies so poll doesn't re-detect
    if (recomp_net_am_i_world_owner((u32)level_get())) {
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
    u32 cur_level = (u32)level_get();

    if (recomp_net_am_i_world_owner(cur_level)) {
        // WORLD OWNER: collect enemy positions and send to network
        EnemyPosEntry buf[MAX_ENEMY_POS_ENTRIES];
        s32 count = 0;
        s32 i;

        for (i = 0; i < suBaddieActorArray->cnt && count < MAX_ENEMY_POS_ENTRIES; i++) {
            Actor *actor = &suBaddieActorArray->data[i];
            if (!actor->marker) continue;
            if (!is_killable_enemy(actor)) continue;

            u16 si = (u16)bkrecomp_get_marker_spawn_index(actor->marker);

            // Install dieFunc proxy on owner too — sends death event immediately
            // when enemy dies, instead of waiting for poll_enemy_deaths detection.
            // AI is NOT suppressed on owner side.
            if (actor->marker->dieFunc && actor->marker->dieFunc != (MarkerCollisionFunc)net_enemy_die_proxy) {
                save_enemy_funcs(si, actor->marker->dieFunc,
                                 actor->marker->collisionFunc, actor->marker->actorUpdateFunc);
                actor->marker->dieFunc = (MarkerCollisionFunc)net_enemy_die_proxy;
            }

            buf[count].spawn_index = si;
            buf[count].marker_id = (u16)actor->marker->id;
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

        s32 e;
        for (e = 0; e < (s32)count; e++) {
            u16 target_spawn = buf[e].spawn_index;
            u16 target_marker = buf[e].marker_id;

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

                // Install dieFunc proxy to track kills from non-owner side.
                // AI is NOT suppressed — enemy processes damage and dies naturally.
                // Position + animation are overridden from host data each frame.
                if (actor->marker->dieFunc && actor->marker->dieFunc != (MarkerCollisionFunc)net_enemy_die_proxy) {
                    save_enemy_funcs(target_spawn, actor->marker->dieFunc,
                                     actor->marker->collisionFunc, actor->marker->actorUpdateFunc);
                    actor->marker->dieFunc = (MarkerCollisionFunc)net_enemy_die_proxy;
                }

                break;
            }
        }

        // Dead enemies are handled via death events re-sent by host on join connect.
        // process_enemy_event() despawns them individually — no aggressive bulk despawn needed.
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
    // Flag sync data (Phase 8)
    u8  file_progress_flags[37]; // 0x2E (0x25 bytes) -> ends at 0x53
    u8  level_specific_flags[8]; // 0x53 -> ends at 0x5B
    u8  volatile_flags[25];      // 0x5B (0x19 bytes) -> ends at 0x74
    u32 map_specific_flags;      // 0x74 (4-byte aligned)
    u8  has_flags;               // 0x78 (1 if flag data present)
    u8  _pad4[3];                // 0x79
    u8  abilities[8];            // 0x7C (learnedAbilities + usedAbilities)
} WorldStateFullData;            // 0x84 = 132 bytes

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

    // Jinjos and notes are per-level — DON'T include in global full sync.
    // They sync via centralized collectible tracking per-level.
    data.jinjo_bits = 0;  // per-level, handled by collectible resync
    data.note_count = 0;  // per-level, handled by collectible resync
    data.lives = (u8)item_getCount(ITEM_16_LIFE);

    // Flag state
    {
        s32 i;
        for (i = 0; i < 37; i++) data.file_progress_flags[i] = gFileProgressFlags.unk8[i];
        for (i = 0; i < 8; i++) data.level_specific_flags[i] = D_80383320.unk8[i];
        for (i = 0; i < 25; i++) data.volatile_flags[i] = gVolatileFlags.unk8[i];
        data.map_specific_flags = D_80367000;
        data.has_flags = 1;
    }

    // Abilities (learned + used, 8 bytes)
    {
        s32 ab_sz;
        u8 *ab_ptr;
        ability_getSizeAndPtr(&ab_sz, &ab_ptr);
        s32 i;
        for (i = 0; i < 8; i++) data.abilities[i] = ab_ptr[i];
    }

    recomp_net_send_world_state_full(&data, sizeof(data), (u32)target_player);
    recomp_printf("[STATE-SYNC] sent full state to player %d (map=%d, flags=yes)\n",
        target_player, data.map_id);
    // Killed enemies are now sent from C++ centralized tracking (level_kills_)
}

// Join: apply received full state from host
static void check_full_sync_receive(void) {
    if (recomp_net_is_host()) return;

    WorldStateFullData data;
    if (!recomp_net_pop_full_state(&data)) return;

    recomp_printf("[STATE-SYNC] applying full state from host (map=%d)\n", data.map_id);

    processing_remote = TRUE;

    // Apply jiggy scores — use game's bit layout: byte=(id-1)/8, bit=id&7
    {
        s32 jid;
        for (jid = 1; jid < 0x65; jid++) {
            s32 byte_idx = (jid - 1) / 8;
            u8 bit_mask = 1 << (jid & 7);
            if (data.jiggy_score[byte_idx] & bit_mask) {
                if (!jiggyscore_isCollected(jid)) {
                    jiggyscore_setCollected(jid, TRUE);
                    item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, 1);
                    dbg_prev_jiggy_total = item_getCount(ITEM_26_JIGGY_TOTAL);
                }
            }
        }
        // Sync prev tracking
        u8 *s = jiggyscore_getPtr();
        if (s) { s32 j; for (j = 0; j < 0xD; j++) prev_jiggyscore[j] = s[j]; }
    }

    // Apply mumbo token scores — use game's bit layout: byte=(id-1)/8, bit=id&7
    {
        s32 tid;
        for (tid = 1; tid < 126; tid++) {
            s32 byte_idx = (tid - 1) / 8;
            u8 bit_mask = 1 << (tid & 7);
            if (data.mumbo_score[byte_idx] & bit_mask) {
                if (!mumboscore_get(tid)) {
                    mumboscore_set(tid, TRUE);
                    item_inc(ITEM_1C_MUMBO_TOKEN);
                }
            }
        }
        u8 *s = func_80321538();
        if (s) { s32 j; for (j = 0; j < 16; j++) prev_mumboscore[j] = s[j]; }
    }

    // Apply honeycomb scores — use game's bit layout: byte=(id-1)/8, bit=id&7
    {
        s32 hid;
        for (hid = 1; hid < 0x19; hid++) {
            s32 byte_idx = (hid - 1) / 8;
            u8 bit_mask = 1 << (hid & 7);
            if (data.honeycomb_score[byte_idx] & bit_mask) {
                if (!honeycombscore_get(hid)) {
                    honeycombscore_set(hid, TRUE);
                    item_inc(ITEM_13_EMPTY_HONEYCOMB);
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

    // Apply flag state + abilities from host
    if (data.has_flags) {
        bkrecomp_net_apply_flag_bulk(
            data.file_progress_flags, 37,
            data.level_specific_flags, 8,
            data.volatile_flags, 25,
            data.map_specific_flags,
            data.abilities, 8);
        recomp_printf("[STATE-SYNC] applied flag state + abilities from host\n");
    }

    // Re-snapshot ALL polling state AFTER full sync.
    // The game engine may react to bulk flags by auto-setting score bitfields
    // (e.g., progression flags imply certain jiggies collected). Without this,
    // the poll would detect these engine-generated bits as "new" and re-send them,
    // causing duplicate items on the host.
    {
        u8 *js = jiggyscore_getPtr();
        if (js) { s32 i; for (i = 0; i < 0xD; i++) prev_jiggyscore[i] = js[i]; }
        u8 *ms = func_80321538();
        if (ms) { s32 i; for (i = 0; i < 16; i++) prev_mumboscore[i] = ms[i]; }
        u8 *hs = honeycombscore_get_ptr();
        if (hs) { s32 i; for (i = 0; i < 3; i++) prev_honeycombscore[i] = hs[i]; }
        prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
        prev_lives = item_getCount(ITEM_16_LIFE);
        dbg_prev_jiggy_total = item_getCount(ITEM_26_JIGGY_TOTAL);
    }

    processing_remote = FALSE;
}

// Ownership transfer send/receive REMOVED — centralized kill tracking in C++ handles this.

// Track map/level changes for ALL players to reset cached state
static u32 prev_global_level = 0xFFFFFFFF;
static u32 prev_global_map = 0xFFFFFFFF;

// Called every frame
RECOMP_EXPORT void bkrecomp_net_process_world_events(void) {
    if (!recomp_net_is_connected()) return;

    // Detect any JIGGY_TOTAL change (including from game engine itself)
    {
        s32 cur_total = item_getCount(ITEM_26_JIGGY_TOTAL);
        if (dbg_prev_jiggy_total >= 0 && cur_total != dbg_prev_jiggy_total) {
            recomp_printf("[JIGGY-DEBUG] *** TOTAL CHANGED %d -> %d (outside net code!) ***\n",
                dbg_prev_jiggy_total, cur_total);
        }
        dbg_prev_jiggy_total = cur_total;
    }

    // Reset cached enemy functions on ANY map or level change (ALL players).
    // Enemies are map-specific — when map changes, all saved function pointers
    // become stale and must be cleared to prevent bus errors.
    // CRITICAL: Skip all processing on the transition frame — game data structures
    // (actors, markers, score pointers) may be in an inconsistent/stale state
    // during the loading frame, causing bus errors on access.
    {
        u32 cur_level = (u32)level_get();
        u32 cur_map = (u32)map_get();
        if (cur_level != prev_global_level || cur_map != prev_global_map) {
            recomp_printf("[NET] Level/map change: level %d->%d map %d->%d, JIGGY_TOTAL=%d\n",
                prev_global_level, cur_level, prev_global_map, cur_map, item_getCount(ITEM_26_JIGGY_TOTAL));
            saved_diefunc_count = 0;
            dying_count = 0;
            prev_global_level = cur_level;
            prev_global_map = cur_map;
            // Reset per-level polling state to match game's level reset
            prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
            // Skip the rest of this frame — let the game finish loading first
            return;
        }
    }

    // Kill tracking is now centralized in C++ — no MIPS-side transfer/resync needed.

    // Full state sync (runs once when new player joins)
    check_full_sync_send();
    check_full_sync_receive();

    // Process pending kills for current map (from late-join sync).
    // Remove entries once successfully applied to prevent repeated despawn attempts.
    if (pending_kill_count > 0 && suBaddieActorArray) {
        u32 cur_map = (u32)map_get();
        s32 k = 0;
        while (k < pending_kill_count) {
            if (pending_kills[k].map_id != cur_map) {
                k++;
                continue;
            }

            bool applied = FALSE;
            s32 i;
            for (i = 0; i < suBaddieActorArray->cnt; i++) {
                Actor *actor = &suBaddieActorArray->data[i];
                if (!actor->marker) continue;
                if (actor->marker->id != pending_kills[k].marker_type) continue;
                u16 si = (u16)bkrecomp_get_marker_spawn_index(actor->marker);
                if (si != pending_kills[k].spawn_index) continue;

                marker_despawn(actor->marker);
                applied = TRUE;
                break;
            }

            if (applied) {
                // Remove this entry by swapping with last
                pending_kills[k].marker_type = pending_kills[pending_kill_count - 1].marker_type;
                pending_kills[k].spawn_index = pending_kills[pending_kill_count - 1].spawn_index;
                pending_kills[k].map_id = pending_kills[pending_kill_count - 1].map_id;
                pending_kill_count--;
                // Don't increment k — check the swapped entry
            } else {
                k++;
            }
        }
    }

    // Jigsaw puzzle disconnect detection
    bkrecomp_net_jigsaw_check_disconnects();

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
        } else if (evt.event_type == EVENT_FLAG) {
            bkrecomp_net_process_flag_event(&evt);
        }
    }
}
