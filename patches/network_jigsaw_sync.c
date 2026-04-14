#include "patches.h"
#include "functions.h"
#include "enums.h"

// From jigsawpicture.h
typedef enum {
    JIGSAW_PICTURE_LEAVE_PODIUM = 1,
    JIGSAW_PICTURE_ENTER_PODIUM = 2,
    JIGSAW_PICTURE_INSTRUCTIONS = 3,
    JIGSAW_PICTURE_WAITING = 4,
    JIGSAW_PICTURE_ADD_PIECE = 5,
    JIGSAW_PICTURE_ADD_ALL = 6,
    JIGSAW_PICTURE_COMPLETE = 7,
    JIGSAW_PICTURE_REMOVE_PIECE = 8
} JigsawPictureState;

// From jigsawpicture.c — internal struct (actor->local)
typedef struct {
    s32 placedPieces;
    s32 placedJiggyCount;
    s32 unk8;
} JigsawPictureActorData;

// Network bridge
void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);
u32  recomp_net_is_connected(void);
u32  recomp_net_get_local_player_id(void);
u32  recomp_net_get_remote_count(void);
extern void bkrecomp_net_set_remote_flag_guard(bool val);

// Decomp externs
extern enum map_e map_get(void);
extern enum level_e level_get(void);
extern s32 item_getCount(enum item_e item);
extern s32 item_adjustByDiffWithoutHud(enum item_e item, s32 diff);
extern void player_getPosition(f32 pos[3]);
extern s32 player_movementGroup(void);
extern bool fileProgressFlag_get(enum file_progress_e index);
extern void fileProgressFlag_set(enum file_progress_e index, s32 set);
extern s32  fileProgressFlag_getN(enum file_progress_e offset, s32 numBits);
extern void fileProgressFlag_setN(enum file_progress_e startIndex, s32 set, s32 length);
extern void comusic_playTrack(enum comusic_e track);

// Forward declarations of replicated helper functions
static s32 getPictureCost(Actor *this);
static bool isPictureComplete(Actor *this);
static bool isBanjoOnPodium(ActorMarker *marker);
static s32 getPicturePiecePosition_local(Actor *this);
static void addOrRemovePieceFromDisplay(Actor *this, s32 position, bool isAdd);
static void setInitialJigsawPictureOpacity(Actor *this);
static s32 local_isPicturePiecePlaced(Actor *this, s32 position);
static s32 getUnknownJigsawPictureIndex(Actor *this);
static s32 getLevelSpecificOpenFlag(Actor *this);
static void jigsaw_opacity_update(Actor *this);

// Overlay functions cannot be called by name from patches.
// We must replicate the ones we need, using CORE functions for visuals.
extern void updateStruct6DsOpacity(s32, s32, s32, f32);
extern void setStruct6DsOpacity(s32, s32);
extern void func_8034DF30(s32, f32[3], f32[3], f32);

// Core game functions
extern s32 func_8028F20C(void);
extern s32 func_8028FB48(s32);
extern void func_8028E6EC(s32);
extern void func_8028F918(s32);
extern void func_8028F94C(s32, f32[3]);
extern s32 subaddie_playerIsWithinSphereAndActive(Actor *, s32);
extern void func_803115C4(s32);
extern s32 progressDialog_showDialogMaskZero(enum file_progress_e);
extern enum asset_e gcdialog_getCurrentTextId(void);
extern void func_80324CFC(f32, enum comusic_e, s32);
extern void func_80324D2C(f32, enum comusic_e);
extern void func_8030E6D4(enum sfx_e);
extern void __bundle_spawnFromFirstActor(enum bundle_e bundle_id, Actor *actor);
extern f32 randf(void);
extern void rand_seed(s32);
extern void particleEmitter_setAlpha(ParticleEmitter *emitter, s32 alpha);
extern void itemPrint_reset(void);
extern void func_802FACA4(s32);
extern struct5Bs *func_803097A0(void);
extern void player_walkToPosition(f32[3], f32, void(*)(ActorMarker *), ActorMarker *);
extern void func_802FAFD4(enum item_e, enum sfx_e);
extern void func_802FAFC0(enum item_e, enum comusic_e);
extern void func_80347958(void);
extern void gcpausemenu_80314AC8(s32);
extern s32 item_adjustByDiffWithHud(enum item_e, s32);
extern void func_80324DBC(f32, enum asset_e, s32, f32[3], ActorMarker *, void (*)(ActorMarker *, enum asset_e, s32), void (*)(ActorMarker *, enum asset_e, s32));

// PICTURE_INFO table (needed for flag sync)
typedef struct {
    u8 cost;
    u8 sizeBits;
    u16 progressFlag;
} JigsawPictureInfo;

