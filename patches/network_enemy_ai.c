// Enemy AI multiplayer awareness patches.
//
// Problem: BK's enemy AI uses player_getPosition() and core1_ce60_*
// helpers, both of which return the LOCAL player's coordinates. In
// multiplayer, if a remote player walks next to an enemy, the owner
// sees the enemy stay idle because, from the owner's perspective, no
// "player" is nearby. Remote players see animations play but no
// movement, because the owner broadcasts static positions.
//
// Status: PARTIAL fix. Aggro detection works across all players.
// Chase targeting still locks onto the local Banjo (see TODO below).
//
//   1. core1_ce60_isPlayerInRange(x, z, distance)  — ACTIVE
//      Returns TRUE if ANY player (local or remote in the same map) is
//      within `distance` of (x, z). Replaces the original check that
//      only looked at the local player. Safe because this helper is
//      only used by enemy / zone-trigger code.
//
//   2. core1_ce60_getPlayerDistance(x, z)  — ACTIVE
//      Returns the XZ distance from (x, z) to the CLOSEST player
//      (local + remote). Same safety argument as above.
//
//   3. player_getPosition(dst[3])  — ACTIVE but currently a no-op
//      This one is also used by camera, HUD, bottles, etc. — so we
//      can't redirect it globally or the camera would chase remote
//      players. We gate the redirect with g_current_ai_actor, set by
//      the patched actor-update dispatcher around each update call.
//      The dispatcher patch (#4) is DISABLED right now, so the flag is
//      never set and this falls through to vanilla every time.
//
//   4. func_803268B4 (actor-update dispatcher)  — DISABLED (#if 0)
//      Copy of the vanilla body with g_current_ai_actor set/clear
//      around actorUpdateFunc / actorUpdate2Func calls. Enabling it
//      caused a boot-time black screen on the join even though the
//      only logical additions were two assignments. Root cause not
//      identified yet — possible Actor struct field mismatch or an
//      extern type discrepancy breaks one of the vanilla helpers.
//
// TODO [ENEMY-AI-CHASE]: Re-enable the dispatcher patch so enemies
// chase the closest player, not just aggro on them. Options to explore:
//   - Narrow the dispatcher patch to only set the flag for killable
//     enemies (skip props, NPCs, bundles) to rule out interference.
//   - Instead of patching the dispatcher, patch actor_update_func_80326224
//     or a more-specific mid-level helper that only enemy AIs hit.
//   - Full per-object ownership migration (bigger architectural change).

#include "patches.h"
#include "functions.h"
#include "enums.h"
#include "prop.h"

// RemoteState layout must match net_recomp_api.cpp's recomp_net_get_remote_state
// output (same as NetFullState in network_patches.c).
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

// Network bridges
extern u32 recomp_net_is_connected(void);
extern u32 recomp_net_get_remote_count(void);
extern u32 recomp_net_get_remote_state(u32 player_id, RemoteState *out);

// Vanilla BK helpers we still need to call.
extern void _player_getPosition(f32 dst[3]);
extern float gu_sqrtf(float val);
extern enum map_e map_get(void);

// ---- Dispatcher externs (for the func_803268B4 patch) ----
// Most helpers and globals are already declared in functions.h / variables.h
// via patches.h. Only a few missing pieces need manual externs.
extern ActorArray *suBaddieActorArray;
extern s32 D_8036E56C;
extern s32 D_8036E570;
extern bool func_803296D8(Actor *this, s32 arg);
extern BKVertexList *func_80330C74(Actor *actor);
extern void func_8033F7A4(ActorMarker *marker, BKVertexList *list);
extern void func_8034C21C(ActorMarker *marker);
extern void func_8032F6A4(s32 position[3], ActorMarker *marker, s32 rotation[3]);
extern void func_80326324(Actor *this);
extern void func_802D7124(Actor *actor, f32 scale);
extern void bundle_update(Actor *actor);
extern void dustEmitter_isActive(s32 emitter);
extern void func_802F2D8C(s32 arg);

