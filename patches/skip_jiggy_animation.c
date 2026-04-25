#include "patches.h"
#include "functions.h"
#include "enums.h"

extern int  bsjig_inJiggyJig(s32 state);
extern s32  player_getWaterState(void);
extern s32  player_movementGroup(void);
extern void func_8029CDA0(void);
extern void func_8029CCC4(void);

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
