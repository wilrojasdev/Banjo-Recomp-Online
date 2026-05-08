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
extern u32 recomp_net_get_local_player_id(void);

// Maximum number of player slots in a session (host + 3 joins).
#define NET_MAX_PLAYERS 4

// Vanilla BK helpers we still need to call.
extern void _player_getPosition(f32 dst[3]);
extern float gu_sqrtf(float val);
extern enum map_e map_get(void);

// Raw local-player coordinates. _player_getPosition is just
// `ml_vec3f_copy(arg0, player_position)` — by reading the global directly we
// avoid recursion when our patched _player_getPosition wants the unredirected
// local position.
extern f32 player_position[3];

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

// === Dispatcher debug trace ===
// Set by network_world_sync.c whenever an enemy death is processed (either
// locally killed or via incoming packet). The dispatcher prints per-actor
// trace lines for the next N frames so we can spot if it hangs while
// updating a dying/just-killed actor.
RECOMP_EXPORT u32 g_dispatcher_trace_frames = 0;

// === Closest-player helpers ===

// Read raw local player position bypassing our own _player_getPosition patch.
static inline void raw_local_player(f32 dst[3]) {
    dst[0] = player_position[0];
    dst[1] = player_position[1];
    dst[2] = player_position[2];
}

// Minimum squared XZ distance from (x, z) to any player on the current map.
//
// IMPORTANT: recomp_net_get_remote_state(player_id, ...) takes a SLOT index
// (0..NET_MAX_PLAYERS-1), not an index from 0..remote_count. The bridge
// just calls get_remote_player(slot) and returns active=0 for empty slots
// or for the local player. We must iterate all slots and skip self — using
// 0..remote_count would only ever query slot 0, which is the host on every
// host-side enemy update, so the loop would never observe the actual remote.
static f32 min_player_xz_dist_sq(f32 x, f32 z) {
    f32 local[3];
    raw_local_player(local);
    f32 dx = x - local[0], dz = z - local[2];
    f32 min_sq = dx * dx + dz * dz;

    if (!recomp_net_is_connected()) return min_sq;

    u32 cur_map = (u32)map_get();
    u32 local_id = recomp_net_get_local_player_id();
    u32 i;
    RemoteState rs;
    for (i = 0; i < NET_MAX_PLAYERS; i++) {
        if (i == local_id) continue;
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
    raw_local_player(out);
    f32 dx = from[0] - out[0], dy = from[1] - out[1], dz = from[2] - out[2];
    f32 min_sq = dx * dx + dy * dy + dz * dz;

    if (!recomp_net_is_connected()) return;

    u32 cur_map = (u32)map_get();
    u32 local_id = recomp_net_get_local_player_id();
    u32 i;
    RemoteState rs;
    for (i = 0; i < NET_MAX_PLAYERS; i++) {
        if (i == local_id) continue;
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
    raw_local_player(dst);
    if (g_current_ai_actor == NULL) return;
    if (!recomp_net_is_connected()) return;

    f32 actor_pos[3];
    actor_pos[0] = g_current_ai_actor->position[0];
    actor_pos[1] = g_current_ai_actor->position[1];
    actor_pos[2] = g_current_ai_actor->position[2];
    find_closest_player_xyz(actor_pos, dst);
}

// === Patched RAW player position getter ===
//
// Vanilla _player_getPosition is the underlying primitive
// (ml_vec3f_copy(dst, player_position)). Many engine helpers
// (subaddie_playerIsWithinSphere, func_8032970C, func_803297FC, func_803292E0,
// func_80329354, func_80329384) call it directly, bypassing the wrapped
// player_getPosition patch above. Without this redirect, enemy aggro/yaw/los
// checks invoked from inside an enemy update see ONLY the local player and
// never react to remote players.
//
// We gate the redirect on g_current_ai_actor — same scope as the wrapped
// version — so consumers outside enemy updates (camera, HUD, dialog
// framing, music cues, particle emitters) keep getting the actual local
// player. Recursion is avoided by reading the player_position[] global
// directly via raw_local_player().
RECOMP_PATCH void _player_getPosition(f32 dst[3]) {
    raw_local_player(dst);
    if (g_current_ai_actor == NULL) return;
    if (!recomp_net_is_connected()) return;

    f32 actor_pos[3];
    actor_pos[0] = g_current_ai_actor->position[0];
    actor_pos[1] = g_current_ai_actor->position[1];
    actor_pos[2] = g_current_ai_actor->position[2];
    find_closest_player_xyz(actor_pos, dst);
}

// === Patched yaw-to-player ===
//
// Vanilla func_80329784 reads Banjo's character-model torso position via
// func_8028E964 → func_8028E924 → baModel_80292284 — a chain that returns
// the LOCAL player's animated joint position, NOT player_position[]. So our
// _player_getPosition redirect doesn't affect this path. Many enemies
// (Bigbutt, Clam, Conga via custom code, etc.) call this to set yaw_ideal
// when chasing or facing the player; if it always returns the yaw to the
// local player, an enemy will never face a remote-only player.
//
// Replicate the vanilla 2-arg atan logic but feed it the closest-player XZ
// when in AI context. Outside AI context, fall through to the local-Banjo
// model path so non-AI consumers (e.g. carry/throw helpers) are unaffected.
extern f32 func_80257204(f32 ax, f32 az, f32 bx, f32 bz);
extern void func_8028E964(f32 dst[3]);

RECOMP_PATCH s32 func_80329784(Actor *this) {
    f32 plyr[3];

    if (g_current_ai_actor != NULL && recomp_net_is_connected()) {
        f32 actor_pos[3];
        actor_pos[0] = g_current_ai_actor->position[0];
        actor_pos[1] = g_current_ai_actor->position[1];
        actor_pos[2] = g_current_ai_actor->position[2];
        find_closest_player_xyz(actor_pos, plyr);
    } else {
        func_8028E964(plyr);
    }
    return (s32)func_80257204(this->position[0], this->position[2], plyr[0], plyr[2]);
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

    // Drain receiver-side deferred kills here — same frame phase as
    // collision-triggered dieFunc (this dispatcher is invoked from
    // func_80330FF4 → spawnQueue_func_802C39D4, the same callsite where
    // BK's collision pipeline fires dieFunc).
    extern void bkrecomp_net_process_deferred_kills(void);
    bkrecomp_net_process_deferred_kills();

    // Live check (instead of captured at entry) so the kill's own frame —
    // where the counter is set mid-frame — is also traced.
    bool dbg_trace = (g_dispatcher_trace_frames > 0);
    if (dbg_trace) {
        recomp_printf("[DISP] frame_remaining=%u arr_cnt=%d\n",
            g_dispatcher_trace_frames, suBaddieActorArray ? suBaddieActorArray->cnt : -1);
    }

    if (suBaddieActorArray != NULL) {
        sp54 = volatileFlag_get(VOLATILE_FLAG_65_CHEAT_ENTERED);
        for (temp_v1 = suBaddieActorArray->cnt - 1; temp_v1 >= 0; temp_v1--) {
            actor = &suBaddieActorArray->data[temp_v1];
            actor_info = actor->actor_info;
            marker = actor->marker;
            anim_ctrl = actor->anctrl;
            temp_s1 = actor->actor_info->unk18;
            if (dbg_trace) {
                recomp_printf("[DISP] [%d] marker=0x%X state=0x%X despawn=%d updFn=%p dieFn=%p\n",
                    temp_v1, (u32)marker->id, actor->state,
                    actor->despawn_flag ? 1 : 0,
                    marker->actorUpdateFunc, marker->dieFunc);
            }
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
                        if (dbg_trace) recomp_printf("[DISP] [%d] -> upd2Fn\n", temp_v1);
                        marker->actorUpdate2Func(actor);
                        if (dbg_trace) recomp_printf("[DISP] [%d] <- upd2Fn\n", temp_v1);
                        if (is_enemy) g_current_ai_actor = NULL;
                        if (anim_ctrl != NULL) {
                            actor->sound_timer = anctrl_getAnimTimer(anim_ctrl);
                        }
                    } else {
                        // Set g_current_ai_actor BEFORE the active-zone gate so
                        // func_803296D8 → subaddie_playerIsWithinSphereAndActive
                        // → _player_getPosition redirects to closest player. Without
                        // this, an enemy whose actor_info->unk18 active radius gates
                        // around the LOCAL player only would freeze its update when
                        // the host is out of range — even if the remote is right
                        // beside it. (Bigbutt: unk18 = 3200.)
                        bool ai_scope = is_enemy;
                        if (ai_scope) g_current_ai_actor = actor;
                        bool gate_ok = !temp_s1 || (temp_s1 && func_803296D8(actor, temp_s1));
                        if (gate_ok) {
                            if (marker->actorUpdateFunc != NULL) {
                                if (dbg_trace) recomp_printf("[DISP] [%d] -> updFn (state=0x%X)\n",
                                    temp_v1, actor->state);
                                marker->actorUpdateFunc(actor);
                                if (dbg_trace) recomp_printf("[DISP] [%d] <- updFn\n", temp_v1);
                                if (anim_ctrl != NULL) {
                                    actor->sound_timer = anctrl_getAnimTimer(anim_ctrl);
                                }
                            }
                        }
                        if (ai_scope) g_current_ai_actor = NULL;
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
            if (dbg_trace) recomp_printf("[DISP] [%d] iter done\n", temp_v1);
        }
    }
    if (D_8036E56C != 0) {
        dustEmitter_isActive(D_8036E56C);
    }
    if (D_8036E570 != NULL) {
        func_802F2D8C(D_8036E570);
    }
    if (dbg_trace) {
        recomp_printf("[DISP] dispatcher exit\n");
        if (g_dispatcher_trace_frames > 0) g_dispatcher_trace_frames--;
    } else if (g_dispatcher_trace_frames > 0) {
        // Counter was armed mid-frame after dispatcher's entry check; print
        // a stub line so we know dispatcher ran without per-actor detail.
        recomp_printf("[DISP] (post-arm partial) exit, will trace next frame\n");
        g_dispatcher_trace_frames--;
    }
}
