// Android no-op replacements for RecompFrontend recompinput exports that
// aren't already provided by android_touch.cpp.
//
// On desktop, recompinput is the SDL/GameController-backed input layer in
// lib/RecompFrontend/recompinput. We don't compile that on Android because
// it depends on SDL2 and the user-facing keybind UI. Touch input is handled
// in android_touch.{h,cpp} and bridged into ultramodern via
// banjo_android::touch::make_input_callbacks().
//
// Don't redeclare symbols already provided by android_touch.cpp:
//   - recompinput::handle_events
//   - recompinput::players::is_single_player_mode
//   - recompinput::players::get_player_is_assigned
//
// If a recompinput call here turns out to need real behavior (e.g. gamepad
// support via Android InputDevice bridging), replace the no-op with a real
// implementation that talks to android_touch's state.

#ifdef __ANDROID__

#include "recompinput/recompinput.h"

namespace recompinput {

// Right-analog suppression flag toggled by camera_and_axis_inversion patches.
// Touch has no right stick, so the flag is meaningless — accept and ignore.
void set_right_analog_suppressed(bool /*suppressed*/) {}

// Right analog stick read. Touch has no right stick → return zero.
void get_right_analog(int /*controller*/, float* x, float* y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
}

// Gyro deltas. No gyro on Android until we wire SensorManager.
void get_gyro_deltas(int /*controller*/, float* x, float* y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
}

// Mouse deltas. No mouse on Android.
void get_mouse_deltas(float* x, float* y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
}

// Whether the current frame should ignore game inputs (e.g. menu open).
// Always false on Android until we add modal touch overlays.
bool game_input_disabled() { return false; }

// Rumble update tick. No haptics yet.
void update_rumble() {}

// Setup-time hooks called from desktop's banjo::init_config(). Android
// doesn't run init_config, but if anything else calls these, they're harmless.
void set_default_mapping_for_controller(GameInput /*input*/, const std::vector<InputField>& /*mapping*/) {}
void set_game_input_name(GameInput /*input*/, const std::string& /*name*/) {}
void set_game_input_description(GameInput /*input*/, const std::string& /*desc*/) {}

namespace players {

void set_single_player_mode(bool /*single*/) {}

}  // namespace players

}  // namespace recompinput

#endif  // __ANDROID__