static JigsawPictureInfo PICTURE_INFO[0xB] = {
    { 1, 0x1, FILEPROG_5D_MM_PUZZLE_PIECES_PLACED },
    { 2, 0x2, FILEPROG_5E_TCC_PUZZLE_PIECES_PLACED },
    { 5, 0x3, FILEPROG_60_CC_PUZZLE_PIECES_PLACED },
    { 7, 0x3, FILEPROG_63_BGS_PUZZLE_PIECES_PLACED },
    { 8, 0x4, FILEPROG_66_FP_PUZZLE_PIECES_PLACED },
    { 9, 0x4, FILEPROG_6A_GV_PUZZLE_PIECES_PLACED },
    {10, 0x4, FILEPROG_6E_MMM_PUZZLE_PIECES_PLACED },
    {12, 0x4, FILEPROG_72_RBB_PUZZLE_PIECES_PLACED },
    {15, 0x4, FILEPROG_76_CCW_PUZZLE_PIECES_PLACED },
    {25, 0x5, FILEPROG_7A_DOG_PUZZLE_PIECES_PLACED },
    { 4, 0x3, FILEPROG_7F_DOUBLE_HEALTH_PUZZLE_PIECES_PLACED }
};

// Particle data for CCW
static ParticleScaleAndLifetimeRanges D_80394830_copy = {
    { 0.17f, 0.24f },
    { 0.08f, 0.13f },
    { 0.0f, 0.01f },
    { 0.9f, 0.9f },
    0.0f,
    0.0f
};

// Network flag type
#define NET_FLAG_JIGSAW_ACTION 4

// Jigsaw actions encoded in flag_index: (puzzle_id << 8) | action
#define JIGSAW_ACTION_LOCK     0
#define JIGSAW_ACTION_UNLOCK   1
#define JIGSAW_ACTION_ADD      2
#define JIGSAW_ACTION_REMOVE   3
#define JIGSAW_ACTION_COMPLETE 4

// === Active jigsaw actor registry ===
// Populated every frame by the patched updateJigsawPictureActor.
// This is how we find actors for remote events (actorArray_findActorFromActorId doesn't work).
#define MAX_PUZZLES 11
static Actor *active_jigsaw_actors[MAX_PUZZLES];

// === Jigsaw lock state ===

static struct {
    bool locked;
    u8   owner_player_id;
} jigsaw_lock_state[MAX_PUZZLES];

static bool net_jigsaw_is_locked(s32 puzzle_id) {
    if (puzzle_id < 1 || puzzle_id > MAX_PUZZLES) return FALSE;
    return jigsaw_lock_state[puzzle_id - 1].locked;
}

static bool net_jigsaw_is_local_owner(s32 puzzle_id) {
    if (puzzle_id < 1 || puzzle_id > MAX_PUZZLES) return FALSE;
    if (!jigsaw_lock_state[puzzle_id - 1].locked) return FALSE;
    return jigsaw_lock_state[puzzle_id - 1].owner_player_id ==
           (u8)recomp_net_get_local_player_id();
}

static void net_jigsaw_lock(s32 puzzle_id, u8 player_id) {
    if (puzzle_id < 1 || puzzle_id > MAX_PUZZLES) return;
    jigsaw_lock_state[puzzle_id - 1].locked = TRUE;
    jigsaw_lock_state[puzzle_id - 1].owner_player_id = player_id;
}

static void net_jigsaw_unlock(s32 puzzle_id) {
    if (puzzle_id < 1 || puzzle_id > MAX_PUZZLES) return;
    jigsaw_lock_state[puzzle_id - 1].locked = FALSE;
    jigsaw_lock_state[puzzle_id - 1].owner_player_id = 0;
}

static void net_jigsaw_send_action(s32 puzzle_id, u32 action, u32 value, u32 map_id) {
    if (!recomp_net_is_connected()) return;
    u32 flag_index = ((u32)puzzle_id << 8) | action;
    recomp_printf("[JIGSAW-SEND] action=%d puzzle=%d value=%d map=%d flag_index=0x%X\n",
        action, puzzle_id, value, map_id, flag_index);
    recomp_net_send_flag_change(NET_FLAG_JIGSAW_ACTION, flag_index, value, map_id);
}

// === Replicated overlay functions (needed because overlay funcs can't be called by name) ===

static void onJigsawPodiumCollide(ActorMarker *marker, ActorMarker *_) {
    marker->isBanjoOnTop = TRUE;
}

static s32 jiggyPositionToID(Actor *this, s32 position) {
    s32 start;
    switch (this->actorTypeSpecificField) {
        case 7: start = (position == 2) ? 0x1a4 : 0x190; break;
        case 3: start = 0x192; break;
        case 8: start = 0x19A; break;
        case 11: start = 0x1AE; break;
        default: start = 0x190; break;
    }
    return start + position;
}

static void addOrRemovePieceFromDisplay(Actor *this, s32 position, bool isAdd) {
    s32 piece = (s32)func_8034C528(jiggyPositionToID(this, position));
    if (piece != 0) {
        updateStruct6DsOpacity(piece, isAdd ? 0 : 0xFF, isAdd ? 0xFF : 0, 1.0f);
    }
}

static s32 local_isPicturePiecePlaced(Actor *this, s32 position) {
    JigsawPictureActorData *local = (JigsawPictureActorData*)&this->local;
    return local->placedPieces & (1 << position);
}

static void setInitialJigsawPictureOpacity(Actor *this) {
    s32 piece;
    s32 i;
    for (i = 0; i < getPictureCost(this); i++) {
        piece = (s32)func_8034C528(jiggyPositionToID(this, i));
        if (piece != 0) {
            setStruct6DsOpacity(piece, (local_isPicturePiecePlaced(this, i)) ? 0xff : 0);
        }
    }
}

