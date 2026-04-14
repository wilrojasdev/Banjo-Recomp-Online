#include "patches.h"
#include "functions.h"
#include "enums.h"

// Network bridge
void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);
u32  recomp_net_is_connected(void);

extern enum map_e map_get(void);

// Internal decomp structures and CRC update functions
extern struct { s32 unk0; s32 unk4; u8 unk8[0x25]; } gFileProgressFlags;
extern void func_8031FC40(void);  // fileProgressFlags CRC1
extern void func_8031FEC0(void);  // fileProgressFlags CRC2

extern struct { u32 unk0; u32 unk4; u8 unk8[8]; } D_80383320;
extern void _levelSpecificFlags_updateCRC1(void);
extern void _levelSpecificFlags_updateCRC2(void);

extern struct { s32 unk0; s32 unk4; u8 unk8[0x19]; } gVolatileFlags;
extern void func_803202D0(void);  // volatileFlags CRC1
extern void func_803203A0(void);  // volatileFlags CRC2

extern u32 D_80367000;  // mapSpecificFlags storage
extern void _mapSpecificFlags_updateCRCs(void);

extern s32  bitfield_get_bit(u8 *array, s32 index);
extern s32  bitfield_get_n_bits(u8 *array, s32 offset, s32 numBits);
extern void bitfield_set_bit(u8 *array, s32 index, s32 set);
extern void bitfield_set_n_bits(u8 *array, s32 startIndex, s32 set, s32 length);

// Flag type constants (must match WorldFlagType enum in net_packets.h)
#define NET_FLAG_MAP_SPECIFIC   0
#define NET_FLAG_LEVEL_SPECIFIC 1
#define NET_FLAG_FILE_PROGRESS  2
#define NET_FLAG_VOLATILE       3
#define NET_FLAG_JIGSAW_ACTION  4
#define NET_FLAG_ABILITY        5
#define NET_FLAG_BOTTLES_ACTION 6

// Bottles lock actions
#define BOTTLES_ACTION_LOCK            0
#define BOTTLES_ACTION_UNLOCK          1
#define BOTTLES_ACTION_LOCK_REFRESHER  2

// Jigsaw sync (from network_jigsaw_sync.c)
extern void bkrecomp_net_process_jigsaw_event(u32 flag_index, u32 value, u32 map_id);

// Bottles visual sync (from network_bottles_sync.c)
extern void bkrecomp_net_bottles_remote_emerge(void);
extern void bkrecomp_net_bottles_remote_hide(void);

// Ability system
extern s32 ability_hasLearned(s32 ability);
extern void ability_getSizeAndPtr(s32 *size, u8 **addr);
extern s32 learnedAbilities;
extern s32 usedAbilities;

// Lair storage — saves abilities across level transitions.
// D_8037DCB8->unlockedMoves (offset 0x1C) must be updated when abilities change,
// otherwise the lair restore at level entry overwrites network-synced abilities.
extern struct FF_StorageStruct *D_8037DCB8;
struct FF_StorageStruct { u8 _pad[0x1C]; u32 unlockedMoves; };

static void sync_lair_ability_state(void) {
    if (D_8037DCB8) {
        D_8037DCB8->unlockedMoves = (u32)learnedAbilities;
    }
}

// Network bridge for local player ID
extern u32 recomp_net_get_local_player_id(void);

// Bottles NPC lock (one player at a time, like jigsaw pedestal)
static struct {
    bool locked;
    u8   owner_player_id;
} bottles_lock_state = { FALSE, 0 };

static void net_bottles_lock(u8 player_id) {
    bottles_lock_state.locked = TRUE;
    bottles_lock_state.owner_player_id = player_id;
}

static void net_bottles_unlock(void) {
    bottles_lock_state.locked = FALSE;
    bottles_lock_state.owner_player_id = 0;
}

// Exported for use from network_bottles_sync or other patches
RECOMP_EXPORT bool bkrecomp_net_bottles_is_locked(void) {
    return bottles_lock_state.locked;
}

RECOMP_EXPORT bool bkrecomp_net_bottles_is_local_owner(void) {
    if (!bottles_lock_state.locked) return FALSE;
    return bottles_lock_state.owner_player_id == (u8)recomp_net_get_local_player_id();
}

