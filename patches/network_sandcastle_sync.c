#include "patches.h"
#include "functions.h"
#include "enums.h"

/*
 * TTC Sandcastle (MAP_A) cheat-code progress sync.
 *
 * Vanilla: inside the sandcastle the floor has 28 letter tiles. Beak-
 * busting on a tile runs __maCastle_checkFloorTileForRegularCheatCode,
 * which walks each entry in sCheatCodes[] and, if the pressed letter
 * matches `code[codeCharacterIdx]`, increments codeCharacterIdx. When a
 * code's index reaches the terminator the code completes:
 *   - BANJOKAZOOIE (bit 0x0001) -> sMapState.banjoKazooieCodeEnteredState
 *     goes 0 -> 2, a 4 s unkC timer runs, then transitions to 3 and the
 *     jiggy mechanism fires.
 *   - Blue eggs / feathers codes (0x000E) -> fileProgress + item cap.
 *   - Bottles-bonus / Wishy-Washy / NoBonus (0x0FF0) -> volatile flags.
 *
 * Coop desired behaviour (per user spec): "notas tocadas compartidas"
 * -- intermediate code progress is accumulated across players. If P1
 * types the first two letters of BANJOKAZOOIE and P2 types the third,
 * the code advances.
 *
 * Completion side effects stay local to the player who pressed the
 * completing tile (SFX, dialog, cutscene). The resulting flags are
 * already broadcast by the phase 9 flag sync, so remote players inherit
 * the final state even if they miss the intermediate progress frames.
 *
 * Mechanism: poll sCheatCodes[].codeCharacterIdx (the 12 main codes)
 * and sMapState.banjoKazooieCodeEnteredState each frame while we are in
 * MAP_A. Broadcast monotonic increases; ignore resets (a local timeout
 * resetting a client's progress should not wipe the other client's).
 * Remote applies clamp to max(local, incoming).
 *
 * Scope: only the 12 entries of sCheatCodes are synced; the 40+ secret
 * "SnS" codes in sSecretsCheatCodes are not (they are narrative hints,
 * and their completions already propagate via fileProgressFlag sync).
 */

// Network bridge
void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);
u32  recomp_net_is_connected(void);

extern enum map_e map_get(void);

/*
 * Layout must match castle.c static declarations. The symbols live in
 * the TTC overlay data section and are resolved at recompile time
 * through datasyms.toml.
 */
typedef struct {
    u32 code_ptr;        // u8 * -- treated as opaque u32 (never deref'd here)
    s16 flagBitMask;
    s16 codeCharacterIdx;
} SandCheatCode;

extern SandCheatCode sCheatCodes[0xD];

typedef struct {
    u32 model1_ptr;
    u32 model2_ptr;
    u8  banjoKazooieCodeEnteredState;
    u8  doorOpeningSfxSourceIdx;
    u8  dullCannonShotSfxSourceId;
    u8  _padB;
    f32 unkC;
    u8  timerState;
    u8  _pad[3];
} SandMapState;

extern SandMapState sMapState;

// Flag type -- keep in sync with dispatch in network_flag_sync.c
#define NET_FLAG_SANDCASTLE_ACTION 12

#define NUM_CHEAT_CODES              12
#define SANDCASTLE_IDX_BANJO_STATE   0xFE  // sentinel in flag_index

#define MAX_PENDING_SANDCASTLE 32

typedef struct {
    u8 code_index;   // 0..NUM_CHEAT_CODES-1 or SANDCASTLE_IDX_BANJO_STATE
    u8 value;
} PendingSandcastleEvent;

static PendingSandcastleEvent pending[MAX_PENDING_SANDCASTLE];
static s32 pending_count = 0;

static s16 last_seen_idx[NUM_CHEAT_CODES];
static u8  last_seen_bk_state;
static bool tracking = FALSE;

RECOMP_EXPORT void bkrecomp_net_sandcastle_remote_apply(u32 code_index, u32 value) {
    if (pending_count >= MAX_PENDING_SANDCASTLE) return;
    pending[pending_count].code_index = (u8)code_index;
    pending[pending_count].value = (u8)value;
    pending_count++;
}

RECOMP_EXPORT void bkrecomp_net_sandcastle_tick(void) {
    if (!recomp_net_is_connected()) {
        tracking = FALSE;
        pending_count = 0;
        return;
    }

    if (map_get() != MAP_A_TTC_SANDCASTLE) {
        tracking = FALSE;
        pending_count = 0;
        return;
    }

    // Apply any pending remote events first so subsequent local-change
    // detection does not re-broadcast them.
    if (pending_count > 0) {
        s32 i;
        for (i = 0; i < pending_count; i++) {
            u8 idx = pending[i].code_index;
            u8 val = pending[i].value;

            if (idx == SANDCASTLE_IDX_BANJO_STATE) {
                if (val > sMapState.banjoKazooieCodeEnteredState) {
                    sMapState.banjoKazooieCodeEnteredState = val;
                    // Start the 4 s reveal timer fresh when a remote
                    // peer completes the BANJOKAZOOIE code.
                    if (val == 2) {
                        sMapState.unkC = 0.0f;
                    }
                    recomp_printf("[SAND-SYNC] applied remote bk_state=%d\n", val);
                }
            } else if (idx < NUM_CHEAT_CODES) {
                if ((s16)val > sCheatCodes[idx].codeCharacterIdx) {
                    sCheatCodes[idx].codeCharacterIdx = (s16)val;
                    recomp_printf("[SAND-SYNC] applied remote code[%d]=%d\n", idx, val);
                }
            }
        }
        pending_count = 0;
    }

    // Baseline on first frame in the map so we do not broadcast the
    // freshly-reset state.
    if (!tracking) {
        s32 i;
        for (i = 0; i < NUM_CHEAT_CODES; i++) {
            last_seen_idx[i] = sCheatCodes[i].codeCharacterIdx;
        }
        last_seen_bk_state = sMapState.banjoKazooieCodeEnteredState;
        tracking = TRUE;
        return;
    }

    // Detect local increments.
    {
        s32 i;
        for (i = 0; i < NUM_CHEAT_CODES; i++) {
            s16 cur = sCheatCodes[i].codeCharacterIdx;
            if (cur > last_seen_idx[i]) {
                recomp_net_send_flag_change(NET_FLAG_SANDCASTLE_ACTION,
                    (u32)i, (u32)cur, (u32)MAP_A_TTC_SANDCASTLE);
                recomp_printf("[SAND-SYNC] sent code[%d]=%d\n", i, cur);
            }
            last_seen_idx[i] = cur;
        }
    }

    {
        u8 bk = sMapState.banjoKazooieCodeEnteredState;
        if (bk > last_seen_bk_state) {
            recomp_net_send_flag_change(NET_FLAG_SANDCASTLE_ACTION,
                SANDCASTLE_IDX_BANJO_STATE, (u32)bk,
                (u32)MAP_A_TTC_SANDCASTLE);
            recomp_printf("[SAND-SYNC] sent bk_state=%d\n", bk);
        }
        last_seen_bk_state = bk;
    }
}