static s32 getUnknownJigsawPictureIndex(Actor *this) {
    switch (this->actorTypeSpecificField) {
        case 3: case 8: case 0xB: return 0x1F;
    }
    return 0x1E;
}

static s32 getLevelSpecificOpenFlag(Actor *this) {
    return this->actorTypeSpecificField + 0x1B;
}

static void activateDoubleHealth(void) {
    func_802FAFD4(ITEM_14_HEALTH, SFX_417_DOUBLE_HEALTH_UPGRADE);
    func_802FAFC0(ITEM_14_HEALTH, COMUSIC_2B_DING_B);
    fileProgressFlag_set(FILEPROG_B9_DOUBLE_HEALTH, TRUE);
    func_80347958();
    item_adjustByDiffWithHud(ITEM_14_HEALTH, 0);
    gcpausemenu_80314AC8(1);
}

static void gruntyLaughCallback(ActorMarker *marker, enum asset_e text_id, s32 arg2) {
    func_8030E6D4(SFX_EA_GRUNTY_LAUGH_1);
}

// Forward declare
static void jigsawPicture_setState_net(Actor *this, s32 nextState);

static void afterPictureComplete(ActorMarker *marker) {
    Actor *this = marker_getActor(reinterpret_cast(ActorMarker *, marker));
    if (this->actorTypeSpecificField < 0xA) {
        levelSpecificFlags_set(getLevelSpecificOpenFlag(this), TRUE);
        return;
    }
    if (this->actorTypeSpecificField == 0xA) {
        func_8028F918(0);
        func_8028F918(2);
        levelSpecificFlags_set(LEVEL_FLAG_3F_LAIR_GRUNTY_DOOR_OPEN, TRUE);
        return;
    }
    if (this->actorTypeSpecificField == 0xB) {
        timedFunc_set_0(1.5f, activateDoubleHealth);
        gcpausemenu_80314AC8(0);
    }
}

static void unlockAdditionalActions(Actor *this) {
    JigsawPictureActorData *local = (JigsawPictureActorData*)&this->local;
    if ((this->actorTypeSpecificField >= 2)
        && (local->placedJiggyCount > 0)
        && !isPictureComplete(this)
        && !fileProgressFlag_get(FILEPROG_DF_CAN_REMOVE_ALL_PUZZLE_PIECES)) {
        if (gcdialog_showDialog(ASSET_F7C_DIALOG_BOTTLES_REMOVE_PIECE_INSTRUCTIONS, 2, NULL, NULL, NULL, NULL)) {
            fileProgressFlag_set(FILEPROG_DF_CAN_REMOVE_ALL_PUZZLE_PIECES, TRUE);
        }
    } else if ((this->actorTypeSpecificField >= 3)
        && (local->placedJiggyCount >= 2)
        && !isPictureComplete(this)
        && !fileProgressFlag_get(FILEPROG_E0_CAN_PLACE_ALL_PUZZLE_PIECES)) {
        if (gcdialog_showDialog(ASSET_F7D_DIALOG_BOTTLES_EXPLAINS_PLACE_ALL, 2, NULL, NULL, NULL, NULL)) {
            fileProgressFlag_set(FILEPROG_E0_CAN_PLACE_ALL_PUZZLE_PIECES, TRUE);
        }
    }
}

static void stoodOnPodiumCallback(ActorMarker *marker) {
    f32 camera_position[3];
    Actor *this = marker_getActor(marker);
    func_8034A174(func_803097A0(), getUnknownJigsawPictureIndex(this), camera_position);
    func_8028E6EC(2);
    func_8028F918(0);
    func_8028F94C(4, camera_position);
    jigsawPicture_setState_net(this,
        fileProgressFlag_get(FILEPROG_17_HAS_HAD_ENOUGH_JIGSAW_PIECES) ? JIGSAW_PICTURE_WAITING : JIGSAW_PICTURE_INSTRUCTIONS);
}

static void walkToPodium(Actor *this) {
    f32 player_position[3];
    f32 target_position[3];
    this->has_met_before = FALSE;
    player_getPosition(player_position);
    target_position[0] = this->position[0];
    target_position[1] = this->position[1] + 50.0f;
    target_position[2] = this->position[2];
    player_walkToPosition(target_position, ml_vec3f_distance(player_position, target_position) / 150.0, stoodOnPodiumCallback, this->marker);
}

static void bottlesInstructionsCallback(ActorMarker *marker, enum asset_e text_id, s32 arg2) {
    Actor *this = marker_getActor(marker);
    jigsawPicture_setState_net(this, (text_id == ASSET_F58_DIALOG_FIRST_PICTURE_INSTRUCTION) ? JIGSAW_PICTURE_LEAVE_PODIUM : JIGSAW_PICTURE_WAITING);
}