RECOMP_EXPORT u8 bkrecomp_net_bottles_get_lock_owner(void) {
    return bottles_lock_state.owner_player_id;
}

RECOMP_EXPORT void bkrecomp_net_bottles_send_lock(void) {
    if (!recomp_net_is_connected()) return;
    u32 my_id = recomp_net_get_local_player_id();
    net_bottles_lock((u8)my_id);
    recomp_net_send_flag_change(NET_FLAG_BOTTLES_ACTION, BOTTLES_ACTION_LOCK, my_id, (u32)map_get());
}

RECOMP_EXPORT void bkrecomp_net_bottles_send_lock_refresher(void) {
    if (!recomp_net_is_connected()) return;
    u32 my_id = recomp_net_get_local_player_id();
    net_bottles_lock((u8)my_id);
    recomp_net_send_flag_change(NET_FLAG_BOTTLES_ACTION, BOTTLES_ACTION_LOCK_REFRESHER, my_id, (u32)map_get());
}

RECOMP_EXPORT void bkrecomp_net_bottles_send_unlock(void) {
    if (!recomp_net_is_connected()) return;
    net_bottles_unlock();
    recomp_net_send_flag_change(NET_FLAG_BOTTLES_ACTION, BOTTLES_ACTION_UNLOCK, 0, (u32)map_get());
}

// Guard: prevents echo loop when applying remote flag changes
static bool net_applying_remote_flag = FALSE;

// Allow other modules (jigsaw sync) to set/clear the remote flag guard
RECOMP_EXPORT void bkrecomp_net_set_remote_flag_guard(bool val) {
    net_applying_remote_flag = val;
}

// === RECOMP_PATCH: Intercept ability learn ===

RECOMP_PATCH void ability_setLearned(s32 ability, bool hasLearned) {
    s32 old = ability_hasLearned(ability) ? 1 : 0;

    // Apply locally
    if (hasLearned) {
        learnedAbilities |= (1 << ability);
    } else {
        learnedAbilities &= ~(1 << ability);
    }

    s32 new_val = hasLearned ? 1 : 0;
    if (!net_applying_remote_flag && recomp_net_is_connected() && old != new_val) {
        recomp_net_send_flag_change(NET_FLAG_ABILITY, (u32)ability, (u32)new_val, (u32)map_get());
        recomp_printf("[ABILITY-SYNC] sent ability %d = %d\n", ability, new_val);
    }
}

// === RECOMP_PATCH: Intercept flag set functions ===
// Only send network events when the flag value ACTUALLY CHANGES.

RECOMP_PATCH void fileProgressFlag_set(enum file_progress_e index, s32 set) {
    s32 old = bitfield_get_bit(gFileProgressFlags.unk8, index);
    bitfield_set_bit(gFileProgressFlags.unk8, index, set);
    func_8031FC40();
    func_8031FEC0();

    s32 new_val = set ? 1 : 0;
    if (!net_applying_remote_flag && recomp_net_is_connected() && old != new_val) {
        recomp_net_send_flag_change(NET_FLAG_FILE_PROGRESS, (u32)index,
            (u32)new_val, (u32)map_get());
    }
}

RECOMP_PATCH void fileProgressFlag_setN(enum file_progress_e startIndex, s32 set, s32 length) {
    s32 old_val = bitfield_get_n_bits(gFileProgressFlags.unk8, startIndex, length);
    bitfield_set_n_bits(gFileProgressFlags.unk8, startIndex, set, length);
    func_8031FC40();
    func_8031FEC0();

    if (!net_applying_remote_flag && recomp_net_is_connected() && old_val != set) {
        s32 i;
        u32 cur_map = (u32)map_get();
        for (i = 0; i < length; i++) {
            u32 val = ((1 << i) & set) ? 1 : 0;
            recomp_net_send_flag_change(NET_FLAG_FILE_PROGRESS,
                (u32)(startIndex + i), val, cur_map);
        }
    }
}

RECOMP_PATCH void levelSpecificFlags_set(s32 index, s32 val) {
    s32 old = bitfield_get_bit(&D_80383320.unk8, index);
    bitfield_set_bit(&D_80383320.unk8, index, val);
    _levelSpecificFlags_updateCRC1();
    _levelSpecificFlags_updateCRC2();

    s32 new_val = val ? 1 : 0;
    if (!net_applying_remote_flag && recomp_net_is_connected() && old != new_val) {
        recomp_net_send_flag_change(NET_FLAG_LEVEL_SPECIFIC, (u32)index,
            (u32)new_val, (u32)map_get());
    }
}

