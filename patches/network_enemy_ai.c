// Enemy AI multiplayer awareness patches.
//
// Problem: BK's enemy AI uses player_getPosition() and core1_ce60_*
// helpers, both of which return the LOCAL player's coordinates. In
// multiplayer, if a remote player walks next to an enemy, the owner
// sees the enemy stay idle because, from the owner's perspective, no
// "player" is nearby. Remote players see animations play but no
// movement, because the owner broadcasts static positions.
//
// Status: full remote-aware aggro + chase. Verify in-game.
//
//   1. core1_ce60_isPlayerInRange(x, z, distance)  — ACTIVE
//      Returns TRUE if ANY player (local or remote in the same map) is
//      within `distance` of (x, z).
//
//   2. core1_ce60_getPlayerDistance(x, z)  — ACTIVE
//      Returns the XZ distance from (x, z) to the CLOSEST player.
//
//   3. player_getPosition(dst[3])  — ACTIVE
//      Redirects to the closest player ONLY while an enemy actor's
//      update function is running (gated by g_current_ai_actor).
//      Camera, HUD, dialog and other consumers keep seeing the local
//      player because the flag is clear outside the dispatcher.
//
//   4. func_803268B4 (actor-update dispatcher)  — ACTIVE (narrow)
//      Mirrors the vanilla body and sets g_current_ai_actor only when
//      the actor has a dieFunc (i.e. is a killable enemy). Props,
//      NPCs, pads and bundles pass through with the flag unchanged.
//      Extern types match the vanilla decomp exactly to avoid any
//      silent ABI mismatch that might have caused the earlier
//      boot-time black screen.

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
    u8  carry_kind;     // 0x3C — bridge writes this byte; struct must be ≥ 0x40
    u8  _pad3[3];
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
//
// NOTE: types here must match the vanilla decomp exactly. A prior attempt
// declared D_8036E570 as s32 (vanilla: void *), dustEmitter_isActive as
// void (vanilla: bool), and func_802F2D8C as taking s32 (vanilla: Struct64s *).
// Those mismatches happen to be ABI-compatible on MIPS register calls, but
// are retained correctly now to rule them out as a cause of the earlier
// boot-time black screen.
extern ActorArray *suBaddieActorArray;
extern s32 D_8036E56C;
extern void *D_8036E570;
extern bool func_803296D8(Actor *this, s32 arg);
extern BKVertexList *func_80330C74(Actor *actor);
extern void func_8033F7A4(ActorMarker *marker, BKVertexList *list);
extern void func_8034C21C(ActorMarker *marker);
extern void func_8032F6A4(s32 position[3], ActorMarker *marker, s32 rotation[3]);
extern void func_80326324(Actor *this);
extern void func_802D7124(Actor *actor, f32 scale);
extern void bundle_update(Actor *actor);
extern bool dustEmitter_isActive(s32 emitter);
extern void func_802F2D8C(Struct64s *arg);

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
// body and add g_current_ai_actor set/clear around the two update calls,
// but ONLY for killable enemies (marker->dieFunc != NULL). Props, NPCs,
// pads, platforms and collectibles pass through untouched, so their
// player_getPosition consumers (dialog framing, camera hints, etc.)
// continue to see the local player. This narrow-scope approach was the
// first recommended next step in todo_enemy_ai_chase.md.
//
// Extern types match vanilla decomp exactly (D_8036E570 as void *,
// dustEmitter_isActive as bool, func_802F2D8C as Struct64s *) to rule
// out any silent ABI divergence that might have caused the earlier
// boot-time black screen.
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
    bool is_enemy;

    static bool traced = FALSE;
    if (!traced) {
        traced = TRUE;
        recomp_printf("[enemy_ai] dispatcher patch active\n");
    }

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
                    is_enemy = (marker->dieFunc != NULL);
                    if (marker->unk2C_2) {
                        if (is_enemy) g_current_ai_actor = actor;
                        marker->actorUpdate2Func(actor);
                        if (is_enemy) g_current_ai_actor = NULL;
                        if (anim_ctrl != NULL) {
                            actor->sound_timer = anctrl_getAnimTimer(anim_ctrl);
                        }
                    } else if (!temp_s1 || (temp_s1 && func_803296D8(actor, temp_s1))) {
                        if (marker->actorUpdateFunc != NULL) {
                            if (is_enemy) g_current_ai_actor = actor;
                            marker->actorUpdateFunc(actor);
                            if (is_enemy) g_current_ai_actor = NULL;
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
    if (D_8036E570 != NULL) {
        func_802F2D8C(D_8036E570);
    }
}
