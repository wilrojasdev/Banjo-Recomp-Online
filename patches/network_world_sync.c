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

// Some enemies run their own custom sync module instead of the generic
// enemy sync below. The generic die-proxy treats every dieFunc call as a
// one-shot kill AND spawns a honeycomb bundle on remote, neither of which
// is correct for:
//   - Nipper (multi-hit boss, 120->80->40->dead lifetime progression)
//   - Buried Treasure (treasure-hunt chest — jiggy already has its own
//     spawn path in __chTreasure_die, and honeycomb-on-remote is wrong)
extern bool bkrecomp_net_is_nipper_marker(u32 marker_id);

// Killable enemies have dieFunc set (death handler from marker_setCollisionScripts).
// NPCs like Bottles, platforms, pads do NOT have dieFunc.
static bool is_killable_enemy(Actor *actor) {
    if (!actor || !actor->marker) return FALSE;
    MarkerCollisionFunc df = actor->marker->dieFunc;
    if (!df) return FALSE;
    // Exclude enemies that run their own custom sync module.
    u32 mid = (u32)actor->marker->id;
    if (bkrecomp_net_is_nipper_marker(mid)) return FALSE;
    if (mid == MARKER_DB_BURIED_TREASURE) return FALSE;
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
#define COLLECTIBLE_GOLD_BULLION     7  // Shared: TTC gold bullions for Blubber quest

#define EVENT_COLLECTIBLE 0
#define EVENT_ENEMY       1
#define EVENT_FLAG        2

// Flag sync (from network_flag_sync.c)
extern void bkrecomp_net_process_flag_event(void *data);

// Save override (from network_save_override.c)
extern void bkrecomp_net_save_override_tick(void);

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

// Join: pending kills received before entering the map — applied when map matches.
// Capacity raised from 32 to 256 after observing silent drops in heavy-combat levels
// (e.g., Gobi/Clanker) where >32 enemies could die before a late joiner enters the map.
// Overflows now log a warning instead of failing silently.
#define MAX_PENDING_KILLS 256
static struct {
    u16 marker_type;
    u16 spawn_index;
    u32 map_id;
} pending_kills[MAX_PENDING_KILLS];
static s32 pending_kill_count = 0;
static u32 pending_kills_dropped = 0;  // Cumulative count of overflow drops

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

// --- Recent-kill dedup: prevents duplicate honeycomb drops when the same
//     (marker, spawn, map) arrives multiple times (rejoin resync overlapping
//     with a live kill, post-transfer re-broadcast, etc.). Cleared on level change.
#define MAX_RECENT_KILLS 128
static struct {
    u16 marker_type;
    u16 spawn_index;
    u32 map_id;
} recent_kills[MAX_RECENT_KILLS];
static s32 recent_kill_count = 0;
static u32 recent_kill_level = 0xFFFFFFFF;

static bool is_recent_kill(u16 marker, u16 spawn, u32 map) {
    s32 i;
    for (i = 0; i < recent_kill_count; i++) {
        if (recent_kills[i].marker_type == marker &&
            recent_kills[i].spawn_index == spawn &&
            recent_kills[i].map_id == map) return TRUE;
    }
    return FALSE;
}

static void remember_kill(u16 marker, u16 spawn, u32 map) {
    if (is_recent_kill(marker, spawn, map)) return;
    if (recent_kill_count < MAX_RECENT_KILLS) {
        s32 ki = recent_kill_count;
        recent_kills[ki].marker_type = marker;
        recent_kills[ki].spawn_index = spawn;
        recent_kills[ki].map_id = map;
        recent_kill_count = ki + 1;
    } else {
        // Ring: drop oldest, shift down, append at end
        s32 i;
        for (i = 1; i < MAX_RECENT_KILLS; i++) {
            recent_kills[i - 1].marker_type = recent_kills[i].marker_type;
            recent_kills[i - 1].spawn_index = recent_kills[i].spawn_index;
            recent_kills[i - 1].map_id = recent_kills[i].map_id;
        }
        recent_kills[MAX_RECENT_KILLS - 1].marker_type = marker;
        recent_kills[MAX_RECENT_KILLS - 1].spawn_index = spawn;
        recent_kills[MAX_RECENT_KILLS - 1].map_id = map;
    }
}

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
extern s32  get_collected_note_count(enum level_e level);
extern s32  level_id_to_level_array_index(enum level_e level_id);

// Persistent note state for cross-world sync:
// - D_80385FF0[level] is the per-level high-score array summed by
//   itemscore_noteScores_getTotal() (the note-door gate).
// - loaded_file_extension_data.level_notes[idx] is BK64's custom bitfield of
//   which note indices have been collected (persists notes across map entries).
// Both must be merged from remote events so players in different worlds
// accumulate shared note progress.
extern u8 D_80385FF0[0xB];
#include "save_extension.h"

// Recompute D_80385FF0[level] from the level_notes bitfield after a remote
// note was added, applying MAX-merge so the counter never regresses.
static void bump_level_notescore(s32 level_id) {
    if (level_id < 1 || level_id > 0xA) return;
    s32 arr_idx = level_id_to_level_array_index((enum level_e)level_id);
    if (arr_idx < 0) return;
    s32 count = get_collected_note_count((enum level_e)level_id);
    if (count > (s32)D_80385FF0[level_id]) {
        D_80385FF0[level_id] = (u8)count;
    }
}

// Forward decl: poll_collected_jinjos() below calls despawn_actor_by_marker_id()
// which is defined further down in the file.
static bool despawn_actor_by_marker_id(u32 marker_id);

// === Persistent per-level jinjo bitfield ===
// Stored in loaded_file_extension_data.jinjos_collected[level_idx] so the
// state survives death, world re-entry, and save/load. Shared across all
// players in a session — once anyone collects a jinjo, it stays collected
// for everyone. Bits 0..4 = Blue/Green/Orange/Pink/Yellow.
static u8 get_jinjo_flags_for_level(s32 level_id) {
    s32 arr_idx = level_id_to_level_array_index((enum level_e)level_id);
    if (arr_idx < 0 || arr_idx >= 9) return 0;
    return loaded_file_extension_data.jinjos_collected[arr_idx];
}

static void mark_jinjo_collected(s32 level_id, u8 bit_mask) {
    s32 arr_idx = level_id_to_level_array_index((enum level_e)level_id);
    if (arr_idx < 0 || arr_idx >= 9) return;
    loaded_file_extension_data.jinjos_collected[arr_idx] |= bit_mask;
}

static u32 jinjo_bit_to_marker_id(u8 bit) {
    if (bit & 0x01) return MARKER_5A_JINJO_BLUE;
    if (bit & 0x02) return MARKER_5B_JINJO_GREEN;
    if (bit & 0x04) return MARKER_5C_JINJO_ORANGE;
    if (bit & 0x08) return MARKER_5D_JINJO_PINK;
    if (bit & 0x10) return MARKER_5E_JINJO_YELLOW;
    return 0;
}

// Per-frame reconciliation between the persistent jinjo bitfield (source
// of truth) and the vanilla ITEM_12_JINJOS counter + live actors. Three
// jobs:
//   1) If the vanilla counter is out of sync with the bitfield (typical
//      after death, which resets the counter but NOT the bitfield), top
//      it up silently so the HUD matches shared progress.
//   2) Despawn any jinjo actor whose bit is already set in the bitfield
//      — catches the vanilla death-respawn that would otherwise let the
//      dead player "collect" an already-shared jinjo.
//   3) Migrate legacy saves: if the per-world jinjo jiggy (id =
//      10*level-9) is already in jiggyscore and the bitfield is empty,
//      pre-mark all 5 bits so the engine doesn't respawn them for a
//      first-time online session on a save that pre-dates this feature.
static s32 jinjo_despawn_last_level = -1;
static void poll_collected_jinjos(void) {
    if (!recomp_net_is_connected() || processing_remote) return;

    s32 cur_level = (s32)level_get();
    if (cur_level < 1 || cur_level > 0xA) return;

    // Migration (once per level entry): if the jinjo-completion jiggy is
    // already collected but the bitfield is empty, seed it to 0x1F.
    if (cur_level != jinjo_despawn_last_level) {
        jinjo_despawn_last_level = cur_level;
        u8 flags = get_jinjo_flags_for_level(cur_level);
        if (flags == 0) {
            s32 jinjo_jiggy_id = 10 * cur_level - 9;
            if (jinjo_jiggy_id > 0 && jinjo_jiggy_id < 0x65 &&
                jiggyscore_isCollected(jinjo_jiggy_id)) {
                mark_jinjo_collected(cur_level, 0x1F);
            }
        }
    }

    u8 collected = get_jinjo_flags_for_level(cur_level);
    if (collected == 0) return;

    // Top up the vanilla counter to match the bitfield (no HUD).
    s32 cur = item_getCount(ITEM_12_JINJOS);
    s32 diff = (s32)collected & ~cur;
    if (diff > 0) {
        processing_remote = TRUE;  // suppress the poll from re-broadcasting
        item_adjustByDiffWithoutHud(ITEM_12_JINJOS, diff);
        prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
        processing_remote = FALSE;
    }

    // Despawn any already-collected jinjo actor still alive on this map.
    s32 b;
    for (b = 0; b < 5; b++) {
        u8 bit = (u8)(1 << b);
        if (collected & bit) {
            u32 marker_id = jinjo_bit_to_marker_id(bit);
            if (marker_id) despawn_actor_by_marker_id(marker_id);
        }
    }
}
static u8  prev_honeycombscore[3] = {0};
static s32 prev_lives = 0;
// Debug: track jiggy total changes from ANY source
static s32 dbg_prev_jiggy_total = -1;
// Non-shared
static s32 prev_eggs = 0;
static s32 prev_red_feathers = 0;
static s32 prev_gold_feathers = 0;
static s32 prev_health = 0;
// Shared (TTC Blubber quest)
static s32 prev_bullions = 0;
// Previous-frame snapshot of MARKER_37_GOLD_BULLION actor spawn_indexes,
// used to identify which specific bullion disappeared (= was picked up
// locally) in the current frame so we can tag the broadcast with the
// correct spawn_index. Up to 4 slots — there are only 2 bullions in
// TTC, but leave headroom.
#define MAX_BULLION_SNAPSHOT 4
static u16 prev_bullion_snapshot[MAX_BULLION_SNAPSHOT];
static s32 prev_bullion_snapshot_count = 0;

// Blubber sync module owns the persistent picked_bullion_mask across
// sub-map transitions. network_world_sync.c updates it on pickup, and
// ships the full mask inside WorldStateFullData on late-join so a
// fresh peer does not see already-collected bullions respawn.
extern void bkrecomp_net_blubber_mark_bullion_picked(u32 spawn_index);
extern u32  bkrecomp_net_blubber_get_picked_bullion_mask(void);
extern void bkrecomp_net_blubber_apply_picked_bullion_mask(u32 mask);

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

// Despawn the actor matching marker_id AND spawn_index. Used when we
// need to target a specific instance (two gold bullions in TTC share
// the same marker id, differ only by spawn order).
static bool despawn_actor_by_marker_and_spawn(u32 marker_id, u32 spawn_index) {
    if (!suBaddieActorArray) return FALSE;
    s32 i;
    for (i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (!actor->marker) continue;
        if (actor->marker->id != marker_id) continue;
        if (bkrecomp_get_marker_spawn_index(actor->marker) != spawn_index) continue;
        marker_despawn(actor->marker);
        return TRUE;
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
                // Persist to bitfield BEFORE broadcasting so death/exit
                // can't lose the state. The persistent flags are the
                // source of truth; ITEM_12_JINJOS is just the vanilla
                // per-run counter that resets on respawn.
                mark_jinjo_collected((s32)cur_level, (u8)new_bits);
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

    // --- Gold bullions (TTC Blubber quest) ---
    // Shared: any player picking one up spawns the bullion in every
    // inventory, and the actor/prop despawns for everyone. To support
    // cross-sub-map despawn (if P2 is inside MAP_5 when P1 picks up,
    // the bullion must still be gone when P2 steps back onto MAP_7),
    // we identify the picked bullion by its spawn_index and carry it
    // in the packet's coll_id. The spawn_index is recovered by
    // diffing a per-frame snapshot of MARKER_37 actors against the
    // previous frame's snapshot.
    {
        // Build current-frame snapshot of *still-live* gold-bullion
        // actors. marker_despawn is deferred when D_8036E574 is set:
        // it marks despawn_flag=1 but leaves the actor in the array
        // until the next cleanup pass. We MUST treat flagged actors
        // as already gone, otherwise the same-frame pickup diff fails
        // to identify which spawn_index disappeared (the bullion is
        // still in the array, so the pickup is broadcast with the
        // 0xFFFF sentinel and remote peers cannot update their mask).
        u16 cur_snapshot[MAX_BULLION_SNAPSHOT];
        s32 cur_snapshot_count = 0;
        if (suBaddieActorArray) {
            s32 j;
            for (j = 0; j < suBaddieActorArray->cnt && cur_snapshot_count < MAX_BULLION_SNAPSHOT; j++) {
                Actor *actor = &suBaddieActorArray->data[j];
                if (!actor->marker) continue;
                if (actor->marker->id != MARKER_37_GOLD_BULLION) continue;
                if (actor->despawn_flag) continue;
                cur_snapshot[cur_snapshot_count++] =
                    (u16)bkrecomp_get_marker_spawn_index(actor->marker);
            }
        }

        s32 cur = item_getCount(ITEM_18_GOLD_BULLIONS);
        if (cur == prev_bullions + 1) {
            // Find which spawn_index was in the previous snapshot but
            // missing from the current one — that is the bullion the
            // local player just picked up.
            u16 picked_si = 0xFFFF;
            s32 pi;
            for (pi = 0; pi < prev_bullion_snapshot_count; pi++) {
                bool still_present = FALSE;
                s32 ci;
                for (ci = 0; ci < cur_snapshot_count; ci++) {
                    if (prev_bullion_snapshot[pi] == cur_snapshot[ci]) {
                        still_present = TRUE;
                        break;
                    }
                }
                if (!still_present) {
                    picked_si = prev_bullion_snapshot[pi];
                    break;
                }
            }

            if (picked_si != 0xFFFF) {
                bkrecomp_net_blubber_mark_bullion_picked((u32)picked_si);
            }
            recomp_net_send_collectible(COLLECTIBLE_GOLD_BULLION,
                (u32)picked_si, 1, cur_map, cur_level);
        }
        prev_bullions = cur;

        // Save current snapshot for next frame's diff. Unroll the
        // copy to a fixed 4-slot assignment so clang -O2 does NOT
        // fold it into a memcpy intrinsic (N64Recomp can't resolve
        // that call at link time).
        prev_bullion_snapshot[0] = (cur_snapshot_count > 0) ? cur_snapshot[0] : 0;
        prev_bullion_snapshot[1] = (cur_snapshot_count > 1) ? cur_snapshot[1] : 0;
        prev_bullion_snapshot[2] = (cur_snapshot_count > 2) ? cur_snapshot[2] : 0;
        prev_bullion_snapshot[3] = (cur_snapshot_count > 3) ? cur_snapshot[3] : 0;
        prev_bullion_snapshot_count = cur_snapshot_count;
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

extern s32 bs_getState(void);

// Death-suppression window: when the owner dies, BK unloads
// suBaddieActorArray as part of the death / respawn transition. Without
// suppression, poll_enemy_deaths misinterprets every vanished enemy as a
// real kill and broadcasts it, causing remote players to see enemies
// mass-despawn with honeycomb drops.
//
// We detect the owner's death via bs_state — the Banjo state machine
// enters a DIE state the moment the death animation starts, well before
// the vanilla life counter decrements (which was our previous, too-late
// signal). While any DIE state is active we hold the suppression counter
// at its max; once it clears we keep suppressing for ~2s more so the
// respawn warp/cutscene finishes before polling resumes.
static s32 death_suppress_frames = 0;
#define DEATH_SUPPRESS_FRAMES 180

static bool owner_is_dying(void) {
    s32 bs = bs_getState();
    switch (bs) {
        case BS_41_DIE:
        case BS_43_ANT_DIE:
        case BS_4E_PUMPKIN_DIE:
        case BS_54_SWIM_DIE:
        case BS_CROC_DIE:
        case BS_WALRUS_DIE:
        case BS_BEE_DIE:
            return TRUE;
        default:
            return FALSE;
    }
}

// Snapshot of alive-enemy positions taken the frame the owner enters a
// DIE state. While the suppression window is active we keep restoring
// actor->position from this snapshot so sync_enemy_positions broadcasts
// the pre-death coordinates — otherwise the engine's respawn routine
// resets every enemy to its initial spawn location, and those initial
// coordinates get sent to remote players on the very next broadcast,
// causing a visual snap on the join side.
static TrackedEnemy pre_death_snapshot[MAX_TRACKED_ENEMIES];
static s32 pre_death_count = 0;
static bool pre_death_captured = FALSE;

static void capture_pre_death_snapshot(void) {
    if (pre_death_captured) return;
    if (!suBaddieActorArray) return;
    s32 count = 0;
    s32 i;
    for (i = 0; i < suBaddieActorArray->cnt && count < MAX_TRACKED_ENEMIES; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (!actor->marker) continue;
        if (!is_killable_enemy(actor)) continue;
        pre_death_snapshot[count].marker_id   = (u16)actor->marker->id;
        pre_death_snapshot[count].spawn_index = (u16)bkrecomp_get_marker_spawn_index(actor->marker);
        pre_death_snapshot[count].pos_x = actor->position[0];
        pre_death_snapshot[count].pos_y = actor->position[1];
        pre_death_snapshot[count].pos_z = actor->position[2];
        count++;
    }
    pre_death_count = count;
    pre_death_captured = TRUE;
    recomp_printf("[DEATH-SUPPRESS] captured %d live enemy positions pre-death\n", count);
}

static void restore_live_enemy_positions(void) {
    if (!suBaddieActorArray || pre_death_count == 0) return;
    s32 i;
    for (i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (!actor->marker) continue;
        u16 mid = (u16)actor->marker->id;
        u16 si  = (u16)bkrecomp_get_marker_spawn_index(actor->marker);

        s32 k;
        for (k = 0; k < pre_death_count; k++) {
            if (pre_death_snapshot[k].marker_id   == mid &&
                pre_death_snapshot[k].spawn_index == si) {
                // Pin the enemy to where it was before the owner died.
                // sync_enemy_positions reads actor->position each frame,
                // so overriding here keeps remote players' view stable.
                actor->position[0] = pre_death_snapshot[k].pos_x;
                actor->position[1] = pre_death_snapshot[k].pos_y;
                actor->position[2] = pre_death_snapshot[k].pos_z;
                break;
            }
        }
    }
}

// After the owner's respawn, BK's engine re-spawns every enemy that had
// been killed before the death. That leaves the owner's local state out
// of sync with the shared world view — and because the owner is the one
// broadcasting positions, those respawned-but-actually-dead enemies also
// appear back on every remote player's screen.
//
// This helper iterates killed_on_map[] (which tracks both local kills and
// remote kills received while the owner was alive) and silently despawns
// any actor matching a dead entry on the current map. It's called each
// frame during the death-suppression window so gradual respawns are
// caught as they happen.
static void resync_killed_on_respawn(void) {
    if (!suBaddieActorArray) return;
    if (killed_on_map_count == 0) return;
    u32 cur_map = (u32)map_get();

    s32 k;
    for (k = 0; k < killed_on_map_count; k++) {
        if (killed_on_map[k].map_id != cur_map) continue;
        u16 want_marker = killed_on_map[k].marker_type;
        u16 want_spawn  = killed_on_map[k].spawn_index;

        s32 i;
        for (i = 0; i < suBaddieActorArray->cnt; i++) {
            Actor *actor = &suBaddieActorArray->data[i];
            if (!actor->marker) continue;
            if (actor->marker->id != want_marker) continue;
            u16 si = (u16)bkrecomp_get_marker_spawn_index(actor->marker);
            if (si != want_spawn) continue;
            // Found a respawned enemy that should be dead — despawn silently
            marker_despawn(actor->marker);
            break;
        }
    }
}

static void poll_enemy_deaths(void) {
    if (!recomp_net_is_connected() || processing_remote) return;
    if (!suBaddieActorArray) return;

    u32 cur_map = (u32)map_get();
    u32 cur_level = (u32)level_get();

    // Only world owner polls — others receive via sync_enemy_positions
    if (!recomp_net_am_i_world_owner(cur_level)) return;

    // --- Owner-death suppression ---
    if (owner_is_dying()) {
        if (death_suppress_frames == 0) {
            recomp_printf("[DEATH-SUPPRESS] owner entered DIE state (bs=0x%X), freezing kill detection\n",
                bs_getState());
            // Capture enemy positions ONCE on the first DIE frame. The
            // engine may reposition them during the respawn transition,
            // but we'll keep overwriting with these saved values.
            capture_pre_death_snapshot();
        }
        death_suppress_frames = DEATH_SUPPRESS_FRAMES;  // hold at max while dying
        prev_enemy_count = 0;
        // Pin live enemies to their pre-death positions (stops the visual
        // snap on joins when the engine resets enemies to spawn points).
        restore_live_enemy_positions();
        // Clear respawned-dead enemies as they re-appear during the
        // respawn sequence. Runs every frame because BK re-spawns
        // gradually.
        resync_killed_on_respawn();
        return;
    }
    if (death_suppress_frames > 0) {
        death_suppress_frames--;
        // Force the next live frame to rebuild the snapshot from scratch
        // instead of comparing against a stale pre-death roster.
        prev_enemy_count = 0;
        restore_live_enemy_positions();
        resync_killed_on_respawn();
        return;
    }
    // Suppression has ended — release the pre-death snapshot so the next
    // death cycle can re-capture fresh positions. The engine will now own
    // enemy movement again from wherever restore_live_enemy_positions
    // last pinned them, so there's no snap.
    if (pre_death_captured) {
        pre_death_captured = FALSE;
        pre_death_count = 0;
        recomp_printf("[DEATH-SUPPRESS] suppression window ended, resuming normal polling\n");
    }

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
        // Reset dedup cache on level change (map transitions keep it).
        recent_kill_count = 0;
        recent_kill_level = cur_level;
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

    // Mass-unload fallback: if a large chunk of the roster vanished in a
    // single frame, treat it as a scene transition (death cutscene,
    // cutscene reload, etc.) rather than a barrage of kills. Real combat
    // rarely drops more than a handful of enemies simultaneously — bombs
    // and splash damage tend to cap at 2-3. 5+ vanishing at once is
    // almost always an engine unload.
    if (prev_enemy_count > 0 && (prev_enemy_count - cur_count) >= 5) {
        recomp_printf("[DEATH-SUPPRESS] mass unload detected (%d->%d), skipping frame\n",
            prev_enemy_count, cur_count);
        // Rebuild snapshot from current roster so the next frame compares
        // against the post-unload state instead of the pre-unload one.
        s32 j;
        for (j = 0; j < cur_count; j++) {
            prev_enemies[j].marker_id = cur_enemies[j].marker_id;
            prev_enemies[j].spawn_index = cur_enemies[j].spawn_index;
            prev_enemies[j].pos_x = cur_enemies[j].pos_x;
            prev_enemies[j].pos_y = cur_enemies[j].pos_y;
            prev_enemies[j].pos_z = cur_enemies[j].pos_z;
        }
        prev_enemy_count = cur_count;
        return;
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
            u8 target_bits = (u8)evt->coll_id;
            // Always persist to bitfield — this is the source of truth.
            mark_jinjo_collected((s32)evt->coll_level_id, target_bits);

            // Apply jinjo bits to the ITEM counter silently if not already set.
            s32 cur = item_getCount(ITEM_12_JINJOS);
            s32 new_bits = (s32)target_bits & ~cur;
            if (new_bits > 0) {
                item_adjustByDiffWithoutHud(ITEM_12_JINJOS, new_bits);
            }
            prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
            // Despawn jinjo actor on the same map
            if (cur_map == evt->coll_map_id) {
                u32 marker_id = jinjo_bit_to_marker_id(target_bits);
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
            // Persistence applies regardless of local map, so cross-world
            // progress survives resyncs. Visual despawn + HUD stay map-gated.
            if (evt->coll_id != 0xFFFE
                && !is_note_collected((s32)evt->coll_map_id, (s32)evt->coll_level_id, (u8)evt->coll_id)) {
                set_note_collected((s32)evt->coll_map_id, (s32)evt->coll_level_id, (u8)evt->coll_id);
                bump_level_notescore((s32)evt->coll_level_id);
                if (cur_map == evt->coll_map_id) {
                    item_adjustByDiffWithoutHud(ITEM_C_NOTE, 1);
                }
            }
            if (cur_map == evt->coll_map_id) {
                if (evt->coll_id == 0xFFFE) {
                    despawn_actor_by_marker_id(MARKER_5F_MUSIC_NOTE);
                } else {
                    bkrecomp_net_hide_note(evt->coll_id);
                }
            }
        } else if (ct == COLLECTIBLE_EXTRA_LIFE) {
            // Don't adjust lives on resync — just despawn
            if (cur_map == evt->coll_map_id) {
                despawn_actor_by_marker_id(MARKER_61_EXTRA_LIFE);
            }
        } else if (ct == COLLECTIBLE_GOLD_BULLION) {
            // Resync (sender_id == 0xFE): don't adjust inventory, just
            // mark the persistent picked mask (so sub-map peers still
            // know this bullion is gone when they later step into
            // MAP_7) and despawn by spawn_index on the same map. The
            // spawn_index is carried in coll_id; 0xFFFF means the
            // sender couldn't identify a specific bullion (legacy
            // fallback to position-based despawn).
            if (evt->coll_id != 0xFFFF) {
                bkrecomp_net_blubber_mark_bullion_picked((u32)evt->coll_id);
            }
            if (cur_map == evt->coll_map_id) {
                if (evt->coll_id != 0xFFFF) {
                    despawn_actor_by_marker_and_spawn(MARKER_37_GOLD_BULLION,
                        (u32)evt->coll_id);
                } else {
                    despawn_nearest_actor(MARKER_37_GOLD_BULLION,
                        evt->coll_pos_x, evt->coll_pos_y, evt->coll_pos_z);
                }
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
            // Persistence (bitfield + D_80385FF0) applies ALWAYS, even when the
            // local player is in a different world — shared collectibles must
            // accumulate for note-door checks regardless of location.
            if (evt->coll_id != 0xFFFE) {
                bool already = is_note_collected((s32)evt->coll_map_id, (s32)evt->coll_level_id, (u8)evt->coll_id);
                if (!already) {
                    set_note_collected((s32)evt->coll_map_id, (s32)evt->coll_level_id, (u8)evt->coll_id);
                    bump_level_notescore((s32)evt->coll_level_id);
                    // HUD counter + pickup SFX only when on the same level as the sender.
                    if (same_level) item_inc(ITEM_C_NOTE);
                }
            } else if (same_level) {
                // Dynamic note: no stable index to dedupe on, so only apply in-level.
                item_inc(ITEM_C_NOTE);
                bump_level_notescore((s32)evt->coll_level_id);
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
            u8 target_bits = (u8)evt->coll_id;
            // Persist to the shared bitfield regardless of level so a
            // player in a different world still records the collection
            // and can't re-collect on entry.
            bool was_complete = (get_jinjo_flags_for_level((s32)evt->coll_level_id) == 0x1F);
            mark_jinjo_collected((s32)evt->coll_level_id, target_bits);
            bool now_complete = (get_jinjo_flags_for_level((s32)evt->coll_level_id) == 0x1F);

            // Apply to the vanilla ITEM_12_JINJOS counter only on the
            // same level AND only for bits not already set — without the
            // filter, a re-collected jinjo after another player's death
            // double-counts for everyone who already had that bit.
            if (same_level) {
                s32 cur = item_getCount(ITEM_12_JINJOS);
                s32 actual_new = (s32)target_bits & ~cur;
                if (actual_new > 0) {
                    item_adjustByDiffWithHud(ITEM_12_JINJOS, actual_new);
                    prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
                }
                // Fire the jinjo-completion jiggy only on the transition
                // <5 -> 5. Guards against a re-broadcast spawning a dup.
                if (!was_complete && now_complete && same_map) {
                    f32 jiggy_pos[3];
                    jiggy_pos[0] = evt->coll_pos_x;
                    jiggy_pos[1] = evt->coll_pos_y + 50.0f;
                    jiggy_pos[2] = evt->coll_pos_z;
                    jiggy_spawn(10 * (s32)level_get() - 9, jiggy_pos);
                }
            }
            if (same_map) {
                u32 marker_id = jinjo_bit_to_marker_id(target_bits);
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
        } else if (ct == COLLECTIBLE_GOLD_BULLION) {
            // Shared bullion. Three responsibilities:
            //   1. Update the persistent picked-bullion mask owned by
            //      the Blubber sync module. This must run regardless
            //      of the receiver's current map so that a player
            //      inside MAP_5 / MAP_6 / MAP_A learns the bullion is
            //      gone and can despawn it on returning to MAP_7.
            //   2. Give the receiver +1 in inventory (clamped to the
            //      natural 2-bullion cap) so either player can deliver
            //      it to Blubber later.
            //   3. Despawn the specific actor in the current map by
            //      spawn_index (carried in coll_id). The legacy
            //      position-based despawn is kept as a fallback when
            //      a sender from an older build transmits 0xFFFF.
            if (evt->coll_id != 0xFFFF) {
                bkrecomp_net_blubber_mark_bullion_picked((u32)evt->coll_id);
            }
            s32 cur_count = item_getCount(ITEM_18_GOLD_BULLIONS);
            if (cur_count < 2) {
                item_inc(ITEM_18_GOLD_BULLIONS);
            }
            prev_bullions = item_getCount(ITEM_18_GOLD_BULLIONS);
            if (same_map) {
                if (evt->coll_id != 0xFFFF) {
                    despawn_actor_by_marker_and_spawn(MARKER_37_GOLD_BULLION,
                        (u32)evt->coll_id);
                } else {
                    despawn_nearest_actor(MARKER_37_GOLD_BULLION,
                        evt->coll_pos_x, evt->coll_pos_y, evt->coll_pos_z);
                }
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

    // Lazy level-change reset for non-owners (owner path already resets in
    // poll_enemy_deaths). Ensures recent_kills doesn't leak across levels.
    u32 cur_lvl = (u32)level_get();
    if (cur_lvl != recent_kill_level) {
        recent_kill_count = 0;
        recent_kill_level = cur_lvl;
    }

    if (cur_map != evt->enemy_map_id) {
        // Save for later — join might not be on this map yet
        if (pending_kill_count < MAX_PENDING_KILLS) {
            pending_kills[pending_kill_count].marker_type = evt->enemy_marker_type;
            pending_kills[pending_kill_count].spawn_index = evt->enemy_spawn_index;
            pending_kills[pending_kill_count].map_id = evt->enemy_map_id;
            pending_kill_count++;
            recomp_printf("[ENEMY-EVT] saved pending kill: marker=0x%X spawn=%d map=%d (cur=%d)\n",
                evt->enemy_marker_type, evt->enemy_spawn_index, evt->enemy_map_id, cur_map);
        } else {
            pending_kills_dropped++;
            // Log first drop and every 16th thereafter so floods don't spam.
            if ((pending_kills_dropped & 0xF) == 1) {
                recomp_printf("[ENEMY-EVT] *** DROPPED pending kill (buffer full): marker=0x%X spawn=%d map=%d cur=%d (total dropped=%u)\n",
                    evt->enemy_marker_type, evt->enemy_spawn_index, evt->enemy_map_id, cur_map, pending_kills_dropped);
            }
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

            // Defensive dedup: if we've already processed a kill for this
            // (marker, spawn, map) as non-resync, downgrade to silent despawn
            // to prevent duplicate honeycomb drops (fixes #6).
            bool already_processed = is_recent_kill(target_marker, target_spawn, cur_map);

            if (is_resync || already_processed) {
                // Silent despawn — enemy was already killed, just remove on re-entry
                marker_despawn(actor->marker);
                if (already_processed && !is_resync) {
                    recomp_printf("[ENEMY-EVT] dedup: silent despawn for marker=0x%X spawn=%d (already killed)\n",
                        target_marker, target_spawn);
                }
            } else {
                // Real-time kill: honeycomb + death animation
                remember_kill(target_marker, target_spawn, cur_map);
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
    // If world owner: remove from prev_enemies so poll doesn't re-detect,
    // and track the kill in killed_on_map so a future death/respawn of this
    // owner can despawn the respawned-but-actually-dead enemy.
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

        // Track remote kills in killed_on_map[] so respawn cleanup catches
        // enemies killed by other players too (not just the owner's local
        // kills). Dedup by (marker, spawn, map).
        if (cur_map == evt->enemy_map_id && killed_on_map_count < MAX_KILLED_ON_MAP) {
            bool already_tracked = FALSE;
            s32 k;
            for (k = 0; k < killed_on_map_count; k++) {
                if (killed_on_map[k].marker_type == target_marker &&
                    killed_on_map[k].spawn_index == target_spawn &&
                    killed_on_map[k].map_id == cur_map) {
                    already_tracked = TRUE;
                    break;
                }
            }
            if (!already_tracked) {
                s32 ki = killed_on_map_count;
                killed_on_map[ki].marker_type = target_marker;
                killed_on_map[ki].spawn_index = target_spawn;
                killed_on_map[ki].pos_x = evt->enemy_pos_x;
                killed_on_map[ki].pos_y = evt->enemy_pos_y;
                killed_on_map[ki].pos_z = evt->enemy_pos_z;
                killed_on_map[ki].map_id = cur_map;
                killed_on_map_count = ki + 1;
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
    u8  picked_bullion_mask; // 0x29 — TTC bullion spawn_index bitmask (was _pad2)
    u16 note_count;          // 0x2A
    u8  lives;               // 0x2C
    u8  bullions;            // 0x2D — ITEM_18_GOLD_BULLIONS (Blubber quest)
    // Flag sync data (Phase 8)
    u8  file_progress_flags[37]; // 0x2E (0x25 bytes) -> ends at 0x53
    u8  level_specific_flags[8]; // 0x53 -> ends at 0x5B
    u8  volatile_flags[25];      // 0x5B (0x19 bytes) -> ends at 0x74
    u32 map_specific_flags;      // 0x74 (4-byte aligned)
    u8  has_flags;               // 0x78 (1 if flag data present)
    u8  _pad4[3];                // 0x79
    u8  abilities[8];            // 0x7C (learnedAbilities + usedAbilities)
    // Cross-world note sync (Phase 13)
    u8  note_scores[11];         // 0x84 (D_80385FF0 mirror: per-level high scores)
    u8  _pad5;                   // 0x8F
    u8  level_notes[9][32];      // 0x90 (288 bytes, 0x120) -> ends at 0x1B0
    // Persistent per-level jinjo bitfield
    u8  jinjos_collected[9];     // 0x1B0
    u8  _pad6[3];                // 0x1B9 (pad to 4-byte boundary)
} WorldStateFullData;            // 0x1BC = 444 bytes

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
    data.bullions = (u8)item_getCount(ITEM_18_GOLD_BULLIONS);
    // TTC bullion spawn_index mask — tells the joiner which specific
    // bullions have already been picked up so their local copies can
    // be despawned before the player walks over a ghost-actor.
    data.picked_bullion_mask =
        (u8)bkrecomp_net_blubber_get_picked_bullion_mask();

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

    // Per-level note state (Phase 13): D_80385FF0 drives the door check,
    // level_notes[] prevents already-collected notes from respawning.
    // Explicit memcpy routes through memcpy_recomp (syms.ld); a plain byte
    // loop here gets folded into a compiler-intrinsic memcpy at -O2, which
    // N64Recomp can't resolve and produces a broken call.
    memcpy(data.note_scores, D_80385FF0, 11);
    memcpy(data.level_notes, loaded_file_extension_data.level_notes, 9 * 32);
    // Per-level persistent jinjo bitfield — shared across all players.
    memcpy(data.jinjos_collected, loaded_file_extension_data.jinjos_collected, 9);

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

    // Apply gold bullions (TTC Blubber quest) — late-join recovery.
    // Clamped to the natural 2-bullion cap.
    {
        s32 cur = item_getCount(ITEM_18_GOLD_BULLIONS);
        s32 target = (s32)data.bullions;
        if (target > 2) target = 2;
        if (target > cur) {
            item_adjustByDiffWithoutHud(ITEM_18_GOLD_BULLIONS, target - cur);
        }
        prev_bullions = item_getCount(ITEM_18_GOLD_BULLIONS);
    }

    // Merge the TTC picked-bullion spawn_index mask. Ensures a late
    // joiner knows which specific bullion actors have already been
    // collected in-session, so despawn_picked_bullions (blubber_tick)
    // can clean them up the next time the joiner sets foot on MAP_7.
    bkrecomp_net_blubber_apply_picked_bullion_mask((u32)data.picked_bullion_mask);

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

    // Merge per-level note state (Phase 13).
    // level_notes -> OR into local bitfield (never lose locally-collected notes).
    // D_80385FF0 -> recompute from merged bitfield, then MAX with host's snapshot
    // (handles vanilla saves where notes predate note-saving bitfield).
    {
        s32 i, b;
        for (i = 0; i < 9; i++) {
            for (b = 0; b < 32; b++) {
                loaded_file_extension_data.level_notes[i].bytes[b] |= data.level_notes[i][b];
            }
        }
        // Apply MAX merge to per-level high-score array.
        for (i = 1; i <= 0xA; i++) {
            s32 arr_idx = level_id_to_level_array_index((enum level_e)i);
            s32 from_bitfield = (arr_idx >= 0) ? get_collected_note_count((enum level_e)i) : 0;
            s32 from_host = (s32)data.note_scores[i];
            s32 cur = (s32)D_80385FF0[i];
            s32 best = cur;
            if (from_bitfield > best) best = from_bitfield;
            if (from_host > best) best = from_host;
            D_80385FF0[i] = (u8)best;
        }
        recomp_printf("[STATE-SYNC] merged note state (bitfield OR, D_80385FF0 MAX)\n");
    }

    // Merge per-level persistent jinjo bitfield — OR into local.
    // After the merge, top up ITEM_12_JINJOS if the receiver is currently
    // in a level whose flags changed. poll_collected_jinjos() will also
    // reconcile and despawn stale actors on subsequent frames.
    {
        s32 i;
        for (i = 0; i < 9; i++) {
            loaded_file_extension_data.jinjos_collected[i] |= data.jinjos_collected[i];
        }
        s32 cur_level = (s32)level_get();
        u8 flags_here = get_jinjo_flags_for_level(cur_level);
        if (flags_here != 0) {
            s32 cur = item_getCount(ITEM_12_JINJOS);
            s32 diff = (s32)flags_here & ~cur;
            if (diff > 0) {
                item_adjustByDiffWithoutHud(ITEM_12_JINJOS, diff);
                prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
            }
        }
        recomp_printf("[STATE-SYNC] merged jinjo bitfield\n");
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
        prev_bullions = item_getCount(ITEM_18_GOLD_BULLIONS);
        dbg_prev_jiggy_total = item_getCount(ITEM_26_JIGGY_TOTAL);
    }

    processing_remote = FALSE;
}

// Ownership transfer send/receive REMOVED — centralized kill tracking in C++ handles this.

// Track map/level changes for ALL players to reset cached state
static u32 prev_global_level = 0xFFFFFFFF;
static u32 prev_global_map = 0xFFFFFFFF;

// Re-snapshot every polling baseline to the current in-memory score state.
// Call this AFTER gameFile_load / clearScoreStates in online mode so the
// first poll doesn't emit every loaded jiggy/mumbo/honeycomb as "just
// collected" events to peers (which would contaminate the other side's
// save with items it never picked up).
RECOMP_EXPORT void bkrecomp_net_reset_poll_baselines(void) {
    {
        u8 *score = jiggyscore_getPtr();
        if (score) { s32 i; for (i = 0; i < 0xD; i++) prev_jiggyscore[i] = score[i]; }
        else       { s32 i; for (i = 0; i < 0xD; i++) prev_jiggyscore[i] = 0; }
    }
    {
        u8 *score = func_80321538();
        if (score) { s32 i; for (i = 0; i < 16; i++) prev_mumboscore[i] = score[i]; }
        else       { s32 i; for (i = 0; i < 16; i++) prev_mumboscore[i] = 0; }
    }
    {
        u8 *score = honeycombscore_get_ptr();
        if (score) { s32 i; for (i = 0; i < 3; i++) prev_honeycombscore[i] = score[i]; }
        else       { s32 i; for (i = 0; i < 3; i++) prev_honeycombscore[i] = 0; }
    }
    prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);
    prev_lives      = item_getCount(ITEM_16_LIFE);
    prev_eggs          = item_getCount(ITEM_D_EGGS);
    prev_red_feathers  = item_getCount(ITEM_F_RED_FEATHER);
    prev_gold_feathers = item_getCount(ITEM_10_GOLD_FEATHER);
    prev_health        = item_getCount(ITEM_14_HEALTH);
    prev_bullions      = item_getCount(ITEM_18_GOLD_BULLIONS);
    dbg_prev_jiggy_total = item_getCount(ITEM_26_JIGGY_TOTAL);
}

// Called every frame
RECOMP_EXPORT void bkrecomp_net_process_world_events(void) {
    if (!recomp_net_is_connected()) return;

    // EEPROM handshake must run every frame regardless of map state —
    // the join needs this to fire during file-select before any game
    // logic runs, so put it ahead of the map-change early-return.
    bkrecomp_net_save_override_tick();

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
    poll_collected_jinjos();
    poll_enemy_deaths();
    sync_enemy_positions();

    // TTC Leaky bucket egg-counter sync
    {
        extern void bkrecomp_net_leaky_tick(void);
        bkrecomp_net_leaky_tick();
    }

    // TTC Sandcastle cheat-code progress sync
    {
        extern void bkrecomp_net_sandcastle_tick(void);
        bkrecomp_net_sandcastle_tick();
    }

    // TTC Nipper (hermit crab) state + lifetime sync
    {
        extern void bkrecomp_net_nipper_tick(void);
        bkrecomp_net_nipper_tick();
    }

    // TTC Blubber (pirate) delivery decrement + quest-complete despawn sync
    {
        extern void bkrecomp_net_blubber_tick(void);
        bkrecomp_net_blubber_tick();
    }

    // TTC Treasure Hunt (red arrow/question/X chain + buried treasure) sync
    {
        extern void bkrecomp_net_treasurehunt_tick(void);
        bkrecomp_net_treasurehunt_tick();
    }

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
