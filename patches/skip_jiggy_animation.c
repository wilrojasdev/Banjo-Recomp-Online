#include "patches.h"
#include "functions.h"
#include "enums.h"
#include "core2/ba/physics.h"

extern int  bsjig_inJiggyJig(s32 state);
extern s32  player_getWaterState(void);
extern s32  player_movementGroup(void);
extern void func_8029CDA0(void);
extern void func_8029CCC4(void);
extern f32  func_8029B41C(void);
extern void yaw_setIdeal(f32 yaw);
extern void yaw_setUpdateState(s32 state);
extern void func_8029957C(s32);

// Make jiggy collection in plain Banjo behave like the transformed/swimming
// path: skip the BS_44_JIG_JIGGY ceremony entirely, no dance actor spawn,
// no BS state transition, no input lock, no pause. The engine already has a
// fully-working "instant" collection routine (func_8029CCC4) — vanilla just
// gates it behind the transformed/water/long-leg condition. We drop the gate.
//
// func_8029CCC4 handles everything correctly: despawns the dance actor if
// one is alive, increments ITEM_E_JIGGY, plays the collection jingle, runs
// the bookkeeping (HUD, idle counter), and queues the deferred 100-jiggy
// cutscene + 10th-jiggy notedoor logic via timedFunc_set_0(4.0f,
// func_8029CBF4). The "already collecting" guard (sp28) still runs through
// func_8029CDA0 so re-touching during an in-flight collection stays safe.
RECOMP_PATCH s32 func_80295EE0(s32 arg0) {
    s32 current_state = bs_getState();

    if (bsjig_inJiggyJig(current_state)) {
        // Already in BS_44 (only reachable if the engine is mid-state for
        // some other reason). Original behavior.
        func_8029CDA0();
    } else {
        baflag_set(BA_FLAG_7_TOUCHING_JIGGY);
        func_8029CCC4();
    }
    return arg0;
}

// Companion patch to the BS_44 skip above. __baMarker_8028B848 fires the
// "first jiggy" / "10 jiggies" dialog on MM_LOBBY and MM main, and when the
// player is plain Banjo it calls __baMarker_8028B7F4 to face them at the
// camera + freeze velocity for the dance. Vanilla also sets the dynamic
// camera mode to 6 via func_8029151C(0xC), expecting BS_44 to clear it back
// via bsjig_jiggy_end → func_80291548 → func_80291488(2). We never enter
// BS_44, so D_8037C062 stays at 6, and cameraMode_update's `case 0x6: break`
// drops the C-stick camera handler entirely — the camera locks and the
// player can't aim it anymore. Replicate the harmless setup (yaw, physics)
// and drop the func_8029151C call. The dialog still appears with the player
// facing the camera; C buttons keep working.
RECOMP_PATCH void __baMarker_8028B7F4(void) {
    yaw_setIdeal(func_8029B41C());
    yaw_setUpdateState(1);
    func_8029957C(3);
    baphysics_set_type(BA_PHYSICS_NORMAL);
    baphysics_set_target_horizontal_velocity(0.0f);
    // Intentionally omit: func_8029151C(0xC) — see comment above.
}
