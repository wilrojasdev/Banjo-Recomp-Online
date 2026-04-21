#include "patches.h"
#include "functions.h"
#include "enums.h"

/*
 * TTC Blubber (pirate) multiplayer sync — delivery decrement + quest
 * completion + synchronized despawn.
 *
 * Vanilla: the player picks up two ACTOR_2A_GOLD_BULLIONs in the world
 * (these are already shared across players via COLLECTIBLE_GOLD_BULLION
 * in network_world_sync.c) and throws them at Blubber near a specific
 * node. Each throw decrements ITEM_18_GOLD_BULLIONS locally and spawns
 * ACTOR_149 / ACTOR_14A which collides with Blubber and sets
 * TTC_SPECIFIC_FLAG_0 (first bullion) or TTC_SPECIFIC_FLAG_1 (second
 * bullion). Once flag 1 fires __chBlubber_checkJiggySpawnedTextAndAdvanceState
 * runs a camera cutscene, schedules __chBlubber_showJiggySpawnedText
 * (1 s later) which drives state 4 (dance) then 5 (march away) and
 * __chBlubber_update2Func finally marker_despawn()s Blubber.
 *
 * Coop spec (per user):
 *   "cuando un jugador le entrega los tablones debe descontarse y
 *    contar para todos los demas por tanto debe desaprecer buble para
 *    ambos cuando se termina toda la entrega"
 *
 * Problems the vanilla flag sync alone does NOT solve:
 *   1. ITEM_18 decrement is local — P2 still sees 2 bullions in
 *      inventory after P1 throws both, long after the quest is done.
 *   2. TTC_SPECIFIC_FLAG_3 is set by P1's native checkJiggyAndAdvance
 *      and then broadcast via mapSpecificFlags sync. By the time P2's
 *      __chBlubber_updateFunc runs next frame, flag 3 is already TRUE,
 *      so checkJiggyAndAdvance early-returns — state 4 is never
 *      entered, Blubber never transitions to state 5, update2Func
 *      never reaches its 0.99 threshold, and the actor is never
 *      despawned on P2. Blubber is effectively frozen.
 *
 * This module sends two actions under NET_FLAG_BLUBBER_ACTION (= 14):
 *   - DELIVER: broadcast on every local ITEM_18 decrement; receiver
 *     adjusts its inventory to the sender's new count.
 *   - QUEST_COMPLETE: broadcast exactly once by the player who set
 *     TTC_SPECIFIC_FLAG_1 locally (hooked from network_flag_sync.c's
 *     mapSpecificFlags_set patch). Receivers force Blubber to state 4
 *     with local->unk24=0, and set flags 2/3 under the remote-guard so
 *     the cutscene does not re-echo. The brief dance + state-5 march
 *     runs naturally from there; update2Func then despawns the actor
 *     locally on each peer. The thrower never applies this action to
 *     itself (senders do not receive their own broadcasts), so the
 *     thrower still sees the full native cutscene + dialog.
 */

// Network bridge
void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);
u32  recomp_net_is_connected(void);

extern enum map_e map_get(void);
extern enum level_e level_get(void);
extern Actor *actorArray_findActorFromMarkerId(s32);
extern void marker_despawn(ActorMarker *marker);
extern void subaddie_set_state(Actor *, u32);
extern void actor_loopAnimation(Actor *);
extern s32 item_getCount(enum item_e item);
extern u32 bkrecomp_get_marker_spawn_index(ActorMarker* marker);
extern ActorArray *suBaddieActorArray;

// Native "put down a carried bullion" used by the throw path. Decrements
// ITEM_18_GOLD_BULLIONS by 1 (WithHud — updates the inventory counter on
// screen) AND resets bacarry_get_marker() if the player is currently
// carrying ACTOR_2A_GOLD_BULLION. This second side-effect is the reason
// we can't just call item_dec here: on the remote peer, if the player
// has already approached Blubber's throw target,
// player_setCarryObjectPoseInCylinder may have reconstituted the carry
// state from the shared ITEM_18 count. Decrementing without clearing
// the carry marker would leave them holding a phantom bullion that
// bacarry_get_markerId() still reports as MARKER_37_GOLD_BULLION,
// letting __func_80387774 fire a second throw and over-set quest flags.
extern void bacarriedobj_dec(enum actor_e actor_id);

// From network_flag_sync.c — suppress re-broadcast of flags set inside
// a remote-event application.
extern void bkrecomp_net_set_remote_flag_guard(bool val);

// Mirror of ActorLocal_Blubber (blubber.c:9-16). Only unk24 (dance
// countdown) is accessed from here. Layout must stay in sync.
typedef struct {
    u8  _pad0[0xE];
    s16 _unkE;
    f32 _throw_target_position[3];
    s32 _throw_target_radius;
    u32 _gold_bullion_throw_target_node_prop;  // opaque pointer
    s32 unk24;
} NetBlubberLocal;

