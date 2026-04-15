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

/* func_80387370 — defeat dialog callback */
static void net_conga_defeat_dialog_cb(ActorMarker *this_marker, enum asset_e text_id, s32 arg2) {
    marker_getActor(this_marker)->velocity_x = 9.0f;
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
                subaddie_set_state_with_direction(actorPtr, CONGA_STATE_ROAR, 0, 1);
                timed_setStaticCameraToNode(0.0f, 0x10);
                func_80324E38(0.0f, 3);
                FUNC_8030E624(SFX_84_GOBI_CRYING, 0.8f, 32750);
                FUNC_8030E624(SFX_84_GOBI_CRYING, 0.8f, 32750);
            } else if (actorPtr->state != CONGA_STATE_MOPEY
                && actorPtr->state != CONGA_STATE_ROAR) {
                net_conga_set_state_sfx(actorPtr, CONGA_STATE_HIT);
                if (actorPtr->unk38_31 == 1) {
                    gcdialog_showDialog(ASSET_B39_DIALOG_CONGA_HIT_BY_EGG, 4,
                                         actorPtr->position, 0, 0, 0);
                }
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
 * RECOMP_PATCH: func_80387100 (jiggy spawn on Conga defeat)
 * Both sides spawn the jiggy — collectible sync deduplicates.
 * ============================================================ */
RECOMP_PATCH void func_80387100(ActorMarker *thisMarker) {
    ActorMarker *m = *(ActorMarker **)&thisMarker;
    Actor *actorPtr;
    f32 position[3];

    actorPtr = marker_getActor(m);
    position[0] = actorPtr->position_x;
    position[1] = actorPtr->position_y + 60.0f;
    position[2] = actorPtr->position_z;
    bundle_setYaw(0.0f);
    func_80333270(JIGGY_A_MM_CONGA, position, net_conga_jiggy_partner, m);
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
    /* NOTE: func_8032A9E4 check omitted (calling convention mismatch).
     * unk138_23 guards this to run at most once anyway. */
    if (!this->unk138_23
        && gcdialog_showDialog(ASSET_B37_DIALOG_CONGA_SAFE_UP_HERE, 0, 0, 0, 0, 0)) {
        this->unk138_23 = 1;
        mapSpecificFlags_set(MM_SPECIFIC_FLAG_A_UNKNOWN, TRUE);
    }

    /* --- First meeting dialog --- */
    if (sp3C && !this->has_met_before) {
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
            gcdialog_showDialog(ASSET_B38_DIALOG_CONGA_DEFEAT, 0xe, this->position,
                                 this->marker, (void *)net_conga_defeat_dialog_cb, NULL);
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
        recomp_printf("[CONGA-DBG] THROW TRIGGER! state=%d\n", this->state);
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

    /* Spawn orange for LOCAL player — only if in Conga's throw sphere */
    {
        f32 plyr_pos[3];
        player_getPosition(plyr_pos);
        if (is_in_sphere(congaPtr, plyr_pos[0], plyr_pos[2], (f32)CONGA_THROW_SPHERE)) {
            spawn_orange_at_target(conga_localPtr, congaPtr, plyr_pos, conga_state, cur_map);
        }
    }

    /* Spawn oranges for REMOTE players in zone */
    if (recomp_net_is_connected()) {
        u32 local_id = recomp_net_get_local_player_id();
        RemoteState rs;

        for (i = 0; i < 4; i++) {
            if (i == local_id) continue;
            if (!recomp_net_get_remote_state(i, &rs)) continue;
            if (rs.map_id != cur_map) continue;
            if (!is_in_sphere(congaPtr, rs.x, rs.z, (f32)CONGA_THROW_SPHERE)) continue;

            if (conga_state == CONGA_STATE_TARGET_BANJO) {
                if (rs.y < 300.0f || rs.y > 600.0f) continue;
                if (SQ(rs.x - CONGA_TREE_X) + SQ(rs.z - CONGA_TREE_Z) >= CONGA_TREE_RADIUS_SQ) continue;
            }

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