// Fixed replica of func_8038EDBC — original decomp has sp28[3] + sp34 as separate vars,
// but on the original MIPS stack they are contiguous, so func_8034DF30 reads sp28 as a
// 4-element array where [3] = opacity. The MIPS cross-compiler doesn't guarantee this
// layout, so we use an explicit f32[4] array with sp28[3] as the alpha channel.
static void jigsaw_opacity_update(Actor *this) {
    s32 sp44;
    s32 sp40;
    JigsawPictureActorData *local;
    s32 sp38;
    f32 sp28[4];

    local = (JigsawPictureActorData*)&this->local;
    sp38 = (this->modelCacheIndex == 0x3B7) ? 0x190 : 0x192;
    sp44 = (s32)func_8034C2C4(this->marker, sp38);
    sp40 = (s32)func_8034C2C4(this->marker, sp38 + 1);

    if ((sp44 != 0) && (sp40 != 0) && (this->marker->unk14_21)) {
        sp28[0] = 1.0f;
        sp28[1] = 1.0f;
        sp28[2] = 1.0f;

        if (isBanjoOnPodium(this->marker) && local->unk8 < 0xFF) {
            local->unk8 = (local->unk8 + 8 < 0xFF) ? local->unk8 + 8 : 0xFF;
        } else if (!isBanjoOnPodium(this->marker) && (local->unk8 > 0)) {
            local->unk8 = (local->unk8 - 8 > 0) ? local->unk8 - 8 : 0;
        }

        sp28[3] = (0xFF - local->unk8) / 255.0f;
        func_8034DF30(sp44, sp28, sp28, 0);
        sp28[3] = 1.0f - sp28[3];
        func_8034DF30(sp40, sp28, sp28, 0);
    }
}

// === Wrapper for jigsawPicture_setState that adds network hooks ===
// This calls the ORIGINAL setState function, then sends network events.
// We track state transitions to know when to send lock/unlock/add/remove/complete.

// jigsawPicture_setState with network hooks — replicated from original + net sends
static void jigsawPicture_setState_net(Actor *this, s32 nextState) {
    JigsawPictureActorData *local = (JigsawPictureActorData*)&this->local;
    f32 position[3];
    s32 jiggy_add_count;
    s32 piece_position;
    s32 i;
    u32 cur_map = (u32)map_get();

    func_8034A174(func_803097A0(), getUnknownJigsawPictureIndex(this), position);

    switch (nextState) {
        case JIGSAW_PICTURE_LEAVE_PODIUM:
            func_8028F918(0);
            // NET: Unlock
            if (recomp_net_is_connected() && net_jigsaw_is_local_owner(this->actorTypeSpecificField)) {
                net_jigsaw_unlock(this->actorTypeSpecificField);
                net_jigsaw_send_action(this->actorTypeSpecificField, JIGSAW_ACTION_UNLOCK, 0, cur_map);
            }
            break;

        case JIGSAW_PICTURE_ENTER_PODIUM:
            walkToPodium(this);
            sfx_playFadeShorthandDefault(SFX_112_TINKER_ATTENTION, 1.0f, 32000, this->position, 500, 1000);
            // NET: Lock
            if (recomp_net_is_connected()) {
                u32 my_id = recomp_net_get_local_player_id();
                net_jigsaw_lock(this->actorTypeSpecificField, (u8)my_id);
                net_jigsaw_send_action(this->actorTypeSpecificField, JIGSAW_ACTION_LOCK, my_id, cur_map);
            }
            break;

        case JIGSAW_PICTURE_INSTRUCTIONS: {
            extern void func_803115C4(s32);
            func_803115C4(0xF7B);
            func_803115C4(0xF80);
            func_803115C4(0xF7F);
            if (item_getCount(ITEM_26_JIGGY_TOTAL) > 0) {
                gcdialog_showDialog(
                    fileProgressFlag_get(FILEPROG_16_STOOD_ON_JIGSAW_PODIUM)
                        ? ASSET_F5A_DIALOG_FIRST_PICTURE_FIRST_PIECE_OBTAINED_AFTER
                        : ASSET_F59_DIALOG_FIRST_PICTURE_FIRST_PIECE_ALREADY_OBTAINED,
                    6, position, this->marker, bottlesInstructionsCallback, NULL);
                fileProgressFlag_set(FILEPROG_17_HAS_HAD_ENOUGH_JIGSAW_PIECES, 1);
            } else {
                gcdialog_showDialog(ASSET_F58_DIALOG_FIRST_PICTURE_INSTRUCTION, 6, position, this->marker, bottlesInstructionsCallback, NULL);
            }
            fileProgressFlag_set(FILEPROG_16_STOOD_ON_JIGSAW_PODIUM, 1);
            fileProgressFlag_set(FILEPROG_A7_NEAR_PUZZLE_PODIUM_TEXT, 1);
            break;
        }

        case JIGSAW_PICTURE_REMOVE_PIECE:
            if (local->placedJiggyCount > 0) {
                comusic_playTrack(SFX_REMOVE_JIGGY);
                this->lifetime_value = 1.0f;
                piece_position = getPicturePiecePosition_local(this);
                addOrRemovePieceFromDisplay(this, piece_position, 0);
                local->placedJiggyCount--;
                local->placedPieces &= ~(1 << piece_position);
                fileProgressFlag_setN(PICTURE_INFO[this->actorTypeSpecificField - 1].progressFlag, local->placedJiggyCount, PICTURE_INFO[this->actorTypeSpecificField - 1].sizeBits);
                item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, 1);
                // NET: Send piece removed
                if (recomp_net_is_connected()) {
                    net_jigsaw_send_action(this->actorTypeSpecificField, JIGSAW_ACTION_REMOVE,
                        (u32)piece_position, cur_map);
                }
            }
            break;

        case JIGSAW_PICTURE_ADD_PIECE:
            if (local->placedJiggyCount < getPictureCost(this)) {
                comusic_playTrack(COMUSIC_67_INSERTING_JIGGY);
                this->lifetime_value = 1.0f;
                local->placedJiggyCount++;
                piece_position = getPicturePiecePosition_local(this);
                addOrRemovePieceFromDisplay(this, piece_position, 1);
                local->placedPieces |= (1 << piece_position);
                fileProgressFlag_setN(PICTURE_INFO[this->actorTypeSpecificField - 1].progressFlag, local->placedJiggyCount, PICTURE_INFO[this->actorTypeSpecificField - 1].sizeBits);
                item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, -1);
                recomp_printf("[JIGGY-DEBUG] Placed piece on puzzle %d, JIGGY_TOTAL now=%d\n",
                    this->actorTypeSpecificField, item_getCount(ITEM_26_JIGGY_TOTAL));
                unlockAdditionalActions(this);
                // NET: Send piece added
                if (recomp_net_is_connected()) {
                    net_jigsaw_send_action(this->actorTypeSpecificField, JIGSAW_ACTION_ADD,
                        (u32)piece_position, cur_map);
                }
            }
            break;

        case JIGSAW_PICTURE_ADD_ALL:
            if (local->placedJiggyCount < getPictureCost(this)) {
                if (item_getCount(ITEM_26_JIGGY_TOTAL) > getPictureCost(this) - local->placedJiggyCount) {
                    jiggy_add_count = getPictureCost(this) - local->placedJiggyCount;
                } else {
                    jiggy_add_count = item_getCount(ITEM_26_JIGGY_TOTAL);
                }
                comusic_playTrack(COMUSIC_67_INSERTING_JIGGY);
                this->lifetime_value = 1.0f;

                for (i = 0; i < jiggy_add_count; i++) {
                    local->placedJiggyCount++;
                    piece_position = getPicturePiecePosition_local(this);
                    addOrRemovePieceFromDisplay(this, piece_position, 1);
                    local->placedPieces |= (1 << piece_position);
                    item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, -1);
                    // NET: Send each piece
                    if (recomp_net_is_connected()) {
                        net_jigsaw_send_action(this->actorTypeSpecificField, JIGSAW_ACTION_ADD,
                            (u32)piece_position, cur_map);
                    }
                }

                fileProgressFlag_setN(PICTURE_INFO[this->actorTypeSpecificField - 1].progressFlag, local->placedJiggyCount, PICTURE_INFO[this->actorTypeSpecificField - 1].sizeBits);
                unlockAdditionalActions(this);
            }
            break;

        case JIGSAW_PICTURE_COMPLETE:
            comusic_playTrack(COMUSIC_65_WORLD_OPENING_B);
            if (this->actorTypeSpecificField == 1) {
                func_80324DBC(1.0f, 0xF7E, 4, NULL, this->marker, gruntyLaughCallback, NULL);
            } else if (this->actorTypeSpecificField == 0xA) {
                func_80324DBC(1.0f, 0xFAC, 4, NULL, this->marker, gruntyLaughCallback, NULL);
            }
            timedFunc_set_1(2.0f, (GenFunction_1) afterPictureComplete, (s32) this->marker);
            this->lifetime_value = 3.0f;
            // NET: Send completion
            if (recomp_net_is_connected()) {
                net_jigsaw_send_action(this->actorTypeSpecificField, JIGSAW_ACTION_COMPLETE, 0, cur_map);
            }
            break;
    }

    subaddie_set_state(this, nextState);
}

