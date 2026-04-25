#include "patches.h"
#include "functions.h"
#include "variables.h"
#include "enums.h"
#include "core2/anctrl.h"
#include <math.h>

/* ============================================================
 * Bridge function declarations
 * ============================================================ */
u32  recomp_net_is_connected(void);
u32  recomp_net_get_remote_state(u32 player_id, void *out);
u32  recomp_net_get_remote_count(void);
u32  recomp_net_get_local_player_id(void);
u32  recomp_net_am_i_world_owner(u32 level_id);
void recomp_net_send_conga_orange(void *spawn_pos, void *velocity, u32 map_id);
u32  recomp_net_pop_conga_orange(void *out);
void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);
u32  recomp_net_is_host(void);

#define NET_FLAG_CONGA_HIT 10
#define NET_FLAG_DIALOG_COMPLETE_ANIM 17

/* NPC tags carried in flag_index of FLAG_DIALOG_COMPLETE_ANIM events.
 * Each NPC that uses post-dialog animation sync gets a unique tag so
 * future quests (Mumbo, Tanktup, Boggy, Trunker, etc.) can reuse the
 * same channel without needing a new flag type. */
#define NPC_DIALOG_ANIM_CHIMPY 1

/* ============================================================
 * Core game function declarations (NOT in headers)
 * ============================================================ */
Actor *actor_spawnWithYaw_s32(s32 actor_id, s32 position[3], s32 yaw);
extern enum map_e map_get(void);
extern enum level_e level_get(void);
extern ActorArray *suBaddieActorArray;
extern s32 func_803297C8(Actor *actor, f32 target_pos[3]);
void func_80333270(enum jiggy_e jiggy_id, f32 position[3],
                    void (*method)(Actor *, ActorMarker *), ActorMarker *other_marker);

/* Functions used by chConga_update — all core (addr < 0x80386000) */
extern s32  subaddie_playerIsWithinSphereAndActive(Actor *, s32);
extern void subaddie_set_ideal_yaw(Actor *, s32);
extern s32  globalTimer_getTime(void);
extern s32  gcdialog_hasCurrentTextId(void);
extern s32  gcdialog_getCurrentTextId(void);
extern s32  player_movementGroup(void);
extern s32  player_is_in_jiggy_jig(void);
extern s32  timedFuncQueue_is_empty();
extern f32  nodeprop_getRadius(NodeProp *);
extern void nodeprop_getPosition_s32(NodeProp *, s32 *);
extern void func_8034A1B4(void *, s32, void *);
extern s32  func_8032A9E4(s32, s32, s32);
extern void timed_exitStaticCamera(f32);
extern s32  mapSpecificFlags_get(s32);
extern s32  jiggyscore_isCollected(s32);
extern Actor *actorArray_findActorFromMarkerId(s32);

#ifndef MIN
#define MIN(s,t) ((s)<(t)?(s):(t))
#endif

/* ============================================================
 * Structs
 * ============================================================ */

typedef struct chconga_s {
    s32 orangeSpawnPosition[3];
    s32 unkC;
    s32 unk10;
    u8  pad14[0x4];
    s32 unk18;
    s32 unk1C;
} ActorLocal_Conga;

typedef struct {
    f32 x, y, z, yaw, pitch, scale;
    u32 map_id;
    u16 animation_id;
    u8  _pad1[2];
    f32 anim_timer, anim_duration;
    u8  anim_playback_type, health, health_total, lives, transformation, bs_state;
    u8  kazooie_flags;
    u8  _pad2;
    f32 horizontal_velocity;
    f32 anim_subrange_start;
    f32 anim_subrange_end;
    u8  carry_kind;     // 0x3C — visual prop replication (CARRY_KIND_*)
    u8  _pad3[3];
} RemoteState;

typedef struct {
    f32 spawn_x, spawn_y, spawn_z;
    f32 vel_x, vel_y, vel_z;
} CongaOrangeEvent;

/* ============================================================
 * Constants
 * ============================================================ */
#define CONGA_THROW_SPHERE    1000
#define CONGA_ACTIVE_SPHERE   2100
#define CONGA_TREE_X          (-5011.0f)
#define CONGA_TREE_Z          (5029.0f)
#define CONGA_TREE_RADIUS_SQ  (52900.0f)

#define CONGA_STATE_IDLE          1
#define CONGA_STATE_HIT           2
#define CONGA_STATE_MOPEY         3
#define CONGA_STATE_TARGET_GROUND 4
#define CONGA_STATE_BEAT_CHEST    5
#define CONGA_STATE_BEAT_CHEST_STOP 6
#define CONGA_STATE_TARGET_BANJO  7
#define CONGA_STATE_ROAR          8

// TRUE when the LOCAL player triggered Conga's defeat (3rd hit).
// Used to restrict camera cutscene + dialog to the attacker only.
static bool local_defeated_conga = FALSE;

// TRUE when the LOCAL player handed the orange to Chimpy. Gates the
// completion dialog + camera so remote ghosts don't see the cutscene
// when their MM_SPECIFIC_FLAG_2 receives the sync.
static bool local_chimpy_delivered = FALSE;

// TRUE when the LOCAL player's orange hit the final pad that triggered
// the JIGGY_8 spawn cutscene. Gates the camera/dialog/fanfare.
static bool local_orangepad_triggered = FALSE;

// Armed by the dialog-complete-anim event from the deliverer (or by
// the late-join FLAG_3 fallback). When TRUE, the chlmonkey state
// machine transitions STATE_4 -> STATE_3 to start the walking
// animation in lockstep with the deliverer closing their dialog.
static bool chimpy_remote_anim_armed = FALSE;

/* ============================================================
 * Helpers
 * ============================================================ */

/* Find Conga actor in suBaddieActorArray */
static Actor *find_conga_actor(void) {
    s32 i;
    if (!suBaddieActorArray) return (Actor *)0;
    for (i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (actor->marker && actor->marker->id == MARKER_7_CONGA) {
            return actor;
        }
    }
    return (Actor *)0;
}

/* ============================================================
 * Helpers: remote player checks
 * ============================================================ */

static bool is_in_sphere(Actor *center, f32 rx, f32 rz, f32 radius) {
    f32 dx = rx - center->position_x;
    f32 dz = rz - center->position_z;
    return (SQ(dx) + SQ(dz)) < SQ(radius);
}

/* Check if any player (local or remote) is within sphere */
static bool net_anyPlayerInSphere(Actor *actor, f32 radius) {
    if (subaddie_playerIsWithinSphereAndActive(actor, (s32)radius)) return TRUE;
    if (recomp_net_is_connected()) {
        u32 local_id = recomp_net_get_local_player_id();
        u32 cur_map = (u32)map_get();
        RemoteState rs;
        u32 i;
        for (i = 0; i < 4; i++) {
            if (i == local_id) continue;
            if (!recomp_net_get_remote_state(i, &rs)) continue;
            if (rs.map_id != cur_map) continue;
            if (is_in_sphere(actor, rs.x, rs.z, radius)) return TRUE;
        }
    }
    return FALSE;
}

