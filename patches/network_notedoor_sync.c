#include "patches.h"
#include "functions.h"
#include "enums.h"

// Network bridge
extern u32 recomp_net_is_connected(void);

// Core game APIs (all below 0x80386000, safe to call from patches)
extern void func_802D3D74(Actor *);
extern void func_802FACA4(s32);
extern void func_80324CFC(f32, enum comusic_e, s32);
extern void func_80324D2C(f32, enum comusic_e);
extern void func_8028F918(s32);
extern void func_8028F66C(s32);
extern void func_8032BC60(Actor *, s32, f32[3]);
extern s32  itemscore_noteScores_getTotal(void);
extern s32  ability_isUnlocked(enum ability_e);
extern enum asset_e gcdialog_getCurrentTextId(void);
extern f32  randf(void);
extern f32  randf2(f32, f32);
extern s32  fileProgressFlag_get(enum file_progress_e);
extern void fileProgressFlag_set(enum file_progress_e, s32);
extern void particleEmitter_setAlpha(ParticleEmitter *, s32);
extern s32  progressDialog_setAndTriggerDialog_0(enum volatile_flags_e);

// Door-opening constants replicated from lair overlay (D_8039347C / D_80393494).
// Overlay data can be externed by address but keeping a local copy removes the
// overlay-mapping dependency and makes the intent explicit.
static const s16 NOTEDOOR_REQUIREMENTS[12] = {
    50, 180, 260, 350, 450, 640, 765, 810, 828, 846, 864, 882
};

// Volatile flags that mark doors 2..7 as already opened through cutscenes.
static const s16 NOTEDOOR_PRE_OPEN_FLAGS[6] = {
    0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B
};

// Per-door flag: did WE call func_8028F918(2) to lock the local player while
// this door fades? If yes, we must call func_8028F918(0) at despawn to unlock.
// Indexed by actorTypeSpecificField (1..12); slot 0 unused.
// Without this flag we would always unlock at despawn regardless of whether
// we locked — which fights with whatever other system currently holds control.
static bool s_notedoor_locked_local[13] = { FALSE };