// === Actor-update context ===
// Set by the patched func_803268B4 before each actorUpdateFunc call,
// cleared after. When set, player_getPosition redirects to the closest
// player relative to this actor.
static Actor *g_current_ai_actor = NULL;

// === Closest-player helpers ===

// Minimum squared XZ distance from (x, z) to any player on the current map.
static f32 min_player_xz_dist_sq(f32 x, f32 z) {
    f32 local[3];
    _player_getPosition(local);
    f32 dx = x - local[0], dz = z - local[2];
    f32 min_sq = dx * dx + dz * dz;

    if (!recomp_net_is_connected()) return min_sq;

    u32 cur_map = (u32)map_get();
    u32 n = recomp_net_get_remote_count();
    u32 i;
    RemoteState rs;
    for (i = 0; i < n; i++) {
        if (!recomp_net_get_remote_state(i, &rs)) continue;
        if (rs.map_id != cur_map) continue;
        dx = x - rs.x; dz = z - rs.z;
        f32 d = dx * dx + dz * dz;
        if (d < min_sq) min_sq = d;
    }
    return min_sq;
}

// Fill out[3] with the position of the player closest (by XYZ) to from[3]
// on the current map. Always writes something valid — local player at
// minimum. Remote players in other maps are ignored.
static void find_closest_player_xyz(const f32 from[3], f32 out[3]) {
    _player_getPosition(out);
    f32 dx = from[0] - out[0], dy = from[1] - out[1], dz = from[2] - out[2];
    f32 min_sq = dx * dx + dy * dy + dz * dz;

    if (!recomp_net_is_connected()) return;

    u32 cur_map = (u32)map_get();
    u32 n = recomp_net_get_remote_count();
    u32 i;
    RemoteState rs;
    for (i = 0; i < n; i++) {
        if (!recomp_net_get_remote_state(i, &rs)) continue;
        if (rs.map_id != cur_map) continue;
        dx = from[0] - rs.x; dy = from[1] - rs.y; dz = from[2] - rs.z;
        f32 d = dx * dx + dy * dy + dz * dz;
        if (d < min_sq) {
            min_sq = d;
            out[0] = rs.x; out[1] = rs.y; out[2] = rs.z;
        }
    }
}

// === Patched range/distance helpers ===
//
// These are called only by zone triggers and enemy aggro code, so we can
// unconditionally return the closest-player value without breaking camera
// or HUD.

RECOMP_PATCH bool core1_ce60_isPlayerInRange(s32 x, s32 z, s32 distance) {
    f32 dist_sq = min_player_xz_dist_sq((f32)x, (f32)z);
    return dist_sq < (f32)(distance * distance);
}

RECOMP_PATCH f32 core1_ce60_getPlayerDistance(f32 x, f32 z) {
    return gu_sqrtf(min_player_xz_dist_sq(x, z));
}

// === Patched player position getter ===
//
// Only redirects when called from inside an actor update (i.e. while
// g_current_ai_actor is set by the patched dispatcher below). Camera,
// HUD and other consumers continue to see the local player.

RECOMP_PATCH void player_getPosition(f32 dst[3]) {
    _player_getPosition(dst);
    if (g_current_ai_actor == NULL) return;
    if (!recomp_net_is_connected()) return;

    f32 actor_pos[3];
    actor_pos[0] = g_current_ai_actor->position[0];
    actor_pos[1] = g_current_ai_actor->position[1];
    actor_pos[2] = g_current_ai_actor->position[2];
    find_closest_player_xyz(actor_pos, dst);
}

