#include "patches.h"
#include "functions.h"
#include "enums.h"

/*
 * TTC Treasure Hunt multiplayer sync.
 *
 * Vanilla (see lib/bk-decomp/src/TTC/ch/treasurehunt.c + treasure.c):
 *   - A single RED_X actor is placed at step-0 position by the level
 *     data. Beak-busting on each of 6 NodeProps advances the global
 *     `chtreasureHunt_puzzleCurrentStep` counter (0->6):
 *       step 0->1 .. 3->4  : spawn RED_ARROW at previous pos
 *                            (hinting toward next RED_X);
 *                            spawn RED_X at next step's position
 *       step 4->5          : same, but RED_QUESTION_MARK at pos 4
 *                            + RED_X at pos 5
 *       step 5->6          : spawn ACTOR_F4_BURIED_TREASURE on the
 *                            map floor at pos 5; the treasure rotates
 *                            in a circle, and when beak-busted its
 *                            __chTreasure_die spawns JIGGY_11_TTC_RED_X
 *   - Each hint actor fades in (state 0), waits while
 *     `local->unk0 >= chtreasureHunt_puzzleCurrentStep` (state 1), then
 *     fades out and despawns once the step passes it (state 2).
 *   - `spawnActorForNextStep` / `spawnRedXForNextStep` and the
 *     __chTreasurehunt_updateFunc are static in the TTC overlay.
 *
 * Coop spec (per user): the X's (and every hint actor leading up to
 * them) must be visible for all players; when the treasure spawns at
 * the end of the chain, it should show up for everyone too. In the
 * vanilla flow only the player who beak-busts each step spawns the
 * next hint actor locally, so the non-advancing player sees nothing
 * new appear.
 *
 * Mechanism: poll `chtreasureHunt_puzzleCurrentStep` in MAP_7_TTC each
 * frame. When it increases locally (ACTOR_55_RED_X beak-bust triggered
 * `chTreasurehunt_checkStepProgressN` natively), broadcast the new
 * value under NET_FLAG_TREASUREHUNT_ACTION. Remote peers advance their
 * own counter and spawn the matching hint actors using the same
 * arrays the overlay reads (accessed via datasyms extern), replicating
 * what spawnActorForNextStep / spawnRedXForNextStep would have done
 * locally. For the final 5->6 transition they spawn
 * ACTOR_F4_BURIED_TREASURE at step 5's floor position.
 *
 * Buried-treasure destruction: the chest is excluded from the generic
 * enemy sync in network_world_sync.c (is_killable_enemy returns FALSE
 * for MARKER_DB_BURIED_TREASURE) because the generic path would also
 * spawn an unwanted HONEYCOMB bundle on the non-killer and because
 * dynamically-spawned actors can drift in spawn_index between clients.
 * A dedicated CHEST_KILLED sub-action is broadcast instead when the
 * chest disappears from the local actor array (after __chTreasure_die's
 * marker_despawn). Remote peers find their local chest by marker id,
 * despawn it, and spawn JIGGY_11_TTC_RED_X at the stored step-5 center
 * position so each player has a jiggy actor to walk into — the jiggy
 * collection itself then propagates through phase 3 (jiggyscore +
 * despawn_jiggy_by_id). The step-5 center is ~300 units from the
 * chest's rotating orbit position at death, which is close enough to
 * stay within reach.
 */

// Network bridge
void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);
u32  recomp_net_is_connected(void);

extern enum map_e map_get(void);
extern Actor *actor_spawnWithYaw_f32(enum actor_e actor_id, f32 position[3], s32 yaw);
extern f32   mapModel_getFloorY(f32 position[3]);
extern Actor *actorArray_findActorFromMarkerId(s32 marker_id);
extern void   marker_despawn(ActorMarker *marker);
extern void   jiggy_spawn(enum jiggy_e jiggy_id, f32 pos[3]);
extern bool   jiggyscore_isCollected(enum jiggy_e jiggy_id);

/*
 * Datasym externs — resolved at recompile time via datasyms.toml. These
 * mirror the static arrays in treasurehunt.c; keep types matching the
 * native declarations.
 */
extern u32 chtreasureHunt_puzzleCurrentStep;
extern f32 sChTreasurehunt_stepPositions[6][3];
extern f32 sChTreasurehunt_StepRedXYaws[6];
extern s32 sChTreasurehunt_StepYaws[6];
extern s32 sChTreasurehunt_StepActors[6];

