#include "patches.h"
#include "functions.h"
#include "enums.h"

// Network bridge
void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);
u32  recomp_net_is_connected(void);

extern enum map_e map_get(void);
extern u32 bkrecomp_get_marker_spawn_index(ActorMarker* marker);
extern ActorArray *suBaddieActorArray;

// Functions used by original hut update (not in public headers)
extern s32 func_80334904(void);        // map loading state check (returns 2 when ready)
extern s32 player_getActiveHitbox();
extern s32 func_8028F20C(void);        // player grounded/valid state check
extern void __chhut_spawnExplosion(ActorMarker *);
extern void player_getPosition(f32 pos[3]);

// Bundle/item spawning
extern Actor *spawnQueue_bundle_f32(s32, s32, s32, s32);
extern void bundle_setYaw(f32);

// Hut smash counter (from bss, shared across all hut instances)
extern s32 mmhut_smashCount;

// Cross-client grublin tagging: registers the hut-spawn-index so the local
// grublin spawned by this hut can be correlated with the remote client's
// grublin via a shared logical id. See network_world_sync.c.
extern void bkrecomp_net_register_hut_grublin(u32 hut_spawn_index, f32 x, f32 y, f32 z);

// Flag type (must match net_packets.h FLAG_HUT_ACTION)
#define NET_FLAG_HUT_ACTION 8

// Hut states (must match decomp hut.c)
#define HUT_STATE_0_INTACT    0
#define HUT_STATE_1_DAMAGED   1
#define HUT_STATE_2_DESTROYED 2

// === Queue for remote hut destruction events ===

#define MAX_PENDING_HUT_EVENTS 8

typedef struct {
    u16 spawn_index;
    u8  smash_count;
    u32 map_id;
} PendingHutEvent;

static PendingHutEvent pending_hut_events[MAX_PENDING_HUT_EVENTS];
static s32 pending_hut_event_count = 0;

// Called from network_flag_sync.c when a remote FLAG_HUT_ACTION event arrives
RECOMP_EXPORT void bkrecomp_net_hut_remote_destroy(u32 spawn_index, u32 smash_count, u32 map_id) {
    if (pending_hut_event_count >= MAX_PENDING_HUT_EVENTS) return;
    pending_hut_events[pending_hut_event_count].spawn_index = (u16)spawn_index;
    pending_hut_events[pending_hut_event_count].smash_count = (u8)smash_count;
    pending_hut_events[pending_hut_event_count].map_id = map_id;
    pending_hut_event_count++;
}

// Spawn explosion + steam at hut position (same as __chhut_spawnExplosion but inline)
static void hut_spawn_explosion(Actor *hut_actor) {
    Actor *explosion = actor_spawnWithYaw_f32(ACTOR_4B_WOOD_EXPLOSION_2, hut_actor->position, 0);
    actor_spawnWithYaw_f32(ACTOR_4D_STEAM_2, explosion->position, 0);
}

// Try to apply pending remote hut destruction events to actors on current map.
// Called each frame from the patched chhut_update (runs for each hut actor).
static bool try_apply_remote_event(Actor *this) {
    if (pending_hut_event_count <= 0) return FALSE;
    if (this->state != HUT_STATE_0_INTACT) return FALSE;

    u16 my_spawn = (u16)bkrecomp_get_marker_spawn_index(this->marker);
    u32 cur_map = (u32)map_get();

    s32 i;
    for (i = 0; i < pending_hut_event_count; i++) {
        if (pending_hut_events[i].map_id != cur_map) continue;
        if (pending_hut_events[i].spawn_index != my_spawn) continue;

        // Match found — apply destruction
        static enum bundle_e hut_bundles[6] = {
            BUNDLE_0_MM_HUT_MUSIC_NOTE,
            BUNDLE_1_MM_HUT_BLUE_EGG,
            BUNDLE_2_MM_HUT_GRUBLIN,
            BUNDLE_3_MM_HUT_JINJO_GREEN,
            BUNDLE_6_MM_HUT_EXTRA_LIFE,
            BUNDLE_4_MM_HUT_JIGGY
        };

        f32 spawn_pos[3];
        spawn_pos[0] = this->position_x;
        spawn_pos[1] = this->position_y + 125.0f;
        spawn_pos[2] = this->position_z;

        // Play destruction (same sequence as local)
        sfxsource_playHighPriority(SFX_5B_HEAVY_STUFF_FALLING);
        subaddie_set_state(this, HUT_STATE_1_DAMAGED);
        actor_playAnimationOnce(this);
        hut_spawn_explosion(this);

        // Spawn the same bundle the destroying player saw
        {
            u8 sc = pending_hut_events[i].smash_count;
            bundle_setYaw(this->yaw);
            if (sc < 5) {
                __spawnQueue_add_4((GenFunction_4) spawnQueue_bundle_f32,
                    hut_bundles[sc],
                    *(s32 *)(&spawn_pos[0]),
                    *(s32 *)(&spawn_pos[1]),
                    *(s32 *)(&spawn_pos[2]));

                // Same as local path: register the hut-spawned grublin so
                // bulk sync / kill packets can correlate with the killer's
                // copy via the shared logical id (= hut spawn_index).
                if (hut_bundles[sc] == BUNDLE_2_MM_HUT_GRUBLIN) {
                    bkrecomp_net_register_hut_grublin(my_spawn,
                        spawn_pos[0], spawn_pos[1], spawn_pos[2]);
                }
            } else {
                jiggy_spawn(JIGGY_5_MM_HUTS, spawn_pos);
            }
        }

        // Sync local smashCount to match
        mmhut_smashCount = (pending_hut_events[i].smash_count + 1) % 6;

        recomp_printf("[HUT-SYNC] applied remote destroy: spawn=%d smash=%d\n",
            my_spawn, pending_hut_events[i].smash_count);

        // Remove this event (swap with last)
        pending_hut_events[i] = pending_hut_events[pending_hut_event_count - 1];
        pending_hut_event_count--;
        return TRUE;
    }
    return FALSE;
}