// === Patched actor-update dispatcher ===
//
// Vanilla func_803268B4 iterates suBaddieActorArray and calls each
// actor's actorUpdateFunc / actorUpdate2Func. We mirror the original
// body byte-for-byte and only add the g_current_ai_actor set/clear
// around the two update calls. Everything else is unchanged.
//
// *** TEMPORARILY DISABLED *** — the join was getting a black screen
// with this patch active. Without the dispatcher hook, g_current_ai_actor
// stays NULL, so the player_getPosition patch falls through to vanilla
// (enemies still chase local Banjo, but at least aggro works remotely
// via the core1_ce60_* patches above). Re-enable once the interaction
// causing the hang is identified.
#if 0
RECOMP_PATCH void func_803268B4(void) {
    s32 temp_v1;
    Actor *actor;
    ActorMarker *marker;
    AnimCtrl *anim_ctrl;
    ActorInfo *actor_info;
    s32 position[3];
    s32 rotation[3];
    BKVertexList *temp_v0_3;
    bool sp54;
    s32 temp_s1;

    if (suBaddieActorArray != NULL) {
        sp54 = volatileFlag_get(VOLATILE_FLAG_65_CHEAT_ENTERED);
        for (temp_v1 = suBaddieActorArray->cnt - 1; temp_v1 >= 0; temp_v1--) {
            actor = &suBaddieActorArray->data[temp_v1];
            actor_info = actor->actor_info;
            marker = actor->marker;
            anim_ctrl = actor->anctrl;
            temp_s1 = actor->actor_info->unk18;
            if (marker->propPtr->unk8_4) {
                if (sp54) {
                    if (actor->actor_info->unk20 && volatileFlag_get(actor->actor_info->unk20)) {
                        marker_despawn(marker);
                    }
                }
                if (!actor->despawn_flag) {
                    if (marker->unk2C_2) {
                        g_current_ai_actor = actor;
                        marker->actorUpdate2Func(actor);
                        g_current_ai_actor = NULL;
                        if (anim_ctrl != NULL) {
                            actor->sound_timer = anctrl_getAnimTimer(anim_ctrl);
                        }
                    } else if (!temp_s1 || (temp_s1 && func_803296D8(actor, temp_s1))) {
                        if (marker->actorUpdateFunc != NULL) {
                            g_current_ai_actor = actor;
                            marker->actorUpdateFunc(actor);
                            g_current_ai_actor = NULL;
                            if (anim_ctrl != NULL) {
                                actor->sound_timer = anctrl_getAnimTimer(anim_ctrl);
                            }
                        }
                    }
                    actor->unk124_7 = TRUE;
                    actor->unk138_28 = FALSE;
                    if (anim_ctrl != NULL) {
                        anctrl_update(anim_ctrl);
                    }
                    if (marker->unk4C) {
                        temp_v0_3 = func_80330C74(actor);
                        if (temp_v0_3) {
                            func_8033F7A4(marker, temp_v0_3);
                            func_8034C21C(marker);
                        }
                    }
                    position[0] = (s32)actor->position[0];
                    position[1] = (s32)actor->position[1];
                    position[2] = (s32)actor->position[2];
                    rotation[0] = (s32)actor->pitch;
                    rotation[1] = (s32)actor->yaw;
                    rotation[2] = (s32)actor->roll;
                    func_8032F6A4(position, marker, rotation);
                    if (actor->unk124_11) {
                        func_80326324(actor);
                    }
                    if (actor->unk148) {
                        if (!actor->despawn_flag) {
                            skeletalAnim_update(actor->unk148, time_getDelta(), marker->unk14_21);
                        } else {
                            skeletalAnim_set(actor->unk148, 0, 0.0f, 0.0f);
                        }
                    }
                    if ((actor_info->shadow_scale != 0.0f) && actor->unk124_6 && marker->unk14_21) {
                        func_802D7124(actor, actor_info->shadow_scale);
                    }
                    if (actor->is_bundle) {
                        actor = &suBaddieActorArray->data[temp_v1];
                        bundle_update(actor);
                    }
                }
            }
        }
    }
    if (D_8036E56C != 0) {
        dustEmitter_isActive(D_8036E56C);
    }
    if (D_8036E570 != 0) {
        func_802F2D8C(D_8036E570);
    }
}
#endif