// === PATCHED: Main actor update function ===
// Minimal changes: replace jigsawPicture_setState calls with our net wrapper,
// and add the lock check. Everything else delegates to ORIGINAL functions.

// Simple helpers that are safe to replicate (pure logic, no visuals)
static s32 getPictureCost(Actor *this) {
    return (this->actorTypeSpecificField != 0 && this->actorTypeSpecificField < 0xC)
        ? PICTURE_INFO[this->actorTypeSpecificField - 1].cost : 0;
}

static bool isPictureComplete(Actor *this) {
    JigsawPictureActorData *local = (JigsawPictureActorData*)&this->local;
    return getPictureCost(this) == local->placedJiggyCount;
}

static bool isBanjoOnPodium(ActorMarker *marker) {
    return func_8028F20C() && func_8028FB48(0x08000000) && marker->isBanjoOnTop;
}

static s32 getPicturePiecePosition_local(Actor *this) {
    JigsawPictureActorData *local;
    s32 previous;
    s32 position;
    s32 i;
    previous = 0;
    local = (JigsawPictureActorData*)&this->local;
    rand_seed(this->actorTypeSpecificField);
    if (this->actorTypeSpecificField >= 0xA) {
        for (i = 0; i < local->placedJiggyCount; i++) {
            position = i;
            previous |= (1 << position);
        }
    } else {
        for (i = 0; i < local->placedJiggyCount; i++) {
            do {
                position = randi2(0, getPictureCost(this));
            } while ((1 << position) & previous);
            previous |= 1 << position;
        }
    }
    return position;
}

