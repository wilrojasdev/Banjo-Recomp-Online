#include "patches.h"
#include "functions.h"
#include "enums.h"

extern u32 recomp_net_is_online_mode(void);
extern u32 recomp_net_get_save_slot(void);
extern u32 recomp_net_is_join_mode(void);

// Game functions needed for auto-load
extern enum map_e map_get(void);
extern void gameSelect_setGameNumber(s32 num);
extern void gameFile_load(s32 gamenum);
extern bool gameFile_isNotEmpty(s32 gamenum);
extern bool chmole_learnedAllSpiralMountainAbilities(void);
extern bool fileProgressFlag_get(enum file_progress_e flag);
extern void warp_lairEnterLairFromSMLevel(s32, s32);
extern void warp_smExitBanjosHouse(s32, s32);
extern void func_80335110(s32);
extern void chBottlesBonus_resetCompleted(void);
extern void ability_setLearned(enum ability_e ability, bool hasLearned);
extern void fileProgressFlag_set(enum file_progress_e flag, bool value);
extern void clearScoreStates(void);
extern void bkrecomp_net_reset_poll_baselines(void);

// Save override (network_save_override.c)
extern bool bkrecomp_net_save_override_is_ready(void);
extern void bkrecomp_net_save_override_enable(void);
extern s32  bkrecomp_net_save_get_host_slot(void);

// @recomp Skip intro cutscenes when online mode is configured — boot directly to file select.
RECOMP_PATCH enum map_e getDefaultBootMap(void) {
    if (recomp_net_is_online_mode()) {
        return MAP_91_FILE_SELECT;
    }
    return MAP_1F_CS_START_RAREWARE;
}

// @recomp Patch file select: in online mode, auto-load save and warp to Lair.
extern void gameSelect_update(Actor *this);
extern void gameFile_8033CE40(void);

static bool online_autoload_done_for_map = FALSE;
static s32 last_autoload_map = -1;

