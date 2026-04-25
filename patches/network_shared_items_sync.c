#include "patches.h"
#include "functions.h"
#include "enums.h"

extern Actor *actorArray_findActorFromMarkerId(s32 marker_id);
extern void marker_despawn(ActorMarker *marker);

/*
 * Shared inventory sync: eggs, red feathers, gold feathers.
 *
 * Coop spec: these three items form a SHARED pool. Anyone picking up an
 * egg increments everyone's count; anyone firing one decrements everyone.
 * Mumbo tokens stay per-player (treated like jiggies — collected once
 * per session, persisted in the host EEPROM).
 *
 * Mechanism: polling-based diff sync. Each frame we read the local
 * D_80385F30[item] count for the three tracked items, compare to the
 * last seen value, and broadcast the signed delta. Receivers apply the
 * delta via item_adjustByDiffWithHud so the player sees the HUD popup
 * (satisfying feedback that the shared pool changed).
 *
 * Diff (vs absolute) is used so concurrent pickups/fires from two
 * players accumulate correctly: P1 +1 and P2 +1 = pool +2, instead of
 * absolute-value broadcasts where the later one would clobber the
 * earlier. The trade-off is signed magnitude is capped at +/-127, but
 * the largest single jump in the game is the mole's 50-egg gift, well
 * within range.
 */

void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);
u32  recomp_net_is_connected(void);
extern enum map_e map_get(void);

extern s32  item_getCount(enum item_e item);
extern s32  item_adjustByDiffWithHud(enum item_e item, s32 diff);
extern void item_adjustByDiffWithoutHud(enum item_e item, s32 diff);

#define NET_FLAG_SHARED_ITEM 16

#define MAX_PENDING_SHARED_ITEM 16

typedef struct {
    u8 item;   // enum item_e
    s8 diff;   // signed delta to apply
} PendingSharedItem;

static PendingSharedItem s_pending[MAX_PENDING_SHARED_ITEM];
static s32 s_pending_count = 0;

// -1 sentinel = uninitialized; first poll baselines without broadcasting.
static s32 s_last_eggs   = -1;
static s32 s_last_red    = -1;
static s32 s_last_gold   = -1;
static s32 s_last_orange = -1;

static bool is_shared_item(u32 item) {
    return item == ITEM_D_EGGS
        || item == ITEM_F_RED_FEATHER
        || item == ITEM_10_GOLD_FEATHER
        || item == ITEM_19_ORANGE;
}

// Called from network_flag_sync.c when a remote FLAG_SHARED_ITEM event arrives.
// `value` is the signed-int8 diff packed into the u8 wire field.
RECOMP_EXPORT void bkrecomp_net_shared_item_remote_apply(u32 item, u32 value) {
    if (!is_shared_item(item)) return;
    if (s_pending_count >= MAX_PENDING_SHARED_ITEM) return;
    s_pending[s_pending_count].item = (u8)item;
    s_pending[s_pending_count].diff = (s8)(u8)value;
    s_pending_count++;
}

static void send_diff_if_changed(enum item_e item, s32 cur, s32 *last) {
    s32 d = cur - *last;
    if (d == 0) return;
    if (d >  127) d =  127;
    if (d < -128) d = -128;
    u8 packed = (u8)(s8)d;
    recomp_net_send_flag_change(NET_FLAG_SHARED_ITEM, (u32)item, (u32)packed, (u32)map_get());
    *last = cur;
}

// Called every frame from bkrecomp_net_process_world_events.
RECOMP_EXPORT void bkrecomp_net_shared_items_tick(void) {
    if (!recomp_net_is_connected()) {
        s_last_eggs   = -1;
        s_last_red    = -1;
        s_last_gold   = -1;
        s_last_orange = -1;
        s_pending_count = 0;
        return;
    }

    // Apply remote diffs first. After applying, rebaseline so the change
    // we just made doesn't get re-broadcast on this same tick.
    if (s_pending_count > 0) {
        s32 i;
        for (i = 0; i < s_pending_count; i++) {
            enum item_e it = (enum item_e)s_pending[i].item;
            s32 d = (s32)s_pending[i].diff;
            if (d != 0) {
                item_adjustByDiffWithHud(it, d);
            }
            /* Side effect: orange tree pickup. Vanilla never sets
             * MM_SPECIFIC_FLAG_1, so the despawn-by-flag path in
             * network_flag_sync.c never fires. Use the +diff event itself
             * as the "someone collected it" trigger and despawn the
             * world copy here. */
            if (it == ITEM_19_ORANGE && d > 0) {
                Actor *orange = actorArray_findActorFromMarkerId(MARKER_36_ORANGE_COLLECTIBLE);
                if (orange && orange->marker) {
                    marker_despawn(orange->marker);
                }
            }
        }
        s_pending_count = 0;
        s_last_eggs   = item_getCount(ITEM_D_EGGS);
        s_last_red    = item_getCount(ITEM_F_RED_FEATHER);
        s_last_gold   = item_getCount(ITEM_10_GOLD_FEATHER);
        s_last_orange = item_getCount(ITEM_19_ORANGE);
        return;
    }

    s32 cur_eggs   = item_getCount(ITEM_D_EGGS);
    s32 cur_red    = item_getCount(ITEM_F_RED_FEATHER);
    s32 cur_gold   = item_getCount(ITEM_10_GOLD_FEATHER);
    s32 cur_orange = item_getCount(ITEM_19_ORANGE);

    // First poll after connect: silent baseline, no broadcast.
    if (s_last_eggs < 0) {
        s_last_eggs   = cur_eggs;
        s_last_red    = cur_red;
        s_last_gold   = cur_gold;
        s_last_orange = cur_orange;
        return;
    }

    send_diff_if_changed(ITEM_D_EGGS,          cur_eggs,   &s_last_eggs);
    send_diff_if_changed(ITEM_F_RED_FEATHER,   cur_red,    &s_last_red);
    send_diff_if_changed(ITEM_10_GOLD_FEATHER, cur_gold,   &s_last_gold);
    send_diff_if_changed(ITEM_19_ORANGE,       cur_orange, &s_last_orange);
}