// === RECOMP_PATCH: Replace chhut_update to add network sync ===

RECOMP_PATCH void chhut_update(Actor *this) {
    static enum bundle_e D_803898D8[6] = {
        BUNDLE_0_MM_HUT_MUSIC_NOTE,
        BUNDLE_1_MM_HUT_BLUE_EGG,
        BUNDLE_2_MM_HUT_GRUBLIN,
        BUNDLE_3_MM_HUT_JINJO_GREEN,
        BUNDLE_6_MM_HUT_EXTRA_LIFE,
        BUNDLE_4_MM_HUT_JIGGY
    };

    f32 diff_pos[3];
    f32 plyr_pos[3];

    if (func_80334904() != 2) {
        return;
    }

    if (!this->initialized) {
        this->marker->collidable = FALSE;
        this->initialized = TRUE;
    }

    switch (this->state) {
        case HUT_STATE_0_INTACT:
            // Check for remote destruction first
            if (try_apply_remote_event(this)) {
                break;
            }

            // Original local beak buster detection
            player_getPosition(plyr_pos);
            diff_pos[0] = plyr_pos[0] - this->position_x;
            diff_pos[1] = plyr_pos[1] - this->position_y;
            diff_pos[2] = plyr_pos[2] - this->position_z;

            if (150.0f < diff_pos[1]
                && player_getActiveHitbox(this->marker) == HITBOX_1_BEAK_BUSTER
                && func_8028F20C()
                && LENGTH_VEC3F(diff_pos) < 350.0f
            ){
                diff_pos[0] = this->position_x;
                diff_pos[1] = this->position_y;
                diff_pos[2] = this->position_z;
                diff_pos[1] += 125.0;

                sfxsource_playHighPriority(SFX_5B_HEAVY_STUFF_FALLING);
                subaddie_set_state(this, HUT_STATE_1_DAMAGED);
                actor_playAnimationOnce(this);
                __spawnQueue_add_1((GenFunction_1) __chhut_spawnExplosion, (s32) this->marker);
                bundle_setYaw(this->yaw);

                if (mmhut_smashCount < 5) {
                    __spawnQueue_add_4((GenFunction_4) spawnQueue_bundle_f32, D_803898D8[mmhut_smashCount], *(s32 *)(&diff_pos[0]), *(s32 *)(&diff_pos[1]), *(s32 *)(&diff_pos[2]));

                    // Register the hut-spawned grublin so cross-client sync
                    // can correlate our local grublin with the remote client's.
                    // hut_spawn_index is deterministic (preplaced level data),
                    // so both clients use it as a shared logical id.
                    if (D_803898D8[mmhut_smashCount] == BUNDLE_2_MM_HUT_GRUBLIN
                        && recomp_net_is_connected()) {
                        u32 hut_spawn = bkrecomp_get_marker_spawn_index(this->marker);
                        bkrecomp_net_register_hut_grublin(hut_spawn,
                            diff_pos[0], diff_pos[1], diff_pos[2]);
                    }
                }
                else {
                    jiggy_spawn(JIGGY_5_MM_HUTS, diff_pos);
                }

                // Send network event BEFORE incrementing smashCount
                if (recomp_net_is_connected()) {
                    u32 spawn_idx = bkrecomp_get_marker_spawn_index(this->marker);
                    recomp_net_send_flag_change(NET_FLAG_HUT_ACTION, spawn_idx,
                        (u32)mmhut_smashCount, (u32)map_get());
                    recomp_printf("[HUT-SYNC] sent destroy: spawn=%d smash=%d\n",
                        spawn_idx, mmhut_smashCount);
                }

                mmhut_smashCount = (mmhut_smashCount + 1) % 6;
            }
            break;

        case HUT_STATE_1_DAMAGED:
            if (anctrl_getAnimTimer(this->anctrl) > 0.99) {
                anctrl_setTransitionDuration(this->anctrl, 0.0f);
                subaddie_set_state(this, HUT_STATE_2_DESTROYED);
                this->position_y -= 160.0f;
            }
            break;

        case HUT_STATE_2_DESTROYED:
            break;
    }
}
