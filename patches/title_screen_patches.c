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

static bool online_autoload_triggered = FALSE;

RECOMP_PATCH void gameSelect_initAndUpdate(Actor *this) {
    // Online mode: skip file select, auto-load save and warp
    if (recomp_net_is_online_mode() && !online_autoload_triggered) {
        online_autoload_triggered = TRUE;

        gameFile_8033CE40();

        s32 slot = (s32)recomp_net_get_save_slot();
        if (recomp_net_is_join_mode()) {
            slot = 0;
        }

        gameSelect_setGameNumber(slot);

        if (gameFile_isNotEmpty(slot)) {
            gameFile_load(slot);
            chBottlesBonus_resetCompleted();

            if (chmole_learnedAllSpiralMountainAbilities() && fileProgressFlag_get(FILEPROG_BD_ENTER_LAIR_CUTSCENE)) {
                timedFunc_set_2(0.0f, (void*)warp_lairEnterLairFromSMLevel, 0, 0);
            } else {
                timedFunc_set_2(0.0f, (void*)warp_smExitBanjosHouse, 0, 0);
            }
            timedFunc_set_1(0.0f, (void*)func_80335110, 1);
        }
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