RECOMP_PATCH void levelSpecificFlags_setN(s32 index, s32 val, s32 n) {
    s32 old_val = bitfield_get_n_bits(&D_80383320.unk8, index, n);
    bitfield_set_n_bits(&D_80383320.unk8, index, val, n);
    _levelSpecificFlags_updateCRC1();
    _levelSpecificFlags_updateCRC2();

    if (!net_applying_remote_flag && recomp_net_is_connected() && old_val != val) {
        s32 i;
        u32 cur_map = (u32)map_get();
        for (i = 0; i < n; i++) {
            u32 v = ((1 << i) & val) ? 1 : 0;
            recomp_net_send_flag_change(NET_FLAG_LEVEL_SPECIFIC,
                (u32)(index + i), v, cur_map);
        }
    }
}

RECOMP_PATCH void volatileFlag_set(enum volatile_flags_e index, s32 set) {
    s32 old = bitfield_get_bit(gVolatileFlags.unk8, index);
    bitfield_set_bit(gVolatileFlags.unk8, index, set);
    func_803202D0();
    func_803203A0();

    s32 new_val = set ? 1 : 0;
    if (!net_applying_remote_flag && recomp_net_is_connected() && old != new_val) {
        recomp_net_send_flag_change(NET_FLAG_VOLATILE, (u32)index,
            (u32)new_val, (u32)map_get());
    }
}

RECOMP_PATCH void volatileFlag_setN(enum volatile_flags_e startIndex, s32 set, s32 length) {
    s32 old_val = bitfield_get_n_bits(gVolatileFlags.unk8, startIndex, length);
    bitfield_set_n_bits(gVolatileFlags.unk8, startIndex, set, length);
    func_803202D0();
    func_803203A0();

    if (!net_applying_remote_flag && recomp_net_is_connected() && old_val != set) {
        s32 i;
        u32 cur_map = (u32)map_get();
        for (i = 0; i < length; i++) {
            u32 val = ((1 << i) & set) ? 1 : 0;
            recomp_net_send_flag_change(NET_FLAG_VOLATILE,
                (u32)(startIndex + i), val, cur_map);
        }
    }
}

RECOMP_PATCH void mapSpecificFlags_set(s32 i, s32 val) {
    s32 old = (D_80367000 & (1 << i)) ? 1 : 0;
    if (val)
        D_80367000 |= 1 << i;
    else
        D_80367000 &= ~(1 << i);
    _mapSpecificFlags_updateCRCs();

    s32 new_val = val ? 1 : 0;
    if (!net_applying_remote_flag && recomp_net_is_connected() && old != new_val) {
        recomp_net_send_flag_change(NET_FLAG_MAP_SPECIFIC, (u32)i,
            (u32)new_val, (u32)map_get());
    }
}

// === RECEIVE: Apply remote flag events ===

// FlagEventData layout (matches net_recomp_api.cpp FLAG case output):
typedef struct {
    u8  event_type;    // 0x00 = 2 (EVENT_FLAG)
    u8  _pad[3];       // 0x01-0x03
    u8  flag_type;     // 0x04
    u8  _fp;           // 0x05
    u16 flag_index;    // 0x06
    u8  flag_value;    // 0x08
    u8  _pad2[3];      // 0x09-0x0B
    u32 flag_map_id;   // 0x0C
} FlagEventData;