// Mirror of ActorLocal_TreasureHunt (treasurehunt.c:8-10). Only unk0 is
// used — a per-actor "step when spawned" marker read by the hint's
// state 1 fade check.
typedef struct {
    s32 unk0;
} NetTreasurehuntLocal;

// Flag type — keep in sync with dispatch in network_flag_sync.c
#define NET_FLAG_TREASUREHUNT_ACTION 15

// Sub-actions encoded in flag_index
#define TREASUREHUNT_ACTION_STEP_ADVANCE  0   // value = new chtreasureHunt_puzzleCurrentStep
#define TREASUREHUNT_ACTION_CHEST_KILLED  1   // value = 1

static u32  pending_target_step = 0;
static bool has_pending_step = FALSE;
static bool has_pending_chest_kill = FALSE;

static u32  last_step = 0;
static bool last_chest_present = FALSE;
static bool tracking = FALSE;

// Called from network_flag_sync.c when a remote FLAG_TREASUREHUNT_ACTION arrives.
// For STEP_ADVANCE we keep only the highest target step — lower values are
// already covered because advance_to_step walks every intermediate step
// and spawns each hint actor along the way.
RECOMP_EXPORT void bkrecomp_net_treasurehunt_remote_apply(u32 action, u32 value) {
    if (action == TREASUREHUNT_ACTION_STEP_ADVANCE) {
        if (!has_pending_step || value > pending_target_step) {
            pending_target_step = value;
            has_pending_step = TRUE;
        }
    } else if (action == TREASUREHUNT_ACTION_CHEST_KILLED) {
        has_pending_chest_kill = TRUE;
    }
}

// Spawn the hint actor that belongs AT step `newStep - 1` (arrow or
// question mark), pointing toward the new RED_X. Mirrors
// __chTreasurehunt_spawnActorForNextStep.
static void spawn_hint_actor(u32 newStep) {
    if (newStep < 1 || newStep > 5) return;

    Actor *actor = actor_spawnWithYaw_f32(
        (enum actor_e)sChTreasurehunt_StepActors[newStep - 1],
        sChTreasurehunt_stepPositions[newStep - 1],
        0);
    if (actor != NULL) {
        NetTreasurehuntLocal *local = (NetTreasurehuntLocal *)&actor->local;
        local->unk0 = (s32)newStep;
        actor->yaw = (f32)sChTreasurehunt_StepYaws[newStep - 1];
        actor->lifetime_value = 0.0f;
        actor->state = 0;
    }
}

// Spawn the new RED_X at position `newStep`. Mirrors
// __chTreasurehunt_spawnRedXForNextStep.
static void spawn_red_x(u32 newStep) {
    if (newStep < 1 || newStep > 5) return;

    Actor *actor = actor_spawnWithYaw_f32(
        ACTOR_55_RED_X,
        sChTreasurehunt_stepPositions[newStep],
        0);
    if (actor != NULL) {
        NetTreasurehuntLocal *local = (NetTreasurehuntLocal *)&actor->local;
        local->unk0 = (s32)newStep;
        actor->yaw = sChTreasurehunt_StepRedXYaws[newStep];
        actor->lifetime_value = 0.0f;
        actor->state = 0;
    }
}

// Mirrors the BURIED_TREASURE spawn in chTreasurehunt_checkStepProgress5:
// reads XZ from step 5's stored position and asks the map for the floor
// Y so the treasure sits correctly on the ground.
static void spawn_buried_treasure(void) {
    f32 pos[3];
    pos[0] = sChTreasurehunt_stepPositions[5][0];
    pos[1] = sChTreasurehunt_stepPositions[5][1];
    pos[2] = sChTreasurehunt_stepPositions[5][2];
    pos[1] = mapModel_getFloorY(pos);

    actor_spawnWithYaw_f32(ACTOR_F4_BURIED_TREASURE, pos, 0);
}

// Advance the local step counter up to `target`, spawning every hint
// actor the native state machine would have spawned at each transition.
// Idempotent: if target <= current, this is a no-op.
static void advance_to_step(u32 target) {
    if (target > 6) target = 6;
    while (chtreasureHunt_puzzleCurrentStep < target) {
        chtreasureHunt_puzzleCurrentStep++;
        u32 step = chtreasureHunt_puzzleCurrentStep;

        if (step == 6) {
            spawn_buried_treasure();
        } else {
            spawn_hint_actor(step);
            spawn_red_x(step);
        }
    }
}