RECOMP_PATCH void updateJigsawPictureActor(Actor *this) {
    JigsawPictureActorData *local;
    s32 face_buttons[6];
    s32 i;
    s32 text_id;
    s32 side_buttons[3];
    f32 delta_time;
    s32 jiggiesPlaced;

    local = (JigsawPictureActorData*)&this->local;
    delta_time = time_getDelta();

    // Register this actor so remote events can find it
    if (this->actorTypeSpecificField >= 1 && this->actorTypeSpecificField <= MAX_PUZZLES) {
        active_jigsaw_actors[this->actorTypeSpecificField - 1] = this;
    }

    if (!this->initialized) {
        this->initialized = TRUE;
    }

    if (!this->volatile_initialized) {
        jiggiesPlaced = fileProgressFlag_getN(PICTURE_INFO[this->actorTypeSpecificField - 1].progressFlag, PICTURE_INFO[this->actorTypeSpecificField - 1].sizeBits);
        local->placedPieces = 0;
        local->placedJiggyCount = 0;
        local->unk8 = (isBanjoOnPodium(this->marker)) ? 0xFF : 1;
        this->has_met_before = TRUE;

        for (i = 0; i < jiggiesPlaced; i++) {
            local->placedJiggyCount++;
            local->placedPieces |= (1 << getPicturePiecePosition_local(this));
        }

        setInitialJigsawPictureOpacity(this);
        marker_setCollisionScripts(this->marker, onJigsawPodiumCollide, NULL, NULL);
        this->marker->propPtr->unk8_3 = TRUE;
        this->volatile_initialized = TRUE;

        // CCW specific
        if (this->actorTypeSpecificField == 9) {
            this->unk1C[0] = 8.0f;
            if (!fileProgressFlag_get(FILEPROG_53_CCW_PUZZLE_PODIUM_SWITCH_PRESSED)) {
                marker_despawn(this->marker);
                return;
            }
            if (!fileProgressFlag_get(FILEPROG_54_CCW_PUZZLE_PODIUM_ACTIVE)) {
                __bundle_spawnFromFirstActor(BUNDLE_20__UNKNOWN, this);
                func_80324CFC(0.0f, COMUSIC_43_ENTER_LEVEL_GLITTER, 0x7FFF);
                func_80324D2C(2.1f, COMUSIC_43_ENTER_LEVEL_GLITTER);
                func_8030E6D4(SFX_113_PAD_APPEARS);
            }
        }
    }

    // CCW podium activation animation
    if ((this->actorTypeSpecificField == 9) && !fileProgressFlag_get(FILEPROG_54_CCW_PUZZLE_PODIUM_ACTIVE)) {
        this->yaw += this->unk1C[0];
        while (this->yaw >= 360.0f) {
            this->yaw -= 360.0f;
        }
        this->unk1C[0] -= 0.089888;
        if (this->unk1C[0] < 0.0f) {
            this->unk1C[0] = 0.0f;
        }
        if (this->marker->unk14_21) {
            s32 sp58[3] = { 0xff, 0xff, 0 };
            ParticleEmitter *sp54;
            sp54 = partEmitMgr_newEmitter(6);
            particleEmitter_setSprite(sp54, ASSET_710_SPRITE_SPARKLE_PURPLE);
            particleEmitter_setAlpha(sp54, 0xFF);
            particleEmitter_setScaleAndLifetimeRanges(sp54, &D_80394830_copy);
            particleEmitter_setPosition(sp54, this->position);
            sp58[2] = randf() * 255.0f;
            particleEmitter_setRGB(sp54, sp58);
            particleEmitter_setSpawnPositionRange(sp54, -30.0f, -40.0f, -30.0f, 30.0f, 20.0f, 30.0f);
            particleEmitter_emitN(sp54, 6);
        }
    }

    controller_copyFaceButtons(0, face_buttons);
    controller_copySideButtons(0, side_buttons);

    jigsaw_opacity_update(this);

    switch (this->state) {
        case JIGSAW_PICTURE_LEAVE_PODIUM:
            if (!this->has_met_before && (!func_8028F20C() || !func_8028FB48(0x08000000))) {
                this->has_met_before = TRUE;
            }

            if (subaddie_playerIsWithinSphereAndActive(this, 300)) {
                if ((this->actorTypeSpecificField == 0xA) && !fileProgressFlag_get(FILEPROG_F6_SEEN_DOOR_OF_GRUNTY_PUZZLE_PODIUM)) {
                    text_id = (item_getCount(ITEM_26_JIGGY_TOTAL) < PICTURE_INFO[this->actorTypeSpecificField - 1].cost)
                        ? ASSET_FAB_DIALOG_GRUNTY_DOOR_NEED_JIGGIES : ASSET_FC0_DIALOG_GRUNTY_DOOR_HAVE_JIGGIES;
                    if (gcdialog_showDialog(text_id, 0, NULL, NULL, NULL, NULL)) {
                        fileProgressFlag_set(FILEPROG_F6_SEEN_DOOR_OF_GRUNTY_PUZZLE_PODIUM, TRUE);
                    }
                } else if (this->actorTypeSpecificField == 1) {
                    progressDialog_showDialogMaskZero(FILEPROG_A7_NEAR_PUZZLE_PODIUM_TEXT);
                }
            }

            // === NET: Lock check before allowing entry ===
            if (isBanjoOnPodium(this->marker) && this->has_met_before && !isPictureComplete(this) && (player_movementGroup() == BSGROUP_0_NONE || player_movementGroup() == BSGROUP_8_TROT)) {
                // Block if another player has the pedestal locked
                if (recomp_net_is_connected() && net_jigsaw_is_locked(this->actorTypeSpecificField) && !net_jigsaw_is_local_owner(this->actorTypeSpecificField)) {
                    break; // Silently block
                }
                jigsawPicture_setState_net(this, JIGSAW_PICTURE_ENTER_PODIUM);
            }
            break;

        case JIGSAW_PICTURE_WAITING:
            if ((gcdialog_getCurrentTextId() != ASSET_F7C_DIALOG_BOTTLES_REMOVE_PIECE_INSTRUCTIONS)
                && (gcdialog_getCurrentTextId() != ASSET_F7D_DIALOG_BOTTLES_EXPLAINS_PLACE_ALL)) {

                if (face_buttons[FACE_BUTTON(BUTTON_A)] == TRUE) {
                    // Inline addPieces to use our net wrapper
                    if (item_getCount(ITEM_26_JIGGY_TOTAL) > 0) {
                        jigsawPicture_setState_net(this, JIGSAW_PICTURE_ADD_PIECE);
                    } else {
                        comusic_playTrack(COMUSIC_2C_BUZZER);
                        if (fileProgressFlag_get(FILEPROG_DE_USED_ALL_YOUR_PUZZLE_PIECES) != 0) {
                            jigsawPicture_setState_net(this, JIGSAW_PICTURE_LEAVE_PODIUM);
                        } else {
                            gcdialog_showDialog(ASSET_FBC_DIALOG_BOTTLES_OUT_OF_JIGGIES, 4, NULL, NULL, NULL, NULL);
                            fileProgressFlag_set(FILEPROG_DE_USED_ALL_YOUR_PUZZLE_PIECES, TRUE);
                        }
                    }
                } else if (face_buttons[FACE_BUTTON(BUTTON_B)] == TRUE) {
                    jigsawPicture_setState_net(this, JIGSAW_PICTURE_LEAVE_PODIUM);
                } else if ((side_buttons[SIDE_BUTTON(BUTTON_Z)] == TRUE) && fileProgressFlag_get(FILEPROG_E0_CAN_PLACE_ALL_PUZZLE_PIECES)) {
                    if (item_getCount(ITEM_26_JIGGY_TOTAL) > 0) {
                        jigsawPicture_setState_net(this, JIGSAW_PICTURE_ADD_ALL);
                    } else {
                        comusic_playTrack(COMUSIC_2C_BUZZER);
                        if (fileProgressFlag_get(FILEPROG_DE_USED_ALL_YOUR_PUZZLE_PIECES) != 0) {
                            jigsawPicture_setState_net(this, JIGSAW_PICTURE_LEAVE_PODIUM);
                        } else {
                            gcdialog_showDialog(ASSET_FBC_DIALOG_BOTTLES_OUT_OF_JIGGIES, 4, NULL, NULL, NULL, NULL);
                            fileProgressFlag_set(FILEPROG_DE_USED_ALL_YOUR_PUZZLE_PIECES, TRUE);
                        }
                    }
                } else if (face_buttons[FACE_BUTTON(BUTTON_C_DOWN)] == TRUE) {
                    if (local->placedJiggyCount) {
                        jigsawPicture_setState_net(this, JIGSAW_PICTURE_REMOVE_PIECE);
                    } else {
                        comusic_playTrack(COMUSIC_2C_BUZZER);
                        jigsawPicture_setState_net(this, JIGSAW_PICTURE_LEAVE_PODIUM);
                    }
                }
            }
            break;

        case JIGSAW_PICTURE_ADD_PIECE:
        case JIGSAW_PICTURE_ADD_ALL:
        case JIGSAW_PICTURE_REMOVE_PIECE:
            if (this->lifetime_value > 0.0f) {
                this->lifetime_value -= delta_time;
            } else {
                jigsawPicture_setState_net(this, isPictureComplete(this) ? JIGSAW_PICTURE_COMPLETE : JIGSAW_PICTURE_WAITING);
            }
            break;

        case JIGSAW_PICTURE_COMPLETE:
            if (this->lifetime_value > 0.0f) {
                this->lifetime_value -= delta_time;
            } else {
                jigsawPicture_setState_net(this, JIGSAW_PICTURE_LEAVE_PODIUM);
            }
            break;
    }

    {
        s32 pad;
        f32 sp44[3];
        s32 pad2;
        this->marker->isBanjoOnTop = FALSE;
        player_getPosition(sp44);

        if (ml_distanceSquared_vec3f(sp44, this->position) < 250000.0f) {
            if (!this->unk38_0) {
                itemPrint_reset();
                this->unk38_0 = TRUE;
            }
            func_802FACA4(0x2B);
        } else if (this->unk38_0) {
            func_802FAD64(0x2B);
            this->unk38_0 = FALSE;
        }
    }
}

