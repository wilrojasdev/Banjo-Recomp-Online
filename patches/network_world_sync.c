#include "patches.h"
#include "functions.h"
#include "enums.h"

// Network bridge
void recomp_net_send_collectible(u32 type, u32 id, u32 collected, u32 map_id, u32 level_id);
u32  recomp_net_pop_world_event(void *out);
u32  recomp_net_is_connected(void);

extern enum map_e map_get(void);
extern enum level_e level_get(void);
extern s32 item_getCount(enum item_e item);
extern s32 item_adjustByDiffWithHud(enum item_e item, s32 diff);
extern void marker_despawn(ActorMarker *marker);
extern ActorArray *suBaddieActorArray;

#define COLLECTIBLE_JINJO 2
#define EVENT_COLLECTIBLE 0

static s32 prev_jinjo_bits = 0;
static bool processing_remote = FALSE;

// Despawn actor by marker ID (removes from world immediately)
static bool despawn_actor_by_marker_id(u32 marker_id) {
    if (!suBaddieActorArray) return FALSE;
    s32 i;
    for (i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (actor->marker && actor->marker->id == marker_id) {
            recomp_printf("[DESPAWN] Found marker_id=0x%X at idx=%d, despawning\n", marker_id, i);
            marker_despawn(actor->marker);
            return TRUE;
        }
    }
    return FALSE;
}

// Debug: dump all jinjo-range actors
static void debug_dump_jinjo_actors(void) {
    if (!suBaddieActorArray) return;
    s32 i;
    for (i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (actor->marker && actor->marker->id >= 0x5A && actor->marker->id <= 0x5E) {
            recomp_printf("[JINJO-ACTOR] idx=%d marker_id=0x%X collidable=%d\n",
                i, actor->marker->id, actor->marker->collidable);
        }
    }
}

// === DETECT: Poll jinjo changes each frame ===
static void poll_jinjos(void) {
    if (!recomp_net_is_connected() || processing_remote) return;

    s32 cur = item_getCount(ITEM_12_JINJOS);
    if (cur != prev_jinjo_bits) {
        s32 new_bits = cur & ~prev_jinjo_bits;
        if (new_bits > 0) {
            recomp_net_send_collectible(COLLECTIBLE_JINJO, (u32)new_bits, 1,
                (u32)map_get(), (u32)level_get());
        }
        prev_jinjo_bits = cur;
    }
}

// === RECEIVE: Apply remote jinjo collection ===
typedef struct {
    u8  event_type;
    u8  _pad[3];
    struct { u8 type; u8 _p; u16 id; u8 collected; u8 _p2[3]; u32 map_id; u8 level_id; } collectible;
} WorldEventData;

static void process_jinjo_event(WorldEventData *evt) {
    processing_remote = TRUE;

    // Apply jinjo collection: increment bitmask + despawn (no animation)
    item_adjustByDiffWithHud(ITEM_12_JINJOS, (s32)evt->collectible.id);
    prev_jinjo_bits = item_getCount(ITEM_12_JINJOS);

    // Despawn the collected jinjo if on same map.
    // Bit mapping: (marker_id + 6) & 31 gives the shift amount.
    //   0x5A blue:   (90+6)&31 = 0 → bit 0x01
    //   0x5B green:  (91+6)&31 = 1 → bit 0x02
    //   0x5C orange: (92+6)&31 = 2 → bit 0x04
    //   0x5D pink:   (93+6)&31 = 3 → bit 0x08
    //   0x5E yellow: (94+6)&31 = 4 → bit 0x10
    if ((u32)map_get() == evt->collectible.map_id) {
        u32 bit = evt->collectible.id;
        u32 marker_id = 0;
        if (bit & 0x01) marker_id = MARKER_5A_JINJO_BLUE;
        else if (bit & 0x02) marker_id = MARKER_5B_JINJO_GREEN;
        else if (bit & 0x04) marker_id = MARKER_5C_JINJO_ORANGE;
        else if (bit & 0x08) marker_id = MARKER_5D_JINJO_PINK;
        else if (bit & 0x10) marker_id = MARKER_5E_JINJO_YELLOW;

        if (marker_id) {
            despawn_actor_by_marker_id(marker_id);
        }
    }

    processing_remote = FALSE;
}

// Called every frame
RECOMP_EXPORT void bkrecomp_net_process_world_events(void) {
    if (!recomp_net_is_connected()) return;

    poll_jinjos();

    WorldEventData evt;
    while (recomp_net_pop_world_event(&evt)) {
        if (evt.event_type == EVENT_COLLECTIBLE && evt.collectible.type == COLLECTIBLE_JINJO) {
            process_jinjo_event(&evt);
        }
    }
}