RECOMP_EXPORT void bkrecomp_net_process_flag_event(void *data) {
    FlagEventData *evt = (FlagEventData *)data;

    net_applying_remote_flag = TRUE;

    u8 ft = evt->flag_type;
    u16 idx = evt->flag_index;
    s32 val = (s32)evt->flag_value;

    // Only log non-spammy flag types (bottles, ability, jigsaw)
    if (ft >= NET_FLAG_JIGSAW_ACTION) {
        recomp_printf("[FLAG-EVENT] type=%d idx=%d val=%d map=%d\n", ft, idx, val, evt->flag_map_id);
    }

    if (ft == NET_FLAG_FILE_PROGRESS) {
        fileProgressFlag_set((enum file_progress_e)idx, val);
    } else if (ft == NET_FLAG_LEVEL_SPECIFIC) {
        levelSpecificFlags_set((s32)idx, val);
    } else if (ft == NET_FLAG_VOLATILE) {
        volatileFlag_set((enum volatile_flags_e)idx, val);
    } else if (ft == NET_FLAG_MAP_SPECIFIC) {
        mapSpecificFlags_set((s32)idx, val);
    } else if (ft == NET_FLAG_JIGSAW_ACTION) {
        // Delegate to jigsaw sync — it manages its own guard via bkrecomp_net_set_remote_flag_guard
        net_applying_remote_flag = FALSE;
        bkrecomp_net_process_jigsaw_event((u32)idx, (u32)evt->flag_value, evt->flag_map_id);
        return;
    } else if (ft == NET_FLAG_ABILITY) {
        ability_setLearned((s32)idx, val);
        sync_lair_ability_state();
        recomp_printf("[ABILITY-SYNC] applied ability[%d] = %d\n", idx, val);
    } else if (ft == NET_FLAG_BOTTLES_ACTION) {
        if (idx == BOTTLES_ACTION_LOCK) {
            net_bottles_lock((u8)val);
            // First-time learn: show emerge animation on remote side
            bkrecomp_net_bottles_remote_emerge();
            recomp_printf("[BOTTLES-SYNC] locked by player %d (emerge)\n", val);
        } else if (idx == BOTTLES_ACTION_LOCK_REFRESHER) {
            net_bottles_lock((u8)val);
            // Refresher: lock only, no emerge animation
            recomp_printf("[BOTTLES-SYNC] locked by player %d (refresher)\n", val);
        } else if (idx == BOTTLES_ACTION_UNLOCK) {
            net_bottles_unlock();
            // Show exit animation on remote side
            bkrecomp_net_bottles_remote_hide();
            recomp_printf("[BOTTLES-SYNC] unlocked\n");
        }
    }

    net_applying_remote_flag = FALSE;
}

// Bulk apply flags from full state sync (called from network_world_sync.c)
RECOMP_EXPORT void bkrecomp_net_apply_flag_bulk(
    u8 *file_progress, s32 fp_size,
    u8 *level_specific, s32 ls_size,
    u8 *volatile_flags, s32 vf_size,
    u32 map_flags,
    u8 *abilities, s32 ab_size)
{
    net_applying_remote_flag = TRUE;

    // File progress flags
    {
        s32 i;
        for (i = 0; i < fp_size && i < 0x25; i++) {
            gFileProgressFlags.unk8[i] = file_progress[i];
        }
        func_8031FC40();
        func_8031FEC0();
    }

    // Level-specific flags
    {
        s32 i;
        for (i = 0; i < ls_size && i < 8; i++) {
            D_80383320.unk8[i] = level_specific[i];
        }
        _levelSpecificFlags_updateCRC1();
        _levelSpecificFlags_updateCRC2();
    }

    // Volatile flags
    {
        s32 i;
        for (i = 0; i < vf_size && i < 0x19; i++) {
            gVolatileFlags.unk8[i] = volatile_flags[i];
        }
        func_803202D0();
        func_803203A0();
    }

    // Map-specific flags
    D_80367000 = map_flags;
    _mapSpecificFlags_updateCRCs();

    // Abilities (8 bytes: learnedAbilities + usedAbilities)
    // OR remote abilities into local — never lose locally learned abilities
    // (e.g., SM abilities from empty slot boot should persist)
    if (abilities && ab_size >= 8) {
        s32 ab_sz;
        u8 *ab_ptr;
        ability_getSizeAndPtr(&ab_sz, &ab_ptr);
        // OR byte-by-byte (both sides are MIPS big-endian, same layout)
        s32 i;
        for (i = 0; i < 8 && i < ab_size; i++) {
            ab_ptr[i] |= abilities[i];
        }
        sync_lair_ability_state();
    }

    net_applying_remote_flag = FALSE;
    recomp_printf("[FLAG-SYNC] applied bulk flags (fp=%d ls=%d vf=%d map=0x%X ab=%d)\n",
        fp_size, ls_size, vf_size, map_flags, ab_size);
}