// === Remote jigsaw event processing ===

static Actor *find_jigsaw_actor(s32 puzzle_id) {
    if (puzzle_id < 1 || puzzle_id > MAX_PUZZLES) return (Actor*)0;
    Actor *actor = active_jigsaw_actors[puzzle_id - 1];
    if (actor) {
        recomp_printf("[JIGSAW-FIND] found puzzle %d actor via registry\n", puzzle_id);
    } else {
        recomp_printf("[JIGSAW-FIND] puzzle %d not in registry\n", puzzle_id);
    }
    return actor;
}

RECOMP_EXPORT void bkrecomp_net_process_jigsaw_event(u32 flag_index, u32 value, u32 map_id) {
    s32 puzzle_id = (s32)(flag_index >> 8);
    u32 action = flag_index & 0xFF;
    u32 cur_map = (u32)map_get();

    recomp_printf("[JIGSAW-SYNC] received action=%d puzzle=%d value=%d map=%d (cur=%d)\n",
        action, puzzle_id, value, map_id, cur_map);

    if (action == JIGSAW_ACTION_LOCK) {
        net_jigsaw_lock(puzzle_id, (u8)value);
        return;
    }

    if (action == JIGSAW_ACTION_UNLOCK) {
        net_jigsaw_unlock(puzzle_id);
        return;
    }

    // For visual actions, only process if on the same map
    if (cur_map != map_id) return;

    Actor *actor = find_jigsaw_actor(puzzle_id);

    // Enable remote flag guard to prevent echo
    bkrecomp_net_set_remote_flag_guard(TRUE);

    if (action == JIGSAW_ACTION_ADD) {
        if (actor) {
            JigsawPictureActorData *local = (JigsawPictureActorData*)&actor->local;
            s32 piece_pos = (s32)value;
            local->placedJiggyCount++;
            local->placedPieces |= (1 << piece_pos);
            // Call ORIGINAL visual update function
            addOrRemovePieceFromDisplay(actor, piece_pos, TRUE);
            comusic_playTrack(COMUSIC_67_INSERTING_JIGGY);
            // Sync progress flag
            if (puzzle_id >= 1 && puzzle_id <= 0xB) {
                fileProgressFlag_setN(PICTURE_INFO[puzzle_id - 1].progressFlag,
                    local->placedJiggyCount, PICTURE_INFO[puzzle_id - 1].sizeBits);
            }
            // Decrease jiggy count for this player too (shared inventory)
            item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, -1);
            recomp_printf("[JIGSAW-SYNC] applied ADD piece %d (count=%d)\n", piece_pos, local->placedJiggyCount);
        }
    }
    else if (action == JIGSAW_ACTION_REMOVE) {
        if (actor) {
            JigsawPictureActorData *local = (JigsawPictureActorData*)&actor->local;
            s32 piece_pos = (s32)value;
            addOrRemovePieceFromDisplay(actor, piece_pos, FALSE);
            local->placedJiggyCount--;
            local->placedPieces &= ~(1 << piece_pos);
            comusic_playTrack(SFX_REMOVE_JIGGY);
            if (puzzle_id >= 1 && puzzle_id <= 0xB) {
                fileProgressFlag_setN(PICTURE_INFO[puzzle_id - 1].progressFlag,
                    local->placedJiggyCount, PICTURE_INFO[puzzle_id - 1].sizeBits);
            }
            // Return jiggy to this player too
            item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, 1);
        }
    }
    else if (action == JIGSAW_ACTION_COMPLETE) {
        comusic_playTrack(COMUSIC_65_WORLD_OPENING_B);
        if (actor) {
            if (puzzle_id == 1) {
                func_80324DBC(1.0f, 0xF7E, 4, NULL, actor->marker, gruntyLaughCallback, NULL);
            } else if (puzzle_id == 0xA) {
                func_80324DBC(1.0f, 0xFAC, 4, NULL, actor->marker, gruntyLaughCallback, NULL);
            }
            timedFunc_set_1(2.0f, (GenFunction_1) afterPictureComplete, (s32) actor->marker);
        }
    }

    bkrecomp_net_set_remote_flag_guard(FALSE);
}