// RECOMP_PATCH of chnotedoor_update.
//
// Purpose: keep the door opening animation + despawn in sync across peers,
// without softlocking the join.
//
// The bug this file fixes:
//   Vanilla only despawns on init OR at the end of the local fade. Once
//   another peer opens the door (flag gets set via flag-sync), the join
//   still sees the door as fully opaque and the fade never plays.
//
// Bugs the PREVIOUS version of this patch introduced:
//   1) Used `remote_forced = flag_set && alpha==0xFF`, which was only
//      true for a single frame. The next frame (alpha=248) the outer
//      condition evaluated to false and the whole fade block was
//      skipped — alpha froze at 248, marker_despawn never ran, and
//      func_8028F918(0) was never called → local player stuck.
//   2) Called func_8028F918(2) for the remote-triggered path too, which
//      locked the control of any peer who happened to be near the door
//      when another player opened it.
//
// Fix:
//   - `flag_set` alone drives "remote triggered" (no alpha gate).
//   - `is_fading` = alpha != 0xFF keeps the outer condition true for
//     every frame until despawn, regardless of flag_set flips.
//   - Control lock is now conditional on (local player approached AND
//     flag wasn't already set by network) and remembered in the
//     s_notedoor_locked_local[] table so we only unlock if we locked.
RECOMP_PATCH void func_80387730(Actor *this) {
    f32 spAC[3];
    ParticleEmitter *temp_s5;
    f32 sp9C[3], sp90[3], sp84[3];
    s32 i;
    f32 phi_f20;
    s32 phi_s4;
    s32 sp6C[3];
    f32 sp60[3];

    func_802D3D74(this);

    s32 door_idx = (s32)this->actorTypeSpecificField;
    enum file_progress_e door_flag = (enum file_progress_e)(door_idx + FILEPROG_39_CCW_OPEN);
    bool flag_set = fileProgressFlag_get(door_flag) != 0;

    if (!this->volatile_initialized) {
        this->volatile_initialized = TRUE;
        this->alpha_124_19 = 0xFF;
        this->unk1C[1] = 0.0f;
        this->unk1C[2] = 3.5f;
        // Reset any stale lock bookkeeping from a previous session.
        if (door_idx >= 1 && door_idx < 13) {
            s_notedoor_locked_local[door_idx] = FALSE;
        }
        if (flag_set) {
            marker_despawn(this->marker);
            return;
        }
        if ((door_idx >= 2) && (door_idx < 8)) {
            if (volatileFlag_get((enum volatile_flags_e)NOTEDOOR_PRE_OPEN_FLAGS[door_idx - 2])) {
                marker_despawn(this->marker);
                return;
            }
        }
    }

    // Pulsing number opacity — runs every frame.
    this->unk1C[1] += this->unk1C[2];
    if (this->unk1C[1] >= 255.0f) {
        this->unk1C[1] = 255.0f;
        this->unk1C[2] = -3.5f;
    }
    if (this->unk1C[1] <= 0.0f) {
        this->unk1C[1] = 0.0f;
        this->unk1C[2] = 3.5f;
    }

    bool is_fading = (this->alpha_124_19 != 0xFF);
    bool can_start_local = !flag_set && ability_isUnlocked(ABILITY_13_1ST_NOTEDOOR);

    // Outer gate. `is_fading` keeps us processing the fade every frame until
    // despawn, so the flag_set transition mid-fade doesn't skip the block.
    if (can_start_local || flag_set || is_fading) {
        player_getPosition(spAC);

        // Hint dialog + "not enough notes" only when the local player could
        // actually start the open (flag not yet set, has the ability).
        if (can_start_local
            && (ml_vec3f_distance(spAC, this->position) < 500.0f)
            && (gcdialog_getCurrentTextId() != 0xF64)) {
            func_802FACA4(0xC);
        }

        s32 required = NOTEDOOR_REQUIREMENTS[door_idx - 1];
        bool has_notes = itemscore_noteScores_getTotal() >= required;

        if (has_notes || flag_set || is_fading) {
            if (this->marker->unk14_21) {
                func_8032BC60(this, 5, sp90);
                func_8032BC60(this, 6, sp84);
                sp9C[0] = (sp90[0] + sp84[0]) / 2;
                sp9C[2] = (sp90[2] + sp84[2]) / 2;
                phi_f20 = 140.0f;
            } else {
                sp9C[0] = this->position[0];
                sp9C[2] = this->position[2];
                phi_f20 = 290.0f;
            }
            sp9C[1] = this->position[1];
            bool local_near = (ml_vec3f_distance(spAC, sp9C) < phi_f20);

            // Start/continue the fade if local is the trigger, the flag
            // arrived from the network, or we're already in progress.
            if (local_near || flag_set || is_fading) {
                if (this->alpha_124_19 == 0xFF) {
                    func_80324CFC(0.0f, COMUSIC_43_ENTER_LEVEL_GLITTER, 32700);
                    func_80324D2C(2.4f, COMUSIC_43_ENTER_LEVEL_GLITTER);
                    // Only lock the local player's control when THIS client
                    // is the one opening the door. Spectating a remote open
                    // (flag arrived via network) leaves control alone so the
                    // peer can keep moving while the animation plays.
                    if (local_near && !flag_set) {
                        func_8028F918(2);
                        if (door_idx >= 1 && door_idx < 13) {
                            s_notedoor_locked_local[door_idx] = TRUE;
                        }
                    }
                }
                if (this->alpha_124_19 < 7U) {
                    this->alpha_124_19 = 0;
                } else {
                    this->alpha_124_19 -= 7;
                }
                if (this->alpha_124_19 == 0) {
                    // Local trigger: set the flag so peers despawn their copy.
                    // Remote trigger: flag is already TRUE — skip to avoid echo.
                    if (!flag_set) {
                        fileProgressFlag_set(door_flag, TRUE);
                    }
                    marker_despawn(this->marker);
                    // Unlock only if we actually locked. Prevents unlocking
                    // control we never took when we were the spectator.
                    if (door_idx >= 1 && door_idx < 13
                        && s_notedoor_locked_local[door_idx]) {
                        func_8028F918(0);
                        func_8028F66C(BS_INTR_35);
                        s_notedoor_locked_local[door_idx] = FALSE;
                    }
                    return;
                }
                if (this->marker->unk14_21) {
                    temp_s5 = partEmitMgr_newEmitter((s32)((f32)this->alpha_124_19 / 11.0));
                    sp6C[2] = 0;
                    particleEmitter_setSprite(temp_s5, ASSET_710_SPRITE_SPARKLE_PURPLE);
                    particleEmitter_setStartingScaleRange(temp_s5, 0.13f, 0.18f);
                    particleEmitter_setFinalScaleRange(temp_s5, 0.08f, 0.13f);
                    particleEmitter_setAccelerationRange(temp_s5, -10.0f, 0.0f, -10.0f, 10.0f, 1600.0f, 10.0f);
                    particleEmitter_setSpawnIntervalRange(temp_s5, 0.0f, 0.01f);
                    particleEmitter_setParticleLifeTimeRange(temp_s5, 1.4f, 1.4f);
                    particleEmitter_setParticleVelocityRange(temp_s5, -100.0f, 100.0f, -100.0f, 100.0f, 0.0f, 100.0f);
                    particleEmitter_setAlpha(temp_s5, this->alpha_124_19);
                    for (phi_s4 = 0; phi_s4 < (s32)((f32)this->alpha_124_19 / 11.0); phi_s4++) {
                        for (i = 0; i < 3; i++) {
                            sp60[i] = randf2(sp90[i], sp84[i]);
                        }
                        particleEmitter_setPosition(temp_s5, sp60);
                        sp6C[0] = (s32)((randf() * 60.0f) + 195.0f);
                        sp6C[1] = (s32)((randf() * 130.0f) + 125.0f);
                        particleEmitter_setRGB(temp_s5, sp6C);
                        particleEmitter_emitN(temp_s5, 1);
                    }
                }
            }
        } else if (can_start_local && (door_idx >= 2)
                   && (ml_vec3f_distance(spAC, this->position) < 290.0f)) {
            progressDialog_setAndTriggerDialog_0(VOLATILE_FLAG_B0_NOT_ENOUGH_NOTES);
        }
    }
}
