#include "patches.h"
#include "functions.h"
#include "enums.h"

/*
 * TTC Leaky (bucket on the beach) multiplayer sync.
 *
 * Vanilla: the player throws eggs at the bucket; each hit increments
 * actor->unk38_31. When it reaches 2, levelSpecificFlags 2 and 5 fire,
 * the sand drains, and the player transitions into MAP_A_TTC_SANDCASTLE.
 *
 * Coop desired behaviour (per user spec): the egg counter is SHARED
 * across all players. If P1 hits the bucket once and leaves, P2 can
 * finish with a single egg. The transition/cutscene stays local to the
 * activator (standard scene-independence rule); remote players see the
 * drained state the next time they enter TTC via the existing flag sync.
 *
 * Mechanism: polling-based (no RECOMP_PATCH of the overlay). Each frame
 * we find the live Leaky actor, compare its unk38_31 against the last
 * seen value, and broadcast increments. Received increments are applied
 * on the next tick and rebaseline our poll state to avoid rebroadcast.
 */

// Network bridge
void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);
u32  recomp_net_is_connected(void);

extern enum map_e map_get(void);
extern Actor *actorArray_findActorFromMarkerId(s32);
extern void comusic_playTrack(enum comusic_e);

// Flag type (must stay in sync with dispatch in network_flag_sync.c).
// 0-9 are used; 10 = conga hit; 11 = leaky.
#define NET_FLAG_LEAKY_ACTION 11

// Leaky has a single live instance per session. A tiny queue is enough
// to absorb late-arriving packets around map transitions.
#define MAX_PENDING_LEAKY 4

static u32 pending_counts[MAX_PENDING_LEAKY];
static s32 pending_leaky_count = 0;

static u32 s_last_seen_count = 0;
static bool s_tracking_actor = FALSE;

// Called from network_flag_sync.c when a remote FLAG_LEAKY_ACTION arrives.
RECOMP_EXPORT void bkrecomp_net_leaky_remote_apply(u32 count, u32 map_id) {
    (void)map_id;  // Leaky only exists on MAP_7, no per-map filtering needed
    if (pending_leaky_count >= MAX_PENDING_LEAKY) return;
    pending_counts[pending_leaky_count++] = count;
}

// Called every frame from bkrecomp_net_process_world_events.
RECOMP_EXPORT void bkrecomp_net_leaky_tick(void) {
    if (!recomp_net_is_connected()) {
        s_tracking_actor = FALSE;
        pending_leaky_count = 0;
        return;
    }

    if (map_get() != MAP_7_TTC_TREASURE_TROVE_COVE) {
        s_tracking_actor = FALSE;
        pending_leaky_count = 0;
        return;
    }

    Actor *leaky = actorArray_findActorFromMarkerId(MARKER_33_LEAKY);
    if (leaky == NULL) {
        s_tracking_actor = FALSE;
        return;
    }

    // First time we see Leaky this map-load: baseline without broadcasting.
    if (!s_tracking_actor) {
        s_last_seen_count = (u32)leaky->unk38_31;
        s_tracking_actor = TRUE;
    }

    // Apply remote events first so subsequent local-change detection does
    // not re-broadcast them.
    if (pending_leaky_count > 0) {
        s32 i;
        for (i = 0; i < pending_leaky_count; i++) {
            u32 target = pending_counts[i];
            if (target > (u32)leaky->unk38_31) {
                leaky->unk38_31 = target;
                comusic_playTrack(COMUSIC_2B_DING_B);
                recomp_printf("[LEAKY-SYNC] applied remote count=%d\n", target);
            }
        }
        pending_leaky_count = 0;
        s_last_seen_count = (u32)leaky->unk38_31;
    }

    // Detect fresh local increment.
    u32 cur = (u32)leaky->unk38_31;
    if (cur > s_last_seen_count) {
        recomp_net_send_flag_change(NET_FLAG_LEAKY_ACTION, 0, cur,
            (u32)MAP_7_TTC_TREASURE_TROVE_COVE);
        recomp_printf("[LEAKY-SYNC] sent count=%d\n", cur);
    }
    s_last_seen_count = cur;
}