// Auto-unlock all puzzles owned by a specific player
RECOMP_EXPORT void bkrecomp_net_jigsaw_unlock_player(u8 player_id) {
    s32 i;
    for (i = 0; i < MAX_PUZZLES; i++) {
        if (jigsaw_lock_state[i].locked && jigsaw_lock_state[i].owner_player_id == player_id) {
            jigsaw_lock_state[i].locked = FALSE;
            jigsaw_lock_state[i].owner_player_id = 0;
        }
    }
}

// Auto-unlock detection on disconnect
static s32 prev_jigsaw_remote_count = -1;

RECOMP_EXPORT void bkrecomp_net_jigsaw_check_disconnects(void) {
    if (!recomp_net_is_connected()) {
        if (prev_jigsaw_remote_count > 0) {
            s32 i;
            for (i = 0; i < MAX_PUZZLES; i++) {
                jigsaw_lock_state[i].locked = FALSE;
                jigsaw_lock_state[i].owner_player_id = 0;
            }
        }
        prev_jigsaw_remote_count = -1;
        return;
    }

    s32 cur_count = (s32)recomp_net_get_remote_count();
    if (prev_jigsaw_remote_count >= 0 && cur_count < prev_jigsaw_remote_count) {
        u8 local_id = (u8)recomp_net_get_local_player_id();
        s32 i;
        for (i = 0; i < MAX_PUZZLES; i++) {
            if (jigsaw_lock_state[i].locked && jigsaw_lock_state[i].owner_player_id != local_id) {
                jigsaw_lock_state[i].locked = FALSE;
                jigsaw_lock_state[i].owner_player_id = 0;
            }
        }
    }
    prev_jigsaw_remote_count = cur_count;
}