RECOMP_PATCH void gameSelect_initAndUpdate(Actor *this) {
    // Online mode: skip file select, auto-load save and warp.
    // Reset the flag if the map changed (e.g. returning to file select after stop_game).
    enum map_e cur_map = map_get();
    if (cur_map != last_autoload_map) {
        online_autoload_done_for_map = FALSE;
        last_autoload_map = cur_map;
    }

    if (recomp_net_is_online_mode() && !online_autoload_done_for_map) {
        bool is_join = recomp_net_is_join_mode();

        // Join waits for the host's EEPROM snapshot before continuing.
        // With the snapshot we can load the exact same save state the host
        // is running on, via the eeprom_readBlocks override. Without it,
        // we'd have to guess at the starting state and deal with polling
        // leaks again.
        if (is_join && !bkrecomp_net_save_override_is_ready()) {
            gameSelect_update(this);
            return;
        }

        online_autoload_done_for_map = TRUE;

        // Fresh RAM state. Vanilla relied on setGameInformationZoombox()
        // to call clearScoreStates when the player focused a slot; our
        // autoload bypasses that scene.
        clearScoreStates();

        if (is_join) {
            // Flip the EEPROM backing store to the RAM buffer filled by
            // the HostEeprom packet. Every eeprom_readBlocks/writeBlocks
            // call from here on will target RAM, so: (1) the game sees
            // the host's save data when we re-index the file table; and
            // (2) any save prompt later just writes to RAM and is
            // discarded at session end — join's on-disk save is never
            // touched.
            bkrecomp_net_save_override_enable();
        }

        // (Re-)index the save data table. For the host this reads the
        // real EEPROM; for the join it reads from the override buffer
        // and populates gameFile_saveData[] with the host's slot layout.
        gameFile_8033CE40();

        s32 slot;
        if (is_join) {
            // Use the exact slot the host is playing (shipped in the
            // HostEeprom packet). Scanning for "first non-empty" is wrong
            // when the host's EEPROM has leftover data in another slot —
            // we'd load that unrelated save and diverge from the host's
            // live state.
            s32 host_slot = bkrecomp_net_save_get_host_slot();
            if (host_slot < 0 || host_slot > 2) {
                // Fallback: if for some reason we never got a slot in
                // the packet, still pick the first non-empty slot rather
                // than crashing on an out-of-range index.
                slot = 0;
                for (s32 i = 0; i < 3; i++) {
                    if (gameFile_isNotEmpty(i)) { slot = i; break; }
                }
            } else {
                slot = host_slot;
            }
        } else {
            slot = (s32)recomp_net_get_save_slot();
        }

        gameSelect_setGameNumber(slot);
        chBottlesBonus_resetCompleted();

        if (gameFile_isNotEmpty(slot)) {
            gameFile_load(slot);

            if (chmole_learnedAllSpiralMountainAbilities() && fileProgressFlag_get(FILEPROG_BD_ENTER_LAIR_CUTSCENE)) {
                timedFunc_set_2(0.0f, (void*)warp_lairEnterLairFromSMLevel, 0, 0);
            } else {
                timedFunc_set_2(0.0f, (void*)warp_smExitBanjosHouse, 0, 0);
            }
        } else {
            // Empty slot (host fresh game). Seed SM base abilities so the
            // player can move, plus the note-door ability gate (see
            // chnotedoor_update — gated on ABILITY_13_1ST_NOTEDOOR, which
            // vanilla only grants via Bottles after 50 MM notes).
            ability_setLearned(ABILITY_0_BARGE, TRUE);
            ability_setLearned(ABILITY_4_CLAW_SWIPE, TRUE);
            ability_setLearned(ABILITY_5_CLIMB, TRUE);
            ability_setLearned(ABILITY_7_FEATHERY_FLAP, TRUE);
            ability_setLearned(ABILITY_8_FLAP_FLIP, TRUE);
            ability_setLearned(ABILITY_A_HOLD_A_JUMP_HIGHER, TRUE);
            ability_setLearned(ABILITY_B_RATATAT_RAP, TRUE);
            ability_setLearned(ABILITY_C_ROLL, TRUE);
            ability_setLearned(ABILITY_F_DIVE, TRUE);
            ability_setLearned(ABILITY_13_1ST_NOTEDOOR, TRUE);
            fileProgressFlag_set(FILEPROG_BD_ENTER_LAIR_CUTSCENE, TRUE);

            timedFunc_set_2(0.0f, (void*)warp_lairEnterLairFromSMLevel, 0, 0);
        }
        timedFunc_set_1(0.0f, (void*)func_80335110, 1);

        // Prime the collectible poll baselines with the final post-load
        // score state. Without this, the first poll tick interprets every
        // loaded-save bit as a fresh collection and broadcasts it.
        bkrecomp_net_reset_poll_baselines();
        return;
    }

    // Normal offline file select — call the update function
    // (init is handled by the game engine's actor system)
    gameSelect_update(this);
}

struct Struct_core2_9B180_1;
typedef struct Struct_core2_9B180_1 Struct_core2_9B180_1;
struct struct_core2_9B180_s;

typedef struct struct_core2_9B180_s{
    s16 unk0;
    // u8 pad2[0x2];
    Struct_core2_9B180_1 *unk4;
    void (*unk8)(struct struct_core2_9B180_s *);
    void (*unkC)(struct struct_core2_9B180_s *);
    void (*unk10)(struct struct_core2_9B180_s *);
}Struct_core2_9B180_0;

extern u8 D_80383330;
extern Struct_core2_9B180_0 D_8036DE00[];

void func_80322318(Struct_core2_9B180_0*);

// @recomp Patched to always allow skipping the intro sequence.
RECOMP_PATCH void func_80322490(void) {
    Struct_core2_9B180_0 *i_ptr;
    static int introFrameCounter = 0;

    introFrameCounter++;

    if (D_80383330 != 0) {
        for(i_ptr = D_8036DE00; i_ptr != &D_8036DE00[6]; i_ptr++){
            // @recomp Always allow skipping thex intro sequence, with a delay of 1 second to prevent
            // issues with accidentally skipping the intro when navigating the launcher with a controller.
            if((i_ptr->unk4 != 0 || (i_ptr->unkC == func_80322318 && map_get() == MAP_1F_CS_START_RAREWARE && introFrameCounter > 30)) 
            && i_ptr->unkC != NULL){
                i_ptr->unkC(i_ptr);
            }
        }
    }
}