// Flag type — keep in sync with dispatch in network_flag_sync.c
#define NET_FLAG_BLUBBER_ACTION 14

#define BLUBBER_ACTION_DELIVER         0   // value = new ITEM_18 count
#define BLUBBER_ACTION_QUEST_COMPLETE  1   // value = 1

#define MAX_PENDING_BLUBBER 8

typedef struct {
    u8 action;
    u8 value;
} PendingBlubberEvent;

static PendingBlubberEvent pending[MAX_PENDING_BLUBBER];
static s32 pending_count = 0;

static s32  last_bullions = 0;
static bool tracking = FALSE;

/*
 * Bitmask of MARKER_37_GOLD_BULLION spawn_indexes that have been picked
 * up by anyone in the co-op session. Persists across TTC sub-map
 * transitions (MAP_5 <-> MAP_7 <-> MAP_A <-> ...) so that when a player
 * who was inside the ship during a pickup steps back onto the beach,
 * the bullion actor that already got collected gets despawned on their
 * client before they can walk over it. Reset only when the local
 * player leaves TTC entirely (level_get() != LEVEL_2).
 */
static u8 picked_bullion_mask = 0;

// Tracks the local level so we can detect exits from TTC.
#define LEVEL_TTC 2
static s32 last_level_for_bullion_reset = -1;

RECOMP_EXPORT void bkrecomp_net_blubber_mark_bullion_picked(u32 spawn_index) {
    if (spawn_index > 7) return;  // mask is u8 — only 8 slots
    picked_bullion_mask |= (u8)(1u << spawn_index);
}

RECOMP_EXPORT u32 bkrecomp_net_blubber_get_picked_bullion_mask(void) {
    return (u32)picked_bullion_mask;
}

RECOMP_EXPORT void bkrecomp_net_blubber_apply_picked_bullion_mask(u32 mask) {
    picked_bullion_mask |= (u8)(mask & 0xFFu);
}

// Iterate the current actor array and despawn every MARKER_37 whose
// spawn_index bit is set in picked_bullion_mask. Called each frame
// while in MAP_7 — cheap since MARKER_37 rarely appears and there are
// at most two of them.
static void despawn_picked_bullions(void) {
    if (picked_bullion_mask == 0 || !suBaddieActorArray) return;

    s32 i;
    for (i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (!actor->marker) continue;
        if (actor->marker->id != MARKER_37_GOLD_BULLION) continue;
        u32 si = bkrecomp_get_marker_spawn_index(actor->marker);
        if (si > 7) continue;
        if (picked_bullion_mask & (1u << si)) {
            marker_despawn(actor->marker);
        }
    }
}

// Called by network_flag_sync.c when the LOCAL client sets flag 1
// (the "second bullion delivered" latch). Only the thrower hits this
// path — remote sets are guarded by net_applying_remote_flag in the
// flag patch. Broadcasting here means the QUEST_COMPLETE signal is
// emitted exactly once, by the player who actually completed the
// quest, so there is no echo loop to suppress.
RECOMP_EXPORT void bkrecomp_net_blubber_on_local_flag1_set(void) {
    if (!recomp_net_is_connected()) return;
    recomp_net_send_flag_change(NET_FLAG_BLUBBER_ACTION,
        BLUBBER_ACTION_QUEST_COMPLETE, 1,
        (u32)MAP_7_TTC_TREASURE_TROVE_COVE);
    recomp_printf("[BLUBBER-SYNC] sent quest complete (local flag1 latch)\n");
}

// Called from network_flag_sync.c when a remote FLAG_BLUBBER_ACTION arrives.
RECOMP_EXPORT void bkrecomp_net_blubber_remote_apply(u32 action, u32 value) {
    if (pending_count >= MAX_PENDING_BLUBBER) return;
    pending[pending_count].action = (u8)action;
    pending[pending_count].value = (u8)value;
    pending_count++;
}

