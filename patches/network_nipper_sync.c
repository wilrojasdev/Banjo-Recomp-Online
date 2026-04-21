#include "patches.h"
#include "functions.h"
#include "enums.h"

/*
 * TTC Nipper (hermit crab) multiplayer sync.
 *
 * Vanilla: Nipper has two markers (MARKER_A5_NIPPER / MARKER_16C_NIPPER,
 * swapped by __chNipper_determineMarkerId based on unk40_31) and a 7-state
 * machine (see nipper.c):
 *   1: idle (walking off-screen)
 *   2: visible, charging attack — unk38_31 increments each frame the
 *      player is in the zone; when it hits lifetime_value Nipper transitions
 *      to state 3
 *   3: attacking
 *   4: taking a hit (dying animation; returns to 3)
 *   5: first-meet spawn sequence
 *   6: dead (transition)
 *   7: dead (permanent)
 *
 * Each beak-buster hit decrements actor->lifetime_value 120 -> 80 -> 40 -> 0;
 * at 0 Nipper moves to state 6 permanently. So lifetime_value is the
 * *hit-counter* the user wants shared; state tells us whether Nipper is
 * tranquilo (2) or atacando (3).
 *
 * Coop spec (per user): "deben compartir el numero de golpes, el estado
 * en que se encuentre. si esta tranquilo o atacando, debe verse para
 * ambos."
 *
 * Mechanism: polling. Each frame while in MAP_7 we find Nipper, apply
 * pending remote events, then broadcast local changes to state and
 * lifetime_value. Nipper is exempted from the generic enemy die-proxy
 * (installed in network_world_sync.c) because the proxy treats every
 * dieFunc call as a kill, which is wrong for a multi-hit enemy — its
 * real dieFunc runs natively on all clients here instead.
 *
 * Only non-transient states are broadcast (2 waiting, 3 attacking,
 * 6 dying, 7 permanently dead). States 1/4/5 are transient — each
 * client reaches them naturally through its local state machine:
 *   - state 1 is the initial idle everyone spawns with;
 *   - state 4 is a hit-recoil that returns to 3 via an animation timer;
 *   - state 5 is the first-meet spawn animation and must stay local
 *     because __chNipper_updateFunc case 1 also shows gcdialog_showDialog
 *     at the same moment. Broadcasting state 5 would race the dialog
 *     guard on the remote (native update runs once per frame BEFORE
 *     our tick processes received events, so has_met_before would not
 *     yet be set when case 1 evaluates).
 * has_met_before itself IS broadcast so the remote skips first-meet
 * when its player later walks into range (native case 1's else-branch
 * goes straight to state 3 via __chNipper_setAnimationDuration).
 */

// Network bridge
void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);
u32  recomp_net_is_connected(void);

extern enum map_e map_get(void);
extern Actor *actorArray_findActorFromMarkerId(s32);
extern void subaddie_set_state_with_direction(Actor *this, s32 myAnimId, f32 anim_start_position, s32 direction);
extern void subaddie_set_state_looped(Actor *this, s32 state);
extern void actor_playAnimationOnce(Actor *this);
extern void anctrl_setDuration(AnimCtrl *ac, f32 duration);

// Flag type — keep in sync with dispatch in network_flag_sync.c
#define NET_FLAG_NIPPER_ACTION 13

// Sub-actions carried in flag_index
#define NIPPER_ACTION_STATE     0  // value = new actor->state (1..7)
#define NIPPER_ACTION_LIFETIME  1  // value = lifetime_value / 40 (so 3/2/1/0)
#define NIPPER_ACTION_MET       2  // value = 1 (has_met_before)

#define MAX_PENDING_NIPPER 16

typedef struct {
    u8 action;
    u8 value;
} PendingNipperEvent;

static PendingNipperEvent pending[MAX_PENDING_NIPPER];
static s32 pending_count = 0;

static u8   last_state = 0;
static u32  last_lifetime = 0;
static bool last_met = FALSE;
static bool tracking = FALSE;

