#include "patches.h"
#include "functions.h"
#include "enums.h"

// Network bridge
extern u32 recomp_net_is_connected(void);
extern bool bkrecomp_net_bottles_is_locked(void);
extern bool bkrecomp_net_bottles_is_local_owner(void);
extern void bkrecomp_net_bottles_send_lock(void);
extern void bkrecomp_net_bottles_send_unlock(void);
extern u8 bkrecomp_net_bottles_get_lock_owner(void);

// Decomp externs
extern enum map_e map_get(void);
extern s32 player_movementGroup(void);
extern void player_getPosition(f32 pos[3]);
extern s32 func_8028F20C(void);   // player_isStable
extern s32 func_8028F0D4(void);   // player state check
extern s32 func_8028EC04(void);   // is in special action
extern s32 func_8028EFC8(void);   // player facing check
extern s32 ability_isUnlocked(s32 ability);
extern s32 subaddie_playerIsWithinSphereAndActive(Actor *actor, s32 radius);
extern f32 nodeprop_getRadius(NodeProp *node_prop);

// From mole.c — internal functions
extern void chmole_startingDialog(Actor *this);
extern void chmole_spawnMolehill(s32 marker_as_int);
extern void func_802D9600(Actor *this);  // hide mole (state→1, invisible)
extern void func_802D9C90(Actor *this);  // free method
extern void chmole_setFacingDirection(Actor *this);
extern void chmole_learnAbility(Actor *this);

// Yaw toward arbitrary position (from code_9E370.c)
extern s32 func_803297C8(Actor *actor, f32 target_pos[3]);

// Network: get remote player position
extern u32 recomp_net_get_local_player_id(void);
typedef struct {
    f32 x, y, z;
    f32 yaw;
    u8 _rest[0x30];
} NetFullState_Pos;
extern u32 recomp_net_get_remote_state(u32 player_id, NetFullState_Pos *out);

// Mole table (from mole.c)
typedef struct {
    s32 teach_text;
    s32 refresher_text;
    s32 camera_node;
    s32 ability;
} ChMoleDescription;
extern ChMoleDescription moleTable[];

// === State tracking ===

// Actor registry — updated every frame from chmole_update.
// actorArray_findActorFromActorId may return NULL for some actors,
// so we keep our own pointer (same pattern as jigsaw actor registry).
static Actor *active_bottles_actor = (Actor*)0;

// TRUE when LOCAL player initiated a conversation with Bottles.
static bool local_talking = FALSE;

// TRUE when a REMOTE player is talking to Bottles.
static bool remote_animating = FALSE;

// Track previous state to detect transitions to state 1 (idle).
static s32 prev_state = 1;

// === Remote animation triggers (called from network_flag_sync.c) ===

RECOMP_EXPORT void bkrecomp_net_bottles_remote_emerge(void) {
    Actor *bottles = active_bottles_actor;
    if (!bottles || !bottles->marker) return;

    // Only show emerge animation for first-time learn.
    // If ability already learned, Bottles uses refresher (state 5, no emerge).
    if (bottles->actorTypeSpecificField >= 9 && bottles->actorTypeSpecificField < 0x13) {
        if (ability_isUnlocked(moleTable[bottles->actorTypeSpecificField - 9].ability)) {
            return;
        }
    }

    remote_animating = TRUE;

    // Animate molehill partner — state 2 = opening
    Actor *molehill = subaddie_getLinkedActor(bottles);
    if (bottles->partnerActor && molehill && bottles->partnerActor->id == 0xB8) {
        subaddie_set_state_with_direction(molehill, 2, 0.0001f, 1);
    }

    // Make Bottles visible and play emerge animation
    bottles->marker->propPtr->unk8_3 = 1;
    anctrl_setSmoothTransition(bottles->anctrl, TRUE);
    subaddie_set_state_with_direction(bottles, 2, 0.0001f, 1);
    actor_playAnimationOnce(bottles);
}

RECOMP_EXPORT void bkrecomp_net_bottles_remote_hide(void) {
    if (!remote_animating) return;  // Nothing to hide

    Actor *bottles = active_bottles_actor;
    if (!bottles || !bottles->marker) {
        remote_animating = FALSE;
        return;
    }

    // Animate molehill partner — state 3 = closing
    Actor *molehill = subaddie_getLinkedActor(bottles);
    if (bottles->partnerActor && molehill && bottles->partnerActor->id == 0xB8) {
        subaddie_set_state_with_direction(molehill, 3, 0.0001f, 1);
    }

    // Play exit animation — visual only
    subaddie_set_state_with_direction(bottles, 4, 0.0001f, 1);
    actor_playAnimationOnce(bottles);
}