// Called every frame from bkrecomp_net_process_world_events.
RECOMP_EXPORT void bkrecomp_net_blubber_tick(void) {
    if (!recomp_net_is_connected()) {
        tracking = FALSE;
        pending_count = 0;
        return;
    }

    // Level-level reset for picked_bullion_mask: keep it alive across
    // TTC sub-map transitions (MAP_5/6/7/A/8F all share LEVEL_2) and
    // clear it once we actually leave TTC entirely. Tracked separately
    // from `tracking` / `pending_count` so the mask keeps working even
    // while we are inside the ship (MAP_5) and the rest of this tick
    // is otherwise idle.
    {
        s32 cur_level = (s32)level_get();
        if (last_level_for_bullion_reset != cur_level) {
            if (last_level_for_bullion_reset == LEVEL_TTC && cur_level != LEVEL_TTC) {
                picked_bullion_mask = 0;
            }
            last_level_for_bullion_reset = cur_level;
        }
    }

    if (map_get() != MAP_7_TTC_TREASURE_TROVE_COVE) {
        tracking = FALSE;
        pending_count = 0;
        return;
    }

    // Despawn any bullion actors that were picked up by anyone in the
    // session (possibly while we were in a sub-map). Runs every frame
    // in MAP_7 so that freshly respawned actors on map re-entry are
    // caught immediately.
    despawn_picked_bullions();

    // Apply pending remote events first.
    if (pending_count > 0) {
        s32 i;
        for (i = 0; i < pending_count; i++) {
            u8 action = pending[i].action;
            u8 value  = pending[i].value;

            if (action == BLUBBER_ACTION_DELIVER) {
                s32 cur = item_getCount(ITEM_18_GOLD_BULLIONS);
                s32 target = (s32)value;
                // Only decrease (matches "delivery" semantic). Use
                // bacarriedobj_dec rather than a blind adjustByDiff so
                // the carry-marker side effect matches what the native
                // throw path does on the sender — the receiver must
                // not be left holding a phantom bullion (see extern
                // comment above for the failure mode this fixes).
                while (target < cur) {
                    bacarriedobj_dec(ACTOR_2A_GOLD_BULLION);
                    cur = item_getCount(ITEM_18_GOLD_BULLIONS);
                }
                recomp_printf("[BLUBBER-SYNC] applied delivery: target=%d final=%d\n", target, cur);
            } else if (action == BLUBBER_ACTION_QUEST_COMPLETE) {
                Actor *blubber = actorArray_findActorFromMarkerId(MARKER_A3_BLUBBER);
                if (blubber != NULL) {
                    // Latch the native cutscene so checkJiggyAndAdvance
                    // and showJiggySpawnedText become no-ops — they
                    // already ran on the thrower. Use the remote-guard
                    // so these flag sets do not broadcast back to the
                    // thrower (which would cause its still-pending
                    // showJiggySpawnedText to skip its own dialog).
                    bkrecomp_net_set_remote_flag_guard(TRUE);
                    mapSpecificFlags_set(TTC_SPECIFIC_FLAG_3_BLUBBER_SHOW_JIGGY_SPAWNED_TEXT_FLAG, TRUE);
                    mapSpecificFlags_set(TTC_SPECIFIC_FLAG_2_BLUBBER_JIGGY_SPAWNED_TEXT_SHOWN, TRUE);
                    bkrecomp_net_set_remote_flag_guard(FALSE);

                    // Jump Blubber into the dance state and immediately
                    // drain the dance counter. State 4's anim-0.99 +
                    // unk24==0 check transitions to state 5 on the
                    // next animation cycle; state 5 + update2Func then
                    // drive the natural despawn (unk48 >= 0.99).
                    subaddie_set_state(blubber, 4);
                    actor_loopAnimation(blubber);
                    blubber->actor_specific_1_f = 0.0f;

                    NetBlubberLocal *local = (NetBlubberLocal *)&blubber->local;
                    local->unk24 = 0;

                    recomp_printf("[BLUBBER-SYNC] applied quest complete — despawn sequence started\n");
                } else {
                    recomp_printf("[BLUBBER-SYNC] quest complete but Blubber actor not found\n");
                }
            }
        }
        pending_count = 0;

        // Re-baseline after apply so the delivery-decrement detection
        // below does not re-broadcast the item change we just applied.
        last_bullions = item_getCount(ITEM_18_GOLD_BULLIONS);
    }

    // First sight of Blubber's map after connect: baseline quietly.
    if (!tracking) {
        last_bullions = item_getCount(ITEM_18_GOLD_BULLIONS);
        tracking = TRUE;
        return;
    }

    // Broadcast delivery decrements. Only the thrower decrements
    // ITEM_18 natively; remote peers only decrement via this DELIVER
    // apply, which is suppressed from re-broadcast by the rebase above.
    {
        s32 cur = item_getCount(ITEM_18_GOLD_BULLIONS);
        if (cur < last_bullions) {
            recomp_net_send_flag_change(NET_FLAG_BLUBBER_ACTION,
                BLUBBER_ACTION_DELIVER, (u32)cur,
                (u32)MAP_7_TTC_TREASURE_TROVE_COVE);
            recomp_printf("[BLUBBER-SYNC] sent delivery: count=%d\n", cur);
        }
        last_bullions = cur;
    }
}
