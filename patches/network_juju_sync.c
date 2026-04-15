#include "patches.h"
#include "functions.h"
#include "enums.h"

// Network bridge
void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);
u32  recomp_net_is_connected(void);

extern enum map_e map_get(void);

// Functions used by juju hitbox collision (from decomp, not in public headers)
extern s32 func_80329784(Actor *);     // get angle-to-player
extern void func_80353580(ActorMarker *); // egg splat effect
extern void func_803892A8(ActorMarker **ptr); // trigger juju segment falling animations
extern Actor *actorArray_findClosestActorFromActorId(f32 position[3], enum actor_e actor_id, s32 arg2, f32 *min_distance_ptr);

// Juju segment functions (from juju.c)
extern bool __chjuju_isEveryJujuStable(ActorMarker **ptr);
extern bool __chjuju_isEveryJujuDespawned(ActorMarker **ptr);
extern void __chjuju_updateCount(ActorMarker **ptr);

// Functions not in public headers
extern void __chjujuhitbox_playRubbingSfx(Actor *);
extern bool subaddie_playerIsWithinSphereAndActive(Actor *, s32);
extern s32  player_movementGroup(void);
extern void __spawnQueue_add_2();
extern s32  mapSpecificFlags_get(s32);

// Juju hitbox local struct (must match decomp jujuhitbox.c)
typedef struct {
    u8           pad0[0x4];
    s32          unk4;          // hit count (number of segments knocked off)
    ActorMarker *jujus[4];     // pointers to the 4 segment markers
    f32          unk18;         // sound pitch/speed
} ActorLocal_JujuHitbox;

// Flag type (must match net_packets.h FLAG_JUJU_ACTION)
#define NET_FLAG_JUJU_ACTION 9

// === Queue for remote juju hit events ===

#define MAX_PENDING_JUJU_EVENTS 4

typedef struct {
    u32 hit_count;   // unk4 value after hit (1-4)
    u32 map_id;
} PendingJujuEvent;

static PendingJujuEvent pending_juju_events[MAX_PENDING_JUJU_EVENTS];
static s32 pending_juju_event_count = 0;

// Called from network_flag_sync.c when a remote FLAG_JUJU_ACTION event arrives
RECOMP_EXPORT void bkrecomp_net_juju_remote_hit(u32 hit_count, u32 map_id) {
    if (pending_juju_event_count >= MAX_PENDING_JUJU_EVENTS) return;
    pending_juju_events[pending_juju_event_count].hit_count = hit_count;
    pending_juju_events[pending_juju_event_count].map_id = map_id;
    pending_juju_event_count++;
}

// === Helpers ===

// Angle check from original decomp (func_80388B30)
static bool juju_facing_check(Actor *this, float arg1) {
    f32 yaw = this->yaw - func_80329784(this);

    if (180.0f <= yaw) {
        yaw -= 360.0f;
    } else if (yaw < -180.0f) {
        yaw += 360.0f;
    }

    if (yaw < 0.0f) {
        yaw = -yaw;
    }

    return (yaw < arg1) ? TRUE : FALSE;
}

// === RECOMP_PATCH: Egg collision callback — adds network sync ===

RECOMP_PATCH void func_80388BEC(NodeProp *node, ActorMarker *marker) {
    f32 distance_to_closest_actor;
    Actor *closest_actor;
    Actor *temp_v0;
    f32 position[3];

    position[0] = (f32) node->x;
    position[1] = (f32) node->y;
    position[2] = (f32) node->z;

    closest_actor = actorArray_findClosestActorFromActorId(position, ACTOR_11_JUJU_CTRL, -1, &distance_to_closest_actor);

    if (closest_actor != NULL
        && !(distance_to_closest_actor > 500.0f)
        && (closest_actor->state == 3)
    ) {
        ActorLocal_JujuHitbox *jujuCtlPtr = (ActorLocal_JujuHitbox *) &closest_actor->local;
        temp_v0 = marker_getActor(jujuCtlPtr->jujus[jujuCtlPtr->unk4]);

        if (temp_v0 != NULL) {
            if (juju_facing_check(temp_v0, 90.0f)) {
                closest_actor->state = 1;
                jujuCtlPtr->unk4++;
                func_803892A8(jujuCtlPtr->jujus);
                func_80353580(marker);
                __spawnQueue_add_4((GenFunction_4)spawnQueue_actor_f32, 0x58, *(s32 *)&position[0], *(s32 *)&position[1], *(s32 *)&position[2]);

                // Network sync: send hit event with the new hit count
                if (recomp_net_is_connected()) {
                    recomp_net_send_flag_change(NET_FLAG_JUJU_ACTION,
                        (u32)jujuCtlPtr->unk4, 0, (u32)map_get());
                    recomp_printf("[JUJU-SYNC] sent hit: count=%d map=%d\n",
                        jujuCtlPtr->unk4, (u32)map_get());
                }
            }
        }
    }
}