static Actor *find_nipper(void) {
    Actor *n = actorArray_findActorFromMarkerId(MARKER_A5_NIPPER);
    if (!n) n = actorArray_findActorFromMarkerId(MARKER_16C_NIPPER);
    return n;
}

/*
 * Mirror __chNipper_setAnimationDuration (static in the TTC overlay).
 * The native update only reaches state 3 via this helper, which loops
 * the attack animation and speeds it up as lifetime_value decreases.
 * Using subaddie_set_state_with_direction instead would set the
 * animation playback as ONCE -- the attack plays a single cycle,
 * stops, and never hits the anim-timer-0.99f check that advances
 * unk1C[0], leaving Nipper frozen and impossible to hit.
 */
static void nipper_enter_attack_state(Actor *nipper) {
    subaddie_set_state_looped(nipper, 3);
    nipper->unk1C[0] = 0.0f;
    s32 life = (s32)nipper->lifetime_value;
    if (life == 120) {
        anctrl_setDuration(nipper->anctrl, 1.2f);
    } else if (life == 80) {
        anctrl_setDuration(nipper->anctrl, 1.05f);
    } else if (life == 40) {
        anctrl_setDuration(nipper->anctrl, 0.9f);
    }
}

/* Apply a synced state value using the same primitive the native
 * state machine uses in that case: LOOPED for 2/3 (continuous
 * animations), WITH_DIRECTION+playOnce for 6 (terminal death anim),
 * WITH_DIRECTION alone for 7 (static pose). */
static void nipper_apply_state(Actor *nipper, u8 state) {
    if (state == 2) {
        subaddie_set_state_looped(nipper, 2);
    } else if (state == 3) {
        nipper_enter_attack_state(nipper);
    } else if (state == 6) {
        subaddie_set_state_with_direction(nipper, 6, 0.01f, 1);
        actor_playAnimationOnce(nipper);
    } else if (state == 7) {
        subaddie_set_state_with_direction(nipper, 7, 0.01f, 1);
    }
}

// Queried from network_world_sync.c to skip Nipper in the generic enemy
// die-proxy + position-override paths.
RECOMP_EXPORT bool bkrecomp_net_is_nipper_marker(u32 marker_id) {
    return (marker_id == MARKER_A5_NIPPER) || (marker_id == MARKER_16C_NIPPER);
}

// Called from network_flag_sync.c when a remote FLAG_NIPPER_ACTION arrives.
RECOMP_EXPORT void bkrecomp_net_nipper_remote_apply(u32 action, u32 value) {
    if (pending_count >= MAX_PENDING_NIPPER) return;
    pending[pending_count].action = (u8)action;
    pending[pending_count].value = (u8)value;
    pending_count++;
}