// Despawn the local BURIED_TREASURE (if any) and spawn the reward jiggy
// at step 5's floor center. Called when a remote CHEST_KILLED arrives.
static void apply_chest_killed(void) {
    Actor *chest = actorArray_findActorFromMarkerId(MARKER_DB_BURIED_TREASURE);
    if (chest != NULL && chest->marker != NULL) {
        marker_despawn(chest->marker);
    }

    // Spawn the jiggy actor locally so the non-killer has something to
    // walk into. Skip if already collected (idempotent against re-apply
    // on resync or duplicate events).
    if (!jiggyscore_isCollected(JIGGY_11_TTC_RED_X)) {
        f32 pos[3];
        pos[0] = sChTreasurehunt_stepPositions[5][0];
        pos[1] = sChTreasurehunt_stepPositions[5][1];
        pos[2] = sChTreasurehunt_stepPositions[5][2];
        pos[1] = mapModel_getFloorY(pos);
        jiggy_spawn(JIGGY_11_TTC_RED_X, pos);
    }

    recomp_printf("[TREASUREHUNT-SYNC] applied chest killed\n");
}

// Called every frame from bkrecomp_net_process_world_events.
RECOMP_EXPORT void bkrecomp_net_treasurehunt_tick(void) {
    if (!recomp_net_is_connected()) {
        tracking = FALSE;
        has_pending_step = FALSE;
        has_pending_chest_kill = FALSE;
        return;
    }

    if (map_get() != MAP_7_TTC_TREASURE_TROVE_COVE) {
        tracking = FALSE;
        has_pending_step = FALSE;
        has_pending_chest_kill = FALSE;
        return;
    }

    // Apply pending remote events first so broadcast diffs below do
    // not re-echo what we just applied.
    if (has_pending_step) {
        if (pending_target_step > chtreasureHunt_puzzleCurrentStep) {
            u32 before = chtreasureHunt_puzzleCurrentStep;
            advance_to_step(pending_target_step);
            recomp_printf("[TREASUREHUNT-SYNC] advanced %d -> %d\n",
                before, chtreasureHunt_puzzleCurrentStep);
        }
        has_pending_step = FALSE;
        last_step = chtreasureHunt_puzzleCurrentStep;
    }
    if (has_pending_chest_kill) {
        apply_chest_killed();
        has_pending_chest_kill = FALSE;
        // Re-baseline chest presence AFTER apply so the local-diff
        // detection below does not also broadcast.
        last_chest_present = FALSE;
    }

    // Baseline quietly on first frame in the map so we do not
    // broadcast the pre-existing step value.
    if (!tracking) {
        last_step = chtreasureHunt_puzzleCurrentStep;
        last_chest_present =
            (actorArray_findActorFromMarkerId(MARKER_DB_BURIED_TREASURE) != NULL);
        tracking = TRUE;
        return;
    }

    // Detect local step increments — only the player who beak-busts a
    // NodeProp bumps the counter, so there is no echo path.
    u32 cur = chtreasureHunt_puzzleCurrentStep;
    if (cur > last_step) {
        recomp_net_send_flag_change(NET_FLAG_TREASUREHUNT_ACTION,
            TREASUREHUNT_ACTION_STEP_ADVANCE, cur,
            (u32)MAP_7_TTC_TREASURE_TROVE_COVE);
        recomp_printf("[TREASUREHUNT-SYNC] sent step=%d\n", cur);
    }
    last_step = cur;

    // Detect local chest disappearance. __chTreasure_die already ran
    // marker_despawn on the killer's client, so by the time our tick
    // runs the actor is gone from suBaddieActorArray. The transition
    // from present -> absent is the broadcast signal. Guarded on step
    // >= 6 so we never emit the event before the chest was even
    // spawned (avoids a stale baseline firing after a map re-entry).
    bool cur_chest_present =
        (actorArray_findActorFromMarkerId(MARKER_DB_BURIED_TREASURE) != NULL);
    if (last_chest_present && !cur_chest_present && chtreasureHunt_puzzleCurrentStep >= 6) {
        recomp_net_send_flag_change(NET_FLAG_TREASUREHUNT_ACTION,
            TREASUREHUNT_ACTION_CHEST_KILLED, 1,
            (u32)MAP_7_TTC_TREASURE_TROVE_COVE);
        recomp_printf("[TREASUREHUNT-SYNC] sent chest killed\n");
    }
    last_chest_present = cur_chest_present;
}