// === Apply remote juju events ===

static void apply_remote_juju_hit(Actor *ctrl_actor) {
    if (pending_juju_event_count <= 0) return;

    u32 cur_map = (u32)map_get();
    ActorLocal_JujuHitbox *jujuCtlPtr = (ActorLocal_JujuHitbox *) &ctrl_actor->local;

    s32 i;
    for (i = 0; i < pending_juju_event_count; i++) {
        if (pending_juju_events[i].map_id != cur_map) continue;

        u32 target_count = pending_juju_events[i].hit_count;

        // Only apply if we haven't already reached this hit count
        if ((u32)jujuCtlPtr->unk4 < target_count && ctrl_actor->state == 3) {
            ctrl_actor->state = 1;
            jujuCtlPtr->unk4 = (s32)target_count;
            func_803892A8(jujuCtlPtr->jujus);

            recomp_printf("[JUJU-SYNC] applied remote hit: count=%d\n", target_count);
        }

        // Remove event (swap with last)
        pending_juju_events[i] = pending_juju_events[pending_juju_event_count - 1];
        pending_juju_event_count--;
        i--;  // Re-check swapped entry
    }
}

// === RECOMP_PATCH: Juju controller update — adds remote event processing ===

// Forward declare for spawnQueue callback
extern void __chjujuhitbox_initialize_all(ActorMarker *, s32);

// Pitch increase helper (from decomp func_80388D60)
static void juju_increase_pitch(Actor *this) {
    ActorLocal_JujuHitbox *jujuCtlPtr = (ActorLocal_JujuHitbox *) &this->local;

    if (!__chjuju_isEveryJujuDespawned(jujuCtlPtr->jujus)) {
        jujuCtlPtr->unk18 *= 1.05;
    }

    this->state = 3;
}

RECOMP_PATCH void chjujuhitbox_update(Actor *this) {
    ActorLocal_JujuHitbox *jujuCtlPtr;
    s32 i;

    jujuCtlPtr = (ActorLocal_JujuHitbox *) &this->local;

    if (!this->initialized) {
        this->initialized = TRUE;
        this->has_met_before = FALSE;
        jujuCtlPtr->unk18 = 0.5f;
    }

    if (!this->volatile_initialized) {
        this->volatile_initialized = TRUE;
        __spawnQueue_add_2((GenFunction_2) __chjujuhitbox_initialize_all, this->marker, jujuCtlPtr->unk4);
        __chjujuhitbox_playRubbingSfx(this);
        return;
    }

    if (subaddie_playerIsWithinSphereAndActive(this, 0xfa) && !subaddie_playerIsWithinSphereAndActive(this, 0x50) && !player_movementGroup()) {
        if (!this->has_met_before && gcdialog_showDialog(ASSET_B44_DIALOG_JUJU_MEET, 0, 0, 0, NULL, NULL)) {
            this->has_met_before = TRUE;
        }
    }

    // Process remote juju events (only when in ready state)
    if (recomp_net_is_connected()) {
        apply_remote_juju_hit(this);
    }

    if (this->state == 1) {
        if (__chjuju_isEveryJujuStable(jujuCtlPtr->jujus)) {
            juju_increase_pitch(this);
        }

        if (__chjuju_isEveryJujuDespawned(jujuCtlPtr->jujus)) {
            marker_despawn(this->marker);
            for (i = 0; i < 4; i++) {
                marker_despawn(jujuCtlPtr->jujus[i]);
            }
            return;
        }
    }
    else {
        __chjuju_updateCount(jujuCtlPtr->jujus);
    }

    if (mapSpecificFlags_get(MM_SPECIFIC_FLAG_9_JUJU_HAS_HALF_TURNED)) {
        __chjujuhitbox_playRubbingSfx(this);
        mapSpecificFlags_set(MM_SPECIFIC_FLAG_9_JUJU_HAS_HALF_TURNED, FALSE);
    }
}