// Called every frame from bkrecomp_net_process_world_events.
RECOMP_EXPORT void bkrecomp_net_nipper_tick(void) {
    if (!recomp_net_is_connected()) {
        tracking = FALSE;
        pending_count = 0;
        return;
    }

    if (map_get() != MAP_7_TTC_TREASURE_TROVE_COVE) {
        tracking = FALSE;
        pending_count = 0;
        return;
    }

    Actor *nipper = find_nipper();
    if (nipper == NULL) {
        tracking = FALSE;
        // Drop any stale pending events — the actor is gone (map
        // transition / despawn) and stored state values would not map
        // cleanly onto a freshly respawned actor.
        pending_count = 0;
        return;
    }

    // Apply remote events first. has_met_before must be set BEFORE any
    // state write so that if the native __chNipper_updateFunc runs
    // between this tick and the next frame, its case-1 dialog guard
    // already sees the flag as TRUE.
    if (pending_count > 0) {
        s32 i;
        // Pass 1: has_met_before
        for (i = 0; i < pending_count; i++) {
            if (pending[i].action == NIPPER_ACTION_MET && pending[i].value) {
                if (!nipper->has_met_before) {
                    nipper->has_met_before = TRUE;
                    recomp_printf("[NIPPER-SYNC] applied has_met_before\n");
                }
            }
        }
        // Pass 2: state + lifetime
        for (i = 0; i < pending_count; i++) {
            u8 action = pending[i].action;
            u8 value  = pending[i].value;

            if (action == NIPPER_ACTION_STATE) {
                // Only non-transient states are synced. Ignore anything
                // else defensively in case the sender sends a stale value.
                bool is_synced_state = (value == 2) || (value == 3)
                                    || (value == 6) || (value == 7);
                if (is_synced_state && (u8)nipper->state != value) {
                    nipper_apply_state(nipper, value);
                    recomp_printf("[NIPPER-SYNC] applied state=%d\n", value);
                }
            } else if (action == NIPPER_ACTION_LIFETIME) {
                f32 target = (f32)((u32)value * 40);  // 3->120, 2->80, 1->40
                // Only accept lower values (monotonic decrease).
                if (target < nipper->lifetime_value) {
                    nipper->lifetime_value = target;
                    recomp_printf("[NIPPER-SYNC] applied lifetime=%d\n", (u32)target);
                }
            }
            // NIPPER_ACTION_MET handled in pass 1 above.
        }
        pending_count = 0;

        // Re-baseline AFTER applying remote events so the broadcast
        // diff below does not interpret the applied values as a fresh
        // local change (which would echo the event back to the sender).
        last_state    = (u8)nipper->state;
        last_lifetime = (u32)nipper->lifetime_value;
        last_met      = nipper->has_met_before ? TRUE : FALSE;
    }

    // First sight of Nipper after entering the map: baseline without
    // broadcasting the pre-existing state.
    if (!tracking) {
        last_state    = (u8)nipper->state;
        last_lifetime = (u32)nipper->lifetime_value;
        last_met      = nipper->has_met_before ? TRUE : FALSE;
        tracking = TRUE;
        return;
    }

    // Broadcast local state changes. Only broadcast the non-transient
    // states that actually carry information across clients; 1/4/5 are
    // transient animations each peer reaches locally.
    {
        u8 cur = (u8)nipper->state;
        bool is_synced_state = (cur == 2) || (cur == 3)
                            || (cur == 6) || (cur == 7);
        if (cur != last_state && is_synced_state) {
            recomp_net_send_flag_change(NET_FLAG_NIPPER_ACTION,
                NIPPER_ACTION_STATE, (u32)cur,
                (u32)MAP_7_TTC_TREASURE_TROVE_COVE);
            recomp_printf("[NIPPER-SYNC] sent state=%d\n", cur);
        }
        last_state = cur;
    }

    // Broadcast lifetime_value decreases (hits landed locally).
    {
        u32 cur = (u32)nipper->lifetime_value;
        if (cur < last_lifetime) {
            u8 compressed = (u8)(cur / 40);
            recomp_net_send_flag_change(NET_FLAG_NIPPER_ACTION,
                NIPPER_ACTION_LIFETIME, (u32)compressed,
                (u32)MAP_7_TTC_TREASURE_TROVE_COVE);
            recomp_printf("[NIPPER-SYNC] sent lifetime=%d (compressed=%d)\n",
                cur, compressed);
        }
        last_lifetime = cur;
    }

    // Broadcast first-meet once. This lets other clients suppress the
    // dialog re-roll on subsequent encounters — dialog stays local to
    // the activator because the decomp gating (inside __chNipper_updateFunc
    // case 1) checks !has_met_before before calling gcdialog_showDialog.
    {
        bool cur = nipper->has_met_before ? TRUE : FALSE;
        if (cur && !last_met) {
            recomp_net_send_flag_change(NET_FLAG_NIPPER_ACTION,
                NIPPER_ACTION_MET, 1,
                (u32)MAP_7_TTC_TREASURE_TROVE_COVE);
            recomp_printf("[NIPPER-SYNC] sent has_met_before\n");
        }
        last_met = cur;
    }
}