/* Local-only tree check — used for scene-local dialogs that must not
 * trigger on remote machines when another player climbs the tree. */
static bool net_isLocalPlayerNearTree(Actor *this) {
    f32 plyr_pos[3];

    if (map_get() != MAP_2_MM_MUMBOS_MOUNTAIN) return FALSE;

    player_getPosition(plyr_pos);
    if (plyr_pos[1] >= 300.0f && plyr_pos[1] <= 600.0f) {
        if (SQ(plyr_pos[0] - CONGA_TREE_X) + SQ(plyr_pos[2] - CONGA_TREE_Z) < CONGA_TREE_RADIUS_SQ) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Replaces __chConga_isPlayerNearCongaTree — checks ALL players */
static bool net_isAnyPlayerNearTree(Actor *this) {
    f32 plyr_pos[3];

    if (map_get() != MAP_2_MM_MUMBOS_MOUNTAIN) return FALSE;
    if (!this->unk10_12) return FALSE;

    /* Check local player */
    player_getPosition(plyr_pos);
    if (plyr_pos[1] >= 300.0f && plyr_pos[1] <= 600.0f) {
        if (SQ(plyr_pos[0] - CONGA_TREE_X) + SQ(plyr_pos[2] - CONGA_TREE_Z) < CONGA_TREE_RADIUS_SQ) {
            return TRUE;
        }
    }

    /* Check remote players */
    if (recomp_net_is_connected()) {
        u32 local_id = recomp_net_get_local_player_id();
        u32 cur_map = (u32)map_get();
        RemoteState rs;
        u32 i;
        for (i = 0; i < 4; i++) {
            if (i == local_id) continue;
            if (!recomp_net_get_remote_state(i, &rs)) continue;
            if (rs.map_id != cur_map) continue;
            if (rs.y >= 300.0f && rs.y <= 600.0f) {
                if (SQ(rs.x - CONGA_TREE_X) + SQ(rs.z - CONGA_TREE_Z) < CONGA_TREE_RADIUS_SQ) {
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

/* Face Conga toward the nearest player (local or remote) */
static void net_conga_face_nearest(Actor *this) {
    f32 best_dist = 999999999.0f;
    f32 plyr_pos[3];
    s32 best_yaw;

    /* Local player */
    player_getPosition(plyr_pos);
    {
        f32 d = SQ(plyr_pos[0] - this->position_x) + SQ(plyr_pos[2] - this->position_z);
        if (d < best_dist) {
            best_dist = d;
            best_yaw = func_80329784(this); /* yaw toward local player */
        }
    }

    /* Remote players */
    if (recomp_net_is_connected()) {
        u32 local_id = recomp_net_get_local_player_id();
        u32 cur_map = (u32)map_get();
        RemoteState rs;
        u32 i;
        for (i = 0; i < 4; i++) {
            if (i == local_id) continue;
            if (!recomp_net_get_remote_state(i, &rs)) continue;
            if (rs.map_id != cur_map) continue;
            {
                f32 d = SQ(rs.x - this->position_x) + SQ(rs.z - this->position_z);
                if (d < best_dist) {
                    f32 target[3];
                    target[0] = rs.x; target[1] = rs.y; target[2] = rs.z;
                    best_dist = d;
                    best_yaw = func_803297C8(this, target);
                }
            }
        }
    }

    subaddie_set_ideal_yaw(this, best_yaw);
    func_80328FB0(this, 3.0f);
}

/* ============================================================
 * Replicated overlay functions (identical logic, core calls only)
 * ============================================================ */

/* __chConga_playRandomNoise */
static void net_conga_noise(void) {
    if ((globalTimer_getTime() & 0xF) == 0xB
        && 0.85 < randf()
        && !gcdialog_hasCurrentTextId()) {
        func_8030E58C(((s32)(randf() * 256.0f) & 1) ? SFX_22_KONGA_NOISE_1 : SFX_23_KONGA_NOISE_2, 1.0f);
    }
}

/* func_803872EC — dialog busy check */
static int net_conga_dialog_busy(void) {
    s32 text_id = gcdialog_getCurrentTextId();
    return text_id == ASSET_B37_DIALOG_CONGA_SAFE_UP_HERE
        || text_id == ASSET_B38_DIALOG_CONGA_DEFEAT
        || volatileFlag_get(VOLATILE_FLAG_1F_IN_CHARACTER_PARADE)
        || text_id == ASSET_B3B_DIALOG_CONGA_ORANGE_PAD_JIGGY
        || text_id == ASSET_B45_DIALOG_JIGGY_COLLECT_10
        || text_id == ASSET_B51_DIALOG_BOTTLES_HOW_TO_EXIT_LEVEL;
}

/* func_8038708C — set state + sound */
static void net_conga_set_state_sfx(Actor *this, s32 anim_id) {
    subaddie_set_state_with_direction(this, anim_id, 0.0f, 1);
    func_8030E58C(SFX_24_KONGA_NOISE_3, randf2(0.9f, 1.1f));
}

/* func_80387370 — defeat dialog callback.
 * Scene-local bits (camera 0x11, ending fade) only run on the defeater.
 * velocity_x is set unconditionally: for the defeater it feeds the local
 * countdown; for any non-defeater path that somehow reaches this callback,
 * velocity_x is redundant since apply_conga_hit already primed it. */
static void net_conga_defeat_dialog_cb(ActorMarker *this_marker, enum asset_e text_id, s32 arg2) {
    marker_getActor(this_marker)->velocity_x = 9.0f;
    if (recomp_net_is_connected() && !local_defeated_conga) {
        return;
    }
    timed_setStaticCameraToNode(0.0f, 0x11);
    timed_exitStaticCamera(3.2f);
    func_80324E38(3.2f, 0);
}

/* func_80387168 — Conga hit/damage callback */
static void net_conga_hit_callback(ActorMarker *marker, ActorMarker *other_marker) {
    Actor *actorPtr = marker_getActor(marker);

    if (((ActorLocal_Conga *)&actorPtr->local)->unkC == 1) {
        if (actorPtr->unk10_12 == 0) {
            ((ActorLocal_Conga *)&actorPtr->local)->unkC = 0;
            if (mapSpecificFlags_get(MM_SPECIFIC_FLAG_A_UNKNOWN))
                actorPtr->unk38_31++;

            actorPtr->unk10_12 = MIN(actorPtr->unk38_31, 0xA);
            if (actorPtr->unk38_31 == 3
                && !jiggyscore_isCollected(JIGGY_A_MM_CONGA)) {
                /* Race-condition guard: if we already applied a remote
                 * defeat (state=ROAR and local_defeated_conga still FALSE),
                 * do not overwrite with local camera/SFX. */
                if (recomp_net_is_connected()
                    && actorPtr->state == CONGA_STATE_ROAR
                    && !local_defeated_conga) {
                    /* Remote already applied the defeat; skip local
                     * camera/SFX to avoid double-firing. */
                } else {
                    local_defeated_conga = TRUE;
                    subaddie_set_state_with_direction(actorPtr, CONGA_STATE_ROAR, 0, 1);
                    timed_setStaticCameraToNode(0.0f, 0x10);
                    func_80324E38(0.0f, 3);
                    FUNC_8030E624(SFX_84_GOBI_CRYING, 0.8f, 32750);
                    FUNC_8030E624(SFX_84_GOBI_CRYING, 0.8f, 32750);
                }
            } else if (actorPtr->state != CONGA_STATE_MOPEY
                && actorPtr->state != CONGA_STATE_ROAR) {
                net_conga_set_state_sfx(actorPtr, CONGA_STATE_HIT);
                if (actorPtr->unk38_31 == 1) {
                    gcdialog_showDialog(ASSET_B39_DIALOG_CONGA_HIT_BY_EGG, 4,
                                         actorPtr->position, 0, 0, 0);
                }
            }

            /* Sync hit count to other players */
            if (recomp_net_is_connected()) {
                recomp_net_send_flag_change(NET_FLAG_CONGA_HIT,
                    (u32)actorPtr->unk38_31, (u32)actorPtr->unk10_12, (u32)map_get());
            }
        }
    }
}

/* func_803870D0 — partner setup on jiggy spawn */
static void net_conga_jiggy_partner(Actor *jiggy_actor, ActorMarker *conga_marker) {
    marker_getActor(conga_marker)->partnerActor = jiggy_actor->marker;
}

/* Forward declaration for throw trigger in chConga_update */
void __chConga_sendOrangeProjectile(ActorMarker *congaMarker);

/* ============================================================
 * RECOMP_PATCH: func_80387100 (Conga jiggy spawn on defeat)
 * Both sides spawn — collectible sync deduplicates.
 * When one player collects, the other's jiggy is despawned by sync.
 * ============================================================ */
RECOMP_PATCH void func_80387100(ActorMarker *thisMarker) {
    ActorMarker *m = *(ActorMarker **)&thisMarker;
    Actor *actorPtr;
    f32 position[3];

    /* Don't spawn if already collected via network sync (timing race guard) */
    if (jiggyscore_isCollected(JIGGY_A_MM_CONGA)) return;

    /* The jiggy spawn itself is shared across all players — everyone sees
     * the reward appear at Conga. Only the defeat dialog + camera cutscene
     * are scene-local (guarded in chConga_update / net_conga_defeat_dialog_cb). */
    actorPtr = marker_getActor(m);
    position[0] = actorPtr->position_x;
    position[1] = actorPtr->position_y + 60.0f;
    position[2] = actorPtr->position_z;
    bundle_setYaw(0.0f);
    func_80333270(JIGGY_A_MM_CONGA, position, net_conga_jiggy_partner, m);
}

/* ============================================================
 * RECOMP_PATCH: __chlmonkey_complete (Chimpy tree cutscene)
 * Camera + sound only for the local player nearby.
 * Ghost just gets the state transition + jiggy spawn.
 * ============================================================ */
RECOMP_PATCH void __chlmonkey_spawnJiggy(s32 x, s32 y, s32 z); // forward decl

RECOMP_PATCH void __chlmonkey_complete(ActorMarker *marker, enum asset_e unused_1, s32 unused_2) {
    Actor *actor = marker_getActor(marker);

    /* Deliverer just closed the completion dialog — broadcast the cue
     * so remote peers fire the leaving animation now instead of when
     * FLAG_2 first synced (~ several seconds earlier). Gated by
     * local_chimpy_delivered so the late-join branch (which calls this
     * function with the jiggy already collected) doesn't echo. */
    if (recomp_net_is_connected() && local_chimpy_delivered) {
        recomp_net_send_flag_change(NET_FLAG_DIALOG_COMPLETE_ANIM,
            NPC_DIALOG_ANIM_CHIMPY, 0, (u32)map_get());
    }

    mapSpecificFlags_set(MM_SPECIFIC_FLAG_4_SHAKE, TRUE);
    subaddie_set_state(actor, 3); // LMONKEY_STATE_3_WALKING

    // Jiggy spawn runs on all sides (collectible sync deduplicates)
    timedFunc_set_3(2.9f, (void *)__chlmonkey_spawnJiggy,
        (s32)actor->position_x, (s32)(actor->position_y + 150.0f), (s32)actor->position_z);

    // Camera cutscene only for the local player, not the ghost
    if (!recomp_net_is_connected() || subaddie_playerIsWithinSphereAndActive(actor, 700)) {
        timed_setStaticCameraToNode(2.3f, 0x12);
        timed_exitStaticCamera(4.3f);
        func_80324E38(4.3f, 0);
    }
}

/* ============================================================
 * RECOMP_PATCH: __chlmonkey_spawnJiggy (Chimpy jiggy spawn)
 * Both sides spawn — collectible sync deduplicates.
 * ============================================================ */
RECOMP_PATCH void __chlmonkey_spawnJiggy(s32 x, s32 y, s32 z) {
    f32 pos[3];
    if (jiggyscore_isCollected(JIGGY_9_MM_CHIMPY)) return;
    pos[0] = (f32)x;
    pos[1] = (f32)y;
    pos[2] = (f32)z;
    jiggy_spawn(JIGGY_9_MM_CHIMPY, pos);
}

/* ============================================================
 * RECOMP_PATCH: spawnJiggy (Orange Pad jiggy spawn)
 * Both sides spawn — collectible sync deduplicates.
 * ============================================================ */
RECOMP_PATCH void spawnJiggy(s32 x, s32 y, s32 z) {
    f32 pos[3];
    if (jiggyscore_isCollected(JIGGY_8_MM_ORANGE_PADS)) return;
    pos[0] = (f32)x;
    pos[1] = (f32)y;
    pos[2] = (f32)z;
    jiggy_spawn(JIGGY_8_MM_ORANGE_PADS, pos);
}

/* ============================================================
 * RECOMP_PATCH: __chjuju_spawnJiggy (Juju jiggy spawn)
 * Both sides spawn — collectible sync deduplicates.
 * ============================================================ */
RECOMP_PATCH void __chjuju_spawnJiggy(s32 x, s32 y, s32 z, s32 yaw) {
    f32 pos[3];
    if (jiggyscore_isCollected(JIGGY_4_MM_JUJU)) return;
    pos[0] = (f32)x;
    pos[1] = (f32)y + 20.0f;
    pos[2] = (f32)z;
    jiggy_spawn(JIGGY_4_MM_JUJU, pos);
}

/* ============================================================
 * RECOMP_PATCH: __chlmonkey_updateBringOrange (Chimpy delivery)
 * In multiplayer, any player can deliver the orange to Chimpy
 * once FLAG_1 (orange collected) is set — no need to carry it.
 * ============================================================ */
extern void player_setCarryObjectPoseInHorizontalRadius(f32 *, f32, s32, Actor **);
extern s32  bacarry_get_markerId(void);
extern s32  player_throwCarriedObject(void);
extern void func_8028FA34(s32, Actor *);

RECOMP_PATCH void __chlmonkey_updateBringOrange(Actor **this_ptr) {
    /* Original: try to make player carry orange within radius */
    player_setCarryObjectPoseInHorizontalRadius(
        (*this_ptr)->position, 800.0f,
        ACTOR_29_ORANGE_COLLECTIBLE, this_ptr);

    /* Original: player carrying orange, near Chimpy, and throws it */
    if (subaddie_playerIsWithinSphereAndActive(*this_ptr, 345)
        && bacarry_get_markerId() == MARKER_36_ORANGE_COLLECTIBLE
        && player_throwCarriedObject()) {
        func_8028FA34(0xc6, *this_ptr);
        (*this_ptr)->has_met_before = TRUE;
        local_chimpy_delivered = TRUE;
        timed_setStaticCameraToNode(1.2f, 0xF);
        func_80324E38(1.2f, 3);
        return;
    }

    /* MULTIPLAYER: if orange was collected (by any player) and local player
     * is near Chimpy, auto-deliver without needing to carry the orange. */
    if (recomp_net_is_connected()
        && mapSpecificFlags_get(MM_SPECIFIC_FLAG_1_ORANGE_HAS_BEEN_COLLECTED)
        && !mapSpecificFlags_get(MM_SPECIFIC_FLAG_2_ORANGE_HAS_BEEN_RETURNED)
        && !(*this_ptr)->has_met_before
        && subaddie_playerIsWithinSphereAndActive(*this_ptr, 345)) {
        func_8028FA34(0xc6, *this_ptr);
        (*this_ptr)->has_met_before = TRUE;
        local_chimpy_delivered = TRUE;
        timed_setStaticCameraToNode(1.2f, 0xF);
        func_80324E38(1.2f, 3);
        mapSpecificFlags_set(MM_SPECIFIC_FLAG_2_ORANGE_HAS_BEEN_RETURNED, TRUE);
    }
}

/* ============================================================
 * RECOMP_PATCH: chConga_update
 * Full replacement with remote player awareness in state transitions.
 * ============================================================ */
RECOMP_PATCH void chConga_update(Actor *this) {
    f32 unused;
    NodeProp *node_prop;
    s32 sp3C;

    this->marker->propPtr->unk8_3 = timedFuncQueue_is_empty(this) ? 1 : 0;

    /* --- Initialization (runs once) --- */
    if (!this->initialized) {
        ((ActorLocal_Conga *)&this->local)->unkC = 1;
        this->unk16C_0 = 1;
        this->initialized = TRUE;
        this->velocity_x = 0.0f;
        this->actor_specific_1_f = 0.0f;
        node_prop = nodeprop_findByActorIdAndActorPosition(0x150, this);
        ((ActorLocal_Conga *)&this->local)->unk1C = nodeprop_getRadius(node_prop);
        nodeprop_getPosition_s32(node_prop, &((ActorLocal_Conga *)&this->local)->unk10);
    }

    /* --- Orange collectible check --- */
    if (0.0f == this->actor_specific_1_f) {
        this->actor_specific_1_f = (actorArray_findActorFromMarkerId(MARKER_36_ORANGE_COLLECTIBLE) != NULL) ? 2.0f : 1.0f;
    }

    /* --- Jiggy countdown after defeat --- */
    /* velocity_x is set to 9.0 by the defeat dialog callback on the player
     * who defeated Conga, and by bkrecomp_net_apply_conga_hit on remote
     * clients, so the jiggy spawn runs on every machine. The dialog and
     * camera cues themselves stay scene-local. */
    if (0.0f != this->velocity_x) {
        this->velocity_x -= 1.0f;
        if (0.0f == this->velocity_x) {
            __spawnQueue_add_1((GenFunction_1)func_80387100, (s32)this->marker);
        }
    }

    /* --- Collision callback (uses our replicated hit handler) --- */
    marker_setCollisionScripts(this->marker, NULL, NULL, (MarkerCollisionFunc)net_conga_hit_callback);

    /* --- Range check: MODIFIED to include remote players --- */
    if (!net_anyPlayerInSphere(this, (f32)CONGA_ACTIVE_SPHERE)
        && this->state != CONGA_STATE_HIT
        && this->state != CONGA_STATE_ROAR) {

        if (this->state > 3 && this->state < 8) {
            actor_loopAnimation(this);
            subaddie_set_state_with_direction(this, CONGA_STATE_IDLE, 0.76f, 1);
        }
        return;
    }

    /* --- Throw sphere check: MODIFIED to include remote players --- */
    sp3C = net_anyPlayerInSphere(this, (f32)CONGA_THROW_SPHERE);

    /* --- Safe up here dialog --- */
    /* Scene-local: only fire when THIS machine's player is on the tree-top
     * platform. func_8032A9E4 is replaced with a tree-zone proximity check
     * (elevation + radius around the tree). The map flag still broadcasts
     * to activate Conga's ballistic mode on all clients. */
    if (!this->unk138_23
        && net_isLocalPlayerNearTree(this)
        && gcdialog_showDialog(ASSET_B37_DIALOG_CONGA_SAFE_UP_HERE, 0, 0, 0, 0, 0)) {
        this->unk138_23 = 1;
        mapSpecificFlags_set(MM_SPECIFIC_FLAG_A_UNKNOWN, TRUE);
    }

    /* --- First meeting dialog --- */
    /* Scene-local: each player sees their own first-meeting dialog only
     * when THEY enter Conga's throw sphere. Using sp3C here would leak
     * the dialog to remote players when only another client is nearby. */
    if (subaddie_playerIsWithinSphereAndActive(this, CONGA_THROW_SPHERE)
        && !this->has_met_before) {
        if (gcdialog_showDialog(
                (player_getTransformation() == TRANSFORM_2_TERMITE)
                    ? ASSET_B3E_DIALOG_CONGA_MEET_AS_TERMITE
                    : ASSET_B3C_DIALOG_CONGA_MEET,
                0, this->position, 0, 0, 0)) {
            this->has_met_before = TRUE;
        }
    }

    /* --- State machine --- */
    if (this->state == CONGA_STATE_IDLE) {
        actor_loopAnimation(this);
        net_conga_face_nearest(this);
        net_conga_noise();

        if (actor_animationIsAt(this, 0.0f) || actor_animationIsAt(this, 0.45f)) {
            if (randf() < 0.2f) {
                anctrl_setDirection(this->anctrl, anctrl_isPlayedForwards(this->anctrl) ? 0 : 1);
            }
        }

        if (actor_animationIsAt(this, 0.66f)) {
            subaddie_maybe_set_state_position_direction(this, CONGA_STATE_BEAT_CHEST_STOP, 0, 1, 0.38f);
        }

        /* IDLE → TARGET_GROUND: any player in sphere, not near tree */
        /* NOTE: func_8032A9E4 (platform check) omitted — it has calling convention
         * issues when compiled with LLVM cross-compiler (always returns 1).
         * The isNearTree check already handles the TARGET_BANJO case. */
        if (sp3C
            && player_movementGroup() != BSGROUP_1_INTR
            && !net_isAnyPlayerNearTree(this)
            && timedFuncQueue_is_empty()
            && !net_conga_dialog_busy()) {
            subaddie_set_state_with_direction(this, CONGA_STATE_TARGET_GROUND, 0.0f, 1);
        }

        /* IDLE → TARGET_BANJO: any player near tree */
        if (player_movementGroup() != BSGROUP_1_INTR
            && net_isAnyPlayerNearTree(this)
            && this->unk38_31 != 0
            && !net_conga_dialog_busy()) {
            subaddie_set_state_with_direction(this, CONGA_STATE_TARGET_BANJO, 0.0f, 1);
        }

    } else if (this->state == CONGA_STATE_BEAT_CHEST_STOP) {
        ((ActorLocal_Conga *)&this->local)->unkC = 1;
        actor_playAnimationOnce(this);
        net_conga_noise();

        if (anctrl_isPlayedForwards(this->anctrl) == TRUE
            && actor_animationIsAt(this, 0.0f)) {
            subaddie_set_state_with_direction(this, CONGA_STATE_BEAT_CHEST, 0.0f, 1);
        } else if (!anctrl_isPlayedForwards(this->anctrl)
            && actor_animationIsAt(this, 0.001f)) {
            subaddie_set_state_with_direction(this, CONGA_STATE_IDLE, 0.76f, 1);
        }

    } else if (this->state == CONGA_STATE_BEAT_CHEST) {
        ((ActorLocal_Conga *)&this->local)->unkC = 1;
        actor_loopAnimation(this);
        net_conga_noise();

        if (actor_animationIsAt(this, 0.99f)) {
            subaddie_maybe_set_state_position_direction(this, CONGA_STATE_BEAT_CHEST_STOP, 0.999f, 0, sp3C ? 1.0f : 0.4f);
        }

        if (actor_animationIsAt(this, 0.9f) || actor_animationIsAt(this, 0.4f)) {
            func_8030E6D4(SFX_3FB_UNKNOWN);
        }

    } else if (this->state == CONGA_STATE_TARGET_GROUND) {
        if (actor_animationIsAt(this, 0.6f)) {
            func_8030E58C(SFX_2_CLAW_SWIPE, 0.7f);
        }

        net_conga_face_nearest(this);

        /* Exit TARGET_GROUND: MODIFIED — stay if any player still in zone */
        if (!sp3C
            || player_is_in_jiggy_jig()
            || net_isAnyPlayerNearTree(this)
            || !timedFuncQueue_is_empty()
            || net_conga_dialog_busy()) {
            subaddie_set_state_with_direction(this, CONGA_STATE_IDLE, 0.0f, 1);
        }

    } else if (this->state == CONGA_STATE_HIT) {
        actor_playAnimationOnce(this);
        if (actor_animationIsAt(this, 0.99f)) {
            subaddie_set_state_with_direction(this, CONGA_STATE_IDLE, 0.0f, 1);
        }

    } else if (this->state == CONGA_STATE_ROAR) {
        actor_playAnimationOnce(this);
        if (actor_animationIsAt(this, 0.99f)) {
            subaddie_set_state_with_direction(this, CONGA_STATE_MOPEY, 0.0f, 1);
            // Only show defeat dialog + camera cutscene for the player who hit Conga,
            // or in single-player. Ghost players just see the state transition.
            if (!recomp_net_is_connected() || local_defeated_conga) {
                gcdialog_showDialog(ASSET_B38_DIALOG_CONGA_DEFEAT, 0xe, this->position,
                                     this->marker, (void *)net_conga_defeat_dialog_cb, NULL);
            }
        }

    } else if (this->state == CONGA_STATE_MOPEY) {
        actor_loopAnimation(this);
        if (jiggyscore_isCollected(JIGGY_A_MM_CONGA)) {
            subaddie_set_state_with_direction(this, CONGA_STATE_IDLE, 0.0f, 1);
        }

    } else if (this->state == CONGA_STATE_TARGET_BANJO) {
        if (this->unk10_12 == 0) {
            if (actor_animationIsAt(this, 0.97f)) {
                ((ActorLocal_Conga *)&this->local)->unkC = 1;
                subaddie_set_state_with_direction(this, CONGA_STATE_BEAT_CHEST_STOP, 0.0f, 1);
            }
        }
    }

    /* --- Throw trigger (same as original) --- */
    if ((this->state == CONGA_STATE_TARGET_GROUND && actor_animationIsAt(this, 0.56f))
        || (this->state == CONGA_STATE_TARGET_BANJO && actor_animationIsAt(this, 0.468f))) {
        func_8034A1B4(this->marker->unk44, 5, &this->local);
        __spawnQueue_add_1((GenFunction_1)__chConga_sendOrangeProjectile, (s32)this->marker);
    }
}

/* ============================================================
 * RECOMP_PATCH: __chConga_sendOrangeProjectile
 * Owner: spawn oranges for local + remote players, send events.
 * Non-owner: no-op (receives via network events).
 * ============================================================ */

static void compute_orange_velocity(Actor *orange, f32 target_pos[3],
                                     s32 conga_state, f32 conga_prop_x, f32 conga_prop_z)
{
    orange->velocity_x = target_pos[0] - orange->position_x;
    orange->velocity_y = (60.0f) * ((conga_state == CONGA_STATE_TARGET_BANJO) ? 0.5f : 1.0f);
    orange->velocity_z = target_pos[2] - orange->position_z;

    if (SQ(target_pos[2] - conga_prop_z) + SQ(target_pos[0] - conga_prop_x) < 40000.0f) {
        f32 temp_f20 = randf2(2.4f, 4.4f);
        f32 temp_f22 = randf2(2.4f, 4.4f);
        orange->velocity[0] *= (randf() < 0.5f) ? temp_f20 : -temp_f20;
        orange->velocity[1] = randf2(1.8f, 2.2f) * 60.0f;
        orange->velocity[2] *= (randf() < 0.5f) ? temp_f22 : -temp_f22;
    }

    {
        f32 sim_y = orange->position_y;
        f32 sim_vy = orange->velocity_y;
        f32 sim_count;
        for (sim_count = 0.0f; !(sim_y < target_pos[1] && sim_vy < 0.0f); sim_count += 1.0f) {
            sim_vy -= 5.0f;
            sim_y += sim_vy;
        }
        if (sim_count > 0.0f) {
            orange->velocity_x /= sim_count;
            orange->velocity_z /= sim_count;
        }
    }
}

static void spawn_orange_at_target(ActorLocal_Conga *conga_local, Actor *conga,
                                    f32 target_pos[3], s32 throw_type, u32 cur_map)
{
    Actor *orangePtr = actor_spawnWithYaw_s32(ACTOR_14_ORANGE_PROJECTILE,
                                               conga_local->orangeSpawnPosition,
                                               conga->yaw);
    if (orangePtr != NULL) {
        f32 spawn_pos[3], vel[3];

        compute_orange_velocity(orangePtr, target_pos, throw_type,
                                 conga->marker->propPtr->x, conga->marker->propPtr->z);

        spawn_pos[0] = orangePtr->position_x;
        spawn_pos[1] = orangePtr->position_y;
        spawn_pos[2] = orangePtr->position_z;
        vel[0] = orangePtr->velocity_x;
        vel[1] = orangePtr->velocity_y;
        vel[2] = orangePtr->velocity_z;
        recomp_net_send_conga_orange(spawn_pos, vel, cur_map);
    }
}

RECOMP_PATCH void __chConga_sendOrangeProjectile(ActorMarker *congaMarker) {
    ActorMarker *m = *(ActorMarker **)&congaMarker;
    Actor *congaPtr = marker_getActor(m);
    ActorLocal_Conga *conga_localPtr = (ActorLocal_Conga *)&congaPtr->local;
    s32 conga_state = congaPtr->state;
    u32 cur_map;
    u32 i;

    /* Non-owner: bail — oranges come from network events */
    if (recomp_net_is_connected() && !recomp_net_am_i_world_owner((u32)level_get())) {
        return;
    }

    /* Original: decrement hit counter in TARGET_BANJO */
    congaPtr->unk10_12 -= (congaPtr->unk10_12 && (conga_state == CONGA_STATE_TARGET_BANJO));
    congaPtr->actor_specific_1_f = 2.0f;
    cur_map = (u32)map_get();

    if (conga_state == CONGA_STATE_TARGET_BANJO) {
        /*
         * TARGET_BANJO (tree-top fight): fire ONE orange at the nearest player
         * in the tree zone. Shared boss fight — hits count for everyone.
         */
        f32 best_target[3];
        f32 best_dist = 999999999.0f;
        bool found = FALSE;

        /* Check local player */
        {
            f32 plyr_pos[3];
            player_getPosition(plyr_pos);
            if (plyr_pos[1] >= 300.0f && plyr_pos[1] <= 600.0f
                && (SQ(plyr_pos[0] - CONGA_TREE_X) + SQ(plyr_pos[2] - CONGA_TREE_Z)) < CONGA_TREE_RADIUS_SQ) {
                f32 d = SQ(plyr_pos[0] - congaPtr->position_x) + SQ(plyr_pos[2] - congaPtr->position_z);
                if (d < best_dist) {
                    best_dist = d;
                    best_target[0] = plyr_pos[0];
                    best_target[1] = plyr_pos[1];
                    best_target[2] = plyr_pos[2];
                    found = TRUE;
                }
            }
        }

        /* Check remote players */
        if (recomp_net_is_connected()) {
            u32 local_id = recomp_net_get_local_player_id();
            RemoteState rs;
            for (i = 0; i < 4; i++) {
                if (i == local_id) continue;
                if (!recomp_net_get_remote_state(i, &rs)) continue;
                if (rs.map_id != cur_map) continue;
                if (rs.y < 300.0f || rs.y > 600.0f) continue;
                if (SQ(rs.x - CONGA_TREE_X) + SQ(rs.z - CONGA_TREE_Z) >= CONGA_TREE_RADIUS_SQ) continue;
                {
                    f32 d = SQ(rs.x - congaPtr->position_x) + SQ(rs.z - congaPtr->position_z);
                    if (d < best_dist) {
                        best_dist = d;
                        best_target[0] = rs.x;
                        best_target[1] = rs.y;
                        best_target[2] = rs.z;
                        found = TRUE;
                    }
                }
            }
        }

        if (found) {
            spawn_orange_at_target(conga_localPtr, congaPtr, best_target, conga_state, cur_map);
        }
    } else {
        /*
         * TARGET_GROUND: fire at each player in the throw sphere independently.
         */

        /* Local player */
        {
            f32 plyr_pos[3];
            player_getPosition(plyr_pos);
            if (is_in_sphere(congaPtr, plyr_pos[0], plyr_pos[2], (f32)CONGA_THROW_SPHERE)) {
                spawn_orange_at_target(conga_localPtr, congaPtr, plyr_pos, conga_state, cur_map);
            }
        }

        /* Remote players */
        if (recomp_net_is_connected()) {
            u32 local_id = recomp_net_get_local_player_id();
            RemoteState rs;
            for (i = 0; i < 4; i++) {
                if (i == local_id) continue;
                if (!recomp_net_get_remote_state(i, &rs)) continue;
                if (rs.map_id != cur_map) continue;
                if (!is_in_sphere(congaPtr, rs.x, rs.z, (f32)CONGA_THROW_SPHERE)) continue;
                {
                    f32 remote_target[3];
                    remote_target[0] = rs.x;
                    remote_target[1] = rs.y;
                    remote_target[2] = rs.z;
                    spawn_orange_at_target(conga_localPtr, congaPtr, remote_target, conga_state, cur_map);
                }
            }
        }
    }
}

/* ============================================================
 * Receive Conga hit sync from remote player.
 * Called from bkrecomp_net_process_flag_event in network_flag_sync.c
 * ============================================================ */
RECOMP_EXPORT void bkrecomp_net_apply_conga_hit(u32 remote_unk38, u32 remote_unk10) {
    Actor *conga = find_conga_actor();
    if (!conga) return;

    /* Only apply if remote hit count is higher (monotonic — prevents revert) */
    if ((s32)remote_unk38 <= conga->unk38_31) return;

    conga->unk38_31 = (s32)remote_unk38;
    conga->unk10_12 = (s32)remote_unk10;
    ((ActorLocal_Conga *)&conga->local)->unkC = 0;

    if (conga->unk38_31 >= 3 && !jiggyscore_isCollected(JIGGY_A_MM_CONGA)) {
        /* Defeat — update state/animation but skip camera + SFX for remote player */
        subaddie_set_state_with_direction(conga, CONGA_STATE_ROAR, 0, 1);
        /* Shared jiggy spawn: mirror velocity_x=9 here so the countdown runs
         * on remote clients too and func_80387100 spawns the jiggy for
         * everyone. The ROAR-state dialog + camera still only fire for the
         * defeater (gated by local_defeated_conga in chConga_update). */
        conga->velocity_x = 9.0f;
    } else if (conga->state != CONGA_STATE_MOPEY && conga->state != CONGA_STATE_ROAR) {
        /* Hit reaction — just update state, no SFX on remote side */
        subaddie_set_state_with_direction(conga, CONGA_STATE_HIT, 0, -1);
    }
}

/* ============================================================
 * Per-frame: non-owner spawns oranges from network events.
 * Called from bkrecomp_net_sync_frame.
 * ============================================================ */
RECOMP_EXPORT void bkrecomp_net_process_conga_oranges(void) {
    CongaOrangeEvent evt;

    if (!recomp_net_is_connected()) return;
    if (recomp_net_am_i_world_owner((u32)level_get())) return;

    while (recomp_net_pop_conga_orange(&evt)) {
        s32 spawn_pos_s32[3];
        Actor *orangePtr;

        spawn_pos_s32[0] = (s32)evt.spawn_x;
        spawn_pos_s32[1] = (s32)evt.spawn_y;
        spawn_pos_s32[2] = (s32)evt.spawn_z;

        orangePtr = actor_spawnWithYaw_s32(ACTOR_14_ORANGE_PROJECTILE, spawn_pos_s32, 0);
        if (orangePtr != NULL) {
            orangePtr->velocity_x = evt.vel_x;
            orangePtr->velocity_y = evt.vel_y;
            orangePtr->velocity_z = evt.vel_z;
        }
    }
}

/* ============================================================
 * RECOMP_PATCH: chlmonkey_update (Chimpy state machine)
 * Scene-local: completion dialog + camera only fire on the player who
 * delivered the orange. Remote clients still see Chimpy walk away and
 * the jiggy spawn (shared). Meeting dialog already local by radius.
 * ============================================================ */
extern void func_8028E668(f32 *pos, f32 a, f32 b, f32 c);
extern void actor_collisionOff(Actor *);
extern void func_80343DEC(Actor *);
extern f32  func_8032970C(Actor *);
extern f32  ml_map_f(f32, f32, f32, f32, f32);
extern s32  item_getCount(enum item_e item);

#define LMONKEY_STATE_1_IDLE    1
#define LMONKEY_STATE_2_JUMPING 2
#define LMONKEY_STATE_3_WALKING 3
#define LMONKEY_STATE_4_LEAVING 4

/* Replica of __chlmonkey_playRandomNoise — overlay fn not callable by name. */
static void net_chlmonkey_noise(Actor *this) {
    static s32 cooldown = 0;
    f32 vol = ml_map_f(func_8032970C(this), 1000000.0f, 343000000.0f, 18000.0f, 0.0f);
    f32 r = randf();
    cooldown--;
    if (cooldown < 0 && randf() < 0.2f) {
        cooldown = 6;
        gcsfx_playWithPitch((r < 0.5f) ? SFX_58_CHIMPY_NOISE_1 : SFX_59_CHIMPY_NOISE_2,
                             randf() * 0.25f + 0.85f, vol);
    }
}

RECOMP_PATCH void chlmonkey_update(Actor *this) {
    func_8028E668(this->position, 35.0f, 0.0f, 65.0f);
    actor_collisionOff(this);
    this->marker->propPtr->unk8_3 = 1;

    if (map_get() != MAP_2_MM_MUMBOS_MOUNTAIN) {
        func_80343DEC(this);
        return;
    }

    if (subaddie_playerIsWithinSphereAndActive(this, 700) && !gcdialog_hasCurrentTextId()) {
        net_chlmonkey_noise(this);
    }

    switch (this->state) {
        case LMONKEY_STATE_1_IDLE:
            if (mapSpecificFlags_get(MM_SPECIFIC_FLAG_2_ORANGE_HAS_BEEN_RETURNED)) {
                subaddie_set_state(this, LMONKEY_STATE_4_LEAVING);

                if (!jiggyscore_isCollected(JIGGY_9_MM_CHIMPY)) {
                    if (!recomp_net_is_connected() || local_chimpy_delivered) {
                        /* Local deliverer: show the completion dialog. The
                         * __chlmonkey_complete callback (already patched)
                         * gates its own camera by radius 700. */
                        gcdialog_showDialog(ASSET_B40_DIALOG_CHIMPY_COMPLETE, 0xe,
                                             this->position, this->marker,
                                             (void *)__chlmonkey_complete, NULL);
                    } else {
                        /* Remote ghost: stay in STATE_4_LEAVING (idle
                         * leaving stance) until the deliverer broadcasts
                         * NET_FLAG_DIALOG_COMPLETE_ANIM at the moment they
                         * close the completion dialog. STATE_4 case below
                         * will transition to STATE_3_WALKING + spawn the
                         * jiggy when chimpy_remote_anim_armed flips TRUE.
                         *
                         * Late-join fallback: if FLAG_3_HAS_LEAVED is
                         * already set, the deliverer's animation is past
                         * the dialog (FLAG_3 latches at unk48 >= 0.24,
                         * after STATE_3 starts). The event for that
                         * transition has already passed, so arm
                         * immediately to catch up. */
                        if (mapSpecificFlags_get(MM_SPECIFIC_FLAG_3_CHIMPY_HAS_LEAVED)) {
                            chimpy_remote_anim_armed = TRUE;
                        }
                    }
                } else {
                    /* Jiggy already collected (late-join case). Trigger the
                     * leave sequence directly. __chlmonkey_complete will
                     * self-gate its camera by radius. */
                    __chlmonkey_complete(this->marker, ASSET_B40_DIALOG_CHIMPY_COMPLETE, -1);
                }
            } else {
                __chlmonkey_updateBringOrange(&this);

                if (subaddie_playerIsWithinSphereAndActive(this, 345)
                    && !subaddie_playerIsWithinSphereAndActive(this, 150)
                    && !item_getCount(ITEM_19_ORANGE)
                    && !this->has_met_before) {
                    gcdialog_showDialog(ASSET_B3F_DIALOG_CHIMPY_MEET, 0xe,
                                         this->position, NULL, NULL, NULL);
                    this->has_met_before = TRUE;
                }

                actor_loopAnimation(this);
                subaddie_maybe_set_state_position_direction(this, LMONKEY_STATE_2_JUMPING, 0.0f, -1, 0.02f);
            }
            break;

        case LMONKEY_STATE_2_JUMPING:
            __chlmonkey_updateBringOrange(&this);
            actor_playAnimationOnce(this);
            if (actor_animationIsAt(this, 0.99f)) {
                subaddie_set_state_with_direction(this, LMONKEY_STATE_1_IDLE, 0.0f, -1);
            }
            break;

        case LMONKEY_STATE_4_LEAVING:
            actor_loopAnimation(this);
            /* Remote ghost: arm flips TRUE on dialog-complete event from
             * the deliverer (or via the FLAG_3 late-join fallback above).
             * Mirror the world side-effects of __chlmonkey_complete here
             * minus the camera/input lock. The deliverer reaches STATE_3
             * directly via __chlmonkey_complete and never needs this
             * branch — gate by !local_chimpy_delivered just to be safe
             * against double-firing if both signals coincide. */
            if (chimpy_remote_anim_armed && !local_chimpy_delivered) {
                chimpy_remote_anim_armed = FALSE;
                mapSpecificFlags_set(MM_SPECIFIC_FLAG_4_SHAKE, TRUE);
                subaddie_set_state(this, LMONKEY_STATE_3_WALKING);
                timedFunc_set_3(2.9f, (void *)__chlmonkey_spawnJiggy,
                    (s32)this->position_x,
                    (s32)(this->position_y + 150.0f),
                    (s32)this->position_z);
            }
            break;

        case LMONKEY_STATE_3_WALKING:
            func_80343DEC(this);
            actor_loopAnimation(this);
            /* Progress thresholds gate the stump raise, the leave flag and
             * the despawn. They read this->unk48 (the chlmonkey-specific
             * progress counter), NOT velocity — those are different fields
             * in the actor struct. */
            if (0.19f <= this->unk48) {
                mapSpecificFlags_set(MM_SPECIFIC_FLAG_0_CHIMPY_STUMP_RAISED, TRUE);
            }
            if (0.24f <= this->unk48) {
                mapSpecificFlags_set(MM_SPECIFIC_FLAG_3_CHIMPY_HAS_LEAVED, TRUE);
            }
            if (0.99f <= this->unk48) {
                marker_despawn(this->marker);
            }
            break;
    }
}

/* ============================================================
 * RECOMP_PATCH: handleOrangeCollision (orange vs pad collision)
 * Scene-local: the jiggy-spawn cutscene camera + dialog + fanfare
 * fire only when the LOCAL player is inside Conga's arena at the
 * moment the final pad is hit. Other clients still see the jiggy
 * appear (shared via func_80387100 equivalent here: spawnJiggy).
 * ============================================================ */
extern void gcStaticCamera_activate(s32);
extern void particleEmitter_setModel(ParticleEmitter *, enum asset_e);

RECOMP_PATCH void handleOrangeCollision(ActorMarker *marker) {
    f32 distance_to_orange_pad;
    Actor *closest_orange_pad;
    f32 position[3];
    ParticleEmitter *p_ctrl;
    s32 camera_id;

    position[0] = marker->propPtr->x;
    position[1] = marker->propPtr->y;
    position[2] = marker->propPtr->z;

    closest_orange_pad = actorArray_findClosestActorFromActorId(
        position, ACTOR_57_ORANGE_PAD, 1 /*ORANGE_PAD_STATE_HIT*/, &distance_to_orange_pad);
    if (!closest_orange_pad || 500.0f < distance_to_orange_pad) {
        return;
    }

    closest_orange_pad->state = 1; /* ORANGE_PAD_STATE_HIT */

    if (actorArray_findClosestActorFromActorId(position, ACTOR_57_ORANGE_PAD, 1, &distance_to_orange_pad)) {
        /* More pads remaining — progress ding (shared audio is fine) */
        coMusicPlayer_playMusic(COMUSIC_2B_DING_B, 22000);
    } else {
        /* Last pad — dispense the jiggy.
         * Scene-local: the static camera, completion dialog and the
         * puzzle-solved fanfare run only on the player whose local
         * orange physics actually triggered this. In our networked
         * setup, the host (world owner) is the authoritative simulator;
         * the non-owner only spawns oranges from events (which still
         * trigger handleOrangeCollision locally when the replicated
         * orange hits a pad). Gate by proximity to the pad so whoever
         * is present in Conga's arena sees the cutscene. */
        bool local_in_arena = subaddie_playerIsWithinSphereAndActive(closest_orange_pad, 2500);

        if (!recomp_net_is_connected() || local_in_arena) {
            camera_id = (closest_orange_pad->secondaryId == 2 /*ORANGE_PAD_RIGHT*/) ? 0x10 /*JIGGY_SPAWN_RIGHT*/
                      : (closest_orange_pad->secondaryId == 1 /*ORANGE_PAD_LEFT*/)  ? 0xF  /*JIGGY_SPAWN_LEFT*/
                                                                                    : 0xE; /*JIGGY_SPAWN_TOP*/
            gcStaticCamera_activate(camera_id);
            coMusicPlayer_playMusic(COMUSIC_2D_PUZZLE_SOLVED_FANFARE, 0x7FFF);
            if (!jiggyscore_isCollected(JIGGY_8_MM_ORANGE_PADS)) {
                gcdialog_showDialog(ASSET_B3B_DIALOG_CONGA_ORANGE_PAD_JIGGY, 4,
                                     NULL, NULL, NULL, NULL);
            }
            local_orangepad_triggered = TRUE;
        }

        /* Jiggy spawn is shared: every client sees the reward appear. */
        position[1] += 50.0f;
        timedFunc_set_3(0.6f, (GenFunction_3)spawnJiggy,
                         (s32)position[0], (s32)position[1], (s32)position[2]);
    }

    /* Orange particles are pure visual — run on every client. */
    p_ctrl = partEmitMgr_newEmitter(30 /*ORANGE_PARTICLE_COUNT*/);
    particleEmitter_setPosition(p_ctrl, closest_orange_pad->position);
    particleEmitter_setModel(p_ctrl, ASSET_89F_MODEL_ORANGE_PARTICLE);
    particleEmitter_setStartingScaleRange(p_ctrl, 0.09f, 0.19f);
    particleEmitter_setFinalScaleRange(p_ctrl, 0.0f, 0.0f);
    particleEmitter_setParticleVelocityRange(p_ctrl, -200.0f, 500.0f, -200.0f, 200.0f, 700.0f, 200.0f);
    particleEmitter_setAccelerationRange(p_ctrl, 0.0f, -1200.0f, 0.0f, 0.0f, -1200.0f, 0.0f);
    particleEmitter_setAngularVelocityRange(p_ctrl, -600.0f, -600.0f, -600.0f, 600.0f, 600.0f, 600.0f);
    particleEmitter_setSpawnIntervalRange(p_ctrl, 0.0f, 0.01f);
    particleEmitter_setParticleLifeTimeRange(p_ctrl, 4.0f, 4.0f);
    particleEmitter_func_802EF9F8(p_ctrl, 0.01f);
    particleEmitter_func_802EFA18(p_ctrl, 3);
    particleEmitter_func_802EFA20(p_ctrl, 1.0f, 1.3f);
    particleEmitter_emitN(p_ctrl, 30);
}

/* ============================================================
 * Receive: dialog-complete animation cue from a remote peer.
 * Routed by bkrecomp_net_process_flag_event in network_flag_sync.c
 * (NET_FLAG_DIALOG_COMPLETE_ANIM = 17).
 *
 * Per-NPC dispatch on flag_index. Currently only Chimpy uses this
 * channel; other NPCs (Mumbo, Tanktup, Boggy, Trunker, etc.) can
 * register their own tags and arm logic without changing the
 * transport layer.
 * ============================================================ */
RECOMP_EXPORT void bkrecomp_net_dialog_complete_anim_remote_apply(u32 npc_id, u32 value) {
    (void)value;
    if (npc_id == NPC_DIALOG_ANIM_CHIMPY) {
        /* The chlmonkey state machine consumes the arm next frame from
         * STATE_4_LEAVING. Idempotent: a duplicate event before the
         * state machine consumes it just keeps the flag TRUE. */
        chimpy_remote_anim_armed = TRUE;
    }
}