// Helper: face Bottles toward the lock owner (ghost position)
static void face_toward_lock_owner(Actor *this) {
    u8 owner_id = bkrecomp_net_bottles_get_lock_owner();
    NetFullState_Pos rs;
    if (recomp_net_get_remote_state((u32)owner_id, &rs)) {
        f32 owner_pos[3];
        owner_pos[0] = rs.x;
        owner_pos[1] = rs.y;
        owner_pos[2] = rs.z;
        this->yaw_ideal = (f32)func_803297C8(this, owner_pos);
    }
    func_80328FB0(this, 4.0f);
}

// === PATCHED UPDATE ===

RECOMP_PATCH void chmole_update(Actor *this) {
    s32 sp50[6];
    f32 sp4C;
    f32 pad44[1];
    Actor *other;
    NodeProp *node_prop;
    f32 sp34[3];

    if (this->actorTypeSpecificField < 8 || this->actorTypeSpecificField >= 0x13)
        return;

    // Register this actor every frame so remote functions can find it
    active_bottles_actor = this;

    // --- Init (unchanged from original) ---
    if (!this->volatile_initialized) {
        this->volatile_initialized = TRUE;
        marker_setFreeMethod(this->marker, func_802D9C90);
        if (this->initialized) {
            other = actorArray_findClosestActorFromActorId(this->position, ACTOR_12C_MOLEHILL, -1, &sp4C);
            this->partnerActor = (other) ? other->marker : NULL;
            if (this->partnerActor) {
                other = subaddie_getLinkedActor(this);
                if (other && this->partnerActor->id == 0xB8) {
                    subaddie_set_state(other, 1);
                }
            }
        }
    }

    if (!this->initialized) {
        node_prop = nodeprop_findByActorIdAndActorPosition(0x372, this);
        if (node_prop == NULL) {
            this->unk38_0 = FALSE;
        } else {
            this->unk38_0 = TRUE;
            nodeprop_getPosition(node_prop, this->unk1C);
        }
        __spawnQueue_add_1((GenFunction_1)chmole_spawnMolehill, reinterpret_cast(s32, this->marker));
        this->marker->propPtr->unk8_3 = FALSE;
        this->marker->collidable = FALSE;
        this->initialized = TRUE;
        if (this->actorTypeSpecificField == 0x12) {
            node_prop = nodeprop_findByActorIdAndActorPosition(0x349, this);
            if (node_prop == NULL) {
                this->velocity[0] = this->position[0];
                this->velocity[1] = this->position[1];
                this->velocity[2] = this->position[2];
                this->actor_specific_1_f = 500.0f;
            } else {
                nodeprop_getPosition(node_prop, this->velocity);
                this->actor_specific_1_f = 2 * nodeprop_getRadius(node_prop);
            }
        }
    }

    // --- NET: Detect return to idle (state 1) from any conversation state ---
    // This fires once on the transition, not every frame.
    if (this->state == 1 && prev_state != 1) {
        if (local_talking) {
            bkrecomp_net_bottles_send_unlock();
            local_talking = FALSE;
        }
        if (remote_animating) {
            remote_animating = FALSE;
        }
    }
    prev_state = this->state;

    // --- Remote animation mode: only run visual states ---
    if (remote_animating) {
        switch (this->state) {
            case 2: // Emerge animation
                this->marker->propPtr->unk8_3 = TRUE;
                face_toward_lock_owner(this);
                if (actor_animationIsAt(this, 0.9999f)) {
                    // Emerge done → idle visible (face toward owner)
                    subaddie_set_state_with_direction(this, 3, 0.0001f, 1);
                    actor_loopAnimation(this);
                }
                break;
            case 3: // Idle visible (waiting for remote conversation to end)
                face_toward_lock_owner(this);
                break;
            case 4: // Exit animation
                if (actor_animationIsAt(this, 0.9999f)) {
                    func_802D9600(this); // hide, go to state 1
                    remote_animating = FALSE;
                }
                break;
        }
        return; // Skip ALL normal logic — no input, no dialog, nothing
    }

    // --- Normal (local) update ---
    controller_copyFaceButtons(0, sp50);

    switch (this->state) {
        case 1: // IDLE
            this->yaw_ideal = func_80329784(this);
            func_80328FB0(this, 4.0f);
            if (func_8028F20C() && func_8028F0D4() && !func_8028EC04()) {
                // NET: Block if another player is talking to Bottles
                if (recomp_net_is_connected() && bkrecomp_net_bottles_is_locked()
                    && !bkrecomp_net_bottles_is_local_owner()) {
                    break;
                }

                if (this->actorTypeSpecificField == 0x12
                    && !ability_isUnlocked(moleTable[this->actorTypeSpecificField - 9].ability)
                    && (player_movementGroup() == BSGROUP_0_NONE || player_movementGroup() == BSGROUP_8_TROT)
                ) {
                    player_getPosition(sp34);
                    if (ml_vec3f_distance(sp34, this->velocity) < this->actor_specific_1_f) {
                        if (recomp_net_is_connected()) {
                            bkrecomp_net_bottles_send_lock();
                            local_talking = TRUE;
                        }
                        chmole_startingDialog(this);
                    }
                } else {
                    if (!player_movementGroup()
                        && subaddie_playerIsWithinSphereAndActive(this, 0xFA)
                        && func_8028EFC8()
                        && sp50[FACE_BUTTON(BUTTON_B)] == 1
                    ) {
                        if (recomp_net_is_connected()) {
                            bkrecomp_net_bottles_send_lock();
                            local_talking = TRUE;
                        }
                        chmole_startingDialog(this);
                    }
                }
            }
            break;

        case 2: // ENTER (emerge from molehill)
            this->marker->propPtr->unk8_3 = TRUE;
            this->yaw_ideal = func_80329784(this);
            func_80328FB0(this, 4.0f);
            if (0.0 < anctrl_getAnimTimer(this->anctrl)
                && anctrl_getAnimTimer(this->anctrl) < 0.16) {
                sfxSource_func_8030E2C4(this->unk44_31);
            }
            if (actor_animationIsAt(this, 0.9999f)) {
                chmole_setFacingDirection(this);
                sfxsource_freeSfxsourceByIndex(this->unk44_31);
                this->unk44_31 = 0;
            } else if (actor_animationIsAt(this, 0.14f)) {
                sfx_playFadeShorthandDefault(SFX_C6_SHAKING_MOUTH, 1.2f, 24000, this->position, 1250, 2500);
            } else if (actor_animationIsAt(this, 0.4f)) {
                sfx_playFadeShorthandDefault(SFX_2C_PULLING_NOISE, 1.2f, 24000, this->position, 1250, 2500);
            } else if (actor_animationIsAt(this, 0.75f)) {
                sfx_playFadeShorthandDefault(SFX_C5_TWINKLY_POP, 1.0f, 32000, this->position, 1250, 2500);
            } else if (actor_animationIsAt(this, 0.35f)) {
                chmole_learnAbility(this);
            }
            break;

        case 3: // FACING (idle visible, waiting)
            this->yaw_ideal = func_80329784(this);
            func_80328FB0(this, 4.0f);
            if ((actor_animationIsAt(this, 0.37f)
                  || actor_animationIsAt(this, 0.66f)
                  || actor_animationIsAt(this, 0.85f))
                && randf() < 0.2) {
                anctrl_setDirection(this->anctrl, 1 ^ anctrl_isPlayedForwards(this->anctrl));
            } else if (actor_animationIsAt(this, 0.25f)
                || actor_animationIsAt(this, 0.28f)
                || actor_animationIsAt(this, 0.31f)) {
                func_8030E878(SFX_6F_BANJO_HEADSCRATCH, randf2(1.4f, 1.55f), 16000, this->position, 1250.0f, 2500.0f);
            } else if (actor_animationIsAt(this, 0.45f)
                || actor_animationIsAt(this, 0.48f)
                || actor_animationIsAt(this, 0.51f)
                || actor_animationIsAt(this, 0.7f)
                || actor_animationIsAt(this, 0.73f)
                || actor_animationIsAt(this, 0.76f)) {
                func_8030E878(SFX_6F_BANJO_HEADSCRATCH, randf2(1.35f, 1.5f), 6000, this->position, 1250.0f, 2500.0f);
            }
            break;

        case 4: // EXIT (go back into molehill)
            if (0.35 < anctrl_getAnimTimer(this->anctrl)
                && anctrl_getAnimTimer(this->anctrl) < 0.9) {
                sfxSource_func_8030E2C4(this->unk44_31);
            } else if (actor_animationIsAt(this, 0.9999f)) {
                func_802D9600(this);
                sfxsource_freeSfxsourceByIndex(this->unk44_31);
                this->unk44_31 = 0;
            }
            break;
    }
}
